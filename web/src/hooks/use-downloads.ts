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
    // The list is the only place progress is visible, so it has to poll while
    // something is moving — otherwise a download started from the UI sits at
    // "running 0%" until the user refreshes by hand. It stops once everything
    // has settled, so an idle tab makes no requests.
    refetchInterval: (query) => {
      const list = query.state.data;
      if (!list || list.length === 0) return false;
      // A conversion runs after the download has settled, so it is a second
      // reason to keep polling: the viewer learns the MP4 is ready from here.
      return list.some((d) => !isTerminal(d.state) || d.conversion.state === 'converting')
        ? 1_000
        : false;
    },
  });
}

export function useDownload(id: string | null) {
  const { api } = useMediaApi();
  return useQuery({
    queryKey: id ? queryKeys.download(id) : queryKeys.download('__none__'),
    queryFn: ({ signal }) => api.downloads.get(id as string, signal),
    enabled: Boolean(id),
    // Poll while the download is in flight, and while a conversion it was
    // asked to run is still going; stop once both have settled.
    refetchInterval: (query) => {
      const data = query.state.data;
      if (!data) return 1_000;
      if (data.conversion.state === 'converting') return 1_000;
      return isTerminal(data.state) ? false : 1_000;
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

/** Pause stops the transfer but keeps everything, so it can be resumed. */
export function usePauseDownload() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (id: string) => api.downloads.pause(id),
    onSuccess: (download) => {
      queryClient.setQueryData(queryKeys.download(download.id), download);
      void queryClient.invalidateQueries({ queryKey: queryKeys.downloadList() });
    },
  });
}

export function useResumeDownload() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (id: string) => api.downloads.resume(id),
    onSuccess: (download) => {
      queryClient.setQueryData(queryKeys.download(download.id), download);
      void queryClient.invalidateQueries({ queryKey: queryKeys.downloadList() });
    },
  });
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

/**
 * Convert a container the browser cannot play into MP4.
 *
 * The mutation only starts the job; the result is followed through the detail
 * query, which keeps polling while `conversion.state` is `converting`. The
 * seeded cache entry means the viewer shows "converting" at once, before the
 * first poll comes back.
 */
export function useConvertDownload() {
  const { api } = useMediaApi();
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (id: string) => api.downloads.convert(id),
    onSuccess: (download) => {
      queryClient.setQueryData(queryKeys.download(download.id), download);
      void queryClient.invalidateQueries({ queryKey: queryKeys.downloadList() });
    },
  });
}
