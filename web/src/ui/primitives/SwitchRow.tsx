/**
 * SwitchRow — a labelled toggle in a bordered row.
 */

export interface SwitchRowProps {
  name: string;
  description?: string;
  checked: boolean;
  onChange: (checked: boolean) => void;
  disabled?: boolean;
}

export function SwitchRow({ name, description, checked, onChange, disabled }: SwitchRowProps) {
  return (
    <div className="switch-row">
      <div className="switch-row__text">
        <div className="switch-row__name">{name}</div>
        {description !== undefined && <div className="switch-row__desc">{description}</div>}
      </div>
      <label className="switch">
        <input
          type="checkbox"
          checked={checked}
          disabled={disabled}
          onChange={(e) => onChange(e.target.checked)}
          aria-label={name}
        />
        <span className="switch__track" />
      </label>
    </div>
  );
}
