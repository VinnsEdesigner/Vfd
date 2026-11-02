/**
 * StatusBadge — maps a download state to a Badge tone.
 *
 * Takes the state as a plain string union re-exported from `hooks`, so this
 * component never imports the domain layer directly.
 */

import { Badge, type BadgeTone } from '../primitives/Badge';
import { statusTone, statusLabel, type DownloadState } from '../../hooks/view-models';

export interface StatusBadgeProps {
  state: DownloadState;
}

export function StatusBadge({ state }: StatusBadgeProps) {
  return <Badge tone={statusTone(state) as BadgeTone}>{statusLabel(state)}</Badge>;
}
