/**
 * Local media storage, one implementation per tier.
 *
 * All four tiers satisfy the same small interface, so the download flow does not
 * care which one the user chose. What differs is *how* bytes land, and that is
 * the whole point of offering a choice:
 *
 *   FileSystemStore  streams into a real file the user can find. No quota.
 *   OpfsStore        streams into the sandbox. Large, but invisible.
 *   IndexedDbStore   buffers the file and stores a Blob. Universal, bounded.
 *   MemoryStore      an object URL that dies with the tab.
 *
 * Nothing here silently falls back. If the chosen tier cannot take the bytes the
 * caller is told, because quietly writing somewhere else is how a user ends up
 * with a file they cannot find.
 */

import type { StorageTierId } from './storage-capabilities';

export interface StoredItem {
  /** Stable id, used as the key in every tier. */
  id: string;
  /** Filename as it should appear to the user. */
  name: string;
  /** MIME type, so the object URL and the file carry the right one. */
  type: string;
  size: number;
  /** Where it actually went. */
  tier: StorageTierId;
  /** Where a user can find it, when that is meaningful. */
  locationHint?: string;
}

/** A file opened for writing. Bytes go in as they arrive from the network. */
export interface StoredWriter {
  write(chunk: Uint8Array): Promise<void>;
  /** Commits. Must be called; an aborted writer leaves nothing behind. */
  close(size: number): Promise<void>;
  abort(): Promise<void>;
}

export interface MediaStore {
  readonly tier: StorageTierId;
  /** Writes a new item, replacing any previous one with the same id. */
  createWriter(item: Omit<StoredItem, 'tier' | 'size'>): Promise<StoredWriter>;
  /** A URL the media element can play. Object URLs must be released. */
  urlFor(id: string): Promise<string | null>;
  /** Releases a URL from `urlFor`. A no-op for tiers without object URLs. */
  release(url: string): void;
  /** Everything currently stored. */
  list(): Promise<StoredItem[]>;
  get(id: string): Promise<StoredItem | null>;
  remove(id: string): Promise<void>;
  /** Total bytes held. */
  usage(): Promise<number>;
}

/* ------------------------------ helpers ------------------------------ */

function isNotFound(e: unknown): boolean {
  return e instanceof DOMException && (e.name === 'NotFoundError' || e.name === 'TypeMismatchError');
}

/** Splits an id into a safe directory + filename for the sandbox tiers. */
function safeName(name: string): string {
  const cleaned = name.replace(/[/\\]/g, '_').replace(/^\.+/, '');
  return cleaned.length > 0 ? cleaned : 'download';
}

/* ------------------------------ memory ------------------------------ */

interface MemoryEntry {
  item: StoredItem;
  bytes: Uint8Array;
}

/**
 * Session-only storage. Honest about its one guarantee: nothing survives.
 */
export class MemoryStore implements MediaStore {
  readonly tier = 'memory' as const;
  private items = new Map<string, MemoryEntry>();

  async createWriter(item: Omit<StoredItem, 'tier' | 'size'>): Promise<StoredWriter> {
    const chunks: Uint8Array[] = [];
    let total = 0;
    return {
      write: async (chunk) => {
        chunks.push(chunk);
        total += chunk.byteLength;
      },
      close: async () => {
        const bytes = new Uint8Array(total);
        let at = 0;
        for (const c of chunks) {
          bytes.set(c, at);
          at += c.byteLength;
        }
        this.items.set(item.id, {
          item: { ...item, tier: 'memory', size: total },
          bytes,
        });
      },
      abort: async () => {
        chunks.length = 0;
      },
    };
  }

  async urlFor(id: string): Promise<string | null> {
    const e = this.items.get(id);
    if (!e) return null;
    // Copy into a fresh buffer: the Blob must not alias a detached array.
    // A fresh copy, so the Blob never aliases a buffer we might reuse.
    return URL.createObjectURL(new Blob([e.bytes.slice()], { type: e.item.type }));
  }

  release(url: string): void {
    URL.revokeObjectURL(url);
  }

  async list(): Promise<StoredItem[]> {
    return [...this.items.values()].map((e) => e.item);
  }

  async get(id: string): Promise<StoredItem | null> {
    return this.items.get(id)?.item ?? null;
  }

  async remove(id: string): Promise<void> {
    this.items.delete(id);
  }

  async usage(): Promise<number> {
    let total = 0;
    for (const e of this.items.values()) total += e.item.size;
    return total;
  }
}

/* ------------------------------ OPFS ------------------------------ */

/**
 * The origin private file system. Real files, written incrementally through a
 * sync access handle in a worker-free path (the async variant, which is enough
 * here and avoids a worker just for this).
 */
export class OpfsStore implements MediaStore {
  readonly tier = 'opfs' as const;
  private urls = new Map<string, string>();

