/**
 * Remembering a folder the user granted.
 *
 * A `FileSystemDirectoryHandle` cannot go in localStorage — it is a structured
 * object, not a string. It CAN go in IndexedDB, which is how editors like VS Code
 * remember recent folders. Doing that is the difference between "pick your
 * downloads folder once" and "pick it every single time you open the app".
 *
 * Remembering the handle is not the same as having permission. The browser
 * decides that, and it can revoke it (a tab backgrounded for a while, a browser
 * restart, a cleared profile). So the handle is stored alongside its permission
 * state, and `requestPermission` is only ever called from a real user gesture —
 * a silent call would be a security error, and pretending it worked would be
 * worse.
 */

const DB_NAME = 'vyzorix-handles';
const STORE = 'directories';
const KEY = 'media-dir';

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE)) db.createObjectStore(STORE);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error ?? new Error('could not open the handle store'));
  });
}

function run<T>(
  db: IDBDatabase,
  mode: IDBTransactionMode,
  fn: (s: IDBObjectStore) => IDBRequest<T>,
): Promise<T> {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, mode);
    const req = fn(tx.objectStore(STORE));
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error ?? new Error('handle store request failed'));
  });
}

/** What the browser currently says about our access to the folder. */
export type DirectoryPermission = 'granted' | 'prompt' | 'denied' | 'unknown';

export async function saveDirectoryHandle(handle: FileSystemDirectoryHandle): Promise<void> {
  try {
    const db = await openDb();
    await run(db, 'readwrite', (s) => s.put(handle, KEY));
  } catch {
    // Remembering is a convenience; failing to remember must not fail the choice.
  }
}

export async function loadDirectoryHandle(): Promise<FileSystemDirectoryHandle | null> {
  try {
    const db = await openDb();
    const handle = await run<FileSystemDirectoryHandle | undefined>(db, 'readonly', (s) =>
      s.get(KEY),
    );
    return handle ?? null;
  } catch {
    return null;
  }
}

export async function forgetDirectoryHandle(): Promise<void> {
  try {
    const db = await openDb();
    await run(db, 'readwrite', (s) => s.delete(KEY));
  } catch {
    /* nothing to forget */
  }
}

/**
 * Read the current permission without prompting. Safe to call on load, because
 * `queryPermission` never shows UI.
 */
export async function queryDirectoryPermission(
  handle: FileSystemDirectoryHandle,
): Promise<DirectoryPermission> {
  if (typeof handle.queryPermission !== 'function') return 'unknown';
  try {
    const state = await handle.queryPermission({ mode: 'readwrite' });
    return state === 'granted' || state === 'prompt' || state === 'denied' ? state : 'unknown';
  } catch {
    return 'unknown';
  }
}

/**
 * Ask for permission. MUST be called from a user gesture — the browser throws
 * otherwise, and this returns 'denied' rather than pretending it succeeded.
 */
export async function requestDirectoryPermission(
  handle: FileSystemDirectoryHandle,
): Promise<DirectoryPermission> {
  if (typeof handle.requestPermission !== 'function') return 'unknown';
  try {
    const state = await handle.requestPermission({ mode: 'readwrite' });
    return state === 'granted' || state === 'prompt' || state === 'denied' ? state : 'unknown';
  } catch {
    return 'denied';
  }
}

/**
 * Restore the remembered folder and report whether it is usable right now.
 *
 * Returns the handle either way, because a handle that needs re-granting is
 * still worth keeping — the UI can offer one tap to re-grant rather than sending
 * the user through the picker again.
 */
export async function restoreDirectory(): Promise<{
  handle: FileSystemDirectoryHandle | null;
  permission: DirectoryPermission;
}> {
  const handle = await loadDirectoryHandle();
  if (!handle) return { handle: null, permission: 'unknown' };
  return { handle, permission: await queryDirectoryPermission(handle) };
}
