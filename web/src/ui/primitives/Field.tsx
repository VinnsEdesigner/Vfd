/**
 * Field — labelled text/number input.
 *
 * `SecretField` is the same shape with a mask and a Show/Hide toggle. The real
 * value lives in `data-original` so toggling never loses it.
 */

import { useId, useState } from 'react';

export interface FieldProps {
  label: string;
  value: string;
  onChange?: (value: string) => void;
  type?: 'text' | 'number';
  placeholder?: string;
  hint?: string;
  min?: number;
  max?: number;
  id?: string;
}

export function Field({
  label,
  value,
  onChange,
  type = 'text',
  placeholder,
  hint,
  min,
  max,
  id,
}: FieldProps) {
  const generated = useId();
  const inputId = id ?? generated;
  return (
    <div className="field">
      <label htmlFor={inputId}>{label}</label>
      <input
        id={inputId}
        type={type}
        value={value}
        placeholder={placeholder}
        spellCheck={false}
        min={min}
        max={max}
        onChange={(e) => onChange?.(e.target.value)}
      />
      {hint !== undefined && <span className="field__hint">{hint}</span>}
    </div>
  );
}

const MASK = '\u2022'.repeat(16);

export interface SecretFieldProps {
  label: string;
  value: string;
  onChange?: (value: string) => void;
  hint?: string;
  id?: string;
}

export function SecretField({ label, value, onChange, hint, id }: SecretFieldProps) {
  const generated = useId();
  const inputId = id ?? generated;
  const [revealed, setRevealed] = useState(false);

  return (
    <div className="field">
      <label htmlFor={inputId}>{label}</label>
      <div className="input-group">
        <input
          id={inputId}
          type="text"
          value={revealed ? value : MASK}
          spellCheck={false}
          autoComplete="off"
          onChange={(e) => onChange?.(e.target.value)}
        />
        <button
          type="button"
          className="input-group__btn"
          onClick={() => setRevealed((r) => !r)}
          aria-pressed={revealed}
        >
          {revealed ? 'Hide' : 'Show'}
        </button>
      </div>
      {hint !== undefined && <span className="field__hint">{hint}</span>}
    </div>
  );
}
