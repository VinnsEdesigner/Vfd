/**
 * ui — the component layer.
 *
 * Only `App` is meant to be consumed from outside; the rest is exported for
 * tests and for composing new pages.
 */
export { App } from './App';
export { BootGate } from './BootGate';
export { ToastProvider, useToast } from './feedback';
export type { ToastApi, ToastTone } from './feedback';
export type { BootGateProps } from './BootGate';
export type { AppProps, ViewId } from './App';
