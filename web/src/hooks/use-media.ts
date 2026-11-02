/**
 * Media hooks: probe a URL and sniff raw bytes.
 */

import { useMutation } from '@tanstack/react-query';
import { useMediaApi } from './provider';

export interface ProbeVariables {
  url: string;
  /** Lets the engine turn manifest BANDWIDTH into real quality sizes. */
  durationSeconds?: number;
}

export function useProbe() {
  const { api } = useMediaApi();
  return useMutation({
    mutationFn: ({ url, durationSeconds }: ProbeVariables) =>
      api.media.probe(url, durationSeconds),
  });
}

export function useDetectBytes() {
  const { api } = useMediaApi();
  return useMutation({
    mutationFn: (bytes: ArrayBuffer | Uint8Array) => api.media.detect(bytes),
  });
}
