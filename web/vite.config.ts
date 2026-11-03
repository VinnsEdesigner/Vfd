import { defineConfig, type Plugin } from 'vite';
import react from '@vitejs/plugin-react';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { readdirSync, readFileSync, statSync, writeFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

/**
 * Stamp the service worker with a hash of the built assets.
 *
 * The worker caches the app shell and purges caches whose name does not match
 * its VERSION. That only works if VERSION changes when the build does — with a
 * hand-written constant it never did, so a new deploy kept serving the old
 * bundle from the cache and the user saw stale code.
 *
 * Hashing the emitted files makes the version a function of the output: any
 * change to the bundle changes the cache name, the old cache is deleted on
 * activate, and the next load fetches the new build.
 */
function stampServiceWorker(): Plugin {
  let outDir = 'dist';

  const hashDir = (dir: string): string => {
    const hash = createHash('sha256');
    const walk = (d: string): void => {
      for (const name of readdirSync(d).sort()) {
        const full = join(d, name);
        if (statSync(full).isDirectory()) {
          walk(full);
        } else if (!full.endsWith('sw.js')) {
          // sw.js carries the hash, so hashing it would be circular.
          hash.update(name);
          hash.update(readFileSync(full));
        }
      }
    };
    walk(dir);
    return hash.digest('hex').slice(0, 12);
  };

  return {
    name: 'stamp-service-worker',
    apply: 'build',
    configResolved(config) {
      outDir = config.build.outDir;
    },
    closeBundle() {
      const swPath = join(outDir, 'sw.js');
      if (!existsSync(swPath)) return;
      const version = hashDir(outDir);
      const source = readFileSync(swPath, 'utf8');
      if (!source.includes("const VERSION = 'v1'")) {
        this.warn('sw.js has no VERSION placeholder to stamp; cache busting may not work');
        return;
      }
      writeFileSync(swPath, source.replace("const VERSION = 'v1'", `const VERSION = '${version}'`));
    },
  };
}

export default defineConfig({
  plugins: [react(), stampServiceWorker()],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: ['./vitest.setup.ts'],
    include: ['src/**/*.test.{ts,tsx}', 'tests/**/*.test.{ts,tsx}'],
  },
});