  constructor(private root: FileSystemDirectoryHandle) {}

  private async dir(id: string, create: boolean): Promise<FileSystemDirectoryHandle | null> {
    try {
      return await this.root.getDirectoryHandle(id, { create });
    } catch (e) {
      if (isNotFound(e)) return null;
      throw e;
    }
  }

  async createWriter(item: Omit<StoredItem, 'tier' | 'size'>): Promise<StoredWriter> {
    const dir = await this.dir(item.id, true);
    if (!dir) throw new Error('could not open the storage directory');
    const file = await dir.getFileHandle('source', { create: true });
    const writable = await file.createWritable();
    return {
      write: async (chunk) => {
        await writable.write(chunk);
      },
      close: async (size) => {
        await writable.close();
        await dir
          .getFileHandle('meta.json', { create: true })
          .then((h) => h.createWritable())
          .then(async (w) => {
            await w.write(JSON.stringify({ ...item, size }));
            await w.close();
          });
      },
      abort: async () => {
        await writable.abort().catch(() => undefined);
        await this.remove(item.id).catch(() => undefined);
      },
    };
  }

  async urlFor(id: string): Promise<string | null> {
    const dir = await this.dir(id, false);
    if (!dir) return null;
    const file = await dir.getFileHandle('source').catch(() => null);
    if (!file) return null;
    const blob = await file.getFile();
    const url = URL.createObjectURL(blob);
    this.urls.set(id, url);
    return url;
  }

  release(url: string): void {
    URL.revokeObjectURL(url);
  }

  async list(): Promise<StoredItem[]> {
    const out: StoredItem[] = [];
    for await (const [name, handle] of this.root.entries()) {
      if (handle.kind !== 'directory') continue;
      const meta = await this.readMeta(name);
      if (meta) out.push(meta);
    }
    return out;
  }

  private async readMeta(id: string): Promise<StoredItem | null> {
    const dir = await this.dir(id, false);
    if (!dir) return null;
    const handle = await dir.getFileHandle('meta.json').catch(() => null);
    if (!handle) return null;
    try {
      const text = await (await handle.getFile()).text();
      const parsed = JSON.parse(text) as Omit<StoredItem, 'tier'>;
      return { ...parsed, tier: 'opfs' };
    } catch {
      return null;
    }
  }

  async get(id: string): Promise<StoredItem | null> {
    return this.readMeta(id);
  }

  async remove(id: string): Promise<void> {
    const url = this.urls.get(id);
    if (url) {
      URL.revokeObjectURL(url);
      this.urls.delete(id);
    }
    await this.root.removeEntry(id, { recursive: true }).catch(() => undefined);
  }

  async usage(): Promise<number> {
    const items = await this.list();
    return items.reduce((a, i) => a + i.size, 0);
  }
}

/* ------------------------------ IndexedDB ------------------------------ */

const DB_NAME = 'vyzorix-media';
const STORE = 'files';

interface IdbRecord {
  id: string;
  name: string;
  type: string;
  size: number;
  blob: Blob;
}

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE)) db.createObjectStore(STORE, { keyPath: 'id' });
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error ?? new Error('could not open IndexedDB'));
  });
}

function tx<T>(db: IDBDatabase, mode: IDBTransactionMode, fn: (s: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    const t = db.transaction(STORE, mode);
    const req = fn(t.objectStore(STORE));
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error ?? new Error('IndexedDB request failed'));
  });
}

/**
 * The universal fallback. It buffers the whole file before storing it, which is
 * why this tier is marked as unsuitable for very large media rather than being
 * presented as equivalent to the others.
 */
export class IndexedDbStore implements MediaStore {
  readonly tier = 'indexeddb' as const;
  private db: IDBDatabase | null = null;
  private urls = new Map<string, string>();

  private async database(): Promise<IDBDatabase> {
    this.db ??= await openDb();
    return this.db;
  }

  async createWriter(item: Omit<StoredItem, 'tier' | 'size'>): Promise<StoredWriter> {
    const db = await this.database();
    const chunks: BlobPart[] = [];
    return {
      write: async (chunk) => {
        // Copied, because the caller reuses its buffer between chunks.
        chunks.push(chunk.slice());
      },
      close: async () => {
        const blob = new Blob(chunks, { type: item.type });
        const rec: IdbRecord = { ...item, size: blob.size, blob };
        await tx(db, 'readwrite', (s) => s.put(rec));
      },
      abort: async () => {
        chunks.length = 0;
      },
    };
  }

  async urlFor(id: string): Promise<string | null> {
    const db = await this.database();
    const rec = await tx<IdbRecord | undefined>(db, 'readonly', (s) => s.get(id));
    if (!rec) return null;
    const url = URL.createObjectURL(rec.blob);
    this.urls.set(id, url);
    return url;
  }

