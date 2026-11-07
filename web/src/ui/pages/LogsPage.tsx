/**
 * LogsPage — what the server has been doing, without a terminal.
 *
 * Vyzor logs to stderr, which a console cannot see, so a user watching a crawl
 * or a download fail had no way to know why: "no file matched", "the host
 * refused us" and "robots.txt disallows this" all looked the same from the UI.
 * The server keeps a bounded in-memory ring of recent lines and serves them
 * incrementally; this page shows them.
 *
 * The design is a console, not a table. Three decisions carry it:
 *
 *   - The level filter is a segmented control with a live count on each, so the
 *     answer to "did anything fail?" is visible without switching to a filter —
 *     a page of INFO chatter with one red line in it still says "1 error".
 *   - Each row carries its level three ways — a coloured rail, a coloured level
 *     chip, and the text colour — because colour alone is not a signal a
 *     colour-blind reader or a greyscale print can use.
 *   - Only ERROR fills its row. If every level had a tint the block would be a
 *     quilt and none of them would stand out.
 *
 * Newest lines are appended, so the view follows the tail; a reader who has
 * scrolled up is left where they are rather than yanked to the bottom.
 */

import { useEffect, useMemo, useRef, useState } from 'react';
import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { Field } from '../primitives/Field';
import { Note } from '../primitives/Note';
import type { LogLine } from '../../hooks';

export interface LogsPageProps {
  lines: readonly LogLine[];
  /** Whether the server is capturing lines at all. */
  enabled: boolean;
  loaded: boolean;
  error: string | null;
  onClear: () => void;
}

/** A level filter, or `all`. */
type Level = LogLine['level'] | 'all';

/** The order the levels are offered in: most severe first, because that is the
 *  order a reader looks for them. */
const LEVELS: ReadonlyArray<{ id: Level; label: string }> = [
  { id: 'all', label: 'All' },
  { id: 'error', label: 'Error' },
  { id: 'warn', label: 'Warn' },
  { id: 'info', label: 'Info' },
  { id: 'debug', label: 'Debug' },
];

function clockTime(ms: number): string {
  return new Date(ms).toLocaleTimeString(undefined, { hour12: false });
}

export function LogsPage({ lines, enabled, loaded, error, onClear }: LogsPageProps) {
  const [filter, setFilter] = useState('');
  const [level, setLevel] = useState<Level>('all');
  const [follow, setFollow] = useState(true);
  const viewRef = useRef<HTMLDivElement>(null);

  /* Counts per level, for the filter's badges. Computed over *all* lines, not
   * the filtered set, so the count does not change as the filter does. */
  const counts = useMemo(() => {
    const c = { error: 0, warn: 0, info: 0, debug: 0 } as Record<LogLine['level'], number>;
    for (const l of lines) c[l.level] += 1;
    return c;
  }, [lines]);

  const shown = useMemo(() => {
    const q = filter.trim().toLowerCase();
    return lines.filter((l) => {
      if (level !== 'all' && l.level !== level) return false;
      if (q && !l.text.toLowerCase().includes(q)) return false;
      return true;
    });
  }, [lines, filter, level]);

  /* Follow the tail, unless the reader has scrolled away from it. Re-pinning
   * the scroll on every poll would fight someone reading back through the log. */
  useEffect(() => {
    if (!follow) return;
    const el = viewRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [shown.length, follow]);

  const onScroll = () => {
    const el = viewRef.current;
    if (!el) return;
    /* "At the bottom" with a small tolerance, so a fractional pixel does not
     * read as a scroll-away and stop the follow. */
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
    setFollow(atBottom);
  };

  const failing = counts.error + counts.warn;

  return (
    <Card title="Server log">
      <div className="log-toolbar">
        <div className="log-toolbar__grow">
          <Field
            label="Search"
            value={filter}
            onChange={setFilter}
            placeholder="crawl, robots, 403…"
            hint="Matches anywhere in the line."
          />
        </div>
        <div className="log-levels" role="group" aria-label="Level">
          {LEVELS.map((l) => {
            const n = l.id === 'all' ? lines.length : counts[l.id];
            return (
              <button
                key={l.id}
                type="button"
                className={l.id === level ? 'log-levels__btn is-active' : 'log-levels__btn'}
                aria-pressed={l.id === level}
                onClick={() => setLevel(l.id)}
              >
                <span className={`log-levels__dot log-levels__dot--${l.id}`} />
                {l.label}
                <span className="log-levels__n">{n}</span>
              </button>
            );
          })}
        </div>
        <div className="log-toolbar__spacer" />
        <span className="log-toolbar__count">
          {shown.length} of {lines.length}
          {failing > 0 ? ` · ${counts.error} error${counts.error === 1 ? '' : 's'}` : ''}
        </span>
        {!follow && (
          <Button
            onClick={() => {
              setFollow(true);
              const el = viewRef.current;
              if (el) el.scrollTop = el.scrollHeight;
            }}
          >
            Follow
          </Button>
        )}
        <Button onClick={onClear} disabled={lines.length === 0}>
          Clear
        </Button>
      </div>

      {error && <Note className="note">Could not reach Vyzor for new lines: {error}</Note>}
      {!enabled && (
        <Note className="note">
          The server is not capturing log lines. Restart it to enable the log view.
        </Note>
      )}

      <div className="log-view" role="log" aria-live="polite" ref={viewRef} onScroll={onScroll}>
        {shown.length === 0 ? (
          <p className="log-view__empty">
            {!loaded
              ? 'Reading the log…'
              : lines.length === 0
                ? 'No lines yet.'
                : 'No lines match.'}
          </p>
        ) : (
          shown.map((l) => (
            <div key={l.seq} className={`log-line log-line--${l.level}`}>
              <span className="log-line__rail" aria-hidden="true" />
              <span className="log-line__time">{clockTime(l.timestamp)}</span>
              <span className="log-line__level">{l.level}</span>
              <span className="log-line__text">{l.text}</span>
            </div>
          ))
        )}
      </div>
    </Card>
  );
}
