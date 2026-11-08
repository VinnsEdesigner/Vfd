/**
 * Torrents — find a torrent, or open one you already have, and download it.
 *
 * Three ways in, because there are three things a person has:
 *
 *   search   a *name*. A torrent index is a search box, so this asks the index
 *            and shows what matches — the same "enter the filename, get results"
 *            shape as the site search, but for a BitTorrent index.
 *   magnet   a *link* someone sent them. A magnet names a hash and a name and
 *            nothing else, so this shows what is known and downloads it.
 *   file     a `.torrent` they downloaded. A `.torrent` *is* the metadata, so
 *            this shows the whole file list before anything is fetched.
 *
 * The detail table is deliberately the index's own shape — seeders, leechers,
 * size, category, when it was posted — because those are what decide between two
 * identically-named torrents, and hiding them would make the user go back to the
 * site to choose.
 *
 * The component is presentational: every request and every piece of state is
 * owned by the caller, so leaving the tab and returning does not throw the
 * results away.
 */

import { useRef, useState } from 'react';
import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { Field } from '../primitives/Field';
import { Note } from '../primitives/Note';
import { EmptyState } from '../primitives/EmptyState';
import { Spinner } from '../primitives/Spinner';
import { DataTable, type Column } from '../primitives/DataTable';
import { QualityPicker } from '../downloads/QualityPicker';
import { IconSearch, IconDownload, IconExternal, IconFile, IconRefresh } from '../primitives/Icons';
import { formatBytes, type QualityOption } from '../../hooks/view-models';
import type { ExtractEntry, ExtractResult, TorrentInfo } from '../../hooks';

export type TorrentMode = 'search' | 'magnet' | 'file';

export interface TorrentsValue {
  /** The name to look for on the index. */
  term: string;
  /** How many results to ask the index for. */
  limit: number;
  /** A magnet link the user pasted. */
  magnet: string;
}

export interface TorrentsProps {
  value: TorrentsValue;
  onChange: (next: TorrentsValue) => void;
  /** The index search's answer, or null before one has run. */
  result: ExtractResult | null;
  running: boolean;
  /** Why the search failed outright, when it did. */
  error: string | null;
  /** Whether the torrent extractor is installed on this Vyzor. */
  available: boolean;
  /** The entry whose download is being queued, so its button can spin. */
  queueingUrl: string | null;
  onSearch: () => void;
  onStop: () => void;
  /** Download one search result at its best files. */
  onDownloadEntry: (entry: ExtractEntry) => void;
  /** The metadata of a magnet or a `.torrent`, once read. */
  inspected: TorrentInfo | null;
  inspecting: boolean;
  /** Why the last inspect failed, when it did. */
  inspectError: string | null;
  /** Read a magnet's metadata (its hash and name). */
  onInspectMagnet: (magnet: string) => void;
  /** Read a `.torrent` file's metadata (its whole file list). */
  onOpenTorrentFile: (file: File) => void;
  /** Forget the last inspect, so a new file starts clean. */
  onClearInspect: () => void;
  /** Download a torrent, with a chosen file selection when one was made. */
  onDownloadTorrent: (opts: { magnet: string; name: string; files?: number[] }) => void;
}