  release(url: string): void {
    URL.revokeObjectURL(url);
  }

  async list(): Promise<StoredItem[]> {
    const db = await this.database();
    const recs = await tx<IdbRecord[]>(db, 'readonly', (s) => s.getAll());
    return recs.map((r) => ({
      id: r.id,
      name: r.name,
      type: r.type,
      size: r.size,
      tier: 'indexeddb' as const,
    }));
  }

  async get(id: string): Promise<StoredItem | null> {
    const db = await this.database();
    const rec = await tx<IdbRecord | undefined>(db, 'readonly', (s) => s.get(id));
    return rec
      ? { id: rec.id, name: rec.name, type: rec.type, size: rec.size, tier: 'indexeddb' }
      : null;
  }

  async remove(id: string): Promise<void> {
    const url = this.urls.get(id);
    if (url) {
      URL.revokeObjectURL(url);
      this.urls.delete(id);
    }
    const db = await this.database();
    await tx(db, 'readwrite', (s) => s.delete(id));
  }

  async usage(): Promise<number> {
    const items = await this.list();
    return items.reduce((a, i) => a + i.size, 0);
  }
}

/* ------------------------------ File System Access ------------------------------ */

/**
 * A directory the user picked. Files are written where they asked, with the name
 * they expect, and no quota applies — this is the only tier where "download to my
 * computer" means literally that.
 */
export class FileSystemStore implements MediaStore {
  readonly tier = 'file-system-access' as const;
  private urls = new Map<string, string>();

  constructor(private dir: FileSystemDirectoryHandle) {}

  get directoryName(): string {
    return this.dir.name;
  }

  async createWriter(item: Omit<StoredItem, 'tier' | 'size'>): Promise<StoredWriter> {
    const handle = await this.dir.getFileHandle(safeName(item.name), { create: true });
    const writable = await handle.createWritable();
    return {
      write: async (chunk) => {
        await writable.write(chunk);
      },
      close: async () => {
        await writable.close();
      },
      abort: async () => {
        await writable.abort().catch(() => undefined);
      },
    };
  }

  async urlFor(id: string): Promise<string | null> {
    // The id is the filename in this tier, so the file can be reopened by name.
    const handle = await this.dir.getFileHandle(id).catch(() => null);
    if (!handle) return null;
    const blob = await handle.getFile();
    const url = URL.createObjectURL(blob);
    this.urls.set(id, url);
    return url;
  }

  release(url: string): void {
    URL.revokeObjectURL(url);
  }

  async list(): Promise<StoredItem[]> {
    const out: StoredItem[] = [];
    for await (const [name, handle] of this.dir.entries()) {
      if (handle.kind !== 'file') continue;
      const fileHandle = handle as FileSystemFileHandle;
      const file = await fileHandle.getFile().catch(() => null);
      if (!file) continue;
      out.push({
        id: name,
        name,
        type: file.type || 'application/octet-stream',
        size: file.size,
        tier: 'file-system-access',
        locationHint: this.dir.name,
      });
    }
    return out;
  }

  async get(id: string): Promise<StoredItem | null> {
    const handle = await this.dir.getFileHandle(id).catch(() => null);
    if (!handle) return null;
    const file = await handle.getFile();
    return {
      id,
      name: file.name,
      type: file.type || 'application/octet-stream',
      size: file.size,
      tier: 'file-system-access',
      locationHint: this.dir.name,
    };
  }

  async remove(id: string): Promise<void> {
    const url = this.urls.get(id);
    if (url) {
      URL.revokeObjectURL(url);
      this.urls.delete(id);
    }
    await this.dir.removeEntry(id).catch(() => undefined);
  }

  async usage(): Promise<number> {
    const items = await this.list();
    return items.reduce((a, i) => a + i.size, 0);
  }
}

/* ------------------------------ construction ------------------------------ */

export interface CreateStoreOptions {
  tier: StorageTierId;
  /** Required for the file-system tier: the directory the user chose. */
  directory?: FileSystemDirectoryHandle;
}

/**
 * Builds the store for a tier. Throws when the tier cannot be constructed, so a
 * caller never gets a store that silently writes somewhere else.
 */
export async function createMediaStore(options: CreateStoreOptions): Promise<MediaStore> {
  switch (options.tier) {
    case 'file-system-access': {
      if (!options.directory) throw new Error('a directory is required for this storage choice');
      return new FileSystemStore(options.directory);
    }
    case 'opfs': {
      const root = await navigator.storage.getDirectory();
      const dir = await root.getDirectoryHandle('media', { create: true });
      return new OpfsStore(dir);
    }
    case 'indexeddb':
      return new IndexedDbStore();
    case 'memory':
    default:
      return new MemoryStore();
  }
}
