/**
 * System hooks: health, capabilities, storage view.
 */

import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useMediaApi } from './provider';
import { queryKeys } from './query-keys';

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
