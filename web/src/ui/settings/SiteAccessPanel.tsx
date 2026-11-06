/**
 * SiteAccessPanel — everything that decides whether a site lets Vyzor in.
 *
 * Some sites refuse an anonymous request from this host: they want a sign-in, or
 * they do not like the address or the client. This is where an operator says how
 * to get past that — the cookies of an account they signed in with, a proxy, a
 * browser to imitate, how hard to retry, a captcha solver, and the shared
 * frontier several Vyzors can crawl through together.
 *
 * Secrets are treated as secrets: a cookie or key Vyzor holds is never returned
 * by the API, so a field starts empty and the panel reports only whether a value
 * is set. The cookie box is a paste target — a browser's cookie export is
 * Netscape format, which is what yt-dlp takes and what Vyzor converts for
 * ffmpeg. Nothing is sent until Save.
 */

import { useState } from 'react';
import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { Field, SecretField } from '../primitives/Field';
import { Note } from '../primitives/Note';

export interface SiteAccessValue {
  /** Whether Vyzor currently holds cookies. The value is never returned. */
  cookieConfigured: boolean;
  /** The cookie names Vyzor holds, for confirming the right export loaded. */
  cookieNames: readonly string[];
  /** The proxy URL, or '' when direct. Not secret. */
  proxy: string;
  /** A yt-dlp / curl_cffi impersonation target, e.g. "chrome". */
  impersonate: string;
  /** How many fresh-identity retries a refused download gets. */
  identityRetries: number;
  /** The captcha provider name, or '' for the built-in stub. */
  captchaProvider: string;
  /** Whether a captcha API key is set. The key itself is never returned. */
  captchaKeySet: boolean;
  /** The shared crawl frontier's remote database URL, or ''. */
  frontierDbUrl: string;
  /** A local SQLite file for the crawl frontier, or ''. */
  frontierDbPath: string;
  /** Whether a frontier token is set. The token itself is never returned. */
  frontierTokenSet: boolean;
}

/** A partial update: a field set to null is left unchanged by the server. */
export interface SiteAccessSave {
  cookies?: string | null;
  proxy?: string | null;
  impersonate?: string | null;
  identityRetries?: number | null;
  captchaProvider?: string | null;
  captchaApiKey?: string | null;
  frontierDbUrl?: string | null;
  frontierDbPath?: string | null;
  frontierToken?: string | null;
}

export interface SiteAccessPanelProps {
  /** What Vyzor currently holds, or null while it is being read. */
  value: SiteAccessValue | null;
  saving?: boolean;
  onSave: (next: SiteAccessSave) => void;
  /** Clear the stored cookies. */
  onClearCookies: () => void;
}

export function SiteAccessPanel({ value, saving, onSave, onClearCookies }: SiteAccessPanelProps) {
  /* The cookie value is never returned, so the box starts empty and only holds
   * what the operator pastes this session. The proxy is not secret, so it is
   * seeded from the server and edited in place. */
  const [cookies, setCookies] = useState('');
  const [proxy, setProxy] = useState<string | null>(null);
  const [impersonate, setImpersonate] = useState<string | null>(null);
  const [retries, setRetries] = useState<string | null>(null);
  const [captchaProvider, setCaptchaProvider] = useState<string | null>(null);
  const [captchaKey, setCaptchaKey] = useState('');
  const [frontierUrl, setFrontierUrl] = useState<string | null>(null);
  const [frontierPath, setFrontierPath] = useState<string | null>(null);
  const [frontierToken, setFrontierToken] = useState('');
  const effectiveProxy = proxy ?? value?.proxy ?? '';
  const effectiveImpersonate = impersonate ?? value?.impersonate ?? '';
  const effectiveRetries = retries ?? String(value?.identityRetries ?? 0);
  const effectiveCaptchaProvider = captchaProvider ?? value?.captchaProvider ?? '';
  const effectiveFrontierUrl = frontierUrl ?? value?.frontierDbUrl ?? '';
  const effectiveFrontierPath = frontierPath ?? value?.frontierDbPath ?? '';

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

      <Card title="Client identity">
        <div className="form">
          <div className="field field--full">
            <Field
              label="Impersonate"
              value={effectiveImpersonate}
              onChange={setImpersonate}
              placeholder="chrome, chrome131, firefox…"
              hint="A site that fingerprints TLS sees libcurl; naming a real browser makes the extractor's request look like one."
            />
          </div>
          <div className="field field--full">
            <Field
              label="Fresh-identity retries"
              type="number"
              min={0}
              max={10}
              value={effectiveRetries}
              onChange={setRetries}
              hint="A refused download is retried with a different browser User-Agent (and proxy, when several are set)."
            />
          </div>
        </div>
        <div className="form-actions">
          <Button
            variant="primary"
            loading={saving}
            onClick={() =>
              onSave({
                impersonate: effectiveImpersonate,
                identityRetries: Number(effectiveRetries) || 0,
              })
            }
          >
            Save identity
          </Button>
        </div>
      </Card>

      <Card title="Captcha">
        <div className="form">
          <div className="field field--full">
            <Field
              label="Provider"
              value={effectiveCaptchaProvider}
              onChange={setCaptchaProvider}
              placeholder="stub (default), 2captcha, capsolver…"
              hint="Only the built-in stub is implemented; a real provider needs an account."
            />
          </div>
          <div className="field field--full">
            <SecretField
              label="API key"
              value={captchaKey}
              onChange={setCaptchaKey}
              hint={
                value?.captchaKeySet
                  ? 'A key is set. Enter a new one to replace it; it is never returned.'
                  : 'Held in memory, never returned by the API.'
              }
            />
          </div>
        </div>
        <div className="form-actions">
          <Button
            variant="primary"
            loading={saving}
            onClick={() =>
              onSave({
                captchaProvider: effectiveCaptchaProvider,
                ...(captchaKey ? { captchaApiKey: captchaKey } : {}),
              })
            }
          >
            Save captcha
          </Button>
        </div>
      </Card>

      <Card title="Shared crawl frontier">
        <div className="form">
          <div className="field field--full">
            <Field
              label="Local database file"
              value={effectiveFrontierPath}
              onChange={setFrontierPath}
              placeholder="/var/lib/vyzor/frontier.db"
              hint="A SQLite file on this machine: durable and resumable with no account. Used when no remote URL is set."
            />
          </div>
          <div className="field field--full">
            <Field
              label="Remote database URL (optional)"
              value={effectiveFrontierUrl}
              onChange={setFrontierUrl}
              placeholder="libsql://…"
              hint="Takes precedence over the local file. With a token, a crawl given a run id shares its queue across workers."
            />
          </div>
          <div className="field field--full">
            <SecretField
              label="Auth token"
              value={frontierToken}
              onChange={setFrontierToken}
              hint={
                value?.frontierTokenSet
                  ? 'A token is set. Enter a new one to replace it; it is never returned.'
                  : 'Held in memory, never returned by the API.'
              }
            />
          </div>
        </div>
        <div className="form-actions">
          <Button
            variant="primary"
            loading={saving}
            onClick={() =>
              onSave({
                frontierDbUrl: effectiveFrontierUrl,
                frontierDbPath: effectiveFrontierPath,
                ...(frontierToken ? { frontierToken } : {}),
              })
            }
          >
            Save frontier
          </Button>
        </div>
      </Card>
    </div>
  );
}
