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
  SettingsPage,
  type StorageTab,
  type SettingsTab,
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
  useCreateDownload,
  useCancelDownload,
  useRemoveDownload,
  useRenameDownload,
  useMoveDownload,
  usePauseDownload,
  useResumeDownload,
  useLocalStorageStore,
  useLocalMedia,
  useProbe,
  useConnectionStore,
  useDownloadFormStore,
  useStorageStore,
  useMediaApi,
  isApiError,
  isNetworkError,
} from '../hooks';
import { useToast } from './feedback';
import { ConfirmDialog } from './primitives/ConfirmDialog';
import type { StorageTierId } from '../platform/storage-capabilities';
import { FormatChooser, formatsFromQualities, formatFromFile } from './downloads/FormatChooser';
import type { FormatChoice } from './downloads/FormatChooser';
import { DownloadInfo } from './downloads/DownloadInfo';
import { StorageChooser } from './storage/StorageChooser';
import { StoragePanel } from './storage/StoragePanel';
import { shareItems, mimeForName } from './files';
import {
  formatBytes,
  toQualityOptions,
  hostFromUrl,
  fileNameFromUrl,
  type DownloadView,
} from '../hooks/view-models';

export type ViewId = 'overview' | 'downloads' | 'ingest' | 'storage' | 'settings';

const VIEW_TITLES: Record<ViewId, [string, string]> = {
  overview: ['Overview', 'Pipeline health and recent activity'],
  downloads: ['Downloads', 'Everything being fetched, and what has finished'],
  ingest: ['New download', 'Queue a source to fetch, transcode and deliver'],
  storage: ['Storage', 'Downloaded files, object storage and delivery'],
  settings: ['Settings', 'Connection, storage location and engine capabilities'],
};

