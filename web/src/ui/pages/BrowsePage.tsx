/**
 * BrowsePage — search the open web for media, then preview, download or save it.
 *
 * The results are *candidates*, not library items. Nothing is fetched until the
 * user asks, which is why each card carries its own actions rather than the grid
 * having a single "download all" affordance that would fire off 24 requests.
 *
 * The source is Wikimedia Commons: open, no key, CORS-enabled, and its media is
 * public domain or freely licensed. That is the only kind of material this app is
 * for, and it means every result is safe to keep.
 */

import { useEffect, useMemo, useState } from 'react';
import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { Field } from '../primitives/Field';
import { EmptyState } from '../primitives/EmptyState';
import { Spinner } from '../primitives/Spinner';
import { Note } from '../primitives/Note';
import { IconSearch, IconRefresh, IconFolder, IconImage, IconMusic, IconFile } from '../primitives/Icons';
import { formatBytes } from '../../hooks/view-models';
import {
  browseKindLabel,
  type BrowseFilter,
  type BrowseItem,
  type BrowseKind,
  type LiveFailure,
  type RenderFailure,
  type RenderMode,
} from '../../hooks';
import { InteractiveView } from './InteractiveView';
import type { InteractiveViewProps } from './InteractiveView';

export interface BrowsePageProps {
  term: string;
  onTermChange: (term: string) => void;
  kind: BrowseKind | 'all';
  onKindChange: (kind: BrowseKind | 'all') => void;
  items: ReadonlyArray<BrowseItem>;
  total: number;
  hasMore: boolean;
  loading: boolean;
  error: string | null;
  /** True while a download request is being accepted by Vyzor. */
  queueingId?: string | null;
  onPreview: (item: BrowseItem) => void;
  onDownload: (item: BrowseItem) => void;
  onSaveToDevice: (item: BrowseItem) => void;
  onNextPage: () => void;
  onPrevPage: () => void;
  page: number;
  /** Whether save-to-device is usable at all on this device. */
  canSaveToDevice: boolean;

  /**
   * Which source the view is reading from:
   *   search  the built-in open-media search
   *   url     a page or directory the user typed
   */
  mode: BrowseMode;
  onModeChange: (mode: BrowseMode) => void;
  /** The URL being read, when the mode is `url`. */
  sourceUrl: string;
  onSourceUrlChange: (url: string) => void;
  /** Runs the read; the search runs on a debounce instead. */
  onReadUrl: () => void;
  /** The page's own title, when the listing had one. */
  sourceTitle: string | null;
  /** How much of the page to list. */
  filter: BrowseFilter;
  onFilterChange: (filter: BrowseFilter) => void;

  /** Whether Vyzor can render a page here at all. */
  canRender: boolean;
  /**
   * The rendered page, once one has been made. Null until the user asks, because
   * a render is an expensive action rather than something a mount should do.
   */
  rendered: { url: string; mode: RenderMode } | null;
  /** True while a render is in flight. */
  rendering: boolean;
  /** Why the last render failed, when it did. */
  renderFailure: RenderFailure | null;
  /** The page's own title, from the render. */
  renderedTitle: string | null;
  /** How long the render took, for the record. */
  renderMs: number | null;
  /** True when Vyzor served this from its cache rather than rendering. */
  renderedFromCache: boolean;
  /** True when Vyzor cleaned the page's markup before serving it. */
  renderedSanitized: boolean;
  /** Ask for a render in the given mode. */
  onRender: (mode: RenderMode) => void;
  /** Clear the rendered result, so the address can be changed. */
  onClearRender: () => void;

  /**
   * Whether Vyzor can drive a page live, and the session that does it.
   *
   * A live page is the richer of the two page views: a render draws it once, a
   * live session keeps it running. When it is available it is the default, with
   * the renderer kept as the fallback for a host that only has that.
   */
  canGoLive: boolean;
  live: InteractiveViewProps['live'];
  /**
   * Whether Vyzor can stream a page as raw video over WebRTC.
   *
   * A third way to look at a page, and the best one when it is available: the
   * picture path costs a JPEG encode and a CDP acknowledgement per frame, and
   * video does not. Kept behind its own flag because it is a heavier opt-in on
   * Vyzor — a session holds an X server as well as a browser.
   */
  canStreamVideo: boolean;
  video: NonNullable<InteractiveViewProps['video']>;
  /** The media the live page has loaded, and the controls for collecting it. */
  liveMedia: NonNullable<InteractiveViewProps['media']>;
  /** Queue a download for something the live page loaded. */
  onDownloadLive: (url: string, quality?: string) => void;
}

