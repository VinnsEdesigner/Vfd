/**
 * Sidebar — brand, primary navigation, and the connection indicator.
 *
 * Navigation is data-driven so adding a view is one entry, and the active view
 * is owned by the parent (the router), not this component.
 */

import type { ReactNode } from 'react';

export interface NavItem {
  id: string;
  label: string;
  icon: ReactNode;
  /** Optional badge, e.g. the number of active downloads. */
  count?: number;
}

export interface SidebarProps {
  items: ReadonlyArray<NavItem>;
  active: string;
  onNavigate: (id: string) => void;
  /** Connection details shown at the foot. */
  endpoint: string;
  connected: boolean;
}

export function Sidebar({ items, active, onNavigate, endpoint, connected }: SidebarProps) {
  return (
    <aside className="sidebar">
      <div className="sidebar__top">
        <nav className="nav" aria-label="Main">
          {items.map((item) => (
            <button
              key={item.id}
              type="button"
              className={item.id === active ? 'nav__item is-active' : 'nav__item'}
              aria-current={item.id === active ? 'page' : undefined}
              onClick={() => onNavigate(item.id)}
            >
              {item.icon}
              {item.label}
              {item.count !== undefined && <span className="nav__count">{item.count}</span>}
            </button>
          ))}
        </nav>
      </div>

      <div className="sidebar__bottom">
        <div className="status">
          <span className={connected ? 'status__dot' : 'status__dot is-down'} />
          <div className="status__text">
            <span className="status__label">{connected ? 'Connected' : 'Disconnected'}</span>
            <code className="status__url">{endpoint}</code>
          </div>
        </div>
      </div>
    </aside>
  );
}
