/**
 * Reading a torrent's metadata, as a mutation.
 *
 * An action rather than a query: it happens when the user opens a `.torrent` or
 * pastes a magnet, and its answer is about *that* input. React Query retrying it
 * would re-parse bytes that will parse the same way every time, and caching it
 * would key on bytes nobody wants cached.
 *
 * The same hook serves both inputs — a `.torrent`'s bytes or a magnet's hash —
 * because the server takes either and answers one shape.
 */

import { useMutation } from '@tanstack/react-query';
import { useMediaApi } from './provider';
import type { TorrentInfo } from '../domain/browse';

export interface TorrentInspectVariables {
  /** The bytes of a `.torrent`, base64. */
  base64?: string;
  /** A magnet link, when there is no file. */
  magnet?: string;
  signal?: AbortSignal;
}

export function useTorrentInspect() {
  const { api } = useMediaApi();
  return useMutation<TorrentInfo, unknown, TorrentInspectVariables>({
    mutationFn: ({ signal, ...input }) => api.torrent.inspect(input, signal),
    retry: false,
  });
}
