/**
 * App — the composition root for the UI.
 *
 * This is the one place that connects hooks (data + state) to pages. Every
 * component below it receives plain props, so they stay testable in isolation
 * and the boundary rule (`ui` may only import `hooks`/`stores`) is satisfied by
 * construction.
 */

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { AppShell, type NavItem } from './layout';
import {
  IconGrid,
  IconList,
  IconSearch,
  IconPlus,
  IconDatabase,
  IconSettings,
  IconPlus as IconNew,
  IconRefresh,
  IconSun,
} from './primitives/Icons';
import { Button } from './primitives/Button';
import { FileViewer } from './viewers';
import {
  OverviewPage,
  DownloadsPage,
  NewIngestPage,
  StoragePage,
  BrowsePage,
  SettingsPage,
  type StorageTab,
  type SettingsTab,
  type BrowseMode,
  type IngestFormValue,
  type ProbeOutcome,
  type StorageCredentialsValue,
  type StorageLocationValue,
  OverviewSkeleton,
  DownloadsSkeleton,
  StorageSkeleton,
  SettingsSkeleton,
  IngestSkeleton,
} from './pages';
import {
  useDownloads,
  useDownloadViewsWithPipeline,
  useSystemStorage,
  useCapabilities,
  useBrowse,
  useBrowseUrl,
  useRenderPage,
  useLiveSession,
  useSetStorageLocation,
  useCreateDownload,
  useCancelDownload,
  useRemoveDownload,
  useRenameDownload,
  useMoveDownload,
  usePauseDownload,
  useResumeDownload,
  useConvertDownload,
  useLocalStorageStore,
  useLocalMedia,
  useProbe,
  useConnectionStore,
  useDownloadFormStore,
  useStorageStore,
  useMediaApi,
  isApiError,
  isNetworkError,
  type BrowseFilter,
  type BrowseItem,
  type BrowseKind,
  type RenderFailure,
  type RenderMode,
} from '../hooks';
import { useToast } from './feedback';
import { ConfirmDialog } from './primitives/ConfirmDialog';
import type { StorageTierId } from '../platform/storage-capabilities';
import { FormatChooser, formatsFromQualities, formatFromFile } from './downloads/FormatChooser';
import type { FormatChoice } from './downloads/FormatChooser';
import { DownloadInfo } from './downloads/DownloadInfo';
import { StorageChooser } from './storage/StorageChooser';
import { StoragePanel } from './storage/StoragePanel';
import type { EngineStorageFacts } from './storage/EngineStorageNotice';
import { shareItems, openWithItem, mimeForName } from './files';
import {
  formatBytes,
  toDeviceView,
  toRemoteView,
  toQualityOptions,
  hostFromUrl,
  fileNameFromUrl,
  type DownloadView,
} from '../hooks/view-models';

export type ViewId = 'overview' | 'downloads' | 'browse' | 'ingest' | 'storage' | 'settings';

const VIEW_TITLES: Record<ViewId, [string, string]> = {
  overview: ['Overview', 'Pipeline health and recent activity'],
  downloads: ['Downloads', 'Everything being fetched, and what has finished'],
  browse: ['Browse', 'Find open media, then preview or keep it'],
  ingest: ['New download', 'Queue a source to fetch, transcode and deliver'],
  storage: ['Storage', 'Downloaded files, object storage and delivery'],
  settings: ['Settings', 'Connection, storage location and engine capabilities'],
};

const DEFAULT_CLOUD_PATH = 's3://vyzorix-media/downloads';
const LOCATION_KEY = 'ingest.storage-location';
const THEME_KEY = 'ingest.theme';

export type Theme = 'light' | 'dark';

/** A deletion awaiting confirmation. */
interface PendingDelete {
  ids: readonly string[];
  title: string;
  message: string;
  details: string[];
}

/**
 * The theme is applied to <html> before React paints (see the inline script in
 * index.html) to avoid a flash of the wrong theme. Here we only read it back so
 * the toggle's own state matches what is already on screen.
 */
function loadTheme(): Theme {
  if (typeof document !== 'undefined') {
    const current = document.documentElement.dataset.theme;
    if (current === 'dark' || current === 'light') return current;
  }
  return 'light';
}

const DEFAULT_LOCATION: StorageLocationValue = {
  target: 'local',
  bucket: 'vyzorix-media',
  keyPrefix: 'downloads/',
};

function loadLocation(): StorageLocationValue {
  if (typeof localStorage === 'undefined') return DEFAULT_LOCATION;
  try {
    const raw = localStorage.getItem(LOCATION_KEY);
    if (!raw) return DEFAULT_LOCATION;
    return { ...DEFAULT_LOCATION, ...(JSON.parse(raw) as Partial<StorageLocationValue>) };
  } catch {
    return DEFAULT_LOCATION;
  }
}

/** Throws if the browser refuses to persist, so the toast can report it. */
function persistLocation(value: StorageLocationValue): void {
  localStorage.setItem(LOCATION_KEY, JSON.stringify(value));
}

export interface AppProps {
  /** Optional initial view; useful for tests and deep links. */
  initialView?: ViewId;
}

/** Turns a thrown transport/API error into something a user can act on. */
function describeError(e: unknown): string {
  if (isApiError(e)) {
    if (e.status === 401) return 'The engine rejected the request — check the bearer token in Settings.';
    if (e.status === 404) return 'The engine does not know that record.';
    if (e.status >= 500) return `The engine failed the request (HTTP ${e.status}).`;
    return e.detail ?? `Request failed (HTTP ${e.status}).`;
  }
  if (isNetworkError(e)) return 'Could not reach the engine — is it running, and is the API URL correct?';
  if (e instanceof Error) return e.message;
  return 'Something went wrong.';
}

