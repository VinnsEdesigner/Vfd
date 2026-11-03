/**
 * Service worker registration.
 *
 * Registration is best-effort and never blocks the app: an unsupported browser,
 * a non-secure context, or a dev server all simply skip it. The result is
 * reported so the UI can say whether offline actually works rather than
 * assuming it does.
 */

export type ServiceWorkerStatus =
  | 'unsupported'
  | 'registered'
  | 'failed'
  | 'disabled';

export interface ServiceWorkerState {
  status: ServiceWorkerStatus;
  /** Why, when it is not simply unsupported. */
  detail?: string;
}

/** True when this environment can run a service worker at all. */
export function serviceWorkerSupported(): boolean {
  return (
    typeof navigator !== 'undefined' &&
    'serviceWorker' in navigator &&
    typeof window !== 'undefined' &&
    window.isSecureContext === true
  );
}

export async function registerServiceWorker(): Promise<ServiceWorkerState> {
  if (typeof navigator === 'undefined' || !('serviceWorker' in navigator)) {
    return { status: 'unsupported', detail: 'This browser has no service worker support.' };
  }
  if (typeof window !== 'undefined' && window.isSecureContext !== true) {
    return {
      status: 'unsupported',
      detail: 'Offline needs a secure (https) connection.',
    };
  }
  try {
    await navigator.serviceWorker.register('/sw.js', { scope: '/' });
    return { status: 'registered' };
  } catch (e) {
    return {
      status: 'failed',
      detail: e instanceof Error ? e.message : 'Registration failed.',
    };
  }
}

/**
 * Ask the worker to keep a stored media URL available offline. Resolves to
 * whether it worked; a failure is not fatal, the media is still on disk.
 */
export async function cacheMediaOffline(url: string): Promise<boolean> {
  if (typeof navigator === 'undefined' || !navigator.serviceWorker?.controller) return false;
  const controller = navigator.serviceWorker.controller;
  return new Promise<boolean>((resolve) => {
    const timer = window.setTimeout(() => {
      navigator.serviceWorker.removeEventListener('message', onMessage);
      resolve(false);
    }, 15_000);
    const onMessage = (event: MessageEvent) => {
      const data = event.data as { type?: string; ok?: boolean } | null;
      if (data?.type !== 'cache-media-done') return;
      window.clearTimeout(timer);
      navigator.serviceWorker.removeEventListener('message', onMessage);
      resolve(Boolean(data.ok));
    };
    navigator.serviceWorker.addEventListener('message', onMessage);
    controller.postMessage({ type: 'cache-media', url });
  });
}