export function Torrents(props: TorrentsProps) {
  const {
    value,
    onChange,
    result,
    running,
    error,
    available,
    queueingUrl,
    onSearch,
    onStop,
    onDownloadEntry,
    inspected,
    inspecting,
    inspectError,
    onInspectMagnet,
    onOpenTorrentFile,
    onClearInspect,
    onDownloadTorrent,
  } = props;
  const [mode, setMode] = useState<TorrentMode>('search');
  const set = (patch: Partial<TorrentsValue>) => onChange({ ...value, ...patch });

  if (!available) {
    return (
      <Card title="Torrents">
        <EmptyState
          title="No torrent extractor on this Vyzor"
          description="Searching a torrent index needs py1337x installed on the host. You can still open a magnet or a .torrent file — those are read here."
        />
      </Card>
    );
  }

  return (
    <>
      <Card
        title="Torrents"
        subtitle="Search a BitTorrent index by name, paste a magnet, or open a .torrent file"
      >
        <div className="browse__modes" role="group" aria-label="How to find a torrent">
          <button
            type="button"
            className={mode === 'search' ? 'browse__mode is-active' : 'browse__mode'}
            aria-pressed={mode === 'search'}
            onClick={() => setMode('search')}
          >
            Search by name
          </button>
          <button
            type="button"
            className={mode === 'magnet' ? 'browse__mode is-active' : 'browse__mode'}
            aria-pressed={mode === 'magnet'}
            onClick={() => setMode('magnet')}
          >
            Paste a magnet
          </button>
          <button
            type="button"
            className={mode === 'file' ? 'browse__mode is-active' : 'browse__mode'}
            aria-pressed={mode === 'file'}
            onClick={() => setMode('file')}
          >
            Open a .torrent
          </button>
        </div>

        {mode === 'search' ? (
          <div className="form">
            <div className="field field--full">
              <Field
                label="File name"
                value={value.term}
                onChange={(term) => set({ term })}
                placeholder="ubuntu 24.04"
                hint="Searched on the index. A release name, a title, an artist — whatever you would type on the site."
              />
            </div>
            <Field
              label="Results"
              type="number"
              min={1}
              max={50}
              value={String(value.limit)}
              onChange={(v) => set({ limit: Math.max(1, Math.min(50, Number(v) || 10)) })}
              hint="How many torrents to list. Each one is a page fetch, so a large number is slow."
            />
          </div>
        ) : mode === 'magnet' ? (
          <div className="form">
            <div className="field field--full">
              <Field
                label="Magnet link"
                value={value.magnet}
                onChange={(magnet) => set({ magnet })}
                placeholder="magnet:?xt=urn:btih:…"
                hint="A magnet names a torrent's hash. The file list is fetched from the swarm when the download starts."
              />
            </div>
          </div>
        ) : (
          <TorrentFileDrop onOpen={onOpenTorrentFile} />
        )}

        <div className="form-actions">
          {mode === 'search' ? (
            running ? (
              <Button onClick={onStop}>Stop</Button>
            ) : (
              <Button variant="primary" onClick={onSearch} disabled={!value.term.trim()}>
                <IconSearch /> Search the index
              </Button>
            )
          ) : mode === 'magnet' ? (
            <>
              <Button
                variant="primary"
                onClick={() => onInspectMagnet(value.magnet.trim())}
                disabled={!isMagnet(value.magnet)}
              >
                <IconFile /> Read the magnet
              </Button>
              <Button
                loading={queueingUrl === value.magnet}
                onClick={() =>
                  onDownloadTorrent({
                    magnet: value.magnet.trim(),
                    name: magnetName(value.magnet) ?? 'the torrent',
                  })
                }
                disabled={!isMagnet(value.magnet)}
              >
                <IconDownload /> Download now
              </Button>
            </>
          ) : (
            <Note className="note">
              Open a `.torrent` file to see everything inside it — every file, its
              size, the piece length and the trackers — before anything is fetched.
            </Note>
          )}
        </div>

        {running && (
          <Note className="note">
            Asking the index. Each result is a page fetch, so a search takes a
            few seconds.
          </Note>
        )}
      </Card>

      {/*
        The metadata of a magnet or a .torrent, once it has been read. A `.torrent`
        has its whole file list; a magnet has only a hash and a name, and says so.
      */}
      {(inspected !== null || inspecting || inspectError !== null) && (
        <TorrentDetail
          info={inspected}
          loading={inspecting}
          error={inspectError}
          queueing={queueingUrl}
          onDownload={onDownloadTorrent}
          onClear={onClearInspect}
        />
      )}

      {mode === 'search' && (
        <Card
          title="Results"
          subtitle={
            result === null
              ? 'Nothing searched yet'
              : `${result.entries.length} torrent${result.entries.length === 1 ? '' : 's'}`
          }
        >
          {error !== null && <Note className="note note--warn">{error}</Note>}

          {running && result === null ? (
            <div className="browse__loading">
              <Spinner size="lg" />
            </div>
          ) : result === null ? (
            <EmptyState
              title="Search a torrent index"
              description="Enter a name and Vyzor asks the index. Every result shows its seeders, leechers, size and category, so you can choose without going back to the site."
            />
          ) : result.entries.length === 0 ? (
            <EmptyState
              title="Nothing found"
              description="That search returned no torrents. It may also mean the index refused this address — a torrent index sits behind a bot wall, and a datacenter address is often turned away. Try a shorter name, or open a magnet instead."
            />
          ) : (
            <TorrentResults
              entries={result.entries}
              queueingUrl={queueingUrl}
              onDownload={onDownloadEntry}
            />
          )}
        </Card>
      )}
    </>
  );
}

