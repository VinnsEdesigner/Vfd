/**
 * Presentation hooks: the only surface the UI is allowed to consume.
 */
export { MediaApiProvider, useMediaApi } from './provider';
export { queryKeys } from './query-keys';
export { searchCommons, SUPPORTED_KINDS } from '../rest/browse';
export { browseKindFor, browseKindLabel } from '../domain/browse';
export type { BrowseFilter, BrowseItem, BrowseKind, BrowseQuery, BrowseResult } from '../domain/browse';
export { useHealth, useCapabilities, useStorageConfig, useSystemStorage, useSetStorageLocation } from './use-system';
export {
  useDownloads,
  useDownload,
  useCreateDownload,
  useCancelDownload,
  useRemoveDownload,
  useRenameDownload,
  useMoveDownload,
  usePauseDownload,
  useResumeDownload,
  useConvertDownload,
} from './use-downloads';
export { useProbe, useDetectBytes } from './use-media';
export { useBrowse, useBrowseUrl } from './use-browse';
export type { UseBrowseArgs } from './use-browse';
export { useLocalMedia } from './use-local-media';
export type { LocalMediaState } from './use-local-media';
export type { ProbeVariables } from './use-media';
export { useDownloadViews, useDownloadViewsWithPipeline } from './use-download-views';
export type { DownloadPipelineOptions } from './use-download-views';
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
  extensionOf,
  hostFromUrl,
  pathFromUrl,
  pipelineStages,
  statusTone,
  statusLabel,
  isActiveState,
  isSettledState,
  isResumableState,
} from './view-models';
export type {
  DownloadView,
  StatusTone,
  QualityOption,
  PipelineStageKey,
  PipelineStageStatus,
  PipelineStageView,
  PipelineOptions,
} from './view-models';
export type { Conversion, ConversionState } from '../domain/media';

// Error helpers, re-exported so `ui` never reaches into `api-client` directly.
export { ApiError, NetworkError, isApiError, isNetworkError } from '../api-client';
export type {
  CreateDownloadVariables,
  RemoveDownloadVariables,
  RenameDownloadVariables,
  MoveDownloadVariables,
} from './use-downloads';

// Re-exported from stores so the UI has exactly one import surface (`../hooks`).
export { useConnectionStore, connectionSettings, DEFAULT_CONNECTION } from '../stores/connection-store';
export type { ConnectionSettings } from '../stores/connection-store';
export { useLocalStorageStore } from '../stores/local-storage-store';
export type { LocalStorageChoice, LocalStorageState } from '../stores/local-storage-store';
export { useStorageStore, effectiveCredentials } from '../stores/storage-store';
export type { StorageState } from '../stores/storage-store';
export { useDownloadFormStore, DEFAULT_DOWNLOAD_DRAFT } from '../stores/download-form-store';
export type { DownloadFormState } from '../stores/download-form-store';
