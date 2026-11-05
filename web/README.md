# web — the Vfd console

The front end for Vyzor. A React + TypeScript single-page app, built with
Vite, deployed as a static bundle, and pointed at a Vyzor with
`VITE_API_BASE_URL` (or at runtime, from **Settings → Connection**).

## Layers

The data/presentation split is enforced by lint, not by convention.

```
src/
  api-client/   transport: fetch, JSON, errors. Knows nothing about the app.
  domain/       pure models, wire types (wire.ts), normalizers
  rest/         typed resources (downloads, media, browse, system) over the transport
  hooks/        React Query hooks + provider — the UI's entry point
  stores/       client state (zustand), framework-agnostic
  ui/           components, pages and styles
```

### Dependency rules (enforced by ESLint)

| Layer | May import |
|---|---|
| `ui` | `ui`, `hooks`, `stores` — **never** `domain`, `rest`, `api-client` |
| `hooks` | `hooks`, `rest`, `domain`, `stores` |
| `stores` | `stores`, `api-client`, `domain` |
| `rest` | `rest`, `api-client`, `domain` |
| `api-client` | `api-client`, `domain` |
| `domain` | `domain` only — pure, zero dependencies |

The rule that matters: **UI can only reach the data layer through `hooks` and
`stores`.** If the server changes, only `domain/normalize.ts` and `rest/` change;
components are untouched.

Enforced in two places:

- `eslint.config.js` — the `boundaries/element-types` rule.
- `tests/boundaries.test.ts` — the same rule exercised as a unit test, so
  `npm test` fails on a violation too.

## Commands

```bash
npm install
npm run typecheck    # tsc --noEmit
npm run lint         # eslint, includes boundary enforcement
npm test             # vitest: domain + presentation + integration + boundaries
npm run dev          # vite dev server
npm run build        # static build to dist/
```

The integration suite starts the **real Vyzor binary** (built from
`../media-ingest-c`) on an ephemeral port and drives it through the actual data
layer, so the wire contract is verified rather than assumed. Build the server
first:

```bash
cd ../media-ingest-c && make
```

## Design notes

- **Secrets never persist.** Storage credentials live in `stores/storage-store.ts`
  in memory only and travel as request headers. There is a test asserting they
  never reach `localStorage`.
- **Wire types are separate from domain types.** `wire.ts` mirrors Vyzor's
  JSON exactly (snake_case, nullable fields); `normalize.ts` is the only bridge.
  A server rename is a one-file change.
- **Unknown values are treated conservatively.** An unrecognised download state
  normalises to `failed` rather than being trusted; an unrecognised media kind
  becomes `unknown`.
- **The live page is a session, not a query.** `useLiveSession` owns a WebSocket
  and its lifetime; it heartbeats so a backgrounded tab is not mistaken for a
  gone one, and it remembers the page you were on so a reconnect resumes there.
