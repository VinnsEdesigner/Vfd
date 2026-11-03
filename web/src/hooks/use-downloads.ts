/**
 * Download hooks. Business logic lives here: polling cadence, cache invalidation,
 * and the "is this download still moving" decision. UI components consume the
 * resulting state and never call the REST layer themselves.
 */

import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useMediaApi } from './provider';
import { queryKeys } from './query-keys';
import { isTerminal, type CreateDownloadInput, type StorageCredentials } from '../domain/media';

export function useDownloads() {
  const { api } = useMediaApi();
  return useQuery({
    queryKey: queryKeys.downloadList(),
    queryFn: ({ signal }) => api.downloads.list(signal),
  });
}

export function useDownload(id: string | null) {
  const { api } = useMediaApi();
  return useQuery({
    queryKey: id ? queryKeys.download(id) : queryKeys.download('__none__'),
    queryFn: ({ signal }) => api.downloads.get(id as string, signal),
    enabled: Boolean(id),
    // Poll while the download is in flight; stop once it settles.
    refetchInterval: (query) => {
      const state = query.state.data?.state;
      if (!state) return 1_000;
      return isTerminal(state) ? false : 1_000;
    },
  });
}

export interface CreateDownloadVariables {
  input: CreateDownloadInput;
  credentials?: StorageCredentials;
}

export function useCreateDownload() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: ({ input, credentials }: CreateDownloadVariables) =>
      api.downloads.create(input, credentials),
    onSuccess: (download) => {
      queryClient.setQueryData(queryKeys.download(download.id), download);
      void queryClient.invalidateQueries({ queryKey: queryKeys.downloadList() });
    },
  });
}

export interface RemoveDownloadVariables {
  id: string;
  /** Also delete the artifacts from disk. Irreversible. */
  removeFiles: boolean;
}

export function useRemoveDownload() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: ({ id, removeFiles }: RemoveDownloadVariables) =>
      api.downloads.remove(id, removeFiles),
    onSuccess: (_result, { id }) => {
      queryClient.removeQueries({ queryKey: queryKeys.download(id) });
      void queryClient.invalidateQueries({ queryKey: queryKeys.downloadList() });
    },
  });
}

export function useCancelDownload() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (id: string) => api.downloads.cancel(id),
    onSuccess: (_void, id) => {
      void queryClient.invalidateQueries({ queryKey: queryKeys.download(id) });
      void queryClient.invalidateQueries({ queryKey: queryKeys.downloadList() });
    },
  });
}

export interface RenameDownloadVariables {
  id: string;
  name: string;
}

/** Rename keeps the bytes; it only changes what the item is called. */
export function useRenameDownload() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: ({ id, name }: RenameDownloadVariables) => api.downloads.rename(id, name),
    onSuccess: (download) => {
      queryClient.setQueryData(queryKeys.download(download.id), download);
      void queryClient.invalidateQueries({ queryKey: queryKeys.downloadList() });
    },
  });
}

export interface MoveDownloadVariables {
  id: string;
  folder: string;
}

/** Move relocates the item's directory inside the library. */
export function useMoveDownload() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: ({ id, folder }: MoveDownloadVariables) => api.downloads.move(id, folder),
    onSuccess: (download) => {
      queryClient.setQueryData(queryKeys.download(download.id), download);
      void queryClient.invalidateQueries({ queryKey: queryKeys.downloadList() });
    },
  });
}
