/**
 * SiteSearch — find a file on a site by name, then download it.
 *
 * A separate way into "New download" from the source form. The source form
 * wants a *URL to a file*; this wants a *site and a name*, because often the
 * person does not know the URL — they know the file is somewhere on
 * alldatasheets.com and they know what it is called.
 *
 * The page it produces is a list of candidates, and picking one fills the
 * source form rather than downloading immediately: a crawl finds files, it does
 * not decide which one you meant, and the form is where the pipeline steps live.
 *
 * A crawl takes tens of seconds, which the UI has to be honest about: it says
 * what it is doing while it runs, and reports what the walk *did* when it finds
 * nothing, so "no results" is an answer rather than a dead end.
 */

import { useState } from 'react';
import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { Field } from '../primitives/Field';
import { SwitchRow } from '../primitives/SwitchRow';
import { Note } from '../primitives/Note';
import { EmptyState } from '../primitives/EmptyState';
import { Spinner } from '../primitives/Spinner';
import { IconSearch, IconDownload } from '../primitives/Icons';
import { QualityPicker } from '../downloads/QualityPicker';
import { formatBytes, type QualityOption } from '../../hooks/view-models';
import type { CrawlHit, CrawlReport } from '../../hooks';

export interface SiteSearchValue {
  /** The site to walk. */
  url: string;
  /** The filename to look for. */
  term: string;
  /** How many links deep to follow. */
  depth: number;
  /** Whether a page with no match may be re-read through the browser. */
  useBrowser: boolean;
  /**
   * Whether the walk honours robots.txt.
   *
   * On by default. Turning it off is only right for a host you own or are
   * authorised to crawl; a site that answers `Disallow: /` has asked not to be
   * crawled.
   */
  obeyRobots: boolean;
}

export interface SiteSearchProps {
  value: SiteSearchValue;
  onChange: (next: SiteSearchValue) => void;
  /** The hits from the last crawl, or null before one has run. */
  hits: CrawlHit[] | null;
  /** What the last walk did, so an empty result can be explained. */
  report: CrawlReport | null;
  /** The term the last crawl used, for the result heading. */
  searchedTerm: string;
  running: boolean;
  /** Why the last crawl failed outright, when it did. */
  error: string | null;
  onRun: () => void;
  onStop: () => void;
  /** Queue a download for one hit, at a chosen height when it offers a choice. */
  onDownload: (hit: CrawlHit, height?: number) => void;
  /** Which hit is being queued, so its button can spin. */
  queueingUrl: string | null;
  /** Whether the browser fallback is available on this Vyzor. */
  canUseBrowser: boolean;
}

