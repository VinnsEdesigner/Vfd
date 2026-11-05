/**
 * Entry point: mounts the app, wires the provider, and imports the styles.
 *
 * The provider is created here (not inside `ui`) so `App` stays a pure
 * component that can be rendered in tests with any transport.
 */

import { useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { useQueryClient } from '@tanstack/react-query';
import { MediaApiProvider, useConnectionStore, useHealth } from '../hooks';
import { App, BootGate, ToastProvider } from '../ui';
import { registerServiceWorker } from '../platform/service-worker';

import '../ui/styles/tokens.css';
import '../ui/styles/base.css';
import '../ui/styles/components.css';

document.body.classList.add('app-shell');

// Registered before the first paint so the installed app is offline-capable from
// the start. Failures are not fatal — the app works without a worker, it just
// cannot open without a network.
void registerServiceWorker().then((state) => {
  if (state.status === 'failed') {
    console.warn('service worker registration failed:', state.detail);
  }
});

/**
 * The single API provider, driven by the connection store.
 *
 * It lives here rather than inside `ui` so `App` stays a pure component that can
 * be rendered in tests against any transport. Following the store is what makes
 * Settings → Connection real: changing the endpoint or token there rebuilds the
 * client, so every request goes to the new Vyzor without a reload. A deployed
 * console can therefore point at whichever Vyzor the user runs.
 */
function ApiConnection({ children }: { children: React.ReactNode }) {
  const baseUrl = useConnectionStore((s) => s.baseUrl);
  const authToken = useConnectionStore((s) => s.authToken);
  return (
    <MediaApiProvider baseUrl={baseUrl} authToken={authToken}>
      {/* Inside the provider, because the client is created by it — calling
          useQueryClient above it would have nothing to return. */}
      <DropStaleVyzorData endpoint={`${baseUrl}\u0000${authToken}`}>{children}</DropStaleVyzorData>
    </MediaApiProvider>
  );
}

/**
 * Drops everything cached when the endpoint or token changes.
 *
 * Every cached answer belongs to Vyzor it came from. Without this, one
 * outlives its endpoint: a capability fetched while the console was still
 * pointing at its own origin — which is not a Vyzor — stays "fresh" for its
 * whole stale window, and the UI keeps reporting what the wrong server said.
 * Changing the endpoint in Settings must mean starting over, not blending two
 * Vyzors' answers.
 */
function DropStaleVyzorData({
  endpoint,
  children,
}: {
  endpoint: string;
  children: React.ReactNode;
}) {
  const queryClient = useQueryClient();
  const previous = useRef(endpoint);
  useEffect(() => {
    if (previous.current !== endpoint) {
      previous.current = endpoint;
      queryClient.clear();
    }
  }, [endpoint, queryClient]);
  return <>{children}</>;
}

/**
 * Holds the splash until Vyzor answers its health check, so the shell
 * never paints against a server that is not up yet.
 */
function Boot() {
  const health = useHealth();
  // Do not block forever: if the probe errors, let the app render so the user
  // can see (and fix) the connection in Settings.
  const [timedOut, setTimedOut] = useState(false);
  useEffect(() => {
    const t = window.setTimeout(() => setTimedOut(true), 4000);
    return () => window.clearTimeout(t);
  }, []);

  const ready = health.isSuccess || health.isError || timedOut;
  return (
    <BootGate ready={ready}>
      <App />
    </BootGate>
  );
}

const container = document.getElementById('root');
if (!container) throw new Error('#root not found');

createRoot(container).render(
  <ApiConnection>
    <ToastProvider>
      <Boot />
    </ToastProvider>
  </ApiConnection>,
);
