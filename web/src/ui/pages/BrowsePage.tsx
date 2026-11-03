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
import { browseKindLabel, type BrowseItem, type BrowseKind } from '../../hooks';

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
  /** True while a download request is being accepted by the engine. */
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
}

export type BrowseMode = 'search' | 'url';

const KIND_FILTERS: ReadonlyArray<{ id: BrowseKind | 'all'; label: string }> = [
  { id: 'all', label: 'Everything' },
  { id: 'image', label: 'Images' },
  { id: 'audio', label: 'Audio' },
  { id: 'video', label: 'Video' },
  { id: 'document', label: 'Documents' },
];

/** A glyph per kind, so a card is scannable when it has no thumbnail. */
function KindGlyph({ kind }: { kind: BrowseKind }) {
  if (kind === 'image') return <IconImage />;
  if (kind === 'audio') return <IconMusic />;
  return <IconFile />;
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
  } = props;

  const debounced = useDebounced(term, 350);
  const searching = term.trim() !== debounced.trim();
  const byUrl = mode === 'url';

  const subtitle = useMemo(() => {
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
  }, [byUrl, sourceUrl, sourceTitle, term, loading, error, items.length, total]);

  const empty = byUrl
    ? sourceUrl.trim()
      ? { title: 'Nothing to show', body: 'That page had no media this app can fetch, or it could not be read.' }
      : { title: 'Paste an address to read', body: 'A directory listing, a gallery, or any page with media links — the engine reads it for you.' }
    : term.trim().length < 2
      ? { title: 'Search for something to keep', body: 'Try “cat”, “birdsong”, “timelapse” — or any subject you need media for.' }
      : searching
        ? { title: 'Typing…', body: 'Results will appear as soon as you pause.' }
        : { title: 'Nothing matched', body: 'Try a broader term, or a different kind of file.' };

  return (
    <>
      <Card title={byUrl ? 'Browse a URL' : 'Browse open media'} subtitle={subtitle}>
        <div className="browse__modes" role="group" aria-label="Where to browse">
          <button
            type="button"
            className={!byUrl ? 'browse__mode is-active' : 'browse__mode'}
            aria-pressed={!byUrl}
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
        </div>

        {byUrl ? (
          <div className="browse__controls">
            <div className="browse__search">
              <Field
                label="Address"
                value={sourceUrl}
                onChange={onSourceUrlChange}
                placeholder="https://example.com/media/"
                hint="A page or directory listing. The engine reads it, so nothing is fetched by this browser and no CORS rule applies."
              />
            </div>
            <Button
              variant="primary"
              onClick={onReadUrl}
              loading={loading}
              disabled={!/^https?:\/\/\S+$/i.test(sourceUrl.trim())}
            >
              <IconSearch /> Read
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

        {/* The kind filter is a search-source idea; a listing is already a flat
            list of files, so it is not offered there. Rendered conditionally
            rather than with `hidden`, because a `display: flex` class rule wins
            over the `[hidden]` attribute and it would still show. */}
        {!byUrl && (
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

      {error !== null && <Note className="note note--warn">{error}</Note>}

      {loading && items.length === 0 ? (
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
                    {item.thumbUrl ? (
                      // Decorative: the title is already announced by the button.
                      <img src={item.thumbUrl} alt="" loading="lazy" />
                    ) : (
                      <span className="bcard__glyph">
                        <KindGlyph kind={item.kind} />
                      </span>
                    )}
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