export function SiteSearch({
  value,
  onChange,
  hits,
  report,
  searchedTerm,
  running,
  error,
  onRun,
  onStop,
  onDownload,
  queueingUrl,
  canUseBrowser,
}: SiteSearchProps) {
  const set = (patch: Partial<SiteSearchValue>) => onChange({ ...value, ...patch });
  const ready = value.url.trim().length > 0;

  return (
    <>
      <Card
        title="Find a file on a site"
        subtitle="Give a site and a name; Vyzor walks the site and lists what matches"
      >
        <div className="form">
          <div className="field field--full">
            <Field
              label="Site URL"
              value={value.url}
              onChange={(url) => set({ url })}
              placeholder="https://www.alldatasheets.com/"
              hint="Where to start walking. Vyzor follows the links from here."
            />
          </div>
          <div className="field field--full">
            <Field
              label="File name"
              value={value.term}
              onChange={(term) => set({ term })}
              placeholder="stm32f103c8"
              hint="Matched against the file name and the link text. Leave empty to list every file found."
            />
          </div>
          <Field
            label="Follow links"
            type="number"
            min={0}
            max={5}
            value={String(value.depth)}
            onChange={(v) => set({ depth: Math.max(0, Math.min(5, Number(v) || 0)) })}
            hint="How many clicks deep to go. 0 reads only the page you gave."
          />
          <div className="field field--full">
            <SwitchRow
              name="Use the browser for empty pages"
              description={
                canUseBrowser
                  ? 'Read a page again through a real browser, so a listing built by JavaScript is found'
                  : 'Unavailable — this Vyzor has no browser'
              }
              checked={value.useBrowser && canUseBrowser}
              onChange={(useBrowser) => set({ useBrowser })}
            />
          </div>
          <div className="field field--full">
            <SwitchRow
              name="Respect robots.txt"
              description="Leave on unless you own or are authorised to crawl this host — a site that disallows crawling has asked not to be walked"
              checked={value.obeyRobots}
              onChange={(obeyRobots) => set({ obeyRobots })}
            />
          </div>
        </div>

        <div className="form-actions">
          {running ? (
            <Button onClick={onStop}>Stop</Button>
          ) : (
            <Button variant="primary" onClick={onRun} disabled={!ready}>
              <IconSearch /> Search the site
            </Button>
          )}
        </div>

        {running && (
          <Note className="note">
            Walking the site. This reads several pages, so it takes up to a minute
            — Vyzor stops itself at the budget rather than running on.
          </Note>
        )}
      </Card>

      <Card
        title="Files found"
        subtitle={
          hits === null
            ? 'Nothing searched yet'
            : hits.length === 0
              ? 'No match'
              : `${hits.length} match${hits.length === 1 ? '' : 'es'}${searchedTerm ? ` for “${searchedTerm}”` : ''}`
        }
      >
        {error !== null && <Note className="note note--warn">{error}</Note>}

        {running && hits === null ? (
          <div className="browse__loading">
            <Spinner size="lg" />
          </div>
        ) : hits === null ? (
          <EmptyState
            title="Search a site for a file"
            description="Give a site and a file name, then search. Vyzor walks the site's links and lists every file whose name matches."
          />
        ) : hits.length === 0 ? (
          <CrawlOutcome report={report} term={searchedTerm} />
        ) : (
          <>
            <ul className="browse__grid">
              {hits.map((hit) => (
                <li key={hit.url} className="bcard">
                  <span className="bcard__thumb">
                    {hit.thumbnail ? (
                      /*
                       * The poster the extractor reported, or an image link the
                       * walk found. Fetched by the browser directly — the engine
                       * is not in this path, and an image request carries no
                       * credentials anyway, so a host that serves its pictures
                       * publicly just works.
                       */
                      <img
                        src={hit.thumbnail}
                        alt=""
                        loading="lazy"
                        onError={(e) => {
                          // A poster that will not load is worse than none: the
                          // card falls back to the glyph rather than a broken
                          // image.
                          const img = e.currentTarget;
                          img.style.display = 'none';
                          img.parentElement?.classList.add('bcard__thumb--failed');
                        }}
                      />
                    ) : (
                      <span className="bcard__glyph">
                        <IconDownload />
                      </span>
                    )}
                  </span>
                  <span className="bcard__body">
                    <span className="bcard__title" title={hit.url}>
                      {hit.title ?? hit.filename}
                    </span>
                    <span className="bcard__meta">
                      {hit.filename}
                      {hit.viaBrowser ? ' · found in the browser' : ''}
                    </span>
                    <span className="bcard__licence" title={hit.foundOn ?? ''}>
                      {scoreLabel(hit.score)}
                      {hit.foundOn ? ` · on ${hostOf(hit.foundOn)}` : ''}
                    </span>
                  </span>
                  <div className="bcard__actions">
                    {hit.qualities.length > 0 ? (
                      <Button
                        size="sm"
                        variant="primary"
                        onClick={() => onDownload(hit, hit.qualities[0]!.height)}
                        title={`Download at ${hit.qualities[0]!.label}`}
                      >
                        Best ({hit.qualities[0]!.label})
                      </Button>
                    ) : (
                      <Button
                        size="sm"
                        variant="primary"
                        loading={queueingUrl === hit.url}
                        onClick={() => onDownload(hit)}
                      >
                        Download
                      </Button>
                    )}
                  </div>
                </li>
              ))}
            </ul>
            {/*
              The renditions of a resolved stream, when there is a choice to make.
              Shown below the list rather than inside each card: a stream's
              qualities are the same set for every entry that resolved, and a
              picker per card would be a wall of identical rows.
            */}
            <ResolvedQualities
              hits={hits}
              queueingUrl={queueingUrl}
              onDownload={onDownload}
            />
            <Note className="note">
              Matches are ranked by how well the name fits, so the closest is first.
              Picking one fills the source form, where the pipeline steps are chosen.
            </Note>
          </>
        )}
      </Card>
    </>
  );
}

