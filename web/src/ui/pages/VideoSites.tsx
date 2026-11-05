/**
 * VideoSites — find a video by name, or list a channel.
 *
 * The third way into "New download", and a different question from the other
 * two. The source form wants a URL you know; the site search wants a site and a
 * file name. This wants either a *name* (find it for me) or a *channel* (list
 * what is there), because on a video site neither the URL nor a crawl is the
 * right instrument: the media URL is signed and computed at play time, so there
 * is nothing for a crawler to read.
 *
 * WHAT A LIST IS
 *
 * A search or a channel returns *entries* — title, duration, uploader — and no
 * stream URLs. That is deliberate on both sides: resolving a hundred items to
 * show a hundred titles would cost a hundred times as much for nothing. Picking
 * one resolves it, and then the quality picker appears.
 *
 * THE FILTERS ARE THE EXTRACTOR'S LANGUAGE
 *
 * `1:20` and `duration > 600` are passed through, not parsed. Re-spelling them
 * here would mean tracking yt-dlp's filter syntax as it changes, and the field
 * hints show the shapes rather than validating them.
 */

import { useState } from 'react';
import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { Field } from '../primitives/Field';
import { Note } from '../primitives/Note';
import { EmptyState } from '../primitives/EmptyState';
import { Spinner } from '../primitives/Spinner';
import { QualityPicker } from '../downloads/QualityPicker';
import { IconSearch, IconDownload, IconPlay, IconRefresh } from '../primitives/Icons';
import { formatBytes, type QualityOption } from '../../hooks/view-models';
import type { ExtractEntry, ExtractResult } from '../../hooks';

export type VideoMode = 'search' | 'channel';

export interface VideoSitesValue {
  /** What to look for, in search mode. */
  term: string;
  /** The channel or playlist to list, in channel mode. */
  channelUrl: string;
  /** How many search results to ask for. */
  limit: number;
  /** How many entries of a list to take, in the extractor's syntax. */
  playlistItems: string;
  /** A filter the extractor applies while listing. */
  matchFilter: string;
  /** Only items from this date, as YYYYMMDD. */
  dateAfter: string;
}

export interface VideoSitesProps {
  value: VideoSitesValue;
  onChange: (next: VideoSitesValue) => void;
  /** The last result: a list of entries, or one resolved item. */
  result: ExtractResult | null;
  running: boolean;
  /** Why the last run failed, when it did. */
  error: string | null;
  /** Whether the extractor is installed on this Vyzor. */
  available: boolean;
  /** The entry being resolved, so its button can spin. */
  resolvingUrl: string | null;
  /** The entry whose download is being queued. */
  queueingUrl: string | null;
  onRun: (mode: VideoMode) => void;
  onStop: () => void;
  /** Resolve an entry to its streams. */
  onResolve: (entry: ExtractEntry) => void;
  /** Queue a download for the resolved item, at a chosen height. */
  onDownload: (url: string, title: string, height?: number) => void;
  /** Queue a download for a list entry directly, at the best height. */
  onDownloadEntry: (entry: ExtractEntry) => void;
}

