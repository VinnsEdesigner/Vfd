/**
 * Spinner — an indeterminate loading indicator.
 *
 * Pure CSS (a rotating arc), sized by the `size` prop and coloured by
 * `currentColor`, so it drops into a button, a panel, or a full-page splash
 * without variant-specific CSS.
 */

export type SpinnerSize = 'sm' | 'md' | 'lg';

export interface SpinnerProps {
  size?: SpinnerSize;
  /** Accessible label; the element is `role="status"` when set. */
  label?: string;
  className?: string;
}

export function Spinner({ size = 'md', label, className }: SpinnerProps) {
  return (
    <span
      className={['spinner', `spinner--${size}`, className].filter(Boolean).join(' ')}
      {...(label ? { role: 'status', 'aria-label': label } : { 'aria-hidden': true })}
    />
  );
}
