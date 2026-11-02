/**
 * System hooks: health, capabilities, storage view.
 */

import { useQuery } from '@tanstack/react-query';
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