export function VideoSites(props: VideoSitesProps) {
  const {
    value,
    onChange,
    result,
    running,
    error,
    available,
    resolvingUrl,
    queueingUrl,
    onRun,
    onStop,
    onResolve,
    onDownload,
    onDownloadEntry,
  } = props;
  const [mode, setMode] = useState<VideoMode>('search');
  const [showFilters, setShowFilters] = useState(false);
  const set = (patch: Partial<VideoSitesValue>) => onChange({ ...value, ...patch });

  const ready = mode === 'search' ? value.term.trim().length > 0 : value.channelUrl.trim().length > 0;

  if (!available) {
    return (
      <Card title="Video sites">
        <EmptyState
          title="No extractor on this Vyzor"
          description="Reading a video site needs yt-dlp installed on the host. Everything else here works without it."
        />
      </Card>
    );
  }

  return (
    <>
      <Card
        title="Video sites"
        subtitle="Find a video by name, or list a channel — the media is resolved when you pick one"
      >
        <div className="browse__modes" role="group" aria-label="How to find a video">
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
            className={mode === 'channel' ? 'browse__mode is-active' : 'browse__mode'}
            aria-pressed={mode === 'channel'}
            onClick={() => setMode('channel')}
          >
            List a channel
          </button>
        </div>

        <div className="form">
          {mode === 'search' ? (
            <div className="field field--full">
              <Field
                label="What are you looking for?"
                value={value.term}
                onChange={(term) => set({ term })}
                placeholder="big buck bunny"
                hint="A title, an artist, a phrase — the video sites' own search is used."
              />
            </div>
          ) : (
            <div className="field field--full">
              <Field
                label="Channel or playlist URL"
                value={value.channelUrl}
                onChange={(channelUrl) => set({ channelUrl })}
                placeholder="https://www.youtube.com/@someChannel"
                hint="Its videos are listed without resolving each one, so a large channel is cheap."
              />
            </div>
          )}

          <Button size="sm" variant="ghost" onClick={() => setShowFilters((v) => !v)}>
            {showFilters ? 'Hide filters' : 'Filters'}
          </Button>

          {showFilters && (
            <>
              <Field
                label="How many entries"
                value={value.playlistItems}
                onChange={(playlistItems) => set({ playlistItems })}
                placeholder="-20:"
                hint="The extractor's syntax: 1:20 for the first twenty, -20: for the newest twenty."
              />
              <Field
                label="Filter"
                value={value.matchFilter}
                onChange={(matchFilter) => set({ matchFilter })}
                placeholder="duration > 600"
                hint="Applied while listing, so a filtered-out item is never resolved. e.g. duration > 600"
              />
              <Field
                label="From date"
                value={value.dateAfter}
                onChange={(dateAfter) => set({ dateAfter })}
                placeholder="20240101"
                hint="YYYYMMDD. Only items from this date on."
              />
            </>
          )}
        </div>

        <div className="form-actions">
          {running ? (
            <Button onClick={onStop}>Stop</Button>
          ) : (
            <Button variant="primary" onClick={() => onRun(mode)} disabled={!ready}>
              <IconSearch /> {mode === 'search' ? 'Search' : 'List the channel'}
            </Button>
          )}
        </div>

        {running && (
          <Note className="note">
            Asking the video site. This reads a page and signs the request, so it
            takes a few seconds.
          </Note>
        )}
      </Card>

      <Card
        title="Results"
        subtitle={
          result === null
            ? 'Nothing searched yet'
            : result.entries.length > 0
              ? `${result.entries.length} item${result.entries.length === 1 ? '' : 's'}`
              : result.title
                ? result.title.slice(0, 60)
                : 'Resolved'
        }
      >
        {error !== null && <Note className="note note--warn">{error}</Note>}

        {running && result === null ? (
          <div className="browse__loading">
            <Spinner size="lg" />
          </div>
        ) : result === null ? (
          <EmptyState
            title="Search a video site"
            description="Type a name to find it, or paste a channel to list what is on it. The media itself is resolved when you pick one."
          />
        ) : result.qualities.length > 0 ? (
          <ResolvedItem
            result={result}
            queueingUrl={queueingUrl}
            onDownload={onDownload}
          />
        ) : result.entries.length === 0 ? (
          <EmptyState
            title="Nothing found"
            description="That search returned no items. Try a shorter phrase, or check the channel URL."
          />
        ) : (
          <ul className="browse__grid">
            {result.entries.map((entry) => (
              <li key={entry.id ?? entry.url} className="bcard">
                <span className="bcard__thumb">
                  {entry.thumbnail ? (
                    <img src={entry.thumbnail} alt="" loading="lazy" />
                  ) : (
                    <span className="bcard__glyph">
                      <IconPlay />
                    </span>
                  )}
                </span>
                <span className="bcard__body">
                  <span className="bcard__title" title={entry.title}>
                    {entry.title}
                  </span>
                  <span className="bcard__meta">
                    {entry.uploader ?? result.extractor ?? ''}
                    {entry.durationSeconds >= 0 ? ` · ${formatDuration(entry.durationSeconds)}` : ''}
                    {entry.isPlaylist ? ' · a playlist' : ''}
                  </span>
                </span>
                <div className="bcard__actions">
                  <Button
                    size="sm"
                    loading={resolvingUrl === entry.url}
                    onClick={() => onResolve(entry)}
                    disabled={entry.isPlaylist || !entry.url}
                    title={entry.isPlaylist ? 'A playlist — open it to list its videos' : 'Find the renditions'}
                  >
                    Choose
                  </Button>
                  <Button
                    size="sm"
                    variant="primary"
                    loading={queueingUrl === entry.url}
                    onClick={() => onDownloadEntry(entry)}
                    disabled={entry.isPlaylist || !entry.url}
                    title="Download at the best quality"
                  >
                    <IconDownload />
                  </Button>
                </div>
              </li>
            ))}
          </ul>
        )}

        {result !== null && result.entries.length > 0 && (
          <Note className="note">
            A list carries titles and durations but no stream URLs — resolving
            every item to show a hundred titles would cost a hundred times as
            much. <strong>Choose</strong> one to see its qualities, or download it
            at the best.
          </Note>
        )}
      </Card>
    </>
  );
}

