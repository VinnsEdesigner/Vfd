/**
 * Presentation hooks: the only surface the UI is allowed to consume.
 */
export { MediaApiProvider, useMediaApi } from './provider';
export { queryKeys } from './query-keys';
export { useHealth, useCapabilities, useStorageConfig, useSystemStorage } from './use-system';
export {
  useDownloads,
  useDownload,
  useCreateDownload,
  useCancelDownload,
  useRemoveDownload,
} from './use-downloads';
export { useProbe, useDetectBytes } from './use-media';
export type { ProbeVariables } from './use-media';
export { useDownloadViews } from './use-download-views';
export {
  qualityLabel,
  toQualityOptions,
  QUALITY_STEPS,
  formatBytes,
  formatRate,
  formatDuration,
  formatTimestamp,
  formatKind,
  fileNameFromUrl,
  statusTone,
  statusLabel,
  isActiveState,
  isSettledState,
} from './view-models';
export type { DownloadView, StatusTone, QualityOption } from './view-models';

// Error helpers, re-exported so `ui` never reaches into `api-client` directly.
export { ApiError, NetworkError, isApiError, isNetworkError } from '../api-client';
export type { CreateDownloadVariables, RemoveDownloadVariables } from './use-downloads';

// Re-exported from stores so the UI has exactly one import surface (`../hooks`).
export { useConnectionStore, connectionSettings, DEFAULT_CONNECTION } from '../stores/connection-store';
export type { ConnectionSettings } from '../stores/connection-store';
export { useStorageStore, effectiveCredentials } from '../stores/storage-store';
export type { StorageState } from '../stores/storage-store';
export { useDownloadFormStore, DEFAULT_DOWNLOAD_DRAFT } from '../stores/download-form-store';
export type { DownloadFormState } from '../stores/download-form-store';
