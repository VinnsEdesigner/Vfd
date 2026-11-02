/**
 * AppShell — sidebar + fixed header + scrolling content.
 *
 * The page itself never scrolls (`body.app-shell`), so the header and sidebar
 * stay put and only `.content` moves. Below 860px the CSS abandons this shell
 * entirely so content is never pushed out of reach.
 */

import type { ReactNode } from 'react';
import { Sidebar, type NavItem } from './Sidebar';
import { TopBar } from './TopBar';

export interface AppShellProps {
  navItems: ReadonlyArray<NavItem>;
  activeView: string;
  onNavigate: (id: string) => void;
  endpoint: string;
  connected: boolean;
  title: string;
  subtitle?: string;
  headerActions?: ReactNode;
  children: ReactNode;
}

export function AppShell({
  navItems,
  activeView,
  onNavigate,
  endpoint,
  connected,
  title,
  subtitle,
  headerActions,
  children,
}: AppShellProps) {
  return (
    <div className="app">
      <Sidebar
        items={navItems}
        active={activeView}
        onNavigate={onNavigate}
        endpoint={endpoint}
        connected={connected}
      />
      <div className="main">
        <TopBar title={title} subtitle={subtitle} actions={headerActions} />
        <div className="content">{children}</div>
      </div>
    </div>
  );
}
