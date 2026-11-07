/**
 * LogsPage — what the server has been doing, without a terminal.
 *
 * Vyzor logs to stderr, which a console cannot see, so a user watching a crawl
 * or a download fail had no way to know why: "no file matched", "the host
 * refused us" and "robots.txt disallows this" all looked the same from the UI.
 * The server keeps a bounded in-memory ring of recent lines and serves them
 * incrementally; this page shows them.
 *
 * It is deliberately plain: a filterable, level-coloured list. The value is in
 * seeing the *reason*, not in a dashboard.
 */

import { useMemo, useState } from 'react';
import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { Field } from '../primitives/Field';
import { Select } from '../primitives/Select';
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

type LevelFilter = 'all' | 'error' | 'info' | 'debug';

function clockTime(ms: number): string {
  const d = new Date(ms);
  return d.toLocaleTimeString(undefined, { hour12: false });
}

export function LogsPage({ lines, enabled, loaded, error, onClear }: LogsPageProps) {
  const [filter, setFilter] = useState('');
  const [level, setLevel] = useState<LevelFilter>('all');

  const shown = useMemo(() => {
    const q = filter.trim().toLowerCase();
    return lines.filter((l) => {
      if (level !== 'all' && l.level !== level) return false;
      if (q && !l.text.toLowerCase().includes(q)) return false;
      return true;
    });
  }, [lines, filter, level]);

  return (
    <Card title="Server log">
      <div className="form">
        <div className="field">
          <Field
            label="Filter"
            value={filter}
            onChange={setFilter}
            placeholder="crawl, robots, 403…"
            hint="Matches anywhere in the line."
          />
        </div>
        <div className="field">
          <Select
            label="Level"
            value={level}
            onChange={(v) => setLevel(v as LevelFilter)}
            options={[
              { value: 'all', label: 'All levels' },
              { value: 'error', label: 'Errors only' },
              { value: 'info', label: 'Info' },
              { value: 'debug', label: 'Debug' },
            ]}
          />
        </div>
      </div>

      <div className="form-actions">
        <Button onClick={onClear} disabled={lines.length === 0}>
          Clear
        </Button>
        <Note className="note">
          {shown.length} of {lines.length} line{lines.length === 1 ? '' : 's'}
        </Note>
      </div>

      {error && (
        <Note className="note">Could not reach Vyzor for new lines: {error}</Note>
      )}
      {!enabled && (
        <Note className="note">
          The server is not capturing log lines. Restart it to enable the log view.
        </Note>
      )}

      <div className="log-view" role="log" aria-live="polite">
        {shown.length === 0 ? (
          <p className="log-view__empty">
            {loaded ? 'No lines match.' : 'Reading the log…'}
          </p>
        ) : (
          shown.map((l) => (
            <div key={l.seq} className={`log-line log-line--${l.level}`}>
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
