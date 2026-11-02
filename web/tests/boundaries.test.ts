/**
 * Architecture guard.
 *
 * The ESLint `boundaries/element-types` rule is the real enforcement, but it
 * only runs in `npm run lint`. This test makes the same invariant part of the
 * unit suite so a violation fails `npm test` too: it lints a fixture that
 * imports across a forbidden edge and asserts the rule rejects it, and it
 * asserts the real source tree is currently clean.
 */

import { ESLint } from 'eslint';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';

const webRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

let eslint: ESLint;
let fixtureDir: string;

beforeAll(async () => {
  eslint = new ESLint({ cwd: webRoot });
  fixtureDir = await mkdtemp(join(tmpdir(), 'mi-boundary-'));
});

afterAll(async () => {
  await rm(fixtureDir, { recursive: true, force: true });
});

async function lintText(code: string, filePath: string): Promise<string[]> {
  const results = await eslint.lintText(code, { filePath });
  return results.flatMap((r) => r.messages.map((m) => `${m.ruleId}: ${m.message}`));
}

describe('layer boundaries', () => {
  it('rejects a ui -> domain import', async () => {
    const messages = await lintText(
      "import { normalizeDownload } from '../domain/normalize';\nexport const x = normalizeDownload;\n",
      join(webRoot, 'src', 'ui', 'violation.ts'),
    );
    expect(messages.some((m) => m.includes('boundaries/element-types'))).toBe(true);
  });

  it('rejects a ui -> rest import', async () => {
    const messages = await lintText(
      "import { createRestApi } from '../rest';\nexport const x = createRestApi;\n",
      join(webRoot, 'src', 'ui', 'violation.ts'),
    );
    expect(messages.some((m) => m.includes('boundaries/element-types'))).toBe(true);
  });

  it('rejects a ui -> api-client import', async () => {
    const messages = await lintText(
      "import { ApiError } from '../api-client/errors';\nexport const x = ApiError;\n",
      join(webRoot, 'src', 'ui', 'violation.ts'),
    );
    expect(messages.some((m) => m.includes('boundaries/element-types'))).toBe(true);
  });

  it('rejects a domain -> rest import (domain must stay pure)', async () => {
    const messages = await lintText(
      "import { createRestApi } from '../rest';\nexport const x = createRestApi;\n",
      join(webRoot, 'src', 'domain', 'violation.ts'),
    );
    expect(messages.some((m) => m.includes('boundaries/element-types'))).toBe(true);
  });

  it('allows a ui -> hooks import', async () => {
    const messages = await lintText(
      "import { useDownloads } from '../hooks';\nexport const x = useDownloads;\n",
      join(webRoot, 'src', 'ui', 'allowed.ts'),
    );
    expect(messages.filter((m) => m.includes('boundaries/element-types'))).toEqual([]);
  });

  it('the real source tree is boundary-clean', async () => {
    const results = await eslint.lintFiles(['src']);
    const violations = results.flatMap((r) =>
      r.messages.filter((msg) => msg.ruleId === 'boundaries/element-types').map(() => r.filePath),
    );
    expect(violations).toEqual([]);
  }, 30_000);
});