/** The index's own detail, as a table — the columns 1337x itself shows. */
function TorrentResults({
  entries,
  queueingUrl,
  onDownload,
}: {
  entries: ReadonlyArray<ExtractEntry>;
  queueingUrl: string | null;
  onDownload: (entry: ExtractEntry) => void;
}) {
  const columns: ReadonlyArray<Column<ExtractEntry>> = [
    {
      key: 'poster',
      header: '',
      width: '52px',
      render: (e) => (
        /*
         * The poster the index reported, when it did. Fetched by the browser
         * directly — it is a public image URL and the request carries no
         * credentials — so a site that serves its thumbnails openly just works.
         * A poster that will not load falls back to the category glyph rather
         * than a broken image.
         */
        <span className="tt__thumb">
          {e.thumbnail ? (
            <img
              src={e.thumbnail}
              alt=""
              loading="lazy"
              onError={(ev) => {
                const img = ev.currentTarget;
                img.style.display = 'none';
                img.parentElement?.classList.add('tt__thumb--failed');
              }}
            />
          ) : (
            <span className="tt__thumbglyph">
              <IconFile />
            </span>
          )}
        </span>
      ),
    },
    {
      key: 'name',
      header: 'Name',
      render: (e) => (
        <span className="tt__name">
          {e.title}
          {e.uploader ? <span className="tt__by"> · {e.uploader}</span> : null}
        </span>
      ),
    },
    {
      key: 'seeders',
      header: 'Seeds',
      width: '72px',
      align: 'right',
      render: (e) => <span className={seederTone(e.seeders)}>{count(e.seeders)}</span>,
    },
    {
      key: 'leechers',
      header: 'Leech',
      width: '72px',
      align: 'right',
      render: (e) => <span className="muted">{count(e.leechers)}</span>,
    },
    {
      key: 'size',
      header: 'Size',
      width: '96px',
      align: 'right',
      render: (e) => <span className="muted">{sizeOf(e)}</span>,
    },
    {
      key: 'category',
      header: 'Category',
      width: '120px',
      render: (e) => <span className="muted">{e.category ?? '—'}</span>,
    },
    {
      key: 'uploaded',
      header: 'Uploaded',
      width: '120px',
      render: (e) => <span className="muted">{e.uploadedText ?? '—'}</span>,
    },
    {
      key: 'actions',
      header: '',
      width: '150px',
      render: (e) => (
        <div className="tt__actions">
          {e.pageUrl && (
            <a
              className="tt__link"
              href={e.pageUrl}
              target="_blank"
              rel="noreferrer noopener"
              title="Open on the index"
              onClick={(ev) => ev.stopPropagation()}
            >
              <IconExternal />
            </a>
          )}
          <Button
            size="sm"
            variant="primary"
            loading={queueingUrl === e.url}
            onClick={() => onDownload(e)}
            disabled={!e.isTorrent}
            title="Download this torrent"
          >
            <IconDownload />
          </Button>
        </div>
      ),
    },
  ];

  return (
    <>
      <DataTable
        columns={columns}
        rows={entries}
        rowKey={(e) => e.id ?? e.url}
        className="table--torrents"
      />
      <Note className="note">
        The seeders decide how fast a torrent arrives — a torrent with none may
        never start. The download takes the whole torrent; open it to choose
        individual files.
      </Note>
    </>
  );
}

/**
 * What a magnet or a `.torrent` contains.
 *
 * A `.torrent` yields the file list, the piece size and the trackers. A magnet
 * yields only a hash and a name, and says so rather than showing an empty list —
 * the file list lives in the swarm, and is fetched when the download starts.
 */