export type BrowseMode = 'search' | 'url' | 'page';

const KIND_FILTERS: ReadonlyArray<{ id: BrowseKind | 'all'; label: string }> = [
  { id: 'all', label: 'Everything' },
  { id: 'image', label: 'Images' },
  { id: 'audio', label: 'Audio' },
  { id: 'video', label: 'Video' },
  { id: 'document', label: 'Documents' },
];

/**
 * What to say when a render did not happen.
 *
 * Each reason gets its own sentence, because they need different responses: a
 * busy renderer is worth retrying, a disabled one never will be, and a refused
 * URL needs the address changed.
 */
function renderFailureLabel(f: RenderFailure): string {
  switch (f) {
    case 'disabled':
      return 'Rendering is turned off on this Vyzor';
    case 'unavailable':
      return 'This Vyzor has no browser installed, so it cannot render pages';
    case 'busy':
      return 'The renderer is busy with another page — try again in a moment';
    case 'timeout':
      return 'That page took too long to render and was stopped';
    case 'refused':
      return 'That address cannot be rendered — only public http(s) pages can';
    default:
      return 'That page could not be rendered';
  }
}

/** The same, as a longer explanation for the empty state. */
function renderFailureDetail(f: RenderFailure): string {
  switch (f) {
    case 'disabled':
      return 'Vyzor has rendering turned off. It is started with --render on a host that has a browser.';
    case 'unavailable':
      return 'Rendering needs a browser on Vyzor host. Vyzor reports this in its capabilities, so the control is only offered where it works.';
    case 'busy':
      return 'Only a few pages can be rendered at once, because each one costs about half a gigabyte. Nothing is queued — ask again shortly.';
    case 'timeout':
      return 'A page that never finishes loading is stopped at Vyzor’s limit rather than left to run.';
    case 'refused':
      return 'Only public http(s) pages are rendered. A file:// path would read Vyzor’s disk, and loopback or link-local addresses are refused.';
    default:
      return 'The browser could not produce a picture of that page.';
  }
}

/** A one-line reason a live session stopped, for the subtitle. */
function failureShort(f: LiveFailure): string {
  switch (f) {
    case 'disabled':
      return 'Live pages are turned off on this Vyzor';
    case 'unavailable':
      return 'This Vyzor has no browser, so it cannot drive a page';
    case 'busy':
      return 'Vyzor is already driving another page';
    case 'refused':
      return 'That address cannot be driven';
    case 'closed':
      return 'The session ended';
    default:
      return 'That page could not be opened';
  }
}

/** A glyph per kind, so a card is scannable when it has no thumbnail. */
function KindGlyph({ kind }: { kind: BrowseKind }) {
  if (kind === 'image') return <IconImage />;
  if (kind === 'audio') return <IconMusic />;
  return <IconFile />;
}

/**
 * The tile on a result card.
 *
 * A search result carries a thumbnail the source made for it. A page read from a
 * URL does not: Vyzor lists what the page references, and a listing has no
 * budget to fetch every referenced image to make one. That is why those cards
 * used to show a glyph for every entry, even for a wall of JPEGs.
 *
 * The fallback is the item's own bytes. An `<img>` pointing at the media URL
 * costs the browser one scaled decode and needs no CORS — displaying an image
 * is not reading it — and a video's first frame comes from
 * `<video preload="metadata">` without fetching the whole file, which is the
 * same trick the file grid already uses. A tile that fails to load falls back to
 * the glyph rather than showing a broken image, which is why this is a component
 * with its own state and not a bare `<img>`.
 */
function CardThumb({ item }: { item: BrowseItem }) {
  const [failed, setFailed] = useState(false);
  const canPreviewImage = item.kind === 'image';
  const canPreviewVideo = item.kind === 'video';
  const src = item.thumbUrl ?? (canPreviewImage ? item.url : null);

  if (failed || (!src && !canPreviewVideo)) {
    return (
      <span className="bcard__glyph">
        <KindGlyph kind={item.kind} />
      </span>
    );
  }
  if (canPreviewVideo && !item.thumbUrl) {
    return (
      <video
        className="bcard__poster"
        src={item.url}
        preload="metadata"
        muted
        playsInline
        onError={() => setFailed(true)}
      />
    );
  }
  return (
    // Decorative: the title is already announced by the button.
    <img src={src!} alt="" loading="lazy" onError={() => setFailed(true)} />
  );
}

