/**
 * EgressPanel — the cookies and proxy Vyzor uses to reach a site that refuses
 * an anonymous request from this host.
 *
 * Two different kinds of thing, and the UI says which is which:
 *
 *   cookies   a session credential for an account the operator signed in with.
 *             Vyzor holds it in memory and never returns the value, so this
 *             panel shows only which cookie names are loaded.
 *   proxy     not a secret; the address is shown in full.
 *
 * The cookie box is a paste target: a browser's cookie export is Netscape
 * format, which is what yt-dlp takes and what Vyzor converts for ffmpeg. Nothing
 * is sent until Save.
 */

import { useState } from 'react';
import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { Field } from '../primitives/Field';
import { Note } from '../primitives/Note';

export interface EgressValue {
  /** Whether Vyzor currently holds cookies. The value is never returned. */
  cookieConfigured: boolean;
  /** The cookie names Vyzor holds, for confirming the right export loaded. */
  cookieNames: readonly string[];
  /** The proxy URL, or '' when direct. Not secret. */
  proxy: string;
}

export interface EgressPanelProps {
  /** What Vyzor currently holds, or null while it is being read. */
  value: EgressValue | null;
  saving?: boolean;
  /**
   * Save cookies and/or proxy. A field set to null is left unchanged; an empty
   * string clears it.
   */
  onSave: (next: { cookies: string | null; proxy: string | null }) => void;
  /** Clear the stored cookies. */
  onClearCookies: () => void;
}

export function EgressPanel({ value, saving, onSave, onClearCookies }: EgressPanelProps) {
  /* The cookie value is never returned, so the box starts empty and only holds
   * what the operator pastes this session. The proxy is not secret, so it is
   * seeded from the server and edited in place. */
  const [cookies, setCookies] = useState('');
  const [proxy, setProxy] = useState<string | null>(null);
  const effectiveProxy = proxy ?? value?.proxy ?? '';

  const names = value?.cookieNames ?? [];
  const status = value?.cookieConfigured
    ? `Vyzor is holding cookies${names.length ? `: ${names.join(', ')}` : ''}. The values are never returned by the API.`
    : 'No cookies are set.';

  return (
    <div className="grid-2 grid-2--even">
      <Card title="Cookies">
        <div className="form">
          <div className="field field--full">
            <label htmlFor="egress-cookies">Netscape cookie export</label>
            <textarea
              id="egress-cookies"
              rows={8}
              spellCheck={false}
              placeholder={'# Netscape HTTP Cookie File\n.example.com\tTRUE\t/\tTRUE\t0\tsessionid\t…'}
              value={cookies}
              onChange={(e) => setCookies(e.target.value)}
            />
            <span className="field__hint">
              Paste the contents of a browser cookie export (Netscape format). Used for
              sites that refuse an anonymous request.
            </span>
          </div>
        </div>
        <div className="form-actions">
          <Button
            variant="primary"
            loading={saving}
            disabled={cookies.trim() === ''}
            onClick={() => onSave({ cookies, proxy: null })}
          >
            Save cookies
          </Button>
          {value?.cookieConfigured && (
            <Button variant="danger" onClick={onClearCookies}>
              Clear cookies
            </Button>
          )}
        </div>
        <Note className="note">{status}</Note>
      </Card>

      <Card title="Proxy">
        <div className="form">
          <div className="field field--full">
            <Field
              label="Proxy URL"
              value={effectiveProxy}
              onChange={setProxy}
              placeholder="http://host:port or socks5://host:port"
              hint="Every outbound request — page fetches, the extractor, the transfer and the browser — goes through it."
            />
          </div>
        </div>
        <div className="form-actions">
          <Button
            variant="primary"
            loading={saving}
            onClick={() => onSave({ cookies: null, proxy: effectiveProxy })}
          >
            Save proxy
          </Button>
        </div>
        <Note className="note">
          A site may refuse this host&apos;s address; a proxy is the way around it. Leave
          empty for a direct connection.
        </Note>
      </Card>
    </div>
  );
}