function TorrentDetail({
  info,
  loading,
  error,
  queueing,
  onDownload,
  onClear,
}: {
  info: TorrentInfo | null;
  loading: boolean;
  error: string | null;
  queueing: string | null;
  onDownload: (opts: { magnet: string; name: string; files?: number[] }) => void;
  onClear: () => void;
}) {
  const [selected, setSelected] = useState<Set<number>>(new Set());
  const [active, setActive] = useState<number | null>(null);

  if (loading) {
    return (
      <Card title="Torrent">
        <div className="browse__loading">
          <Spinner size="lg" />
        </div>
      </Card>
    );
  }
  if (error !== null) {
    return (
      <Card title="Torrent">
        <Note className="note note--warn">{error}</Note>
        <div className="form-actions">
          <Button onClick={onClear}>Close</Button>
        </div>
      </Card>
    );
  }
  if (!info) return null;

  const magnet = info.magnet ?? '';
  const name = info.name ?? 'the torrent';
  const total = info.totalBytes >= 0 ? formatBytes(info.totalBytes) : 'unknown size';

  /*
   * The file picker. A torrent is often a set — a film and its subtitles, or a
   * whole season — and fetching all of it to get one file wastes the difference,
   * so the files are offered with their sizes and any subset can be taken. The
   * indices are 1-based, matching the order aria2 numbers them.
   */
  const options: QualityOption[] = info.files.map((f, i) => ({
    // Reusing the picker's `height` as the file index keeps one control for
    // both jobs; the label is the file's own name.
    height: i + 1,
    width: 0,
    label: f.name,
    codecs: f.path,
    bytes: f.bytes,
    sizeLabel: f.bytes >= 0 ? formatBytes(f.bytes) : 'size unknown',
    url: magnet,
  }));

  const chosenFiles = selected.size > 0 ? [...selected].sort((a, b) => a - b) : undefined;

  return (
    <Card
      title="Torrent"
      subtitle={info.complete ? 'Read from the .torrent file' : 'A magnet — the file list is fetched from the swarm'}
    >
      <div className="tt__detail">
        {/*
          A poster, when the torrent carries one.
          The image lives inside the torrent, so it is fetched from a webseed
          (`<webseed>/<path>`) — the server picked the largest real image and
          gave the URL. A torrent with no webseed has none, and the panel simply
          shows no poster rather than a placeholder.
        */}
        {info.poster && (
          <img
            className="tt__poster"
            src={info.poster}
            alt=""
            loading="lazy"
            onError={(ev) => {
              const img = ev.currentTarget;
              img.style.display = 'none';
            }}
          />
        )}
        <div className="kv">
          <div className="kv__row">
            <span className="kv__k">Name</span>
            <span className="kv__v kv__v--strong">{name}</span>
          </div>
          <div className="kv__row">
            <span className="kv__k">Total size</span>
          <span className="kv__v muted">{total}</span>
        </div>
        {info.infoHash && (
          <div className="kv__row">
            <span className="kv__k">Info hash</span>
            <span className="kv__v muted kv__v--mono">{info.infoHash}</span>
          </div>
        )}
        {info.complete && (
          <>
            <div className="kv__row">
              <span className="kv__k">Files</span>
              <span className="kv__v muted">{info.files.length}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">Pieces</span>
              <span className="kv__v muted">
                {info.pieceCount} × {formatBytes(info.pieceLength)}
              </span>
            </div>
            {info.isPrivate && (
              <div className="kv__row">
                <span className="kv__k">Private</span>
                <span className="kv__v muted">Yes — no DHT or peer exchange</span>
              </div>
            )}
            {info.createdBy && (
              <div className="kv__row">
                <span className="kv__k">Created by</span>
                <span className="kv__v muted">{info.createdBy}</span>
              </div>
            )}
          </>
        )}
        {info.trackers.length > 0 && (
          <div className="kv__row">
            <span className="kv__k">Trackers</span>
            <span className="kv__v muted">{info.trackers.length}</span>
          </div>
        )}
        {info.webseeds.length > 0 && (
          <div className="kv__row">
            <span className="kv__k">Webseeds</span>
            <span className="kv__v muted">{info.webseeds.length}</span>
          </div>
        )}
      </div>
      </div>

      {info.complete && info.files.length > 0 ? (
        <>
          <div style={{ marginTop: 18 }}>
            <label className="tt__picklabel">
              Files — pick any to download only those, or take the whole torrent
            </label>
            <QualityPicker
              variant="file"
              options={options}
              selectedHeight={active}
              onSelect={(i) => {
                setActive(i);
                setSelected((prev) => {
                  const next = new Set(prev);
                  if (next.has(i)) next.delete(i);
                  else next.add(i);
                  return next;
                });
              }}
            />
          </div>
          <div className="form-actions">
            <Button
              variant="primary"
              loading={queueing === magnet}
              onClick={() => onDownload({ magnet, name, files: chosenFiles })}
            >
              <IconDownload />{' '}
              {chosenFiles
                ? `Download ${chosenFiles.length} file${chosenFiles.length === 1 ? '' : 's'}`
                : 'Download all'}
            </Button>
            <Button onClick={onClear}>Close</Button>
          </div>
        </>
      ) : (
        <>
          <Note className="note">
            A magnet names the hash and the name only. The files inside are known
            once a client has fetched the metadata from the swarm, so this list
            appears when the download starts.
          </Note>
          <div className="form-actions">
            <Button
              variant="primary"
              loading={queueing === magnet}
              onClick={() => onDownload({ magnet, name })}
              disabled={!magnet}
            >
              <IconDownload /> Download the torrent
            </Button>
            <Button onClick={onClear}>Close</Button>
          </div>
        </>
      )}
      {info.trackers.length > 0 && (
        <details className="sites">
          <summary>Trackers — {info.trackers.length}</summary>
          <div className="tt__trackers">
            {info.trackers.map((t) => (
              <div key={t} className="tt__tracker" title={t}>
                {t}
              </div>
            ))}
          </div>
        </details>
      )}
    </Card>
  );
}

