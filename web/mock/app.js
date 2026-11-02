/* ==========================================================================
   Media Ingest — console mock

   Standalone design reference (no build step). Data is simulated locally so the
   UI can be evaluated before real components exist. Shapes mirror
   web/src/domain/media.ts so the port to hooks/ is 1:1.
   ========================================================================== */

const STEPS = [
  { key: 'probe', name: 'Probe', meta: 'HEAD + byte sniff' },
  { key: 'fetch', name: 'Fetch', meta: 'Ranged, 8 connections' },
  { key: 'verify', name: 'Verify', meta: 'SHA-256 checksum' },
  { key: 'transcode', name: 'Transcode', meta: 'HLS adaptive ladder' },
  { key: 'upload', name: 'Upload', meta: 'S3-compatible PUT' },
  { key: 'purge', name: 'Purge', meta: 'Edge cache invalidation' },
];

const MASK = '••••••••••••••••';
const KIND = { mp4: 'MP4', matroska: 'MKV', 'mpeg-ts': 'MPEG-TS', hls: 'HLS', dash: 'DASH' };
const now = () => Date.now() / 1000;

const state = {
  view: 'overview',
  storageTab: 'files',
  settingsTab: 'general',
  filter: 'all',
  downloadTarget: 'local', // 'local' | 'cloud'
};

/* ------------------------------ data ------------------------------ */

state.downloads = [
  dl('a1f4c9', 'https://cdn.example.com/library/nightfall-1080.mp4', 'done', 6),
  dl('7b2e10', 'https://cdn.example.com/library/atlas-docs-ep4.mp4', 'downloading', 1),
  dl('c93d55', 'https://origin.internal/raw/studio-cut-A.mov', 'downloading', 1),
  dl('44aa71', 'https://cdn.example.com/library/field-notes.mkv', 'queued', 0),
  dl('e07b12', 'https://mirror.example.net/archive/1998-reel.mp4', 'failed', 3),
  dl('11d6f8', 'https://cdn.example.com/library/blue-hour.mp4', 'done', 6),
  dl('9c3a02', 'https://origin.internal/raw/interview-take-7.mov', 'cancelled', 1),
  dl('2f8b41', 'https://cdn.example.com/library/harbour-lights-4k.mp4', 'done', 6),
];

function dl(id, url, dlState, stage) {
  const bytes = 180_000_000 + Math.floor(Math.random() * 900_000_000);
  const t = now();
  const done = dlState === 'done';
  const name = url.split('/').pop();
  const ext = name.split('.').pop();
  return {
    id,
    url,
    name,
    state: dlState,
    stage,
    bytes,
    downloaded: done ? bytes : Math.floor(bytes * (stage / STEPS.length)),
    speed: dlState === 'downloading' ? 8_000_000 + Math.random() * 42_000_000 : 0,
    media: { kind: ext === 'mkv' ? 'matroska' : 'mp4' },
    sha256: done ? '9f2c' + id + 'e41ba7c8d0b4f61a' : null,
    createdAt: t - Math.random() * 7200,
    finishedAt: done || dlState === 'failed' ? t - Math.random() * 900 : 0,
    artifacts: done
      ? [
          `https://cdn.example.com/hls/${id}/master.m3u8`,
          `https://cdn.example.com/hls/${id}/1080p/index.m3u8`,
          `https://cdn.example.com/hls/${id}/720p/index.m3u8`,
        ]
      : [],
    error: dlState === 'failed' ? 'Range 3 failed after 5 attempts (HTTP 503 Service Unavailable)' : null,
    duration: 120 + Math.floor(Math.random() * 600),
    resolution: done ? '1920 × 1080' : '—',
    codec: 'H.264 / AAC',
  };
}

/* ------------------------------ formatting ------------------------------ */

