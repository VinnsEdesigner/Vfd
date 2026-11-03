/// <reference types="vite/client" />

/**
 * Ambient declarations for the file-system APIs.
 *
 * TypeScript's bundled DOM lib (5.6) ships `FileSystemDirectoryHandle` without
 * its async iterator, and types `createWritable().write` against a plain
 * `ArrayBuffer` rather than the `ArrayBufferLike` a `Uint8Array` actually
 * carries. Both are gaps in the lib, not in the platform, so they are declared
 * here rather than worked around with casts at every call site.
 */

interface FileSystemHandle {
  /** Present on every modern engine; the spec makes this a plain method. */
  queryPermission?(descriptor?: { mode?: 'read' | 'readwrite' }): Promise<PermissionState>;
  requestPermission?(descriptor?: { mode?: 'read' | 'readwrite' }): Promise<PermissionState>;
}

interface FileSystemDirectoryHandle {
  entries(): AsyncIterableIterator<[string, FileSystemHandle]>;
  keys(): AsyncIterableIterator<string>;
  values(): AsyncIterableIterator<FileSystemHandle>;
}

interface FileSystemWritableFileStream {
  /**
   * The lib types this as `BufferSource`, which resolves to a view over a plain
   * `ArrayBuffer`. A `Uint8Array` read from a fetch stream is backed by
   * `ArrayBufferLike`, so the widened overload is added rather than casting at
   * every call site.
   */
  write(data: ArrayBufferView | Blob | string): Promise<void>;
}

interface Window {
  showDirectoryPicker?(options?: {
    id?: string;
    mode?: 'read' | 'readwrite';
    startIn?: string;
  }): Promise<FileSystemDirectoryHandle>;
}
