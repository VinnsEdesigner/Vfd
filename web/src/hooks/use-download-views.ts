/**
 * useDownloadViews — maps domain downloads to the view models the UI renders.
 *
 * Memoised so a re-render triggered by polling does not rebuild every row, and
 * so the UI receives a stable array reference when nothing changed.
 */

import { useMemo } from 'react';
import { toDownloadViews, type DownloadView } from './view-models';
import type { Download } from '../domain/media';

export function useDownloadViews(downloads: ReadonlyArray<Download>): DownloadView[] {
  return useMemo(() => toDownloadViews([...downloads]), [downloads]);
}