/** The drop target for a `.torrent` file. */
function TorrentFileDrop({ onOpen }: { onOpen: (file: File) => void }) {
  const inputRef = useRef<HTMLInputElement | null>(null);
  const [dragging, setDragging] = useState(false);

  return (
    <div
      className={dragging ? 'tt__drop is-dragging' : 'tt__drop'}
      onDragOver={(e) => {
        e.preventDefault();
        setDragging(true);
      }}
      onDragLeave={() => setDragging(false)}
      onDrop={(e) => {
        e.preventDefault();
        setDragging(false);
        const f = e.dataTransfer.files[0];
        if (f) onOpen(f);
      }}
    >
      <IconFile />
      <p className="tt__droptext">Drop a .torrent file here, or</p>
      <Button onClick={() => inputRef.current?.click()}>Choose a file</Button>
      <input
        ref={inputRef}
        type="file"
        accept=".torrent,application/x-bittorrent"
        className="tt__fileinput"
        onChange={(e) => {
          const f = e.target.files?.[0];
          if (f) onOpen(f);
          // Reset so the same file can be chosen twice.
          e.target.value = '';
        }}
      />
      <Note className="note">
        A `.torrent` file carries its own file list, so everything inside is shown
        before anything is downloaded.
      </Note>
    </div>
  );
}

/** Is this a magnet link? */
export function isMagnet(value: string): boolean {
  return /^magnet:\?/i.test(value.trim());
}

/** The display name out of a magnet's `dn=`, decoded. */
export function magnetName(magnet: string): string | null {
  const m = /[?&]dn=([^&]+)/i.exec(magnet);
  if (!m || !m[1]) return null;
  try {
    return decodeURIComponent(m[1].replace(/\+/g, ' '));
  } catch {
    return m[1];
  }
}

/** A count, or an em dash when the index did not report one. */
function count(n: number): string {
  return n < 0 ? '—' : String(n);
}

/** The size as the index shows it, falling back to a formatted byte count. */
function sizeOf(e: ExtractEntry): string {
  if (e.sizeText) return e.sizeText;
  return e.bytes >= 0 ? formatBytes(e.bytes) : '—';
}

/** A seeder count reads as health: none is dead, many is fast. */
function seederTone(seeders: number): string {
  if (seeders < 0) return 'muted';
  if (seeders === 0) return 'tt__seeders tt__seeders--dead';
  if (seeders < 5) return 'tt__seeders tt__seeders--low';
  return 'tt__seeders tt__seeders--ok';
}

/** Re-exported so the parent can label the tab without repeating the wording. */
export const TORRENT_MODE_LABELS: Record<TorrentMode, string> = {
  search: 'Search by name',
  magnet: 'Paste a magnet',
  file: 'Open a .torrent',
};

export { IconRefresh };
