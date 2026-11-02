/**
 * Presentation provider: owns the React Query client and the RestApi instance,
 * and exposes both to hooks. UI components mount this and never see the data
 * layer directly.
 */

import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { createContext, useContext, useMemo, useState, type ReactNode } from 'react';
import { createRestApi, type RestApi, type TransportConfig } from '../rest';

interface MediaApiContextValue {
  api: RestApi;
  queryClient: QueryClient;
}

const MediaApiContext = createContext<MediaApiContextValue | null>(null);

export interface MediaApiProviderProps {
  children: ReactNode;
  /** Server base URL, e.g. http://127.0.0.1:8787 */
  baseUrl: string;
  authToken?: string;
  queryClient?: QueryClient;
  fetchImpl?: typeof fetch;
}

export function MediaApiProvider({
  children,
  baseUrl,
  authToken,
  queryClient,
  fetchImpl,
}: MediaApiProviderProps) {
  const [defaultClient] = useState(
    () =>
      new QueryClient({
        defaultOptions: {
          queries: {
            retry: 1,
            staleTime: 2_000,
            refetchOnWindowFocus: false,
          },
        },
      }),
  );

  const value = useMemo<MediaApiContextValue>(() => {
    const config: TransportConfig = { baseUrl };
    if (authToken) config.authToken = authToken;
    if (fetchImpl) config.fetchImpl = fetchImpl;
    return {
      api: createRestApi(config),
      queryClient: queryClient ?? defaultClient,
    };
  }, [baseUrl, authToken, queryClient, defaultClient, fetchImpl]);

  return (
    <MediaApiContext.Provider value={value}>
      <QueryClientProvider client={value.queryClient}>{children}</QueryClientProvider>
    </MediaApiContext.Provider>
  );
}

export function useMediaApi(): MediaApiContextValue {
  const ctx = useContext(MediaApiContext);
  if (!ctx) {
    throw new Error('useMediaApi must be used within a <MediaApiProvider>');
  }
  return ctx;
}