/** One resolved item, with its renditions. */
function ResolvedItem({
  result,
  queueingUrl,
  onDownload,
}: {
  result: ExtractResult;
  queueingUrl: string | null;
  onDownload: (url: string, title: string, height?: number) => void;
}) {
  const [selected, setSelected] = useState<number | null>(null);
  const url = result.url ?? '';
  const title = result.title ?? 'the video';
  const options: QualityOption[] = result.qualities.map((q) => ({
    url,
    height: q.height,
    width: 0,
    label: q.label,
    codecs: '',
    bytes: q.bytes,
    sizeLabel: q.bytes >= 0 ? formatBytes(q.bytes) : 'size unknown',
  }));
  const active = selected ?? result.qualities[0]!.height;
  const chosen = result.qualities.find((q) => q.height === active) ?? result.qualities[0]!;

  return (
    <>
      <div className="kv">
        <div className="kv__row">
          <span className="kv__k">Title</span>
          <span className="kv__v kv__v--strong">{title}</span>
        </div>
        {result.extractor && (
          <div className="kv__row">
            <span className="kv__k">Source</span>
            <span className="kv__v muted">{result.extractor}</span>
          </div>
        )}
        {result.durationSeconds >= 0 && (
          <div className="kv__row">
            <span className="kv__k">Duration</span>
            <span className="kv__v muted">{formatDuration(result.durationSeconds)}</span>
          </div>
        )}
      </div>
      <div style={{ marginTop: 16 }}>
        <QualityPicker options={options} selectedHeight={active} onSelect={setSelected} />
      </div>
      <div className="form-actions">
        <Button
          variant="primary"
          loading={queueingUrl === url}
          onClick={() => onDownload(url, title, chosen.height)}
        >
          <IconDownload /> Download {chosen.label}
        </Button>
      </div>
      <Note className="note">
        Choosing a smaller rendition downloads less, not a scaled copy of the
        same file.
      </Note>
    </>
  );
}

/** Seconds as a clock, e.g. "10:35". */
function formatDuration(seconds: number): string {
  if (seconds < 0) return '';
  const s = Math.round(seconds);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const two = (n: number) => String(n).padStart(2, '0');
  return h > 0 ? `${h}:${two(m)}:${two(sec)}` : `${m}:${two(sec)}`;
}

/** Exported so the page can label the tab without duplicating the wording. */
export const VIDEO_MODE_LABELS: Record<VideoMode, string> = {
  search: 'Search by name',
  channel: 'List a channel',
};

/** Re-exported for the parent, which owns the request. */
export { IconRefresh };
