/**
 * Starts the compiled C server for integration tests.
 *
 * The binary is built by the Makefile in ../media-ingest-c. We spawn it with
 * `--port 0` and read the bound port from its first stdout line, so tests never
 * collide on a fixed port.
 */

import { spawn, type ChildProcessByStdio } from 'node:child_process';
import type { Readable } from 'node:stream';

type ServerProcess = ChildProcessByStdio<null, Readable, Readable>;
import { existsSync } from 'node:fs';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..', '..');
const serverBin = join(repoRoot, 'media-ingest-c', 'build', 'media-ingest');

export interface ServerHandle {
  baseUrl: string;
  port: number;
  stop: () => Promise<void>;
}

export interface StartServerOptions {
  authToken?: string;
  env?: Record<string, string>;
}

export async function startServer(options: StartServerOptions = {}): Promise<ServerHandle> {
  if (!existsSync(serverBin)) {
    throw new Error(
      `server binary not found at ${serverBin}. Run "make" in media-ingest-c first.`,
    );
  }

  const workDir = await mkdtemp(join(tmpdir(), 'mi-web-test-'));
  const args = ['serve', '--port', '0', '--work-dir', workDir, '--host', '127.0.0.1'];
  if (options.authToken) args.push('--auth-token', options.authToken);

  const child: ServerProcess = spawn(serverBin, args, {
    env: { ...process.env, ...options.env },
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  const port = await readPort(child);

  return {
    baseUrl: `http://127.0.0.1:${port}`,
    port,
    stop: async () => {
      await new Promise<void>((resolveStop) => {
        child.once('exit', () => resolveStop());
        child.kill('SIGTERM');
        // Fall back to SIGKILL if it does not exit promptly.
        setTimeout(() => child.kill('SIGKILL'), 2000);
      });
      await rm(workDir, { recursive: true, force: true });
    },
  };
}

/** The server prints {"listening":...,"port":N,...} as its first stdout line. */
function readPort(child: ServerProcess): Promise<number> {
  return new Promise((resolvePort, rejectPort) => {
    let buffered = '';
    const timer = setTimeout(() => {
      rejectPort(new Error('timed out waiting for the server to report its port'));
    }, 10_000);

    child.stdout.on('data', (chunk: Buffer) => {
      buffered += chunk.toString('utf8');
      const line = buffered.split('\n')[0];
      if (!line) return;
      try {
        const parsed = JSON.parse(line) as { port?: number };
        if (typeof parsed.port === 'number') {
          clearTimeout(timer);
          resolvePort(parsed.port);
        }
      } catch {
        /* not the handshake line yet */
      }
    });

    child.stderr.on('data', (chunk: Buffer) => {
      process.stderr.write(`[server] ${chunk.toString('utf8')}`);
    });

    child.once('exit', (code) => {
      clearTimeout(timer);
      rejectPort(new Error(`server exited early with code ${code}`));
    });
  });
}
