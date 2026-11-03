/**
 * Button — the single button primitive.
 *
 * Presentation layer: imports nothing from the data layer.
 */

import { forwardRef, type ButtonHTMLAttributes, type ReactNode } from 'react';
import { Spinner } from './Spinner';

export type ButtonVariant = 'default' | 'primary' | 'ghost' | 'danger';
export type ButtonSize = 'md' | 'sm';

export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  size?: ButtonSize;
  /** Renders a square, icon-only button. Provide `aria-label`. */
  icon?: boolean;
  /**
   * Shows a spinner in place of the leading content and disables the button.
   * Any press starts work, so every pressable button should be able to show it.
   */
  loading?: boolean;
  children?: ReactNode;
}

function classes(variant: ButtonVariant, size: ButtonSize, icon: boolean): string {
  const parts = ['btn'];
  if (variant === 'primary') parts.push('btn--primary');
  if (variant === 'ghost') parts.push('btn--ghost');
  if (variant === 'danger') parts.push('btn--danger');
  if (size === 'sm') parts.push('btn--sm');
  if (icon) parts.push('btn--icon');
  return parts.join(' ');
}

/** Forwards its ref so a dialog can move focus to a specific button. */
export const Button = forwardRef<HTMLButtonElement, ButtonProps>(function Button({
  variant = 'default',
  size = 'md',
  icon = false,
  loading = false,
  className,
  type = 'button',
  disabled,
  children,
  ...rest
}: ButtonProps, ref) {
  const cls = [classes(variant, size, icon), loading ? 'is-loading' : '', className]
    .filter(Boolean)
    .join(' ');

  return (
    <button
      ref={ref}
      type={type}
      className={cls}
      disabled={disabled || loading}
      aria-busy={loading || undefined}
      {...rest}
    >
      {loading && <Spinner size={size === 'sm' ? 'sm' : 'md'} />}
      {children}
    </button>
  );
});
