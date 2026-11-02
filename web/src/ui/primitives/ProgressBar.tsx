/**
 * ProgressBar — a track plus a percentage readout.
 *
 * Used inside tables where space is tight. The richer in-progress presentation
 * (with transfer rate) is `DownloadCard`, which composes its own bar.
 */

export type ProgressTone = 'running' | 'done' | 'failed';

export interface ProgressBarProps {
  /** 0..1. Clamped, and safe when the total is unknown. */
  fraction: number;
  tone?: ProgressTone;
  /** Show the numeric percentage at the right. */
  showValue?: boolean;
}

export function ProgressBar({ fraction, tone = 'running', showValue = true }: ProgressBarProps) {
  const clamped = Math.min(Math.max(Number.isFinite(fraction) ? fraction : 0, 0), 1);
  const pct = Math.round(clamped * 100);
  const fillCls = tone === 'done' ? 'progress__fill progress__fill--done'
    : tone === 'failed' ? 'progress__fill progress__fill--failed'
    : 'progress__fill';

  return (
    <div className="progress">
      <div
        className="progress__track"
        role="progressbar"
        aria-valuenow={pct}
        aria-valuemin={0}
        aria-valuemax={100}
      >
        <div className={fillCls} style={{ width: `${pct}%` }} />
      </div>
      {showValue && <span className="progress__value">{pct}%</span>}
    </div>
  );
}
