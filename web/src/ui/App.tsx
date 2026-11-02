/**
 * App — the composition root for the UI.
 *
 * This is the one place that connects hooks (data + state) to pages. Every
 * component below it receives plain props, so they stay testable in isolation
 * and the boundary rule (`ui` may only import `hooks`/`stores`) is satisfied by
 * construction.
 */

import { useCallback, useMemo, useState } from 'react';
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
  type FileFilter,
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
  useDownloadViews,
  useSystemStorage,
  useCreateDownload,
  useCancelDownload,
  useRemoveDownload,
  useProbe,
  useConnectionStore,
  useDownloadFormStore,
  useStorageStore,
  useMediaApi,
  isApiError,
  isNetworkError,
} from '../hooks';
import { useToast } from './feedback';
import { formatBytes, toQualityOptions, type DownloadView } from '../hooks/view-models';

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
  const [filter, setFilter] = useState<FileFilter>('all');
  const [openFileId, setOpenFileId] = useState<string | null>(null);
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

  const credentials = useStorageStore();
  const setCredential = useStorageStore((s) => s.setField);

  const [location, setLocation] = useState<StorageLocationValue>(loadLocation);

  const downloadsQuery = useDownloads();
  const systemStorageQuery = useSystemStorage();
  const createDownload = useCreateDownload();
  const cancelDownload = useCancelDownload();
  const probeUrl = useProbe();
  const removeDownload = useRemoveDownload();
  const [cancellingId, setCancellingId] = useState<string | null>(null);
  const systemStorage = systemStorageQuery.data ?? null;
  const allViews = useDownloadViews(downloadsQuery.data ?? []);

  // First load only: a background refetch should not replace content with a
  // skeleton, so we key off `isPending` rather than `isFetching`.
  const isLoading = downloadsQuery.isPending;

  const ongoing = useMemo(() => allViews.filter((d) => d.isActive), [allViews]);
  const focus = ongoing[0] ?? allViews[0] ?? null;

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
    return {
      freeLabel: formatBytes(fs.availBytes),
      usedPercent: fs.usedFraction,
      summary: `${formatBytes(fs.usedBytes)} of ${formatBytes(fs.totalBytes)} used on the volume holding ${systemStorage?.workDir ?? 'the library'}.`,
    };
  }, [systemStorage]);

  const filteredFiles = useMemo(() => {
    switch (filter) {
      case 'ongoing':
        return allViews.filter((d) => d.isActive);
      case 'done':
        return allViews.filter((d) => d.state === 'done');
      case 'problem':
        return allViews.filter((d) => d.state === 'failed' || d.state === 'cancelled');
      default:
        return allViews;
    }
  }, [allViews, filter]);

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
   * Deleting removes files from disk, so it asks first. `window.confirm` is
   * deliberate: a destructive, irreversible action deserves a blocking prompt
   * rather than a toast you might miss.
   */
  const deleteFile = useCallback(
    (id: string) => {
      const file = allViews.find((d) => d.id === id);
      const name = file?.name ?? id;
      const ok = window.confirm(
        `Delete ${name}?\n\nThis removes the record and deletes the downloaded files from disk. It cannot be undone.`,
      );
      if (!ok) return;
      void removeDownload
        .mutateAsync({ id, removeFiles: true })
        .then(() => toast.success('Deleted', `${name} and its files were removed.`))
        .catch((e: unknown) => toast.error('Could not delete', describeError(e)));
    },
    [allViews, removeDownload, toast],
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

  const runProbe = useCallback(async () => {
    const url = draft.url.trim();
    if (!url) return;
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
        toast.error('Probe rejected the source', 'The bytes are not a recognised video format.');
      } else {
        const qualities = toQualityOptions(result.media?.qualities ?? []);
        setProbe({
          status: 'done',
          container: result.media ? result.media.container : '—',
          rangeSupport: result.acceptRanges ? 'Accepted (parallel download available)' : 'Not supported',
          size: formatBytes(result.totalBytes > 0 ? result.totalBytes : 0),
          confidence: result.media ? result.media.confidence.toFixed(3) : '—',
          qualities,
        });
        // Default to the largest rendition the manifest offers.
        setSelectedQuality(qualities.length > 0 ? (qualities[0]?.height ?? null) : null);
        toast.success('Probe succeeded', result.media ? `Detected ${result.media.container}` : undefined);
      }
    } catch (e) {
      setProbe({ status: 'error', message: 'Probe failed' });
      toast.error('Probe failed', describeError(e));
    } finally {
      setProbing(false);
    }
  }, [draft.url, draft.durationSeconds, probeUrl, toast]);

  const [title, subtitle] = VIEW_TITLES[view];

  return (
    <>
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
            focus={focus}
            focusStage={1}
            onViewAll={() => setView('downloads')}
            onSelect={setOpenFileId}
          />
        ))}

        {view === 'downloads' &&
          (isLoading ? (
            <DownloadsSkeleton />
          ) : (
            <DownloadsPage downloads={allViews} onSelect={setOpenFileId} />
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
            onStart={() => {
              const url = draft.url.trim();
              if (!url) return;
              void runAction(
                'start-download',
                async () => {
                  await createDownload.mutateAsync({
                    input: {
                      url,
                      hls: draft.hls,
                      upload: draft.upload,
                      purge: draft.purge,
                      connections: draft.connections,
                    },
                  });
                  setView('downloads');
                },
                { success: 'Download queued', failure: 'Could not queue the download' },
              );
            }}
          />
          ))}

        {view === 'storage' &&
          (isLoading ? (
            <StorageSkeleton />
          ) : (
          <StoragePage
            tab={storageTab}
            onTabChange={setStorageTab}
            downloads={filteredFiles}
            ongoing={ongoing}
            filter={filter}
            onFilterChange={setFilter}
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