/** Waits for the user to stop typing before searching. */
function useDebounced<T>(value: T, ms: number): T {
  const [settled, setSettled] = useState(value);
  useEffect(() => {
    const t = window.setTimeout(() => setSettled(value), ms);
    return () => window.clearTimeout(t);
  }, [value, ms]);
  return settled;
}

export function BrowsePage(props: BrowsePageProps) {
  const {
    term,
    onTermChange,
    kind,
    onKindChange,
    items,
    total,
    hasMore,
    loading,
    error,
    queueingId = null,
    onPreview,
    onDownload,
    onSaveToDevice,
    onNextPage,
    onPrevPage,
    page,
    canSaveToDevice,
    mode,
    onModeChange,
    sourceUrl,
    onSourceUrlChange,
    onReadUrl,
    sourceTitle,
    filter,
    onFilterChange,
    canRender,
    rendered,
    rendering,
    renderFailure,
    renderedTitle,
    renderMs,
    renderedFromCache,
    renderedSanitized,
    onRender,
    onClearRender,
    canGoLive,
    live,
    canStreamVideo,
    video,
    liveMedia,
    onDownloadLive,
  } = props;

  /**
   * Which page view is showing: the driveable live session, a rendered
   * picture, or raw video.
   *
   * Video is the best of the three when Vyzor can do it — no JPEG encode
   * and no per-frame acknowledgement — so it is preferred when available.
   */
  const [pageView, setPageView] = useState<'live' | 'picture' | 'video'>('live');

  const debounced = useDebounced(term, 350);
  const searching = term.trim() !== debounced.trim();
  const byUrl = mode === 'url';
  const byPage = mode === 'page';
  /** A live session is the default page view when Vyzor can host one. */
  const liveActive = byPage && canGoLive;
  /**
   * Whether the video stage is the one showing.
   *
   * The first time capabilities report video, the view moves to it: it is
   * strictly better than the JPEG stream, so choosing it is not a preference
   * worth asking about. Once the user picks something else, that wins.
   */
  const [videoPreferred, setVideoPreferred] = useState(false);
  useEffect(() => {
    if (canStreamVideo && !videoPreferred) {
      setVideoPreferred(true);
      setPageView('video');
    }
  }, [canStreamVideo, videoPreferred]);
  const videoActive = byPage && canStreamVideo && pageView === 'video';
  const liveOpen = live.status === 'open' || live.status === 'connecting';

  const subtitle = useMemo(() => {
    if (byPage) {
      if (!canRender && !canGoLive) return 'This Vyzor cannot show pages';
      if (liveActive) {
        if (!sourceUrl.trim()) return 'Use the page live, or draw a picture of it';
        if (liveOpen) return 'Live — click into the page to use it';
        if (live.status === 'connecting') return 'Opening the page…';
        if (live.status === 'closed' && live.failure) return failureShort(live.failure);
      }
      if (!sourceUrl.trim()) return 'View a page as a real browser would — Vyzor renders it';
      if (rendering) return 'Rendering that page…';
      if (renderFailure) return renderFailureLabel(renderFailure);
      if (rendered) {
        const label = renderedTitle ? `“${renderedTitle}”` : 'that page';
        // A cached answer is worth saying so: it explains why this one was
        // instant when the first one took a second or two.
        if (renderedFromCache) return `${label} — from the cache`;
        return renderMs ? `${label} — rendered in ${(renderMs / 1000).toFixed(1)}s` : label;
      }
      return sourceUrl.trim();
    }
    if (byUrl) {
      if (!sourceUrl.trim()) return 'Read a page or directory you choose — its media is listed, nothing is fetched';
      if (loading) return 'Reading that page…';
      if (error) return 'That page could not be read';
      if (items.length === 0) return 'No media found at that address';
      const where = sourceTitle ? `“${sourceTitle}”` : 'that page';
      return `${items.length} file${items.length === 1 ? '' : 's'} at ${where}`;
    }
    if (term.trim().length < 2) return 'Search open media — images, audio, video and documents';
    if (loading) return 'Searching…';
    if (error) return 'The search failed';
    if (items.length === 0) return `No results for “${term.trim()}”`;
    const totalLabel = total > 0 ? ` of about ${total.toLocaleString()}` : '';
    return `${items.length}${totalLabel} result${items.length === 1 ? '' : 's'} for “${term.trim()}”`;
  }, [
    byPage,
    canRender,
    canGoLive,
    liveActive,
    liveOpen,
    live.status,
    live.failure,
    rendering,
    renderFailure,
    rendered,
    renderedTitle,
    renderedFromCache,
    renderMs,
    byUrl,
    sourceUrl,
    sourceTitle,
    term,
    loading,
    error,
    items.length,
    total,
  ]);

  const empty = byUrl
    ? sourceUrl.trim()
      ? { title: 'Nothing to show', body: 'That page had no media this app can fetch, or it could not be read.' }
      : { title: 'Paste an address to read', body: 'A directory listing, a gallery, or any page with media links — Vyzor reads it for you.' }
    : term.trim().length < 2
      ? { title: 'Search for something to keep', body: 'Try “cat”, “birdsong”, “timelapse” — or any subject you need media for.' }
      : searching
        ? { title: 'Typing…', body: 'Results will appear as soon as you pause.' }
        : { title: 'Nothing matched', body: 'Try a broader term, or a different kind of file.' };

  return (
    <>
      <Card
        title={byPage ? 'View a page' : byUrl ? 'Browse a URL' : 'Browse open media'}
        subtitle={subtitle}
      >
        <div className="browse__modes" role="group" aria-label="Where to browse">
          <button
            type="button"
            className={mode === 'search' ? 'browse__mode is-active' : 'browse__mode'}
            aria-pressed={mode === 'search'}
            onClick={() => onModeChange('search')}
          >
            Search open media
          </button>
          <button
            type="button"
            className={byUrl ? 'browse__mode is-active' : 'browse__mode'}
            aria-pressed={byUrl}
            onClick={() => onModeChange('url')}
          >
            Read a URL
          </button>
          <button
            type="button"
            className={byPage ? 'browse__mode is-active' : 'browse__mode'}
            aria-pressed={byPage}
            onClick={() => onModeChange('page')}
          >
            View a page
          </button>
        </div>

        {byUrl || byPage ? (
          <div className="browse__controls">
            <div className="browse__search">
              <Field
                label="Address"
                value={sourceUrl}
                onChange={onSourceUrlChange}
                placeholder="https://example.com/"
                hint={
                  byPage
                    ? 'The page is fetched by Vyzor and shown here. A site that forbids being embedded cannot be shown, and says so.'
                    : 'A page or directory listing. Vyzor reads it, so nothing is fetched by this browser and no CORS rule applies.'
                }
              />
            </div>
            <Button
              variant="primary"
              onClick={() => {
                if (!byPage) {
                  onReadUrl();
                } else if (videoActive) {
                  // The video session is opened by the stage when it mounts, so
                  // there is nothing to start here; showing the stage is enough.
                  setPageView('video');
                } else if (liveActive && pageView === 'live') {
                  // A live session is opened at a starting size; the stage then
                  // resizes it to fit.
                  live.onOpen(sourceUrl.trim(), 1024, 768);
                } else {
                  onRender('image');
                }
              }}
              loading={byPage ? (liveActive && pageView === 'live' ? live.status === 'connecting' : rendering) : loading}
              disabled={
                !/^https?:\/\/\S+$/i.test(sourceUrl.trim()) ||
                (byPage && !canRender && !canGoLive)
              }
            >
              <IconSearch />{' '}
              {byPage ? (liveActive && pageView === 'live' ? 'Open live' : 'Render') : 'Read'}
            </Button>
          </div>
        ) : (
          <div className="browse__controls">
            <div className="browse__search">
              <Field
                label="Search"
                value={term}
                onChange={onTermChange}
                placeholder="What are you looking for?"
                hint="Wikimedia Commons — public domain and freely licensed media. Nothing is downloaded until you ask."
              />
            </div>
            <Button
              variant="primary"
              onClick={() => onTermChange(term)}
              loading={loading}
              disabled={term.trim().length < 2}
            >
              <IconSearch /> Search
            </Button>
          </div>
        )}

        {/* In URL mode the choice is how much of the page to list: only what
            this app can play, or everything the page references, the way a
            browser shows it. */}
        {byUrl && (
          <div className="browse__modes" role="group" aria-label="What to list">
            <button
              type="button"
              className={filter === 'media' ? 'browse__mode is-active' : 'browse__mode'}
              aria-pressed={filter === 'media'}
              onClick={() => onFilterChange('media')}
            >
              Media only
            </button>
            <button
              type="button"
              className={filter === 'all' ? 'browse__mode is-active' : 'browse__mode'}
              aria-pressed={filter === 'all'}
              onClick={() => onFilterChange('all')}
            >
              Everything
            </button>
          </div>
        )}

        {/* The kind filter is a search-source idea; a listing is already a flat
            list of files, so it is not offered there. Rendered conditionally
            rather than with `hidden`, because a `display: flex` class rule wins
            over the `[hidden]` attribute and it would still show. */}
        {mode === 'search' && (
        <div className="browse__kinds" role="group" aria-label="Filter by kind">
          {KIND_FILTERS.map((k) => (
            <button
              key={k.id}
              type="button"
              className={k.id === kind ? 'browse__kind is-active' : 'browse__kind'}
              aria-pressed={k.id === kind}
              onClick={() => onKindChange(k.id)}
            >
              {k.label}
            </button>
          ))}
        </div>
        )}
      </Card>

      {/* The page view. A live session is the default when Vyzor can host
          one: it is the page itself, usable, rather than a picture of it. The
          renderer remains for a host that only has that, and as an explicit
          "Picture" mode where both exist. */}
      {byPage && (
        <>
          {!canRender && !canGoLive ? (
            <Card>
              <EmptyState
                title="This Vyzor cannot show pages"
                description="Showing a page needs a browser on Vyzor host — either to drive it live or to draw it once. Vyzor reports this in its capabilities, so this control is only offered where it works."
              />
            </Card>
          ) : (
            <>
              {/* When both are possible, let the user choose; when only one is,
                  it is used without ceremony. */}
              {(canGoLive || canStreamVideo) && canRender && (
                <div className="browse__modes" role="group" aria-label="How to show the page">
                  {canGoLive && (
                    <button
                      type="button"
                      className={pageView === 'live' ? 'browse__mode is-active' : 'browse__mode'}
                      aria-pressed={pageView === 'live'}
                      onClick={() => setPageView('live')}
                    >
                      Live page
                    </button>
                  )}
                  {canStreamVideo && (
                    <button
                      type="button"
                      className={pageView === 'video' ? 'browse__mode is-active' : 'browse__mode'}
                      aria-pressed={pageView === 'video'}
                      onClick={() => setPageView('video')}
                    >
                      Video
                    </button>
                  )}
                  <button
                    type="button"
                    className={pageView === 'picture' ? 'browse__mode is-active' : 'browse__mode'}
                    aria-pressed={pageView === 'picture'}
                    onClick={() => setPageView('picture')}
                  >
                    Picture
                  </button>
                </div>
              )}

              {videoActive ? (
                <InteractiveView
                  url={sourceUrl.trim()}
                  live={live}
                  video={video}
                  useVideo
                  media={liveMedia}
                  onDownload={onDownloadLive}
                />
              ) : canGoLive && (pageView === 'live' || !canRender) ? (
                <InteractiveView
                  url={sourceUrl.trim()}
                  live={live}
                  media={liveMedia}
                  onDownload={onDownloadLive}
                />
              ) : rendering ? (
                <Card>
                  <div className="browse__rendering">
                    <Spinner size="lg" />
                    <p className="browse__rendering-note">
                      Rendering with a real browser. This takes a second or two, and a page that never
                      finishes is stopped rather than left to run.
                    </p>
                  </div>
                </Card>
              ) : renderFailure ? (
                <Card>
                  <EmptyState
                    title={renderFailureLabel(renderFailure)}
                    description={renderFailureDetail(renderFailure)}
                  />
                  <div className="browse__page-actions">
                    <Button onClick={() => onRender('image')}>Try again</Button>
                    <a className="btn btn--sm" href={sourceUrl} target="_blank" rel="noreferrer">
                      Open in a new tab
                    </a>
                  </div>
                </Card>
              ) : rendered ? (
                <div className="browse__page">
                  <div className="browse__page-bar">
                    <span className="browse__page-url" title={sourceUrl}>
                      {sourceUrl}
                    </span>
                    {/* Two ways to look at the same render: a picture, or the DOM
                        after its scripts ran. The picture is the default because a
                        PNG cannot be sanitised wrongly. */}
                    <div className="browse__modes" role="group" aria-label="How to show the page">
                      <button
                        type="button"
                        className={rendered.mode === 'image' ? 'browse__mode is-active' : 'browse__mode'}
                        aria-pressed={rendered.mode === 'image'}
                        onClick={() => onRender('image')}
                      >
                        Picture
                      </button>
                      <button
                        type="button"
                        className={rendered.mode === 'html' ? 'browse__mode is-active' : 'browse__mode'}
                        aria-pressed={rendered.mode === 'html'}
                        onClick={() => onRender('html')}
                      >
                        Page text
                      </button>
                    </div>
                    <Button size="sm" onClick={onClearRender}>
                      Clear
                    </Button>
                    <a className="btn btn--sm" href={sourceUrl} target="_blank" rel="noreferrer">
                      Open in a new tab
                    </a>
                  </div>
                  {rendered.mode === 'image' ? (
                    <img
                      className="browse__shot"
                      src={rendered.url}
                      alt={`Rendered view of ${sourceUrl}`}
                    />
                  ) : (
                    <>
                      {/* The page text is a *cleaned* copy, not the live site: its
                          scripts, handlers and frames are gone. Saying so matters,
                          because it looks like the page and is not. */}
                      {renderedSanitized && (
                        <Note className="browse__sanitized-note">
                          A cleaned copy of the page. Scripts, forms and embedded frames are
                          removed, and links open here rather than navigating away.
                        </Note>
                      )}
                      <iframe
                        className="browse__frame"
                        src={rendered.url}
                        title={`Rendered text of ${sourceUrl}`}
                        // The strongest sandbox: an opaque origin, no scripts, no
                        // forms, no popups, no top-level navigation. This — not the
                        // Vyzor's sanitiser — is the boundary that makes rendering
                        // someone else's markup safe.
                        sandbox=""
                        referrerPolicy="no-referrer"
                      />
                    </>
                  )}
                </div>
              ) : (
                <Card>
                  <EmptyState
                    title="Render a page"
                    description="Vyzor runs a real browser and draws the page, so a site that forbids being embedded — or that builds itself with JavaScript — still shows. Press Render."
                  />
                </Card>
              )}
            </>
          )}
        </>
      )}

      {error !== null && <Note className="note note--warn">{error}</Note>}

      {/* A page view shows the page; it has no result cards. */}
      {byPage ? null : loading && items.length === 0 ? (
        <div className="browse__loading">
          <Spinner size="lg" />
        </div>
      ) : items.length === 0 ? (
        <Card>
          <EmptyState title={empty.title} description={empty.body} />
        </Card>
      ) : (
        <>
          <ul className="browse__grid">
            {items.map((item) => (
              <li key={item.id} className="bcard">
                <button
                  type="button"
                  className="bcard__preview"
                  onClick={() => onPreview(item)}
                  aria-label={`Preview ${item.title}`}
                >
                  <span className="bcard__thumb">
                    <CardThumb item={item} />
                  </span>
                  <span className="bcard__body">
                    <span className="bcard__title" title={item.title}>
                      {item.title}
                    </span>
                    <span className="bcard__meta">
                      {browseKindLabel(item.kind)}
                      {item.bytes >= 0 ? ` · ${formatBytes(item.bytes)}` : ''}
                      {item.width && item.height ? ` · ${item.width}×${item.height}` : ''}
                    </span>
                    <span className="bcard__licence" title={item.licence}>
                      {item.licence}
                    </span>
                  </span>
                </button>

                <div className="bcard__actions">
                  <Button
                    size="sm"
                    variant="primary"
                    loading={queueingId === item.id}
                    onClick={() => onDownload(item)}
                  >
                    Download
                  </Button>
                  {canSaveToDevice && (
                    <Button size="sm" onClick={() => onSaveToDevice(item)} title="Save straight to this device">
                      <IconFolder /> Save
                    </Button>
                  )}
                </div>
              </li>
            ))}
          </ul>

          {/* A listing arrives whole, so there is nothing to page through. */}
          {!byUrl && (
            <div className="browse__pager">
              <Button size="sm" onClick={onPrevPage} disabled={page <= 1 || loading}>
                <IconRefresh /> Previous
              </Button>
              <span className="browse__page">Page {page}</span>
              <Button size="sm" onClick={onNextPage} disabled={!hasMore || loading}>
                Next
              </Button>
            </div>
          )}
        </>
      )}
    </>
  );
}
