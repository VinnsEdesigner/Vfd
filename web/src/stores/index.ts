/**
 * Presentation stores: client-side state, framework-agnostic.
 */
export { useConnectionStore, connectionSettings, DEFAULT_CONNECTION } from './connection-store';
export type { ConnectionSettings } from './connection-store';
export { useStorageStore, effectiveCredentials } from './storage-store';
export type { StorageState } from './storage-store';
export { useDownloadFormStore, DEFAULT_DOWNLOAD_DRAFT } from './download-form-store';
export type { DownloadFormState } from './download-form-store';
