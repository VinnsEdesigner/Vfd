# media-web

The front end for the `media-ingest` pipeline. Structured in strict layers, with
the data/presentation split enforced by lint rather than by convention.

No UI is implemented yet — this is the data and presentation foundation.

## Layers

```
src/
  api-client/   transport: fetch, JSON, errors. Knows nothing about the app.
  domain/       pure models (media.ts), wire types (wire.ts), normalizers
  rest/         typed resources (jobs, media, system) over the transport
  hooks/        React Query hooks + provider — the UI's entry point
  stores/       client state (zustand), framework-agnostic
  ui/           components (empty for now)
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

This is enforced in two places:

- `eslint.config.js` — the `boundaries/element-types` rule.
- `tests/boundaries.test.ts` — the same rule exercised as a unit test, so
  `npm test` fails on a violation too.

## Commands

```bash
npm install
npm run typecheck    # tsc --noEmit
npm run lint         # eslint, includes boundary enforcement
npm test             # vitest: domain + presentation + integration + boundaries
```

The integration suite starts the **real C server** (built from
`../media-ingest-c`) on an ephemeral port and drives it through the actual data
layer, so the wire contract is verified rather than assumed. Build the server
first:

```bash
cd ../media-ingest-c && make
```

## Usage sketch (for when the UI lands)

```tsx
import { MediaApiProvider, useJobs, useCreateJob } from './hooks';

<MediaApiProvider baseUrl="http://127.0.0.1:8787" authToken={token}>
  <JobsPage />
</MediaApiProvider>;

function JobsPage() {
  const { data: jobs } = useJobs();          // presentation layer
  const create = useCreateJob();
  // ...render; no imports from rest/ or domain/ here
}
```

## Design notes

- **Secrets never persist.** Storage credentials live in `stores/storage-store.ts`
  in memory only and travel as request headers. There is a test asserting they
  never reach `localStorage`.
- **Wire types are separate from domain types.** `wire.ts` mirrors the C
  server's JSON exactly (snake_case, nullable fields); `normalize.ts` is the only
  bridge. A server rename is a one-file change.
- **Unknown values are treated conservatively.** An unrecognised job state
  normalises to `failed` rather than being trusted; an unrecognised media kind
  becomes `unknown`.