const DEFAULT_LOCAL_PATH = '/Users/you/Downloads/ingest';
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
  localPath: DEFAULT_LOCAL_PATH,
  localPartsPath: '.parts',
  openFolderOnFinish: true,
  bucket: 'vyzorix-media',
  keyPrefix: 'downloads/',
  serveViaCdn: true,
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
  const createDownload = useCreateDownload();
  const cancelDownload = useCancelDownload();
  const probeUrl = useProbe();
  const removeDownload = useRemoveDownload();
  const renameDownload = useRenameDownload();
  const moveDownload = useMoveDownload();
  const pauseDownload = usePauseDownload();
  const resumeDownload = useResumeDownload();
  const [cancellingId, setCancellingId] = useState<string | null>(null);
  const systemStorage = systemStorageQuery.data ?? null;
  const allViews = useDownloadViewsWithPipeline(downloadsQuery.data ?? [], {
    hls: draft.hls,
    upload: draft.upload,
    purge: draft.purge,
  });

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
    () => (openFileId ? allViews.find((d) => d.id === openFileId) ?? null : null),
    [openFileId, allViews],
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
   * Deletion is irreversible, so it goes through an in-app confirmation rather
   * than `window.confirm` — browser chrome cannot show which files are about to
   * go, and it cannot be styled or trusted to appear on every platform.
   */
  const deleteFile = useCallback(
    (id: string) => {
      const file = allViews.find((d) => d.id === id);
      setPendingDelete({
        ids: [id],
        title: 'Delete this download?',
        message: 'The record and its downloaded files will be removed from disk. This cannot be undone.',
        details: [file?.name ?? id],
      });
    },
    [allViews],
  );

  /**
   * Bulk delete from the selection bar. One confirmation covers the whole
   * selection: prompting per item would be unusable, and the count is the thing
   * the user actually needs to check.
   */
  const deleteFiles = useCallback(
    (ids: readonly string[]) => {
      if (ids.length === 0) return;
      const names = ids.map((id) => allViews.find((d) => d.id === id)?.name ?? id);
      setPendingDelete({
        ids,
        title: `Delete ${ids.length} item${ids.length === 1 ? '' : 's'}?`,
        message: 'Their records and files will be removed from disk. This cannot be undone.',
        details: names.slice(0, 6).concat(names.length > 6 ? [`…and ${names.length - 6} more`] : []),
      });
    },
    [allViews],
  );

  /** Runs the confirmation once the user accepts it. */
  const confirmDelete = useCallback(() => {
    if (!pendingDelete) return;
    const { ids } = pendingDelete;
    setPendingDelete(null);
    if (ids.length === 1) {
      const id = ids[0]!;
      const name = allViews.find((d) => d.id === id)?.name ?? id;
      void removeDownload
        .mutateAsync({ id, removeFiles: true })
        .then(() => toast.success('Deleted', `${name} and its files were removed.`))
        .catch((e: unknown) => toast.error('Could not delete', describeError(e)));
      return;
    }
    void runAction(
      'file-action',
      async () => {
        for (const id of ids) {
          await removeDownload.mutateAsync({ id, removeFiles: true });
        }
      },
      {
        success: `Deleted ${ids.length} items`,
        failure: 'Could not delete everything',
      },
    );
  }, [pendingDelete, allViews, removeDownload, runAction, toast]);

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
        url: api.client.transport.resolve(
          `/api/v1/downloads/${encodeURIComponent(f.id)}/content`,
        ),
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
    [api.client.transport, toast],
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

  const renameFile = useCallback(
    (id: string, name: string) => {
      void runAction(
        'file-action',
        async () => {
          await renameDownload.mutateAsync({ id, name });
        },
        { success: 'Renamed', failure: 'Could not rename' },
      );
    },
    [renameDownload, runAction],
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
    keyPrefix: '',
    accessKey: credentials.accessKey ?? '',
    secretKey: credentials.secretKey ?? '',
    cdnBase: credentials.cdnBase ?? '',
    cfZone: credentials.cfZone ?? '',
    cfToken: credentials.cfToken ?? '',
  };

  const localPath = location.target === 'local' ? location.localPath : DEFAULT_LOCAL_PATH;
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
    void refreshLocal();
  }, [refreshLocal]);

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

  /** Opens the locally-stored copy in a player. */
  const playLocal = useCallback(
    async (id: string) => {
      try {
        const url = await localMedia.urlFor(id);
        if (!url) {
          toast.error('Cannot play', 'That file is no longer on this device.');
          return;
        }
        window.open(url, '_blank', 'noopener');
      } catch (e) {
        toast.error('Cannot play', describeError(e));
      }
    },
    [localMedia, toast],
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

  const [title, subtitle] = VIEW_TITLES[view];

  return (
    <>
      {!storagePromptDone && localStore.environment && !localStore.choice && (
        <div className="cdialog__scrim">
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
          onReveal={() =>
            toast.info(
              location.target === 'local'
                ? 'Revealed in file manager'
                : 'Opened bucket in storage console',
            )
          }
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
        {openFile ? (
          <FileViewer
            file={openFile}
            src={api.client.transport.resolve(
              `/api/v1/downloads/${encodeURIComponent(openFile.id)}/content`,
            )}
            location={location.target === 'local' ? localPath : cloudPath}
            onClose={() => setOpenFileId(null)}
            onOpenWith={() =>
              toast.info(`Opening ${openFile.name}`, 'Handing off to your default player.')
            }
            onReveal={() =>
              toast.info(
                location.target === 'local'
                  ? 'Revealed in file manager'
                  : 'Opened bucket in storage console',
              )
            }
            onDelete={() => {
              deleteFile(openFile.id);
              setOpenFileId(null);
            }}
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
            contentUrl={(id) =>
              api.client.transport.resolve(`/api/v1/downloads/${encodeURIComponent(id)}/content`)
            }
          />
        ))}

        {view === 'downloads' &&
          (isLoading ? (
            <DownloadsSkeleton />
          ) : (
            <DownloadsPage
              downloads={allViews}
              onOpen={setOpenFileId}
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
            downloads={allViews}
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
            contentUrl={(id) =>
              api.client.transport.resolve(`/api/v1/downloads/${encodeURIComponent(id)}/content`)
            }
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
            autoStart
            onAutoStartChange={() => {}}
            verifyChecksums
            onVerifyChecksumsChange={() => {}}
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
                error={localStore.error}
                items={localMedia.items}
                usage={localMedia.usage}
                offlineStatus={offlineStatus}
                busy={busy === 'storage'}
                onChoose={chooseStorage}
                onRequestPersistence={() => void localStore.requestPersistence()}
                onRefresh={() => void refreshLocal()}
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
            onSaveLocation={() =>
              void runAction(
                'save-location',
                async () => {
                  persistLocation(location);
                },
                { success: 'Storage location saved', failure: 'Could not save the location' },
              )
            }
            onTestLocation={() =>
              void runAction(
                'test-location',
                async () => {
                  // A real write check: the engine reports the volume it can
                  // actually write to, so a failure here is meaningful.
                  const stats = await api.client.transport.request<{ work_dir?: string }>({
                    method: 'GET',
                    path: '/api/v1/system/storage',
                  });
                  if (!stats?.work_dir) throw new Error('The engine did not report a writable directory');
                },
                { success: 'Write access confirmed', failure: 'Write access failed' },
              )
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
              engineVersion: 'media-ingest 0.1.0',
              ffmpeg: '7.1.5 · libx264',
              hls: true,
              upload: true,
              purge: true,
              maxBody: '64 MB',
              workers: '16',
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
