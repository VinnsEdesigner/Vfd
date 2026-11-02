/**
 * Tabs — controlled tab strip.
 *
 * Presentational only: the parent owns which tab is active, so the state can
 * live in a store or the URL rather than inside this component.
 */

export interface TabItem<T extends string> {
  id: T;
  label: string;
  count?: number;
}

export interface TabsProps<T extends string> {
  items: ReadonlyArray<TabItem<T>>;
  active: T;
  onChange: (id: T) => void;
  /** Accessible name for the tab list. */
  label?: string;
  className?: string;
}

export function Tabs<T extends string>({ items, active, onChange, label, className }: TabsProps<T>) {
  return (
    <div className={['tabs', className].filter(Boolean).join(' ')} role="tablist" aria-label={label}>
      {items.map((item) => {
        const isActive = item.id === active;
        return (
          <button
            key={item.id}
            type="button"
            role="tab"
            aria-selected={isActive}
            className={isActive ? 'tab is-active' : 'tab'}
            onClick={() => onChange(item.id)}
          >
            {item.label}
            {item.count !== undefined && <span className="tab__count">{item.count}</span>}
          </button>
        );
      })}
    </div>
  );
}
