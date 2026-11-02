/**
 * StatCard — one headline number with a unit and a trend footer.
 *
 * `value` is pre-formatted by the caller (hooks owns formatting), so this stays
 * a dumb presentational component.
 */

import type { ReactNode } from 'react';

export type TrendDirection = 'up' | 'down' | 'flat';

export interface StatCardProps {
  label: string;
  value: string;
  unit?: string;
  /** e.g. "▲ 2" */
  trend?: string;
  trendDirection?: TrendDirection;
  footnote?: string;
}

export function StatCard({
  label,
  value,
  unit,
  trend,
  trendDirection = 'flat',
  footnote,
}: StatCardProps) {
  return (
    <div className="stat">
      <div className="stat__label">{label}</div>
      <div className="stat__value">
        {value}
        {unit !== undefined && <span className="stat__unit">{unit}</span>}
      </div>
      {(trend !== undefined || footnote !== undefined) && (
        <div className="stat__foot">
          {trend !== undefined && <span className={`trend trend--${trendDirection}`}>{trend}</span>}
          {footnote !== undefined && <span>{footnote}</span>}
        </div>
      )}
    </div>
  );
}

export interface StatsGridProps {
  children: ReactNode;
}

export function StatsGrid({ children }: StatsGridProps) {
  return <section className="stats">{children}</section>;
}
