import js from '@eslint/js';
import tseslint from 'typescript-eslint';
import boundaries from 'eslint-plugin-boundaries';

/**
 * Layered architecture, enforced by lint rather than convention.
 *
 *   ui        -> hooks, stores            (presentation; may NOT touch data)
 *   stores    -> api-client               (client state; no React)
 *   hooks     -> rest, domain, stores     (presentation adapters)
 *   rest      -> api-client, domain       (typed resources)
 *   domain    -> nothing (pure)           (models + normalizers)
 *   api-client-> domain only              (transport; no app knowledge)
 *
 * The rule that matters most: `ui` can never import from `domain`, `rest` or
 * `api-client`. That is what keeps the data layer swappable.
 */
export default tseslint.config(
  // The mock is a standalone, build-free browser script: excluded from the
  // typed app's lint so its DOM globals don't trip no-undef.
  { ignores: ['dist', 'node_modules', 'coverage', 'mock/**'] },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ['**/*.{ts,tsx}'],
    plugins: { boundaries },
    settings: {
      // The boundaries plugin uses eslint-module-utils/resolve, which needs a
      // resolver that understands TypeScript extensions and path aliases.
      'import/resolver': {
        typescript: {
          alwaysTryTypes: true,
          project: './tsconfig.json',
        },
        node: true,
      },
      'boundaries/elements': [
        { type: 'ui', pattern: 'src/ui/**', mode: 'full' },
        { type: 'hooks', pattern: 'src/hooks/**', mode: 'full' },
        { type: 'stores', pattern: 'src/stores/**', mode: 'full' },
        { type: 'rest', pattern: 'src/rest/**', mode: 'full' },
        { type: 'domain', pattern: 'src/domain/**', mode: 'full' },
        { type: 'api-client', pattern: 'src/api-client/**', mode: 'full' },
      ],
      'boundaries/ignore': ['**/*.test.ts', '**/*.test.tsx', 'tests/**'],
    },
    rules: {
      'boundaries/element-types': [
        'error',
        {
          default: 'disallow',
          rules: [
            // Presentation. May only reach the presentation-adjacent layers.
            { from: 'ui', allow: ['ui', 'hooks', 'stores'] },
            // Presentation state. Talks to transport + domain types, not REST resources.
            { from: 'stores', allow: ['stores', 'api-client', 'domain'] },
            // Presentation adapters. Owns the mapping between REST and UI.
            { from: 'hooks', allow: ['hooks', 'rest', 'domain', 'stores'] },
            // Typed resources over the transport.
            { from: 'rest', allow: ['rest', 'api-client', 'domain'] },
            // Pure domain. No imports from any other layer.
            { from: 'domain', allow: ['domain'] },
            // Transport. Depends only on the domain (for types) and itself.
            { from: 'api-client', allow: ['api-client', 'domain'] },
          ],
        },
      ],
      'boundaries/no-unknown': 'off',
      'boundaries/no-ignored': 'off',
      'boundaries/no-unknown-files': 'off',
    },
  },
  {
    // Tests may reach into any layer: they are the place we assert on internals.
    files: ['**/*.test.ts', '**/*.test.tsx', 'tests/**/*.ts'],
    rules: {
      'boundaries/element-types': 'off',
    },
  },
);
