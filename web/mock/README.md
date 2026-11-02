# Media Ingest — console mock

A clean, professional UI mock for the ingest pipeline. Built as a design
reference **before** real components, so layout and styling can be agreed
cheaply.

No build step, no dependencies. Open `index.html` directly, or serve the folder:

```bash
cd web/mock && python3 -m http.server 12000
# → http://127.0.0.1:12000/
```

## Design principles

Deliberately plain and product-grade — the kind of console an operations team
would use all day. Nothing decorative competes with the data.

- **Purely neutral surfaces.** Background and all greys are true neutrals
  (R = G = B, zero colour cast) — `#ededed` light / `#0e0e0e` dark. White cards,
  hairline borders, 1px shadows. No gradients, no texture, no glow.
- **One accent colour: rose.** `#e11d48` (rose 600) in light, `#fb7185`
  (rose 400) in dark — 400 is needed on dark for contrast. Rose marks only the
  primary action and the active nav item.
- **Status is white / gray / red — nothing else.** A job is finished (white),
  in progress (gray), or broken (red). Green appears only as a small muted
  confirmation dot (`#15803d` light / `#4b9c6e` dark), never as a fill or a badge.
- **Geist + Geist Mono.** A neutral, well-spaced UI sans. All identifiers and
  numbers use the mono so figures align and don't shift as they tick.
- **Light and dark are both first-class.** Real token sets for each, not an
  inverted filter. Toggle in the header; the choice persists.

## Views

| View | Contents |
|---|---|
| **Overview** | Four stat cards, recent downloads, live pipeline view for the active download |
| **Downloads** | Every download with status, progress bar, live transfer rate and format |
| **New ingest** | Source form, pipeline step switches, live probe result panel |
| **Storage** | Tabs: **Files**, Object storage, CDN & delivery |
| **Settings** | Tabs: Connection, **Storage location**, Engine |

### Storage → Files

The download library, split in two:

- **In progress** — a card per active download with a bar pinned to its bottom
  edge. The bar carries the percentage on the left and the live transfer rate on
  the right, so both readouts travel with the progress itself.
- **Library** — a filterable table (All / Ongoing / Finished / Failed) with
  file name, status, size, storage location and an **Open** action on anything
  finished. Open shows a player sheet with the file's metadata and
  "Open in default player" / "Show in folder".

### Settings → Storage location

Choose **This device** or **Cloud storage**. The choice is not cosmetic — it
switches the path shown on every file row (`/Users/you/Downloads/ingest` vs
`s3://vyzorix-media/downloads`), the fields below it, and the summary line on
the Files tab.

Interactive:

- **New ingest** in the header (or the sidebar item) → the form.
- **Probe only** → fills the probe panel with a detected-media readout.
- **Start download** → queues a download and jumps to Downloads.
- Any **download row or card** → a detail drawer with the pipeline breakdown,
  artifacts and sha-256. Close with the button or `Esc`.
- **Open** on a finished file → the player sheet.
- **Storage location** radio → switches local vs cloud everywhere it is shown.
- **Show/Hide** on any credential field.
- **Theme toggle** in the header.

Active downloads advance on a 1s timer, with the transfer rate jittering, so
progress, speed and the pipeline steps can all be judged in motion.

## Accessibility

Every text/background pair was measured against WCAG AA (4.5:1 for body text).
Several first-draft values failed and were corrected:

| Token | Before | After |
|---|---|---|
| `--text-3` (light) | 3.70 | **5.33** |
| `--text-3` on `--bg` (light) | 3.70 | **4.55** |
| `--state-fail-fg` on its tint | 4.81 | **5.91** |
| `--ok` on its soft tint | — | **4.61** |

Dark mode is checked separately and passes (lowest pair: `--text-3` at 5.23).
Status badges are measured against their own backgrounds, not just against
white — a red chip on a red tint is where these usually fail.

The rose accent is verified both as text on white (4.70) and as white text on
the rose fill (4.70).

## Layout

An app shell, not a scrolling page: `body` is `100vh` with `overflow: hidden`,
and the main column is a two-row grid (`var(--topbar-h)` + `1fr`). The header
and sidebar therefore cannot scroll away — only `.content` scrolls. This is
more robust than `position: sticky`, which silently does nothing if the page
never overflows.

## Tuning it

Everything visual is in `styles.css`. The token block at the top is the whole
design system:

```css
--accent: #e11d48;   /* rose 600; try #be123c (700) or #f43f5e (500) */
--bg: #ededed;       /* keep R=G=B for a true neutral */
--r-lg: 12px;        /* 8px = tighter, 16px = softer */
--sidebar-w: 236px;
--topbar-h: 65px;
```

For a denser, more data-heavy feel, drop the table cell padding from
`13px 20px` to `9px 16px` and the stat font-size from `28px` to `24px`.

## Mapping to the real app

The mock is shaped to match the layers already built, so porting is mechanical:

- Job objects mirror `src/domain/media.ts` — `id`, `url`, `state`, `progress`,
  `media`, `sha256`, `artifacts`, `error`.
- Views correspond to what `hooks/` exposes: `useDownloads`, `useCreateDownload`,
  `useProbe`, `useCapabilities`, `useStorageConfig`.
- **Naming note:** the mock calls these *downloads*, not *jobs*. The current
  `web/src/` layer still uses `jobs` (`JobsResource`, `useJobs`, `POST /api/v1/jobs`).
  Porting the UI means renaming that through the data layer — one rename in
  `domain/media.ts`, `rest/jobs.ts`, `hooks/use-jobs.ts` and the server routes —
  rather than an alias in the components.
- Nothing here is imported by `src/`. It is a standalone reference, and stays
  useful as the design spec after real components land.

When it's ported, the components live in `src/ui/` and may only import from
`hooks/` and `stores/` — enforced by ESLint, not convention.
