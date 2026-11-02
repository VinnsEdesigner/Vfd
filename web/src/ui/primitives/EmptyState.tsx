/**
 * EmptyState — the "nothing here" panel, with an icon slot.
 */

import type { ReactNode } from 'react';
import { IconInbox } from './Icons';

export interface EmptyStateProps {
  title: string;
  description?: string;
  icon?: ReactNode;
  action?: ReactNode;
}

export function EmptyState({ title, description, icon, action }: EmptyStateProps) {
  return (
    <div className="empty">
      {icon ?? <IconInbox />}
      <h3>{title}</h3>
      {description !== undefined && <p>{description}</p>}
      {action !== undefined && <div style={{ marginTop: 16 }}>{action}</div>}
    </div>
  );
}
