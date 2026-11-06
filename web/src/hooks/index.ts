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
export { useCrawl } from './use-crawl';
export type { CrawlVariables } from './use-crawl';
export type { CrawlHit, CrawlQuery, CrawlReport, CrawlResult } from '../domain/browse';
export { useExtract } from './use-extract';
export type { ExtractVariables } from './use-extract';
export type { ExtractEntry, ExtractQuery, ExtractResult } from '../domain/browse';
/* The sites list is presentation data, but it is domain-shaped, so `ui` reaches
 * it through this layer like everything else. */
export { KNOWN_SITES, accessLabel, canSearch } from '../domain/sites';
export type { SiteEntry, SiteAccess } from '../domain/sites';
export { useBrowse, useBrowseUrl, usePageFrame, useRenderPage } from './use-browse';
export type { UseBrowseArgs } from './use-browse';
export { useLiveSession, lastLiveUrl } from './use-live';
export type { UseLiveSession } from './use-live';
export { useLiveVideo } from './use-live-video';
export type { UseLiveVideo, LiveVideoStatus } from './use-live-video';
export type { LiveVideoState, LiveVideoViewport } from '../domain/rtc';
/* The media a live page loaded, and how to read its numbers. Re-exported here
 * because `ui` may only reach `domain` through this layer. */
export type {
  LiveMediaItem,
  LiveMediaKind,
  LiveMediaVariant,
} from '../domain/live-media';
export {
  formatMediaSize,
  formatBandwidth,
  mediaFormatLabel,
  mediaLabel,
  mediaThumb,
} from '../domain/live-media';
export type { LiveStreamStats } from '../rest/browse';
export type {
  FetchedPage,
  LiveFailure,
  LiveFrame,
  LiveMessage,
  LivePageState,
  RenderFailure,
  RenderMode,
  RenderedPage,
} from '../domain/browse';
export type { InteractiveCapability, RenderCapability } from '../domain/media';
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