export function App({ initialView = 'overview' }: AppProps) {
  const toast = useToast();
  const { api } = useMediaApi();
  const [view, setView] = useState<ViewId>(initialView);
  const [storageTab, setStorageTab] = useState<StorageTab>('files');
  const [settingsTab, setSettingsTab] = useState<SettingsTab>('general');
  const [openFileId, setOpenFileId] = useState<string | null>(null);
  /** Browse view: the term is presentation state, so it lives here. */
  const [browseTerm, setBrowseTerm] = useState('');
  const [browseKind, setBrowseKind] = useState<BrowseKind | 'all'>('all');
  const [browsePage, setBrowsePage] = useState(1);
  /** Which source Browse is reading: the built-in search, or a URL the user typed. */
  const [browseMode, setBrowseMode] = useState<BrowseMode>('search');
  const [browseUrl, setBrowseUrl] = useState('');
  /** The URL actually submitted, so typing does not fire a request per keystroke. */
  const [browseUrlSubmitted, setBrowseUrlSubmitted] = useState<string | null>(null);
  /** How much of a URL's page to list: only media, or everything on it. */
  const [browseFilter, setBrowseFilter] = useState<BrowseFilter>('media');
  /**
   * The rendered page and how it went.
   *
   * Held here rather than in a query because a render is an action: it costs
   * about half a gigabyte and a second or two, so it happens when asked and not
   * again on a remount. `rendered` is the URL to show; `renderMs` and the title
   * come from the engine's headers.
   */
  const [rendered, setRendered] = useState<{ url: string; mode: RenderMode } | null>(null);
  const [renderedTitle, setRenderedTitle] = useState<string | null>(null);
  const [renderMs, setRenderMs] = useState<number | null>(null);
  const [renderFailure, setRenderFailure] = useState<RenderFailure | null>(null);
  /** True when the last render came from the engine's cache, not a fresh draw. */
  const [renderedFromCache, setRenderedFromCache] = useState(false);
  /** True when the engine cleaned the page's markup before serving it. */
  const [renderedSanitized, setRenderedSanitized] = useState(false);
  const renderPage = useRenderPage();
  /** The live page session. Its state is owned here so switching views does not
   *  tear it down; the hook revokes each frame's blob as the next arrives. */
  const live = useLiveSession();
  const [browseQueueingId, setBrowseQueueingId] = useState<string | null>(null);
  /** A browse result being previewed, if any. */
  const [browsePreview, setBrowsePreview] = useState<BrowseItem | null>(null);
  const [inspectedId, setInspectedId] = useState<string | null>(null);
  const [pendingDelete, setPendingDelete] = useState<PendingDelete | null>(null);
  /** Formats offered before a download starts; null when not choosing. */
  const [choosingFormats, setChoosingFormats] = useState<FormatChoice[] | null>(null);
  /** The download whose live detail panel is open. */
  const [infoId, setInfoId] = useState<string | null>(null);
  /** Which transfer action is in flight, for its button. */
  const [transferBusy, setTransferBusy] = useState<string | null>(null);
  /** First-run storage prompt; dismissed once a choice is made. */
  const [storagePromptDone, setStoragePromptDone] = useState(false);
  const [offlineStatus, setOfflineStatus] = useState('checking…');
  const [probe, setProbe] = useState<ProbeOutcome>({ status: 'idle' });
  const [probing, setProbing] = useState(false);
  const [selectedQuality, setSelectedQuality] = useState<number | null>(null);

  const [theme, setTheme] = useState<Theme>(loadTheme);
  /** Which async action is in flight, so its button can show a spinner. */
  const [busy, setBusy] = useState<string | null>(null);

  const endpoint = useConnectionStore((s) => s.baseUrl);
  const setEndpoint = useConnectionStore((s) => s.setBaseUrl);
  const authToken = useConnectionStore((s) => s.authToken);
  const setAuthToken = useConnectionStore((s) => s.setAuthToken);

  const draft = useDownloadFormStore();
  const setDraftField = useDownloadFormStore((s) => s.setField);

  const localStore = useLocalStorageStore();
  const localMedia = useLocalMedia();
  const credentials = useStorageStore();
  const setCredential = useStorageStore((s) => s.setField);

  const [location, setLocation] = useState<StorageLocationValue>(loadLocation);

  const downloadsQuery = useDownloads();
  const systemStorageQuery = useSystemStorage();
  const capabilitiesQuery = useCapabilities();
  const capabilities = capabilitiesQuery.data ?? null;
  const setStorageLocation = useSetStorageLocation();
  const browseQuery = useBrowse({ term: browseTerm, kind: browseKind, page: browsePage });
  const browseUrlQuery = useBrowseUrl(browseUrlSubmitted, browseFilter);
  const createDownload = useCreateDownload();
  const cancelDownload = useCancelDownload();
  const probeUrl = useProbe();
  const removeDownload = useRemoveDownload();
  const renameDownload = useRenameDownload();
  const moveDownload = useMoveDownload();
  const pauseDownload = usePauseDownload();
  const resumeDownload = useResumeDownload();
  const convertDownload = useConvertDownload();
  const [cancellingId, setCancellingId] = useState<string | null>(null);
  const systemStorage = systemStorageQuery.data ?? null;
  const allViews = useDownloadViewsWithPipeline(downloadsQuery.data ?? [], {
    hls: draft.hls,
    upload: draft.upload,
    purge: draft.purge,
  });

  /**
   * The library the file manager shows: engine records plus the files saved on
   * this device. A device copy is a first-class entry, so it can be played,
   * shared and opened like any other file rather than only played by handing a
   * URL to a new tab.
   *
   * A file saved from a download appears once, as the device copy: the local
   * bytes are what the user asked to keep, and showing both would double it.
   */
  const libraryViews = useMemo<DownloadView[]>(() => {
    const deviceViews = localMedia.items.map((item) =>
      toDeviceView({
        id: item.id,
        name: item.name,
        type: item.type,
        size: item.size,
        url: localMedia.urls.get(item.id) ?? '',
      }),
    );
    const localIds = new Set(deviceViews.map((v) => v.id));
    return [...deviceViews, ...allViews.filter((v) => !localIds.has(v.id))];
  }, [localMedia.items, localMedia.urls, allViews]);

  // First load only: a background refetch should not replace content with a
  // skeleton, so we key off `isPending` rather than `isFetching`.
  const isLoading = downloadsQuery.isPending;

  const ongoing = useMemo(() => allViews.filter((d) => d.isActive), [allViews]);

  const counts = useMemo(() => {
    const completed = allViews.filter((d) => d.state === 'done').length;
    const failed = allViews.filter((d) => d.state === 'failed').length;
    return { active: ongoing.length, completed, failed };
  }, [allViews, ongoing]);

  const processedLabel = useMemo(
    () => formatBytes(systemStorage?.downloads.bytesTransferred ?? 0),
    [systemStorage],
  );

  const libraryLabel = useMemo(
    () => formatBytes(systemStorage?.library.bytes ?? 0),
    [systemStorage],
  );

  const disk = useMemo(() => {
    const fs = systemStorage?.filesystem;
    if (!fs || fs.totalBytes <= 0) return null;
    // Name the volume the way the OS does: its mount point, plus the filesystem
    // type when the kernel reported one. Falling back to the work dir would be
    // a lie about what the figures describe.
    const mount = systemStorage?.mountPoint;
    const type = systemStorage?.fsType;
    const volume = mount
      ? `the ${mount} volume${type ? ` (${type})` : ''}`
      : (systemStorage?.workDir ?? 'the library volume');
    return {
      freeLabel: formatBytes(fs.availBytes),
      usedPercent: fs.usedFraction,
      summary: `${formatBytes(fs.usedBytes)} of ${formatBytes(fs.totalBytes)} used on ${volume}.`,
    };
  }, [systemStorage]);

  const infoDownload = useMemo(
    () => (infoId ? allViews.find((d) => d.id === infoId) ?? null : null),
    [infoId, allViews],
  );

  const openFile = useMemo(
    () => (openFileId ? libraryViews.find((d) => d.id === openFileId) ?? null : null),
    [openFileId, libraryViews],
  );

  /** A browse result being previewed, as the viewer's input shape. */
  const browsePreviewView = useMemo(
    () => (browsePreview ? toRemoteView(browsePreview) : null),
    [browsePreview],
  );

  /** The bytes to play: the device copy when there is one, else the engine. */
  const contentUrlFor = useCallback(
    (view: DownloadView) =>
      view.localUrl && view.localUrl !== ''
        ? view.localUrl
        : api.client.transport.resolve(`/api/v1/downloads/${encodeURIComponent(view.id)}/content`),
    [api.client.transport],
  );

  /**
   * Runs an async action with its button spinning and reports the real outcome.
   * Errors are surfaced, never swallowed — a failed write that looks successful
   * is worse than no feedback at all.
   */
  const runAction = useCallback(
    async (
      key: string,
      action: () => Promise<void>,
      messages: { success: string; failure: string },
    ) => {
      setBusy(key);
      try {
        await action();
        toast.success(messages.success);
      } catch (e) {
        toast.error(messages.failure, describeError(e));
      } finally {
        setBusy(null);
      }
    },
    [toast],
  );

  /**
   * Start a container-to-MP4 conversion. The progress is followed through the
   * download query, which polls while `conversion.state` is `converting`, so
   * the viewer updates itself without anything being polled here.
   */
  const convertFile = useCallback(
    (id: string) => {
      void convertDownload
        .mutateAsync(id)
        .then(() => toast.success('Converting to MP4 — this plays here when it is ready'))
        .catch((e: unknown) => toast.error('Could not start the conversion', describeError(e)));
    },
    [convertDownload, toast],
  );

  const cancelDownloadById = useCallback(
    (id: string) => {
      setCancellingId(id);
      void cancelDownload
        .mutateAsync(id)
        .then(() => toast.success('Download cancelled'))
        .catch((e: unknown) => toast.error('Could not cancel the download', describeError(e)))
        .finally(() => setCancellingId(null));
    },
    [cancelDownload, toast],
  );

  /**
   * Render the page at the address, in the given mode.
   *
   * The engine's answer decides what is shown: a success gives the URL to point
   * at (the image is streamed by the browser, not fetched into JS), and a
   * failure gives the reason. Nothing is guessed from a missing image, because
   * an image that never loads cannot say why.
   */
  const renderBrowsedPage = useCallback(
    (mode: RenderMode) => {
      const url = browseUrl.trim();
      if (!url) return;
      setRenderFailure(null);
      setRendered(null);
      renderPage
        .mutateAsync({ url, mode })
        .then((result) => {
          if (result.ok) {
            setRendered({ url: result.objectUrl, mode });
            setRenderedTitle(result.title);
            setRenderMs(result.elapsedMs);
            setRenderedFromCache(result.fromCache);
            setRenderedSanitized(result.sanitized);
            if (result.truncated) {
              toast.info('The page was large, so the result was cut short');
            }
          } else {
            setRenderFailure(result.failure);
          }
        })
        .catch((e: unknown) => {
          setRenderFailure('failed');
          toast.error('Could not render that page', describeError(e));
        });
    },
    [api.render, browseUrl, renderPage, toast],
  );

  /**
   * Release the previous render's blob when a new one replaces it, or when the
   * view goes away.
   *
   * A blob URL pins its bytes in memory until it is revoked, so a user rendering
   * a dozen pages would otherwise hold a dozen screenshots alive for the life of
   * the tab. The cleanup runs on every change of the URL and on unmount, which
   * covers both replacing it and leaving.
   */
  useEffect(() => {
    const url = rendered?.url;
    if (!url || !url.startsWith('blob:')) return undefined;
    return () => URL.revokeObjectURL(url);
  }, [rendered?.url]);

  /** The MP4 the engine produced, or null for anything it does not hold. */
  const convertedUrlFor = useCallback(
    (view: DownloadView) =>
      view.source === 'engine'
        ? api.client.transport.resolve(
            `/api/v1/downloads/${encodeURIComponent(view.id)}/converted`,
          )
        : null,
    [api.client.transport],
  );

  /**
   * Deletion is irreversible, so it goes through an in-app confirmation rather
   * than `window.confirm` — browser chrome cannot show which files are about to
   * go, and it cannot be styled or trusted to appear on every platform.
   */
  /**
   * Which of these ids live on the engine, on this device, or both.
   *
   * The file manager shows one entry per item, but the two stores are separate:
   * deleting a device copy must not delete the engine's record, and deleting an
   * engine record must not silently leave an orphaned local copy.
   */
  const partitionIds = useCallback(
    (ids: readonly string[]) => {
      const engine = new Set(allViews.map((v) => v.id));
      const device = new Set(localMedia.items.map((i) => i.id));
      return {
        engineIds: ids.filter((id) => engine.has(id)),
        deviceIds: ids.filter((id) => device.has(id)),
        // Engine-only: nothing local to remove.
        engineOnly: ids.filter((id) => engine.has(id) && !device.has(id)),
        deviceOnly: ids.filter((id) => device.has(id) && !engine.has(id)),
      };
    },
    [allViews, localMedia.items],
  );

  const deleteFile = useCallback(
    (id: string) => {
      const file = libraryViews.find((d) => d.id === id);
      const { engineIds, deviceIds } = partitionIds([id]);
      const onDevice = deviceIds.length > 0;
      setPendingDelete({
        ids: [id],
        title: 'Delete this item?',
        message:
          engineIds.length > 0 && onDevice
            ? 'The engine record and this device’s copy will both be removed. This cannot be undone.'
            : engineIds.length > 0
              ? 'The record and its downloaded files will be removed from disk. This cannot be undone.'
              : 'This copy will be removed from this device. This cannot be undone.',
        details: [file?.name ?? id],
      });
    },
    [libraryViews, partitionIds],
  );

  /**
   * Bulk delete from the selection bar. One confirmation covers the whole
   * selection: prompting per item would be unusable, and the count is the thing
   * the user actually needs to check.
   */
  const deleteFiles = useCallback(
    (ids: readonly string[]) => {
      if (ids.length === 0) return;
      const names = ids.map((id) => libraryViews.find((d) => d.id === id)?.name ?? id);
      setPendingDelete({
        ids,
        title: `Delete ${ids.length} item${ids.length === 1 ? '' : 's'}?`,
        message: 'Their records and files will be removed from disk. This cannot be undone.',
        details: names.slice(0, 6).concat(names.length > 6 ? [`…and ${names.length - 6} more`] : []),
      });
    },
    [libraryViews],
  );

  /** Runs the confirmation once the user accepts it. */
  const confirmDelete = useCallback(() => {
    if (!pendingDelete) return;
    const { ids } = pendingDelete;
    setPendingDelete(null);
    const { engineIds, deviceIds } = partitionIds(ids);
    const nameOf = (id: string) => libraryViews.find((d) => d.id === id)?.name ?? id;

    // Remove the device copies first: a failed engine delete must not leave the
    // user believing a local file went with it.
    void (async () => {
      for (const id of deviceIds) {
        await localMedia.remove(id).catch(() => undefined);
      }
      if (engineIds.length === 0) {
        toast.success(
          'Deleted',
          ids.length === 1
            ? `${nameOf(ids[0]!)} was removed from this device.`
            : `${deviceIds.length} copies were removed from this device.`,
        );
        return;
      }
      if (engineIds.length === 1) {
        const id = engineIds[0]!;
        await removeDownload
          .mutateAsync({ id, removeFiles: true })
          .then(() => toast.success('Deleted', `${nameOf(id)} and its files were removed.`))
          .catch((e: unknown) => toast.error('Could not delete', describeError(e)));
        return;
      }
      await runAction(
        'file-action',
        async () => {
          for (const id of engineIds) {
            await removeDownload.mutateAsync({ id, removeFiles: true });
          }
        },
        {
          success: `Deleted ${engineIds.length} items`,
          failure: 'Could not delete everything',
        },
      );
    })();
  }, [pendingDelete, partitionIds, libraryViews, localMedia, removeDownload, runAction, toast]);

  /**
   * Share via the Web Share API, with a URL and then clipboard fallback. The
   * outcome is reported truthfully: copying to the clipboard is not "shared",
   * and a cancelled share sheet is not an error.
   */
  const shareFiles = useCallback(
    (files: readonly DownloadView[]) => {
      if (files.length === 0) return;
      const targets = files.map((f) => ({
        id: f.id,
        name: f.name,
        // A device file is shared from its own bytes; an engine item from the
        // endpoint that serves them.
        url: contentUrlFor(f),
        mime: mimeForName(f.name),
      }));
      void shareItems(targets)
        .then((outcome) => {
          if (outcome.method === 'cancelled') return;
          if (outcome.method === 'clipboard') {
            toast.info('Link copied', 'This browser cannot open a share sheet, so the link is on your clipboard.');
            return;
          }
          toast.success(
            'Shared',
            outcome.count === 1 ? targets[0]?.name : `${outcome.count} items`,
          );
        })
        .catch((e: unknown) => toast.error('Could not share', describeError(e)));
    },
    [contentUrlFor, toast],
  );

  /**
   * Hand a file to another app on the device. There is no web "open with" API,
   * so this is the share sheet carrying the bytes — which is what makes Android
   * offer its app chooser. When the platform cannot take files the file is
   * downloaded instead, and the toast says so rather than claiming a handoff.
   */
  const openWithFile = useCallback(
    (file: DownloadView) => {
      const target = {
        id: file.id,
        name: file.name,
        // The bytes themselves: a device copy is already here, an engine item is
        // fetched from the endpoint that serves it.
        url: contentUrlFor(file),
        mime: mimeForName(file.name),
      };
      void openWithItem(target)
        .then((result) => {
          if (result.method === 'files' && result.message === '') return; // cancelled
          toast.success('Open with', result.message);
        })
        .catch((e: unknown) => toast.error('Could not open', describeError(e)));
    },
    [contentUrlFor, toast],
  );

  /** Row-menu variants of the viewer's open/share actions, keyed by id. */
  const openWithById = useCallback(
    (id: string) => {
      const file = libraryViews.find((d) => d.id === id);
      if (file) openWithFile(file);
    },
    [libraryViews, openWithFile],
  );
  const shareById = useCallback(
    (id: string) => {
      const file = libraryViews.find((d) => d.id === id);
      if (file) shareFiles([file]);
    },
    [libraryViews, shareFiles],
  );

  /** Move into a library folder. Reversible, so no confirmation. */
  const moveFiles = useCallback(
    (ids: readonly string[], folder: string) => {
      if (ids.length === 0) return;
      void runAction(
        'file-action',
        async () => {
          for (const id of ids) {
            await moveDownload.mutateAsync({ id, folder });
          }
        },
        {
          success: `Moved to ${folder}`,
          failure: 'Could not move everything',
        },
      );
    },
    [moveDownload, runAction],
  );

  /**
   * Rename an item in whichever store holds it.
   *
   * A device copy is renamed through the device store, which for the folder tier
   * renames the real file on disk. An engine item goes to the engine. When both
   * exist we do both, so the two do not drift apart and leave the library
   * disagreeing with itself.
   */
  const renameFile = useCallback(
    (id: string, name: string) => {
      const { engineIds, deviceIds } = partitionIds([id]);
      void runAction(
        'file-action',
        async () => {
          if (deviceIds.length > 0) {
            // The folder tier keys on the filename, so the id can change; refresh
            // picks the new one up, which is why nothing here holds the old id.
            await localMedia.rename(id, name);
          }
          if (engineIds.length > 0) {
            await renameDownload.mutateAsync({ id, name });
          }
        },
        { success: 'Renamed', failure: 'Could not rename' },
      );
    },
    [partitionIds, localMedia, renameDownload, runAction],
  );


  /**
   * Queue a browse result with the engine.
   *
   * A document or a text file is not media, and the engine refuses an
   * unrecognised payload unless it is told to accept one — so the flag is set
   * from the item's kind rather than blanket-applied.
   */
  const downloadBrowsed = useCallback(
    (item: BrowseItem) => {
      setBrowseQueueingId(item.id);
      void runAction(
        'browse-download',
        async () => {
          await createDownload.mutateAsync({
            input: {
              url: item.url,
              hls: false,
              upload: false,
              purge: false,
              connections: 4,
              ...(item.kind === 'document' || item.kind === 'text' ? { allowNonMedia: true } : {}),
            },
          });
        },
        { success: 'Queued', failure: `Could not queue ${item.title}` },
      ).finally(() => setBrowseQueueingId(null));
    },
    [createDownload, runAction],
  );

  /** Save a browse result straight onto this device, without the engine. */
  const saveBrowsedToDevice = useCallback(
    (item: BrowseItem) => {
      void runAction(
        'browse-save',
        async () => {
          await localMedia.saveRemote({
            id: item.id,
            url: item.url,
            name: item.filename,
            type: item.mime,
            bytes: item.bytes,
          });
        },
        { success: 'Saved to this device', failure: `Could not save ${item.title}` },
      );
    },
    [localMedia, runAction],
  );

  const toggleTheme = useCallback(() => {
    setTheme((prev) => {
      const next: Theme = prev === 'dark' ? 'light' : 'dark';
      document.documentElement.dataset.theme = next;
      try {
        localStorage.setItem(THEME_KEY, next);
      } catch {
        // storage unavailable; the theme still applies for this session
      }
      return next;
    });
  }, []);

  const navItems: NavItem[] = [
    { id: 'overview', label: 'Overview', icon: <IconGrid /> },
    { id: 'downloads', label: 'Downloads', icon: <IconList />, count: counts.active },
    { id: 'browse', label: 'Browse', icon: <IconSearch /> },
    { id: 'ingest', label: 'New download', icon: <IconPlus /> },
    { id: 'storage', label: 'Storage', icon: <IconDatabase /> },
    { id: 'settings', label: 'Settings', icon: <IconSettings /> },
  ];

  const ingestValue: IngestFormValue = {
    url: draft.url,
    connections: draft.connections,
    saveAs: draft.url.split('/').pop() ?? '',
    hls: draft.hls,
    upload: draft.upload,
    purge: draft.purge,
    durationSeconds: draft.durationSeconds ?? 0,
  };

  const credentialsValue: StorageCredentialsValue = {
    endpoint: credentials.endpoint ?? '',
    region: credentials.region ?? '',
    bucket: credentials.bucket ?? '',
    accessKey: credentials.accessKey ?? '',
    secretKey: credentials.secretKey ?? '',
    cdnBase: credentials.cdnBase ?? '',
    cfZone: credentials.cfZone ?? '',
    cfToken: credentials.cfToken ?? '',
  };

  // The engine owns the download directory; the row label shows its real path.
  const localPath = systemStorage?.workDir ?? '';
  const cloudPath =
    location.target === 'cloud' ? `s3://${location.bucket}/${location.keyPrefix}` : DEFAULT_CLOUD_PATH;

  /**
   * Probes the source and returns what was found, so the same result can drive
   * both the probe panel and the format chooser. `announce` keeps the toasts out
   * of the way when this is a step inside "start a download" rather than a probe
   * the user asked for on its own.
   */
  const runProbe = useCallback(
    async (announce = true): Promise<ProbeOutcome | null> => {
      const url = draft.url.trim();
      if (!url) return null;
      setProbing(true);
      setProbe({ status: 'probing' });
      try {
        const result = await probeUrl.mutateAsync({
          url,
          ...(draft.durationSeconds && draft.durationSeconds > 0
            ? { durationSeconds: draft.durationSeconds }
            : {}),
        });
        if (result.media && !result.media.isVideo) {
          setProbe({ status: 'error', message: 'Not a video payload' });
          if (announce) {
            toast.error('Probe rejected the source', 'The bytes are not a recognised video format.');
          }
          return null;
        }
        const qualities = toQualityOptions(result.media?.qualities ?? []);
        const outcome: ProbeOutcome = {
          status: 'done',
          container: result.media ? result.media.container : '—',
          resolution:
            result.media && result.media.width > 0 && result.media.height > 0
              ? `${result.media.width}\u00d7${result.media.height}`
              : undefined,
          rangeSupport: result.acceptRanges ? 'Accepted (parallel download available)' : 'Not supported',
          size: formatBytes(result.totalBytes > 0 ? result.totalBytes : 0),
          confidence: result.media ? result.media.confidence.toFixed(3) : '—',
          qualities,
        };
        setProbe(outcome);
        // Default to the largest rendition the manifest offers.
        setSelectedQuality(qualities.length > 0 ? (qualities[0]?.height ?? null) : null);
        if (announce) {
          toast.success('Probe succeeded', result.media ? `Detected ${result.media.container}` : undefined);
        }
        return outcome;
      } catch (e) {
        setProbe({ status: 'error', message: 'Probe failed' });
        toast.error('Probe failed', describeError(e));
        return null;
      } finally {
        setProbing(false);
      }
    },
    [draft.url, draft.durationSeconds, probeUrl, toast],
  );

  /**
   * Creating a download needs the bytes queued, and the choice of format is part
   * of that. So the flow is: probe to see what is actually there, show the
   * formats it supports with their sizes, then start only what was chosen. If
   * the probe cannot tell us anything the single "Source" format is offered
   * rather than guessing.
   */
  const beginDownload = useCallback(async () => {
    const url = draft.url.trim();
    if (!url) return;
    const outcome = await runProbe(false);
    if (!outcome || outcome.status !== 'done') {
      toast.error('Cannot start', 'The source could not be read, so nothing was queued.');
      return;
    }
    const formats =
      outcome.qualities && outcome.qualities.length > 0
        ? formatsFromQualities(outcome.qualities)
        : formatFromFile(outcome.container ?? '', outcome.size ?? 'unknown', outcome.resolution);
    setChoosingFormats(formats);
  }, [draft.url, runProbe, toast]);

  const startWithFormat = useCallback(
    (qualityId: string) => {
      const url = draft.url.trim();
      if (!url) return;
      void runAction(
        'start-download',
        async () => {
          await createDownload.mutateAsync({
            input: {
              url,
              quality: qualityId,
              hls: draft.hls,
              upload: draft.upload,
              purge: draft.purge,
              connections: draft.connections,
            },
          });
          setChoosingFormats(null);
          setView('downloads');
        },
        { success: 'Download queued', failure: 'Could not queue the download' },
      );
    },
    [draft.url, draft.hls, draft.upload, draft.purge, draft.connections, createDownload, runAction],
  );

  /** Pause / resume / cancel, each reported honestly and never optimistically. */
  const transferAction = useCallback(
    (kind: 'pause' | 'resume' | 'cancel', id: string) => {
      setTransferBusy(kind);
      const run =
        kind === 'pause'
          ? pauseDownload.mutateAsync(id)
          : kind === 'resume'
            ? resumeDownload.mutateAsync(id)
            : cancelDownload.mutateAsync(id);
      const done =
        kind === 'pause' ? 'Paused' : kind === 'resume' ? 'Resumed' : 'Cancelled';
      const failed =
        kind === 'pause'
          ? 'Could not pause'
          : kind === 'resume'
            ? 'Could not resume'
            : 'Could not cancel';
      void run
        .then(() => toast.success(done))
        .catch((e: unknown) => toast.error(failed, describeError(e)))
        .finally(() => setTransferBusy(null));
    },
    [pauseDownload, resumeDownload, cancelDownload, toast],
  );

  /**
   * Detection runs once on mount: it is read-only, so it is safe to do without a
   * gesture. The stored list is refreshed whenever the chosen tier changes.
   */
  const refreshLocal = useCallback(async () => {
    await localStore.refresh();
  }, [localStore]);

  // Runs once: detection is read-only and does not depend on anything that
  // changes, so re-running it on every render would be pure waste.
  const detectedOnce = useRef(false);
  useEffect(() => {
    if (detectedOnce.current) return;
    detectedOnce.current = true;
    // Restore a folder granted in a previous session before detecting, so the
    // effective tier reflects whether we can actually write through it.
    void (async () => {
      await localStore.restore();
      await refreshLocal();
    })();
  }, [refreshLocal, localStore]);

  const activeTier = localStore.effectiveTier();

  useEffect(() => {
    if (typeof navigator === 'undefined' || !('serviceWorker' in navigator)) {
      setOfflineStatus('not supported');
      return;
    }
    if (!navigator.serviceWorker.controller) {
      setOfflineStatus('registered, not yet controlling this page');
      return;
    }
    setOfflineStatus('active');
  }, []);

  /** Records the choice; the folder tier also stores the granted handle. */
  const chooseStorage = useCallback(
    (tier: StorageTierId, directory?: FileSystemDirectoryHandle) => {
      localStore.choose(tier, directory);
      setStoragePromptDone(true);
    },
    [localStore],
  );

  const storedIdSet = useMemo(
    () => new Set(localMedia.items.map((i) => i.id)),
    [localMedia.items],
  );

  /**
   * Opens a device file in the in-app viewer.
   *
   * It used to hand the blob URL to a new tab, which loses the app's player and
   * its controls. The file manager now plays it in place, like any other entry.
   */
  const playLocal = useCallback(
    (id: string) => {
      if (!localMedia.urls.has(id)) {
        toast.error('Cannot play', 'That file is no longer on this device.');
        return;
      }
      setOpenFileId(id);
    },
    [localMedia.urls, toast],
  );

  const removeLocal = useCallback(
    async (id: string) => {
      try {
        await localMedia.remove(id);
        toast.success('Removed from this device');
      } catch (e) {
        toast.error('Could not remove', describeError(e));
      }
    },
    [localMedia, toast],
  );

  /** Streams a finished download onto this device. */
  const saveLocally = useCallback(
    async (id: string) => {
      const file = allViews.find((d) => d.id === id);
      if (!file) return;
      try {
        await localMedia.save(file);
        toast.success('Saved to this device', file.name);
      } catch (e) {
        toast.error('Could not save to this device', describeError(e));
      }
    },
    [allViews, localMedia, toast],
  );

  /**
   * Where the engine writes, and whether that is this same machine. The browser
   * cannot see or choose this path — the engine owns it — so the honest thing is
   * to report it rather than let the browser storage options imply they are the
   * whole story.
   */
  const engineStorage = useMemo<EngineStorageFacts | null>(() => {
    const store = systemStorage;
    if (!store) return null;
    let sameDevice = false;
    try {
      const engineHost = new URL(endpoint).hostname;
      const pageHost = typeof window === 'undefined' ? '' : window.location.hostname;
      const loopback = (h: string) => h === 'localhost' || h === '127.0.0.1' || h === '::1';
      sameDevice = engineHost === pageHost || (loopback(engineHost) && loopback(pageHost));
    } catch {
      sameDevice = false;
    }
    return {
      workDir: store.workDir,
      mountPoint: store.mountPoint,
      fsType: store.fsType,
      libraryBytes: store.library.bytes,
      libraryFiles: store.library.files,
      freeBytes: store.filesystem ? store.filesystem.availBytes : null,
      sameDevice,
    };
  }, [systemStorage, endpoint]);

  const [title, subtitle] = VIEW_TITLES[view];

  return (
    <>
      {!storagePromptDone && localStore.environment && !localStore.choice && (
        <div className="storchooser__scrim">
          <div className="storchooser__modal">
            <StorageChooser
              environment={localStore.environment}
              selected={activeTier}
              directoryName={null}
              busy={localStore.detecting}
              error={localStore.error}
              onChoose={chooseStorage}
              onRequestPersistence={() => void localStore.requestPersistence()}
              onRefresh={() => void refreshLocal()}
            />
          </div>
        </div>
      )}

      {choosingFormats && (
        <FormatChooser
          source={fileNameFromUrl(draft.url)}
          host={hostFromUrl(draft.url)}
          kind={probe.container ?? ''}
          formats={choosingFormats}
          initialId={choosingFormats[0]?.id ?? null}
          busy={busy === 'start-download'}
          onConfirm={startWithFormat}
          onCancel={() => setChoosingFormats(null)}
        />
      )}

      {infoDownload && (
        <DownloadInfo
          download={infoDownload}
          busy={transferBusy}
          onPause={() => transferAction('pause', infoDownload.id)}
          onResume={() => transferAction('resume', infoDownload.id)}
          onCancel={() => transferAction('cancel', infoDownload.id)}
          onOpen={() => {
            setInfoId(null);
            setOpenFileId(infoDownload.id);
          }}
          onOpenWith={() => void openWithFile(infoDownload)}
          onDelete={() => {
            setInfoId(null);
            deleteFile(infoDownload.id);
          }}
          onClose={() => setInfoId(null)}
          onSaveLocally={() => void saveLocally(infoDownload.id)}
          onPlayLocal={() => void playLocal(infoDownload.id)}
          saving={localMedia.saving.has(infoDownload.id)}
          saveProgress={localMedia.progress.get(infoDownload.id) ?? 0}
          savedLocally={localMedia.has(infoDownload.id)}
          {...(localMedia.error
            ? { localUnavailableReason: `Local storage is unavailable: ${localMedia.error}` }
            : {})}
        />
      )}

      {pendingDelete && (
        <ConfirmDialog
          title={pendingDelete.title}
          message={pendingDelete.message}
          details={pendingDelete.details}
          confirmLabel="Delete"
          destructive
          busy={busy === 'file-action'}
          onConfirm={confirmDelete}
          onCancel={() => setPendingDelete(null)}
        />
      )}
      <AppShell
        navItems={navItems}
        activeView={view}
        onNavigate={(id) => setView(id as ViewId)}
        endpoint={endpoint}
        connected={!downloadsQuery.isError}
        title={title}
        subtitle={subtitle}
        headerActions={
          <>
            <Button variant="ghost" icon aria-label="Toggle theme" onClick={toggleTheme}>
              <IconSun />
            </Button>
            <Button
              loading={downloadsQuery.isFetching}
              onClick={() =>
                void runAction(
                  'refresh',
                  async () => {
                    await downloadsQuery.refetch();
                  },
                  { success: 'Refreshed', failure: 'Could not refresh' },
                )
              }
            >
              <IconRefresh /> Refresh
            </Button>
            <Button variant="primary" onClick={() => setView('ingest')}>
              <IconNew /> New download
            </Button>
          </>
        }
      >
        {browsePreview && browsePreviewView ? (
          <FileViewer
            file={browsePreviewView}
            src={browsePreview.url}
            downloadUrl={browsePreview.url}
            location={`${browsePreview.source} — ${browsePreview.licence}`}
            onClose={() => setBrowsePreview(null)}
            onOpenWith={() => void openWithItem({ id: browsePreview.id, name: browsePreview.filename, url: browsePreview.url, mime: browsePreview.mime })}
            onShare={() => shareItems([{ id: browsePreview.id, name: browsePreview.filename, url: browsePreview.url, mime: browsePreview.mime }])}
            onDelete={() => {
              // Nothing local to delete: this is a remote candidate. Queue it.
              downloadBrowsed(browsePreview);
              setBrowsePreview(null);
            }}
          />
        ) : openFile ? (
          <FileViewer
            file={openFile}
            src={contentUrlFor(openFile)}
            downloadUrl={contentUrlFor(openFile)}
            location={openFile.source === 'device' ? 'On this device' : location.target === 'local' ? localPath : cloudPath}
            onClose={() => setOpenFileId(null)}
            onOpenWith={() => void openWithFile(openFile)}
            onShare={() => shareFiles([openFile])}
            onDelete={() => {
              deleteFile(openFile.id);
              setOpenFileId(null);
            }}
            {...(openFile.source === 'engine' ? { onConvert: () => convertFile(openFile.id) } : {})}
            converting={openFile.conversion.state === 'converting'}
            conversionError={openFile.conversion.error}
            convertedUrl={convertedUrlFor(openFile)}
          />
        ) : (
        <>
        {view === 'overview' && (isLoading ? <OverviewSkeleton /> : (
          <OverviewPage
            downloads={allViews}
            activeCount={counts.active}
            completedCount={counts.completed}
            failedCount={counts.failed}
            processedLabel={processedLabel}
            libraryLabel={libraryLabel}
            libraryFiles={systemStorage?.library.files ?? 0}
            disk={disk}
            onViewAll={() => setView('downloads')}
            onSelect={setOpenFileId}
            onOpenWith={openWithById}
            onShare={shareById}
            contentUrl={(id) => {
              const view = libraryViews.find((v) => v.id === id);
              return view ? contentUrlFor(view) : '';
            }}
          />
        ))}

        {view === 'browse' && (
          <BrowsePage
            term={browseTerm}
            onTermChange={(t) => {
              setBrowseTerm(t);
              setBrowsePage(1);
            }}
            kind={browseKind}
            onKindChange={(k) => {
              setBrowseKind(k);
              setBrowsePage(1);
            }}
            // Whichever source is active supplies the cards; the other query is
            // left alone so switching back does not re-fetch.
            items={(browseMode === 'url' ? browseUrlQuery.data : browseQuery.data)?.items ?? []}
            total={(browseMode === 'url' ? browseUrlQuery.data : browseQuery.data)?.total ?? -1}
            hasMore={(browseMode === 'url' ? browseUrlQuery.data : browseQuery.data)?.hasMore ?? false}
            loading={browseMode === 'url' ? browseUrlQuery.isFetching : browseQuery.isFetching}
            error={
              browseMode === 'url'
                ? browseUrlQuery.error
                  ? describeError(browseUrlQuery.error)
                  : null
                : browseQuery.error
                  ? describeError(browseQuery.error)
                  : null
            }
            queueingId={browseQueueingId}
            onPreview={setBrowsePreview}
            onDownload={downloadBrowsed}
            onSaveToDevice={saveBrowsedToDevice}
            page={browsePage}
            onNextPage={() => setBrowsePage((p) => p + 1)}
            onPrevPage={() => setBrowsePage((p) => Math.max(1, p - 1))}
            canSaveToDevice={activeTier !== 'memory'}
            mode={browseMode}
            onModeChange={setBrowseMode}
            sourceUrl={browseUrl}
            onSourceUrlChange={setBrowseUrl}
            onReadUrl={() => setBrowseUrlSubmitted(browseUrl.trim())}
            sourceTitle={browseUrlQuery.data?.title ?? null}
            filter={browseFilter}
            onFilterChange={setBrowseFilter}
            canRender={Boolean(capabilities?.render?.available)}
            rendered={rendered}
            rendering={renderPage.isPending}
            renderFailure={renderFailure}
            renderedTitle={renderedTitle}
            renderMs={renderMs}
            onRender={renderBrowsedPage}
            renderedFromCache={renderedFromCache}
            renderedSanitized={renderedSanitized}
            canGoLive={Boolean(capabilities?.interactive?.available)}
            live={{
              status: live.status,
              frame: live.frame,
              page: live.page,
              failure: live.failure,
              reason: live.reason,
              onOpen: live.open,
              onClose: live.close,
              send: live.send,
            }}
            onClearRender={() => {
              setRendered(null);
              setRenderedTitle(null);
              setRenderMs(null);
              setRenderFailure(null);
              setRenderedFromCache(false);
              setRenderedSanitized(false);
            }}
          />
        )}

        {view === 'downloads' &&
          (isLoading ? (
            <DownloadsSkeleton />
          ) : (
            <DownloadsPage
              downloads={allViews}
              onOpen={setOpenFileId}
              onOpenWith={openWithById}
              onShare={shareById}
              onCancel={cancelDownloadById}
              onDelete={deleteFile}
              onInfo={setInfoId}
              onSaveLocally={(id) => void saveLocally(id)}
              onPlayLocal={(id) => void playLocal(id)}
              storedIds={storedIdSet}
              savingIds={localMedia.saving}
              cancellingId={cancellingId}
              onRefresh={() =>
                void runAction(
                  'refresh-downloads',
                  async () => {
                    await downloadsQuery.refetch();
                  },
                  { success: 'Refreshed', failure: 'Could not refresh' },
                )
              }
              refreshing={downloadsQuery.isFetching}
              onNewDownload={() => setView('ingest')}
              contentUrl={(id) =>
                api.client.transport.resolve(`/api/v1/downloads/${encodeURIComponent(id)}/content`)
              }
            />
          ))}

        {view === 'ingest' &&
          (isLoading ? (
            <IngestSkeleton />
          ) : (
          <NewIngestPage
            value={ingestValue}
            onChange={(next) => {
              setDraftField('url', next.url);
              setDraftField('connections', next.connections);
              setDraftField('hls', next.hls);
              setDraftField('upload', next.upload);
              setDraftField('purge', next.purge);
              setDraftField('durationSeconds', next.durationSeconds);
            }}
            probe={probe}
            probing={probing}
            onProbe={runProbe}
            selectedQuality={selectedQuality}
            onSelectQuality={setSelectedQuality}
            onStart={() => void beginDownload()}
          />
          ))}

        {view === 'storage' &&
          (isLoading ? (
            <StorageSkeleton />
          ) : (
          <StoragePage
            tab={storageTab}
            onTabChange={setStorageTab}
            downloads={libraryViews}
            ongoing={ongoing}
            target={location.target}
            localPath={localPath}
            cloudPath={cloudPath}
            credentials={credentialsValue}
            onCredentialsChange={(next) => {
              setCredential('endpoint', next.endpoint);
              setCredential('region', next.region);
              setCredential('bucket', next.bucket);
              setCredential('accessKey', next.accessKey);
              setCredential('secretKey', next.secretKey);
              setCredential('cdnBase', next.cdnBase);
              setCredential('cfZone', next.cfZone);
              setCredential('cfToken', next.cfToken);
            }}
            onSelect={setOpenFileId}
            onOpenFile={setOpenFileId}
            onGoToSettings={() => {
              setSettingsTab('location');
              setView('settings');
            }}
            onCancelDownload={cancelDownloadById}
            cancellingId={cancellingId}
            onDeleteFile={deleteFile}
            onDeleteFiles={deleteFiles}
            onShareFiles={shareFiles}
            onMoveFiles={moveFiles}
            onRenameFile={renameFile}
            onSaveLocally={(id) => void saveLocally(id)}
            storedIds={storedIdSet}
            savingIds={localMedia.saving}
            contentUrl={(id) => {
              const view = libraryViews.find((v) => v.id === id);
              return view ? contentUrlFor(view) : '';
            }}
            onTestStorage={() =>
              void runAction(
                'test-storage',
                async () => {
                  await api.client.transport.request({
                    method: 'GET',
                    path: '/api/v1/storage/config',
                  });
                },
                {
                  success: 'Connection successful',
                  failure: 'Storage is not reachable',
                },
              )
            }
            busy={busy}
          />
          ))}

        {view === 'settings' &&
          (isLoading ? (
            <SettingsSkeleton />
          ) : (
          <SettingsPage
            tab={settingsTab}
            onTabChange={setSettingsTab}
            endpoint={endpoint}
            onEndpointChange={setEndpoint}
            authToken={authToken}
            onAuthTokenChange={setAuthToken}
            darkTheme={theme === 'dark'}
            onDarkThemeChange={() => toggleTheme()}
            location={location}
            onLocationChange={setLocation}
            downloads={allViews}
            inspectedId={inspectedId}
            onInspectChange={setInspectedId}
            storageSection={
              <StoragePanel
                environment={localStore.environment}
                detecting={localStore.detecting}
                selected={activeTier}
                directoryName={localStore.choice?.directoryName ?? null}
                directoryPermission={localStore.directoryPermission}
                error={localStore.error}
                items={localMedia.items}
                usage={localMedia.usage}
                offlineStatus={offlineStatus}
                engineStorage={engineStorage}
                busy={busy === 'storage'}
                onChoose={chooseStorage}
                onRegrant={() =>
                  void localStore.regrantDirectory().then(() => localMedia.refresh())
                }
                onRequestPersistence={() => void localStore.requestPersistence()}
                onRefresh={() => {
                  // Re-detect the device *and* re-read what is stored: after a
                  // rebuild or a re-grant the folder's contents are what changed.
                  void refreshLocal();
                  void localMedia.refresh();
                }}
                onRemove={(id) => void removeLocal(id)}
                onPlay={(id) => void playLocal(id)}
              />
            }
            onSaveConnection={() =>
              void runAction(
                'save-connection',
                async () => {
                  const health = await fetch(`${endpoint.replace(/\/+$/, '')}/healthz`, {
                    signal: AbortSignal.timeout(6000),
                  });
                  if (!health.ok) throw new Error(`Health check returned HTTP ${health.status}`);
                },
                { success: 'Settings saved', failure: 'Could not reach that endpoint' },
              )
            }
            onSaveLocation={(dir) =>
              void runAction(
                'save-location',
                async () => {
                  await setStorageLocation.mutateAsync(dir);
                  persistLocation(location);
                },
                { success: 'Storage location saved', failure: 'Could not save the location' },
              )
            }
            engineLocation={
              systemStorage
                ? {
                    workDir: systemStorage.workDir,
                    writable: systemStorage.writable,
                    writeError: systemStorage.writeError,
                    mountPoint: systemStorage.mountPoint,
                    fsType: systemStorage.fsType,
                  }
                : null
            }
            busy={busy}
            usage={
              disk
                ? {
                    library: libraryLabel,
                    transferred: processedLabel,
                    free: disk.freeLabel,
                    usedPercent: Math.round(disk.usedPercent * 100),
                    summary: disk.summary,
                  }
                : undefined
            }
            engine={{
              engineVersion: capabilities?.engineVersion
                ? `media-ingest ${capabilities.engineVersion}`
                : 'unknown',
              ffmpeg: capabilities
                ? capabilities.ffmpeg
                  ? 'available'
                  : 'not installed'
                : 'unknown',
              hls: capabilities?.hls ?? false,
              upload: capabilities?.upload ?? false,
              purge: capabilities?.purge ?? false,
              maxBody:
                capabilities?.maxBodyBytes != null
                  ? formatBytes(capabilities.maxBodyBytes)
                  : 'unknown',
              workers: capabilities?.workers != null ? String(capabilities.workers) : 'unknown',
            }}
          />
          ))}
        </>
        )}
      </AppShell>
    </>
  );
}

export type { DownloadView };