const fmtBytes = (n) => {
  if (!n) return '0 B';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n.toFixed(n < 10 && i > 0 ? 1 : 0)} ${u[i]}`;
};
const fmtSpeed = (bps) => (bps > 0 ? `${(bps / 1e6).toFixed(1)} MB/s` : '—');
const fmtClock = (s) => {
  const m = Math.floor(s / 60);
  const sec = Math.floor(s % 60);
  return `${m}:${String(sec).padStart(2, '0')}`;
};
const fmtWhen = (t) =>
  t ? new Date(t * 1000).toLocaleString('en-GB', { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' }) : '—';
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const pct = (d) => (d.bytes ? Math.round((d.downloaded / d.bytes) * 100) : 0);
const isSettled = (s) => s === 'done' || s === 'failed' || s === 'cancelled';
const isActive = (s) => s === 'downloading' || s === 'queued';

/* ------------------------------ icons ------------------------------ */

const ICON = {
  check: '<svg viewBox="0 0 16 16" fill="none"><path d="M3.5 8.5l3 3 6-7" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>',
  x: '<svg viewBox="0 0 16 16" fill="none"><path d="M4 4l8 8M12 4l-8 8" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg>',
  info: '<svg viewBox="0 0 20 20" fill="none"><circle cx="10" cy="10" r="7.2" stroke="currentColor" stroke-width="1.5"/><path d="M10 9v4.5M10 6.6v.2" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/></svg>',
  inbox: '<svg viewBox="0 0 24 24" fill="none"><path d="M3 13l2.5-7A2 2 0 0 1 7.4 4.5h9.2a2 2 0 0 1 1.9 1.5L21 13v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/><path d="M3 13h5l1 2h6l1-2h5" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/></svg>',
  file: '<svg viewBox="0 0 24 24" fill="none"><path d="M14 3v5h5" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/><path d="M6 3h8l5 5v13a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/></svg>',
  play: '<svg viewBox="0 0 24 24" fill="none"><path d="M8 5.5v13l11-6.5z" fill="currentColor"/></svg>',
  folder: '<svg viewBox="0 0 20 20" fill="none"><path d="M3 5.5A1.5 1.5 0 0 1 4.5 4h3l1.5 2h6A1.5 1.5 0 0 1 16.5 7.5v7A1.5 1.5 0 0 1 15 16H4.5A1.5 1.5 0 0 1 3 14.5z" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"/></svg>',
  cloud: '<svg viewBox="0 0 20 20" fill="none"><path d="M6 15h8.5a3 3 0 0 0 .4-6 4.5 4.5 0 0 0-8.7-1A3.5 3.5 0 0 0 6 15z" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"/></svg>',
};

/* ------------------------------ fragments ------------------------------ */

const badge = (s) => `<span class="badge badge--${s}">${s}</span>`;

function progressCell(d) {
  const p = pct(d);
  const cls = d.state === 'done' ? 'is-done' : d.state === 'failed' ? 'is-failed' : '';
  return `<div class="progress">
    <div class="progress__track"><div class="progress__fill ${cls}" style="width:${p}%"></div></div>
    <span class="progress__value">${p}%</span>
  </div>`;
}

function downloadRow(d) {
  return `<tr class="is-clickable" data-dl="${d.id}">
    <td><span class="cell-id">${d.id}</span></td>
    <td><span class="cell-url" title="${esc(d.url)}">${esc(d.url)}</span></td>
    <td>${badge(d.state)}</td>
    <td>${progressCell(d)}</td>
    <td class="num">${fmtBytes(d.bytes)}</td>
    <td class="num muted">${d.state === 'downloading' ? fmtSpeed(d.speed) : '—'}</td>
    <td><span class="tag">${KIND[d.media.kind] ?? '—'}</span></td>
  </tr>`;
}

function downloadTable(list) {
  if (!list.length) return emptyState('No downloads yet', 'Start an ingest to see it appear here.');
  return `<table class="table">
    <thead><tr>
      <th>ID</th><th>Source</th><th>Status</th><th>Progress</th><th>Size</th><th>Speed</th><th>Format</th>
    </tr></thead>
    <tbody>${list.map(downloadRow).join('')}</tbody>
  </table>`;
}

function emptyState(title, sub) {
  return `<div class="empty">${ICON.inbox}<h3>${title}</h3><p>${sub}</p></div>`;
}

function stepsPanel(d) {
  return `<div class="steps">${STEPS.map((s, i) => {
    let cls = '';
    if (i < d.stage) cls = 'is-done';
    else if (i === d.stage && isActive(d.state)) cls = 'is-active';
    else if (i === d.stage && d.state === 'failed') cls = 'is-fault';
    const glyph = cls === 'is-done' ? ICON.check : cls === 'is-fault' ? ICON.x : String(i + 1);
    const right = cls === 'is-done' ? 'Complete' : cls === 'is-active' ? 'Running' : cls === 'is-fault' ? 'Failed' : 'Pending';
    return `<div class="step ${cls}">
      <span class="step__dot">${glyph}</span>
      <div class="step__body">
        <div class="step__name">${s.name}</div>
        <div class="step__meta">${s.meta}</div>
      </div>
      <span class="step__right">${right}</span>
    </div>`;
  }).join('')}</div>`;
}

function field(id, label, value, hint) {
  return `<div class="field">
    <label for="${id}">${label}</label>
    <input id="${id}" type="text" value="${esc(value)}" spellcheck="false" />
    ${hint ? `<span class="field__hint">${hint}</span>` : ''}
  </div>`;
}

function secretField(id, label, value, hint) {
  return `<div class="field">
    <label for="${id}">${label}</label>
    <div class="input-group">
      <input id="${id}" type="text" value="${MASK}" data-original="${esc(value)}" spellcheck="false" autocomplete="off" />
      <button class="input-group__btn" data-reveal="${id}">Show</button>
    </div>
    ${hint ? `<span class="field__hint">${hint}</span>` : ''}
  </div>`;
}

function switchRow(id, name, desc, on) {
  return `<div class="switch-row">
    <div class="switch-row__text">
      <div class="switch-row__name">${name}</div>
      <div class="switch-row__desc">${desc}</div>
    </div>
    <label class="switch">
      <input type="checkbox" id="${id}" ${on ? 'checked' : ''} />
      <span class="switch__track"></span>
    </label>
  </div>`;
}

function tabs(items, active, attr) {
  return `<div class="tabs" role="tablist">${items
    .map(
      (t) =>
        `<button class="tab ${t.id === active ? 'is-active' : ''}" role="tab" data-${attr}="${t.id}">` +
        `${t.label}${t.count !== undefined ? `<span class="tab__count">${t.count}</span>` : ''}</button>`,
    )
    .join('')}</div>`;
}

/* ------------------------------ views ------------------------------ */

const VIEW_META = {
  overview: ['Overview', 'Pipeline health and recent activity'],
  downloads: ['Downloads', 'Everything being fetched, and what has finished'],
  ingest: ['New ingest', 'Queue a source for download, transcode and delivery'],
  storage: ['Storage', 'Downloaded files, object storage and delivery'],
  settings: ['Settings', 'Connection, storage location and engine capabilities'],
};

const views = {
  overview() {
    const active = state.downloads.filter((d) => isActive(d.state)).length;
    const done = state.downloads.filter((d) => d.state === 'done').length;
    const failed = state.downloads.filter((d) => d.state === 'failed').length;
    const moved = state.downloads.reduce((a, d) => a + d.downloaded, 0);
    const focus = state.downloads.find((d) => d.state === 'downloading') ?? state.downloads[0];

    return `
      <section class="stats">
        <div class="stat">
          <div class="stat__label">Active downloads</div>
          <div class="stat__value">${active}<span>running</span></div>
          <div class="stat__foot"><span class="trend trend--up">▲ 2</span> since last hour</div>
        </div>
        <div class="stat">
          <div class="stat__label">Completed</div>
          <div class="stat__value">${done}<span>files</span></div>
          <div class="stat__foot"><span class="trend trend--flat">—</span> no change</div>
        </div>
        <div class="stat">
          <div class="stat__label">Failed</div>
          <div class="stat__value">${failed}<span>files</span></div>
          <div class="stat__foot"><span class="trend ${failed ? 'trend--down' : 'trend--flat'}">${failed ? '▲ 1' : '—'}</span> needs attention</div>
        </div>
        <div class="stat">
          <div class="stat__label">Data processed</div>
          <div class="stat__value">${fmtBytes(moved).split(' ')[0]}<span>${fmtBytes(moved).split(' ')[1]}</span></div>
          <div class="stat__foot"><span class="trend trend--up">▲ 12%</span> vs yesterday</div>
        </div>
      </section>

      <div class="grid-2">
        <section class="card">
          <div class="card__head">
            <div>
              <div class="card__title">Recent downloads</div>
              <div class="card__sub">Latest ${Math.min(6, state.downloads.length)} of ${state.downloads.length}</div>
            </div>
            <button class="btn btn--sm btn--ghost" data-goto="downloads">View all</button>
          </div>
          <div class="card__body card__body--flush">${downloadTable(state.downloads.slice(0, 6))}</div>
        </section>

        <div class="stack">
          <section class="card">
            <div class="card__head">
              <div>
                <div class="card__title">Pipeline</div>
                <div class="card__sub">Download ${esc(focus.id)}</div>
              </div>
              ${badge(focus.state)}
            </div>
            <div class="card__body">${stepsPanel(focus)}</div>
          </section>

          <section class="card">
            <div class="card__head"><div class="card__title">Supported formats</div></div>
            <div class="card__body" style="display:flex;flex-wrap:wrap;gap:7px">
              ${['MP4', 'MKV', 'MOV', 'AVI', 'FLV', 'Ogg', 'WMV', 'MPEG-TS', 'HLS', 'DASH']
                .map((f) => `<span class="tag">${f}</span>`).join('')}
            </div>
          </section>
        </div>
      </div>`;
  },

  downloads() {
    return `
      <section class="card">
        <div class="card__head">
          <div>
            <div class="card__title">All downloads</div>
            <div class="card__sub">${state.downloads.length} total · polling every second</div>
          </div>
          <div style="display:flex;gap:8px">
            <button class="btn btn--sm">Filter</button>
            <button class="btn btn--sm">Export</button>
          </div>
        </div>
        <div class="card__body card__body--flush">${downloadTable(state.downloads)}</div>
      </section>`;
  },

  ingest() {
    return `
      <div class="grid-2">
        <section class="card">
          <div class="card__head"><div class="card__title">Source</div></div>
          <div class="card__body">
            <div class="form">
              <div class="field field--full">
                <label for="f-url">Source URL</label>
                <input id="f-url" type="text" value="https://cdn.example.com/library/nightfall-1080.mp4" spellcheck="false" />
                <span class="field__hint">Direct file or manifest. The engine verifies the bytes before downloading.</span>
              </div>
              <div class="field">
                <label for="f-conn">Connections</label>
                <input id="f-conn" type="number" value="8" min="1" max="32" />
              </div>
              <div class="field">
                <label for="f-name">Save as</label>
                <input id="f-name" type="text" value="nightfall-1080.mp4" spellcheck="false" />
              </div>
              <div class="field field--full">
                <label>Pipeline steps</label>
                ${switchRow('t-hls', 'Transcode to HLS', 'Adaptive bitrate ladder, never upscales', true)}
                ${switchRow('t-up', 'Upload to object storage', 'S3-compatible multipart upload', false)}
                ${switchRow('t-purge', 'Purge CDN cache', 'Invalidate edge cache after upload', false)}
              </div>
            </div>
            <div class="form-actions">
              <button class="btn btn--primary" id="startDownload">Start download</button>
              <button class="btn" id="probeBtn">Probe only</button>
            </div>
          </div>
        </section>

        <section class="card">
          <div class="card__head"><div class="card__title">Probe result</div><span class="card__sub">Dry run</span></div>
          <div class="card__body" id="probeBody">
            <div class="kv">
              <div class="kv__row"><span class="kv__k">Status</span><span class="kv__v muted">Awaiting probe</span></div>
              <div class="kv__row"><span class="kv__k">Container</span><span class="kv__v muted">—</span></div>
              <div class="kv__row"><span class="kv__k">Range support</span><span class="kv__v muted">—</span></div>
              <div class="kv__row"><span class="kv__k">Size</span><span class="kv__v muted">—</span></div>
            </div>
            <div class="note" style="margin-top:16px">${ICON.info}<span>Detection reads the raw bytes, never the file extension or Content-Type header. Non-video payloads are refused before the download begins.</span></div>
          </div>
        </section>
      </div>`;
  },

  storage() {
    const t = state.storageTab;
    const doneCount = state.downloads.filter((d) => d.state === 'done').length;
    return `
      ${tabs(
        [
          { id: 'files', label: 'Files', count: doneCount },
          { id: 'object', label: 'Object storage' },
          { id: 'cdn', label: 'CDN & delivery' },
        ],
        t,
        'storage-tab',
      )}
      ${t === 'files' ? storageFiles() : ''}
      ${t === 'object' ? storageObject() : ''}
      ${t === 'cdn' ? storageCdn() : ''}`;
  },

  settings() {
    const t = state.settingsTab;
    return `
      ${tabs(
        [
          { id: 'general', label: 'Connection' },
          { id: 'location', label: 'Storage location' },
          { id: 'engine', label: 'Engine' },
        ],
        t,
        'settings-tab',
      )}
      ${t === 'general' ? settingsGeneral() : ''}
      ${t === 'location' ? settingsLocation() : ''}
      ${t === 'engine' ? settingsEngine() : ''}`;
  },
};

/* ------------------------------ storage: files ------------------------------ */

function fileList() {
  const f = state.filter;
  if (f === 'ongoing') return state.downloads.filter((d) => isActive(d.state));
  if (f === 'done') return state.downloads.filter((d) => d.state === 'done');
  if (f === 'problem') return state.downloads.filter((d) => d.state === 'failed' || d.state === 'cancelled');
  return state.downloads;
}

function storageFiles() {
  const ongoing = state.downloads.filter((d) => isActive(d.state));
  const list = fileList();
  const totalBytes = state.downloads.filter((d) => d.state === 'done').reduce((a, d) => a + d.bytes, 0);
  const target = state.downloadTarget;

  return `
    <section class="card">
      <div class="card__head">
        <div>
          <div class="card__title">Downloaded files</div>
          <div class="card__sub">
            ${state.downloads.filter((d) => d.state === 'done').length} finished · ${fmtBytes(totalBytes)} ·
            saving to <strong>${target === 'local' ? 'this device' : 'object storage'}</strong>
          </div>
        </div>
        <button class="btn btn--sm btn--ghost" data-goto="settings">Change location</button>
      </div>

      <div class="card__body">
        <div class="files__label">In progress${ongoing.length ? ` · ${ongoing.length}` : ''}</div>
        ${
          ongoing.length
            ? `<div class="dl-list">${ongoing.map(downloadCard).join('')}</div>`
            : `<div class="files__none">Nothing downloading right now.</div>`
        }
      </div>

      <div class="card__head files__head-split">
        <div class="card__title">Library</div>
        ${tabs(
          [
            { id: 'all', label: 'All' },
            { id: 'ongoing', label: 'Ongoing' },
            { id: 'done', label: 'Finished' },
            { id: 'problem', label: 'Failed' },
          ],
          state.filter,
          'filter',
        )}
      </div>
      <div class="card__body card__body--flush">${fileTable(list)}</div>
    </section>`;
}

/* An in-progress download. The bar is pinned to the bottom of the card and
   carries both the percentage and the live transfer rate. */
function downloadCard(d) {
  const p = pct(d);
  const remaining = d.speed > 0 ? (d.bytes - d.downloaded) / d.speed : 0;
  return `
    <div class="dl" data-dl="${d.id}">
      <div class="dl__top">
        <span class="dl__icon">${ICON.file}</span>
        <div class="dl__meta">
          <div class="dl__name">${esc(d.name)}</div>
          <div class="dl__sub">
            ${fmtBytes(d.downloaded)} of ${fmtBytes(d.bytes)}${d.state === 'downloading' ? ` · ${fmtClock(remaining)} left` : ' · waiting to start'}
          </div>
        </div>
        <div class="dl__speed">
          <span class="dl__speed-value">${fmtSpeed(d.speed)}</span>
          <span class="dl__speed-label">transfer</span>
        </div>
      </div>
      <div class="dl__bar" role="progressbar" aria-valuenow="${p}" aria-valuemin="0" aria-valuemax="100">
        <div class="dl__bar-fill" style="width:${p}%"></div>
        <span class="dl__bar-pct">${p}%</span>
        <span class="dl__bar-speed">${fmtSpeed(d.speed)}</span>
      </div>
    </div>`;
}

function fileTable(list) {
  if (!list.length) return emptyState('No files here', 'Try a different filter.');
  return `<table class="table file-table">
    <thead><tr>
      <th>File</th><th>Status</th><th>Size</th><th>Location</th><th>Added</th><th class="th-actions"></th>
    </tr></thead>
    <tbody>${list.map(fileRow).join('')}</tbody>
  </table>`;
}

function fileRow(d) {
  const openable = d.state === 'done';
  const local = state.downloadTarget === 'local';
  return `<tr data-dl="${d.id}">
    <td>
      <div class="file-cell">
        <span class="file-cell__icon">${ICON.file}</span>
        <span class="file-cell__name" title="${esc(d.name)}">${esc(d.name)}</span>
      </div>
    </td>
    <td>${badge(d.state)}</td>
    <td class="num">${fmtBytes(d.bytes)}</td>
    <td>
      <span class="loc">
        ${local ? ICON.folder : ICON.cloud}
        <span class="loc__path">${local ? '/Users/you/Downloads/ingest' : 's3://vyzorix-media/downloads'}</span>
      </span>
    </td>
    <td class="num muted">${fmtWhen(d.createdAt)}</td>
    <td class="td-actions">
      ${
        openable
          ? `<button class="btn btn--sm" data-open="${d.id}">${ICON.play} Open</button>
             <button class="btn btn--sm btn--ghost" data-reveal-file="${d.id}" title="Show in folder">${ICON.folder}</button>`
          : `<button class="btn btn--sm btn--ghost" data-details="${d.id}">Details</button>`
      }
    </td>
  </tr>`;
}

/* ------------------------------ storage: object + cdn ------------------------------ */

function storageObject() {
  return `
    <div class="grid-2 grid-2--even">
      <section class="card">
        <div class="card__head"><div class="card__title">S3-compatible credentials</div></div>
        <div class="card__body">
          <div class="form">
            ${field('s-endpoint', 'Endpoint', 'https://acct.r2.cloudflarestorage.com')}
            ${field('s-region', 'Region', 'auto')}
            ${field('s-bucket', 'Bucket', 'vyzorix-media')}
            ${field('s-prefix', 'Key prefix', 'hls/')}
            ${secretField('s-access', 'Access key ID', 'YOUR_ACCESS_KEY_ID')}
            ${secretField('s-secret', 'Secret access key', 'YOUR_SECRET_ACCESS_KEY')}
          </div>
          <div class="form-actions"><button class="btn btn--primary" id="testStorage">Test connection</button></div>
        </div>
      </section>
      <section class="card">
        <div class="card__head"><div class="card__title">Published artifacts</div></div>
        <div class="card__body card__body--flush">${artifactTable()}</div>
      </section>
    </div>`;
}

function storageCdn() {
  return `
    <section class="card">
      <div class="card__head"><div class="card__title">CDN &amp; delivery</div></div>
      <div class="card__body">
        <div class="form">
          ${field('s-cdn', 'CDN base URL', 'https://cdn.example.com', 'Used to build public artifact URLs.')}
          ${field('s-cf-zone', 'Cloudflare zone ID', 'a1b2c3d4e5f6a7b8')}
          ${secretField('s-cf-token', 'Cloudflare API token', 'YOUR_API_TOKEN')}
        </div>
        <div class="note" style="margin-top:18px">${ICON.info}<span>Credentials are held in memory for this session only. They are never written to browser storage and never persisted on the server.</span></div>
      </div>
    </section>`;
}

function artifactTable() {
  const rows = state.downloads.flatMap((d) => d.artifacts.map((url) => ({ id: d.id, url })));
  if (!rows.length) return emptyState('Nothing published', 'Completed downloads with upload enabled will appear here.');
  return `<table class="table artifact-table">
    <thead><tr><th>ID</th><th>Object</th><th>Content type</th></tr></thead>
    <tbody>${rows.map((r) => `<tr>
      <td><span class="cell-id">${r.id}</span></td>
      <td><span class="cell-url" title="${esc(r.url)}">${esc(r.url)}</span></td>
      <td><span class="tag">${r.url.endsWith('.m3u8') ? 'application/vnd.apple.mpegurl' : 'video/mp2t'}</span></td>
    </tr>`).join('')}</tbody>
  </table>`;
}

/* ------------------------------ settings ------------------------------ */

function settingsGeneral() {
  return `
    <div class="grid-2 grid-2--even">
      <section class="card">
        <div class="card__head"><div class="card__title">API connection</div></div>
        <div class="card__body">
          <div class="form">
            <div class="field field--full">
              <label for="c-url">API endpoint</label>
              <input id="c-url" type="text" value="http://127.0.0.1:8787" spellcheck="false" />
            </div>
            ${secretField('c-token', 'Bearer token', 'YOUR_BEARER_TOKEN', 'Required for all endpoints except health checks.')}
          </div>
          <div class="form-actions"><button class="btn btn--primary" id="reconnect">Save &amp; reconnect</button></div>
        </div>
      </section>
      <section class="card">
        <div class="card__head"><div class="card__title">Behaviour</div></div>
        <div class="card__body">
          ${switchRow('pref-dark', 'Dark theme', 'Follows your last choice in the header', document.documentElement.dataset.theme === 'dark')}
          ${switchRow('pref-auto', 'Start downloads automatically', 'Begin fetching as soon as a source is added', true)}
          ${switchRow('pref-verify', 'Verify checksums', 'Hash every finished file with SHA-256', true)}
        </div>
      </section>
    </div>`;
}

function settingsLocation() {
  const local = state.downloadTarget === 'local';
  return `
    <section class="card">
      <div class="card__head">
        <div>
          <div class="card__title">Where downloads are saved</div>
          <div class="card__sub">Applies to new downloads. Existing files stay where they are.</div>
        </div>
      </div>
      <div class="card__body">
        <div class="choice-grid">
          <label class="choice ${local ? 'is-active' : ''}">
            <input type="radio" name="dl-target" value="local" ${local ? 'checked' : ''} />
            <span class="choice__icon">${ICON.folder}</span>
            <span class="choice__body">
              <span class="choice__name">This device</span>
              <span class="choice__desc">Save to the local filesystem. Finished files can be opened in your player.</span>
            </span>
            <span class="choice__radio"></span>
          </label>
          <label class="choice ${!local ? 'is-active' : ''}">
            <input type="radio" name="dl-target" value="cloud" ${!local ? 'checked' : ''} />
            <span class="choice__icon">${ICON.cloud}</span>
            <span class="choice__body">
              <span class="choice__name">Cloud storage</span>
              <span class="choice__desc">Upload straight to the configured bucket and serve through the CDN.</span>
            </span>
            <span class="choice__radio"></span>
          </label>
        </div>

        ${
          local
            ? `<div class="form" style="margin-top:22px">
                 ${field('loc-path', 'Download folder', '/Users/you/Downloads/ingest', 'Created automatically if it does not exist.')}
                 ${field('loc-tmp', 'Temporary parts folder', '.parts', 'Ranged downloads are assembled here before moving.')}
               </div>
               <div style="margin-top:18px">
                 ${switchRow('loc-open', 'Open folder when a download finishes', 'Reveal the file in Finder or Explorer', true)}
               </div>`
            : `<div class="form" style="margin-top:22px">
                 ${field('loc-bucket', 'Bucket', 'vyzorix-media')}
                 ${field('loc-key', 'Key prefix', 'downloads/')}
               </div>
               <div style="margin-top:18px">
                 ${switchRow('loc-cdn', 'Serve files through the CDN', 'Return a public URL instead of a local path', true)}
               </div>`
        }

        <div class="form-actions">
          <button class="btn btn--primary" id="saveLocation">Save location</button>
          <button class="btn" id="testLocation">Test write access</button>
        </div>
      </div>
    </section>

    <section class="card">
      <div class="card__head"><div class="card__title">Disk usage</div></div>
      <div class="card__body">
        <div class="usage">
          <div class="usage__row"><span>Ingest library</span><span class="num">${fmtBytes(state.downloads.filter((d) => d.state === 'done').reduce((a, d) => a + d.bytes, 0))}</span></div>
          <div class="usage__row"><span>Temporary parts</span><span class="num">1.2 GB</span></div>
          <div class="usage__row"><span>Free space</span><span class="num">184 GB</span></div>
        </div>
        <div class="usage__bar"><span style="width:38%"></span></div>
        <div class="field__hint" style="margin-top:8px">62 GB of 246 GB used on this device.</div>
      </div>
    </section>`;
}

function settingsEngine() {
  return `
    <section class="card">
      <div class="card__head"><div class="card__title">Engine capabilities</div></div>
      <div class="card__body">
        <div class="kv">
          <div class="kv__row"><span class="kv__k">Engine version</span><span class="kv__v">media-ingest 0.1.0</span></div>
          <div class="kv__row"><span class="kv__k">FFmpeg</span><span class="kv__v">7.1.5 · libx264</span></div>
          <div class="kv__row"><span class="kv__k">HLS transcoding</span><span class="kv__v kv__v--strong">Enabled</span></div>
          <div class="kv__row"><span class="kv__k">Object storage</span><span class="kv__v kv__v--strong">Enabled</span></div>
          <div class="kv__row"><span class="kv__k">CDN purge</span><span class="kv__v kv__v--strong">Enabled</span></div>
          <div class="kv__row"><span class="kv__k">Max request body</span><span class="kv__v">64 MB</span></div>
          <div class="kv__row"><span class="kv__k">Worker threads</span><span class="kv__v">16</span></div>
        </div>
      </div>
    </section>`;
}

/* ------------------------------ drawer ------------------------------ */

function openDrawer(id) {
  const d = state.downloads.find((x) => x.id === id);
  if (!d) return;
  closeDrawer();
  const overlay = document.createElement('div');
  overlay.className = 'overlay';
  overlay.addEventListener('click', closeDrawer);
  const drawer = document.createElement('aside');
  drawer.className = 'drawer';
  drawer.innerHTML = `
    <div class="drawer__head">
      <div>
        <div class="drawer__title">${esc(d.name)}</div>
        <div class="card__sub">${esc(d.url)}</div>
      </div>
      <button class="btn btn--icon btn--ghost" id="closeDrawer" aria-label="Close">
        <svg viewBox="0 0 20 20"><path d="M5 5l10 10M15 5L5 15" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/></svg>
      </button>
    </div>
    <div class="drawer__body">
      <div class="kv">
        <div class="kv__row"><span class="kv__k">Status</span><span class="kv__v">${badge(d.state)}</span></div>
        <div class="kv__row"><span class="kv__k">Progress</span><span class="kv__v">${pct(d)}%</span></div>
        ${d.state === 'downloading' ? `<div class="kv__row"><span class="kv__k">Speed</span><span class="kv__v kv__v--strong">${fmtSpeed(d.speed)}</span></div>` : ''}
        <div class="kv__row"><span class="kv__k">Size</span><span class="kv__v">${fmtBytes(d.bytes)}</span></div>
        <div class="kv__row"><span class="kv__k">Format</span><span class="kv__v">${KIND[d.media.kind] ?? '—'}</span></div>
        <div class="kv__row"><span class="kv__k">Resolution</span><span class="kv__v">${d.resolution}</span></div>
        <div class="kv__row"><span class="kv__k">Codec</span><span class="kv__v">${d.codec}</span></div>
        <div class="kv__row"><span class="kv__k">Duration</span><span class="kv__v">${fmtClock(d.duration)}</span></div>
        <div class="kv__row"><span class="kv__k">SHA-256</span><span class="kv__v">${d.sha256 ?? '—'}</span></div>
        <div class="kv__row"><span class="kv__k">Added</span><span class="kv__v">${fmtWhen(d.createdAt)}</span></div>
        ${d.error ? `<div class="kv__row"><span class="kv__k">Error</span><span class="kv__v" style="color:var(--state-fail-fg)">${esc(d.error)}</span></div>` : ''}
      </div>

      <div class="drawer__section">
        <div class="drawer__label">Pipeline</div>
        ${stepsPanel(d)}
      </div>

      <div class="drawer__section">
        <div class="drawer__label">Artifacts</div>
        ${d.artifacts.length ? d.artifacts.map((a) => `<div class="artifact">${esc(a)}</div>`).join('') : '<div class="muted" style="font-size:13px">None published yet.</div>'}
      </div>

      <div class="drawer__section" style="display:flex;gap:8px;flex-wrap:wrap">
        ${d.state === 'done' ? `<button class="btn btn--sm btn--primary" data-open="${d.id}">${ICON.play} Open file</button>` : ''}
        ${!isSettled(d.state) ? '<button class="btn btn--sm" id="cancelDl">Cancel download</button>' : ''}
        <button class="btn btn--sm btn--ghost">Copy link</button>
      </div>
    </div>`;
  document.body.appendChild(overlay);
  document.body.appendChild(drawer);
  drawer.querySelector('#closeDrawer').addEventListener('click', closeDrawer);
  const cancel = drawer.querySelector('#cancelDl');
  if (cancel) {
    cancel.addEventListener('click', () => {
      d.state = 'cancelled';
      d.speed = 0;
      closeDrawer();
      render();
    });
  }
  drawer.querySelectorAll('[data-open]').forEach((b) => b.addEventListener('click', () => openFile(b.dataset.open)));
}

/* ------------------------------ open a file ------------------------------ */

/* Stands in for "open this downloaded file in the device's default player". */
function openFile(id) {
  const d = state.downloads.find((x) => x.id === id);
  if (!d || d.state !== 'done') return;
  closeFile();
  const local = state.downloadTarget === 'local';
  const overlay = document.createElement('div');
  overlay.className = 'overlay';
  overlay.addEventListener('click', closeFile);
  const modal = document.createElement('div');
  modal.className = 'filemodal';
  modal.innerHTML = `
    <div class="filemodal__head">
      <div class="filemodal__title">${ICON.file}<span>${esc(d.name)}</span></div>
      <button class="btn btn--icon btn--ghost" id="closeFile" aria-label="Close">
        <svg viewBox="0 0 20 20"><path d="M5 5l10 10M15 5L5 15" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/></svg>
      </button>
    </div>
    <div class="player">
      <div class="player__frame">
        <span class="player__play">${ICON.play}</span>
        <span class="player__badge">${KIND[d.media.kind] ?? 'MP4'} · ${d.resolution}</span>
      </div>
      <div class="player__controls">
        <span class="player__time">0:00</span>
        <div class="player__scrub"><span style="width:0%"></span></div>
        <span class="player__time">${fmtClock(d.duration)}</span>
      </div>
    </div>
    <div class="filemodal__meta">
      <div class="kv">
        <div class="kv__row"><span class="kv__k">Size</span><span class="kv__v">${fmtBytes(d.bytes)}</span></div>
        <div class="kv__row"><span class="kv__k">Duration</span><span class="kv__v">${fmtClock(d.duration)}</span></div>
        <div class="kv__row"><span class="kv__k">Codec</span><span class="kv__v">${d.codec}</span></div>
        <div class="kv__row"><span class="kv__k">Saved to</span><span class="kv__v">${local ? '/Users/you/Downloads/ingest' : 's3://vyzorix-media/downloads/'}</span></div>
      </div>
    </div>
    <div class="filemodal__actions">
      <button class="btn btn--primary" id="openExternal">Open in default player</button>
      <button class="btn" id="revealFile">${ICON.folder} Show in folder</button>
      <button class="btn btn--ghost" id="copyPath">Copy path</button>
    </div>`;
  document.body.appendChild(overlay);
  document.body.appendChild(modal);
  modal.querySelector('#closeFile').addEventListener('click', closeFile);
  modal.querySelector('#openExternal').addEventListener('click', () => toast(`Opening ${d.name} in your default player`));
  modal
    .querySelector('#revealFile')
    .addEventListener('click', () => toast(local ? 'Revealed in file manager' : 'Opened bucket in storage console'));
  modal.querySelector('#copyPath').addEventListener('click', () => toast('Path copied'));
}

function closeFile() {
  document.querySelectorAll('.overlay, .filemodal').forEach((el) => el.remove());
}

function closeDrawer() {
  document.querySelectorAll('.overlay, .drawer').forEach((el) => el.remove());
}

/* ------------------------------ toast ------------------------------ */

function toast(message) {
  const el = document.createElement('div');
  el.className = 'toast';
  el.textContent = message;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), 2200);
}

/* ------------------------------ render ------------------------------ */

function render() {
  const [title, subtitle] = VIEW_META[state.view];
  document.getElementById('viewTitle').textContent = title;
  document.getElementById('viewSubtitle').textContent = subtitle;
  document.getElementById('content').innerHTML = `<div class="view is-active">${views[state.view]()}</div>`;
  document.getElementById('navCount').textContent = String(
    state.downloads.filter((d) => isActive(d.state)).length,
  );
  wire();
}

function wire() {
  document.querySelectorAll('tr[data-dl]').forEach((row) =>
    row.addEventListener('click', () => openDrawer(row.dataset.dl)),
  );
  document.querySelectorAll('.dl[data-dl]').forEach((card) =>
    card.addEventListener('click', () => openDrawer(card.dataset.dl)),
  );
  document.querySelectorAll('[data-details]').forEach((b) =>
    b.addEventListener('click', (e) => {
      e.stopPropagation();
      openDrawer(b.dataset.details);
    }),
  );
  document.querySelectorAll('[data-open]').forEach((b) =>
    b.addEventListener('click', (e) => {
      e.stopPropagation();
      openFile(b.dataset.open);
    }),
  );
  document.querySelectorAll('[data-reveal-file]').forEach((b) =>
    b.addEventListener('click', (e) => {
      e.stopPropagation();
      toast(state.downloadTarget === 'local' ? 'Revealed in file manager' : 'Opened bucket in storage console');
    }),
  );
  document.querySelectorAll('[data-goto]').forEach((b) =>
    b.addEventListener('click', (e) => {
      e.stopPropagation();
      go(b.dataset.goto);
    }),
  );

  document.querySelectorAll('[data-storage-tab]').forEach((b) =>
    b.addEventListener('click', () => {
      state.storageTab = b.dataset.storageTab;
      render();
    }),
  );
  document.querySelectorAll('[data-settings-tab]').forEach((b) =>
    b.addEventListener('click', () => {
      state.settingsTab = b.dataset.settingsTab;
      render();
    }),
  );
  document.querySelectorAll('[data-filter]').forEach((b) =>
    b.addEventListener('click', () => {
      state.filter = b.dataset.filter;
      render();
    }),
  );

  document.querySelectorAll('input[name="dl-target"]').forEach((r) =>
    r.addEventListener('change', () => {
      state.downloadTarget = r.value;
      render();
    }),
  );

  document.querySelectorAll('[data-reveal]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const input = document.getElementById(btn.dataset.reveal);
      const hidden = input.value === MASK;
      input.value = hidden ? input.dataset.original ?? '' : MASK;
      btn.textContent = hidden ? 'Hide' : 'Show';
    });
  });

  const probe = document.getElementById('probeBtn');
  if (probe) probe.addEventListener('click', runProbe);
  const start = document.getElementById('startDownload');
  if (start) start.addEventListener('click', startDownload);
  const test = document.getElementById('testStorage');
  if (test) test.addEventListener('click', () => toast('Connection successful — bucket is reachable'));
  const testLoc = document.getElementById('testLocation');
  if (testLoc) testLoc.addEventListener('click', () => toast('Write access confirmed'));
  const saveLoc = document.getElementById('saveLocation');
  if (saveLoc) saveLoc.addEventListener('click', () => toast('Storage location saved'));
  const reconn = document.getElementById('reconnect');
  if (reconn) reconn.addEventListener('click', () => toast('Settings saved'));
  const dark = document.getElementById('pref-dark');
  if (dark) {
    dark.addEventListener('change', () => {
      document.getElementById('themeBtn').click();
      render();
    });
  }
}

function runProbe() {
  const body = document.getElementById('probeBody');
  if (!body) return;
  body.innerHTML = `<div class="kv"><div class="kv__row"><span class="kv__k">Status</span><span class="kv__v muted">Probing…</span></div></div>`;
  setTimeout(() => {
    body.innerHTML = `
      <div class="kv">
        <div class="kv__row"><span class="kv__k">Status</span><span class="kv__v kv__v--strong">Video detected</span></div>
        <div class="kv__row"><span class="kv__k">Container</span><span class="kv__v">MP4 · ISO-BMFF</span></div>
        <div class="kv__row"><span class="kv__k">Range support</span><span class="kv__v">Accepted (parallel download available)</span></div>
        <div class="kv__row"><span class="kv__k">Size</span><span class="kv__v">842 MB</span></div>
        <div class="kv__row"><span class="kv__k">Confidence</span><span class="kv__v kv__v--strong">0.970</span></div>
      </div>
      <div class="note" style="margin-top:16px">${ICON.info}<span>Detection reads the raw bytes, never the file extension or Content-Type header. Non-video payloads are refused before the download begins.</span></div>`;
  }, 650);
}

function startDownload() {
  const url = document.getElementById('f-url')?.value || 'https://cdn.example.com/library/new.mp4';
  const name = document.getElementById('f-name')?.value || url.split('/').pop();
  const id = Math.random().toString(16).slice(2, 8);
  const item = dl(id, url, 'downloading', 1);
  item.name = name;
  item.speed = 12_000_000 + Math.random() * 30_000_000;
  state.downloads.unshift(item);
  go('downloads');
  toast(`${name} queued`);
}

function go(view) {
  state.view = view;
  document.querySelectorAll('.nav__item').forEach((b) => b.classList.toggle('is-active', b.dataset.view === view));
  render();
}

/* ------------------------------ chrome ------------------------------ */

document.querySelectorAll('.nav__item').forEach((btn) =>
  btn.addEventListener('click', () => go(btn.dataset.view)));

document.getElementById('newJobBtn').addEventListener('click', () => go('ingest'));

document.getElementById('refreshBtn').addEventListener('click', () => {
  const dot = document.getElementById('connDot');
  dot.classList.add('is-down');
  setTimeout(() => dot.classList.remove('is-down'), 700);
  render();
  toast('Refreshed');
});

document.getElementById('themeBtn').addEventListener('click', () => {
  const root = document.documentElement;
  const next = root.dataset.theme === 'dark' ? 'light' : 'dark';
  root.dataset.theme = next;
  try {
    localStorage.setItem('ingest.theme', next);
  } catch {
    /* storage unavailable */
  }
});

try {
  const saved = localStorage.getItem('ingest.theme');
  if (saved) document.documentElement.dataset.theme = saved;
} catch {
  /* storage unavailable */
}

document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') {
    closeDrawer();
    closeFile();
  }
});

/* ------------------------------ live simulation ------------------------------ */

// Advance active downloads so progress, speed and the pipeline can be judged in motion.
setInterval(() => {
  let changed = false;
  for (const d of state.downloads) {
    if (d.state !== 'downloading') continue;
    changed = true;
    // jitter the rate so the speed readout looks alive
    d.speed = Math.max(4_000_000, Math.min(60_000_000, d.speed + (Math.random() - 0.5) * 6_000_000));
    d.downloaded = Math.min(d.bytes, d.downloaded + d.speed);
    if (d.downloaded >= d.bytes) {
      if (d.stage < STEPS.length - 1) {
        d.stage += 1;
        d.downloaded = Math.floor(d.bytes * (d.stage / STEPS.length));
      } else {
        d.state = 'done';
        d.speed = 0;
        d.sha256 = '9f2c' + d.id + 'e41ba7c8d0b4f61a';
        d.resolution = '1920 × 1080';
        d.finishedAt = now();
        d.artifacts = [
          `https://cdn.example.com/hls/${d.id}/master.m3u8`,
          `https://cdn.example.com/hls/${d.id}/1080p/index.m3u8`,
          `https://cdn.example.com/hls/${d.id}/720p/index.m3u8`,
        ];
      }
    }
  }
  if (changed && (state.view === 'overview' || state.view === 'downloads' || state.view === 'storage')) render();
}, 1000);

render();
