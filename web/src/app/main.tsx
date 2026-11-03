/**
 * Entry point: mounts the app, wires the provider, and imports the styles.
 *
 * The provider is created here (not inside `ui`) so `App` stays a pure
 * component that can be rendered in tests with any transport.
 */

import { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { MediaApiProvider, useHealth } from '../hooks';
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
 * Holds the splash until the engine answers its health check, so the shell
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
  <MediaApiProvider baseUrl={import.meta.env.VITE_API_BASE_URL ?? 'http://127.0.0.1:8787'}>
    <ToastProvider>
      <Boot />
    </ToastProvider>
  </MediaApiProvider>,
);
