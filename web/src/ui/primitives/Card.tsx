/**
 * Card — surface with an optional header and a body.
 *
 * `flush` removes body padding, which is what a table wants; the body then
 * scrolls sideways rather than clipping.
 */

import type { ReactNode } from 'react';

export interface CardProps {
  title?: ReactNode;
  subtitle?: ReactNode;
  /** Leading app icon shown beside the title. */
  icon?: ReactNode;
  /** Rendered at the right of the header. */
  actions?: ReactNode;
  /** Extra header content below the title row (e.g. tabs). */
  footerHead?: ReactNode;
  flush?: boolean;
  className?: string;
  bodyClassName?: string;
  children?: ReactNode;
}

export function Card({
  title,
  subtitle,
  icon,
  actions,
  footerHead,
  flush = false,
  className,
  bodyClassName,
  children,
}: CardProps) {
  const hasHead =
    title !== undefined || subtitle !== undefined || actions !== undefined || icon !== undefined;
  const cls = ['card', className].filter(Boolean).join(' ');
  const bodyCls = ['card__body', flush ? 'card__body--flush' : '', bodyClassName]
    .filter(Boolean)
    .join(' ');

  return (
    <section className={cls}>
      {hasHead && (
        <header className={icon ? 'card__head card__head--icon' : 'card__head'}>
          {icon}
          <div className="card__head-text">
            {title !== undefined && <div className="card__title">{title}</div>}
            {subtitle !== undefined && <div className="card__sub">{subtitle}</div>}
          </div>
          {actions}
        </header>
      )}
      {footerHead}
      <div className={bodyCls}>{children}</div>
    </section>
  );
}
