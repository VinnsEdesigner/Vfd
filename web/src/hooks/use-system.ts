/**
 * System hooks: health, capabilities, storage view.
 */

import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useMediaApi } from './provider';
import { queryKeys } from './query-keys';
import type { EgressUpdate } from '../domain/media';

export function useHealth() {
  const { api } = useMediaApi();
  return useQuery({
    queryKey: queryKeys.health(),
    queryFn: ({ signal }) => api.system.health(signal),
    refetchInterval: 15_000,
  });
}

export function useCapabilities() {
  const { api } = useMediaApi();
  return useQuery({
    queryKey: queryKeys.capabilities(),
    queryFn: ({ signal }) => api.system.capabilities(signal),
    staleTime: 5 * 60_000,
    /*
     * Capabilities describe the *Vyzor*, and Vyzor can change underneath a
     * console that is already open — restarted with rendering turned on, or with
     * live sessions, or pointed at a different host by a reverse proxy. The app
     * disables refetch-on-focus globally (a background refetch should not
     * replace content with a skeleton), but for this query that default is
     * wrong: without a refetch a long-lived tab keeps the answer it first got
     * and never notices Vyzor can now do more, which shows up as a feature
     * that stays hidden after it becomes available.
     *
     * The query is one small GET, so re-asking on focus and on a slow interval is
     * cheap next to being wrong about what Vyzor can do.
     */
    refetchOnWindowFocus: true,
    refetchInterval: 60_000,
  });
}

export function useSystemStorage() {
  const { api } = useMediaApi();
  return useQuery({
    queryKey: queryKeys.systemStorage(),
    queryFn: ({ signal }) => api.system.systemStorage(signal),
    refetchInterval: 10_000,
  });
}

export function useStorageConfig() {
  const { api } = useMediaApi();
  return useQuery({
    queryKey: queryKeys.storageConfig(),
    queryFn: ({ signal }) => api.system.storageConfig(signal),
  });
}

/**
 * The cookies and proxy Vyzor uses to reach a site that refuses this host.
 *
 * A query, not a mutation: it is a small GET, and the panel shows what is
 * currently set, so it should read fresh when the settings tab is opened.
 */
export function useEgressSettings() {
  const { api } = useMediaApi();
  return useQuery({
    queryKey: queryKeys.egressSettings(),
    queryFn: ({ signal }) => api.system.egressSettings(signal),
  });
}

/** Set cookies and/or proxy. The saved answer replaces the cached report. */
export function useSetEgress() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (next: EgressUpdate) => api.system.setEgress(next),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: queryKeys.egressSettings() });
    },
  });
}

/** Point Vyzor at a different download directory. */
export function useSetStorageLocation() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (dir: string) => api.system.setLocation(dir),
    onSuccess: () => {
      void queryClient.invalidateQueries({ queryKey: queryKeys.systemStorage() });
    },
  });
}
