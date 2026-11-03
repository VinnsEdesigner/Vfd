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
}

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
  } = props;

  const debounced = useDebounced(term, 350);
  const searching = term.trim() !== debounced.trim();

  const subtitle = useMemo(() => {
    if (term.trim().length < 2) return 'Search open media — images, audio, video and documents';
    if (loading) return 'Searching…';
    if (error) return 'The search failed';
    if (items.length === 0) return `No results for “${term.trim()}”`;
    const totalLabel = total > 0 ? ` of about ${total.toLocaleString()}` : '';
    return `${items.length}${totalLabel} result${items.length === 1 ? '' : 's'} for “${term.trim()}”`;
  }, [term, loading, error, items.length, total]);

  const empty = term.trim().length < 2
    ? { title: 'Search for something to keep', body: 'Try “cat”, “birdsong”, “timelapse” — or any subject you need media for.' }
    : searching
      ? { title: 'Typing…', body: 'Results will appear as soon as you pause.' }
      : { title: 'Nothing matched', body: 'Try a broader term, or a different kind of file.' };

  return (
    <>
      <Card title="Browse open media" subtitle={subtitle}>
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

          <div className="browse__pager">
            <Button size="sm" onClick={onPrevPage} disabled={page <= 1 || loading}>
              <IconRefresh /> Previous
            </Button>
            <span className="browse__page">Page {page}</span>
            <Button size="sm" onClick={onNextPage} disabled={!hasMore || loading}>
              Next
            </Button>
          </div>
        </>
      )}
    </>
  );
}