/**
 * What a resolved stream offers, and a way to take a smaller one.
 *
 * A stream is a set of renditions rather than one file, and which to take is the
 * viewer's decision — a phone on mobile data does not want the 4K master. Before
 * this, the download always took the largest: measured at 362 MB where the 720p
 * rendition is 30 MB, a twelvefold difference the viewer had no say in.
 *
 * Only shown when there is a genuine choice, so an ordinary file — the common
 * case for this feature — gains no extra UI.
 */
function ResolvedQualities({
  hits,
  queueingUrl,
  onDownload,
}: {
  hits: ReadonlyArray<CrawlHit>;
  queueingUrl: string | null;
  onDownload: (hit: CrawlHit, height?: number) => void;
}) {
  const [selected, setSelected] = useState<number | null>(null);
  /*
   * The first resolved hit, and its renditions.
   *
   * A crawl of a streaming page usually yields the media plus the page's own
   * links, and only the media has qualities — so this finds the one that does
   * rather than assuming the first hit is it.
   */
  const target = hits.find((h) => h.qualities.length > 0) ?? null;
  if (!target) return null;

  const options: QualityOption[] = target.qualities.map((q) => ({
    url: target.url,
    height: q.height,
    width: 0,
    label: q.label,
    codecs: '',
    bytes: q.bytes,
    sizeLabel: q.bytes >= 0 ? formatBytes(q.bytes) : 'size unknown',
  }));
  const active = selected ?? target.qualities[0]!.height;
  const chosen = target.qualities.find((q) => q.height === active) ?? target.qualities[0]!;

  return (
    <Card
      title="Quality"
      subtitle={`${target.filename.slice(0, 60)} — ${target.qualities.length} rendition${
        target.qualities.length === 1 ? '' : 's'
      }`}
    >
      <QualityPicker options={options} selectedHeight={active} onSelect={setSelected} />
      <div className="form-actions">
        <Button
          variant="primary"
          loading={queueingUrl === target.url}
          onClick={() => onDownload(target, chosen.height)}
        >
          <IconDownload /> Download {chosen.label}
        </Button>
      </div>
      <Note className="note">
        The sizes are what the source declares for each rendition. Choosing a
        smaller one downloads less, not a scaled copy of the same file.
      </Note>
    </Card>
  );
}

/**
 * What a walk with no match actually did.
 *
 * "No results" is not an answer on its own — the useful question is whether the
 * site was read and nothing matched, or whether it could not be read at all.
 * The two need different responses from the user, so they are said separately.
 */
function CrawlOutcome({ report, term }: { report: CrawlReport | null; term: string }) {
  if (!report || report.pagesRead === 0) {
    return (
      <EmptyState
        title="The site could not be read"
        description="Nothing was fetched from that address. Check the URL, or try again — a host that blocks automated clients cannot be walked."
      />
    );
  }

  const parts: string[] = [
    `${report.pagesRead} page${report.pagesRead === 1 ? '' : 's'} read`,
  ];
  if (report.pagesRendered > 0) parts.push(`${report.pagesRendered} needed the browser`);
  if (report.pagesFailed > 0) parts.push(`${report.pagesFailed} could not be fetched`);

  const reason =
    report.pagesRendered > 0
      ? 'Some pages had to be read through the browser, so their links were seen.'
      : 'The site was read, and no file on it matched that name.';

  return (
    <EmptyState
      title={term ? `No file matching “${term}”` : 'No files found'}
      description={`${parts.join(' · ')}. ${reason} Try a shorter name, or follow more links.`}
    />
  );
}

/** The score, as something a person can read rather than a number. */
function scoreLabel(score: number): string {
  if (score >= 90) return 'Exact name';
  if (score >= 75) return 'Strong match';
  if (score >= 45) return 'Good match';
  return 'Loose match';
}

/** The host of a URL, for saying where a result was found. */
function hostOf(url: string): string {
  try {
    return new URL(url).host;
  } catch {
    return url;
  }
}

/** Exported for the parent's use when it turns a hit into a form value. */
export function formatHitSize(bytes: number): string {
  return bytes >= 0 ? formatBytes(bytes) : 'unknown size';
}
