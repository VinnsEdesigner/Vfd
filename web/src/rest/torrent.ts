/**
 * Torrents: what a magnet or a `.torrent` file contains.
 *
 * A BitTorrent index answers a search with magnets, and a magnet is only a hash
 * and a name — the file list lives in the swarm. A `.torrent` file is the
 * opposite: it *is* the metadata, so its file list, piece size and trackers are
 * all readable here without touching the network.
 *
 * So this resource has two questions:
 *
 *   inspect   given a `.torrent`'s bytes (or a magnet), what is inside?
 *   search    the extractor's job — a torrent index search is `api.extract` with
 *             a `site`, and it lives there because it is the same call a video
 *             search is.
 *
 * A magnet cannot be expanded without a client, so `inspect` on one reports
 * `complete: false` and the UI says so rather than showing an empty file list as
 * if the torrent had no files.
 */

import type { ApiClient } from '../api-client';
import { normalizeTorrentInfo } from '../domain/normalize';
import type { TorrentInfo } from '../domain/browse';
import type { WireTorrentInfo, WireTorrentInspectRequest } from '../domain/wire';

/** Parsing a `.torrent` is local and fast; the timeout is only a backstop. */
const INSPECT_TIMEOUT_MS = 20_000;

export interface TorrentResource {
  /** Read a `.torrent` file's bytes (base64), or a magnet's hash and name. */
  inspect(
    input: { base64?: string; magnet?: string },
    signal?: AbortSignal,
  ): Promise<TorrentInfo>;
}

export function createTorrentResource(client: ApiClient): TorrentResource {
  return {
    async inspect(input, signal) {
      const body: WireTorrentInspectRequest = {};
      if (input.base64) body.base64 = input.base64;
      if (input.magnet) body.magnet = input.magnet;

      const timeout = AbortSignal.timeout(INSPECT_TIMEOUT_MS);
      const combined = signal ? AbortSignal.any([signal, timeout]) : timeout;

      const wire = await client.transport.request<WireTorrentInfo>({
        method: 'POST',
        path: '/api/v1/torrent/inspect',
        body,
        signal: combined,
      });
      return normalizeTorrentInfo(wire);
    },
  };
}
