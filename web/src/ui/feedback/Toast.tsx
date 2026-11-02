/**
 * Toast — transient feedback for an action's outcome.
 *
 * Lives in the presentation layer because a toast is pure UI: it reports what
 * already happened rather than driving anything.
 *
 * Accessibility: success/info announce politely (`role="status"`), errors
 * assertively (`role="alert"`), so a failure is not missed by a screen reader.
 * Timers pause on hover and on focus so a long error message can be read.
 */

import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react';
import { IconCheck, IconX, IconInfo } from '../primitives/Icons';

export type ToastTone = 'success' | 'error' | 'info';

export interface ToastRecord {
  id: string;
  tone: ToastTone;
  title: string;
  description?: string;
  /** ms before auto-dismiss; 0 keeps it until dismissed. */
  duration: number;
}

export interface ToastApi {
  success: (title: string, description?: string) => void;
  error: (title: string, description?: string) => void;
  info: (title: string, description?: string) => void;
  dismiss: (id: string) => void;
}

const ToastContext = createContext<ToastApi | null>(null);

let seq = 0;
const nextId = () => `t${++seq}`;

const DEFAULT_DURATION: Record<ToastTone, number> = {
  success: 3500,
  info: 4000,
  // Errors linger: the user usually needs to read and act on them.
  error: 7000,
};

export function ToastProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<ToastRecord[]>([]);

  const dismiss = useCallback((id: string) => {
    setToasts((list) => list.filter((t) => t.id !== id));
  }, []);

  const push = useCallback(
    (tone: ToastTone, title: string, description?: string) => {
      const record: ToastRecord = {
        id: nextId(),
        tone,
        title,
        duration: DEFAULT_DURATION[tone],
        ...(description !== undefined ? { description } : {}),
      };
      setToasts((list) => [...list, record]);
    },
    [],
  );

  const api = useMemo<ToastApi>(
    () => ({
      success: (t, d) => push('success', t, d),
      error: (t, d) => push('error', t, d),
      info: (t, d) => push('info', t, d),
      dismiss,
    }),
    [push, dismiss],
  );

  return (
    <ToastContext.Provider value={api}>
      {children}
      <ToastViewport toasts={toasts} onDismiss={dismiss} />
    </ToastContext.Provider>
  );
}

/** Throws when used outside the provider — a silent no-op would hide bugs. */
export function useToast(): ToastApi {
  const ctx = useContext(ToastContext);
  if (!ctx) throw new Error('useToast must be used within a <ToastProvider>');
  return ctx;
}

interface ToastViewportProps {
  toasts: ToastRecord[];
  onDismiss: (id: string) => void;
}

export function ToastViewport({ toasts, onDismiss }: ToastViewportProps) {
  if (toasts.length === 0) return null;
  return (
    <div className="toasts" aria-live="polite">
      {toasts.map((t) => (
        <ToastItem key={t.id} toast={t} onDismiss={onDismiss} />
      ))}
    </div>
  );
}

function ToastItem({ toast, onDismiss }: { toast: ToastRecord; onDismiss: (id: string) => void }) {
  const [paused, setPaused] = useState(false);
  const [progress, setProgress] = useState(1);
  const remainingRef = useRef(toast.duration);

  // Auto-dismiss, ticking a progress bar so the timeout is visible rather than
  // something that happens unexpectedly.
  useEffect(() => {
    if (toast.duration <= 0 || paused) return;
    const startedAt = Date.now();
    const startedRemaining = remainingRef.current;

    const tick = window.setInterval(() => {
      const elapsed = Date.now() - startedAt;
      const left = startedRemaining - elapsed;
      if (left <= 0) {
        window.clearInterval(tick);
        onDismiss(toast.id);
        return;
      }
      setProgress(left / toast.duration);
    }, 50);

    return () => {
      window.clearInterval(tick);
      remainingRef.current = Math.max(0, startedRemaining - (Date.now() - startedAt));
    };
  }, [paused, toast.duration, toast.id, onDismiss]);

  const isError = toast.tone === 'error';

  return (
    <div
      className={`toast toast--${toast.tone}`}
      role={isError ? 'alert' : 'status'}
      aria-live={isError ? 'assertive' : 'polite'}
      onMouseEnter={() => setPaused(true)}
      onMouseLeave={() => setPaused(false)}
      onFocus={() => setPaused(true)}
      onBlur={() => setPaused(false)}
    >
      <span className="toast__icon" aria-hidden="true">
        {toast.tone === 'success' ? <IconCheck /> : toast.tone === 'error' ? <IconX /> : <IconInfo />}
      </span>

      <div className="toast__body">
        <div className="toast__title">{toast.title}</div>
        {toast.description !== undefined && (
          <div className="toast__desc">{toast.description}</div>
        )}
      </div>

      <button
        type="button"
        className="toast__close"
        aria-label="Dismiss"
        onClick={() => onDismiss(toast.id)}
      >
        <IconX />
      </button>

      {toast.duration > 0 && (
        <span className="toast__timer" aria-hidden="true">
          <span className="toast__timer-fill" style={{ transform: `scaleX(${progress})` }} />
        </span>
      )}
    </div>
  );
}
