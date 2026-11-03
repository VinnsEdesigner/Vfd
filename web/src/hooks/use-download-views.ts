/**
 * useDownloadViews — maps domain downloads to the view models the UI renders.
 *
 * Memoised so a re-render triggered by polling does not rebuild every row, and
 * so the UI receives a stable array reference when nothing changed.
 */

import { useMemo } from 'react';
import { pipelineStages, toDownloadViews, type DownloadView } from './view-models';
import type { CreateDownloadInput, Download } from '../domain/media';

/** The options a record was created with, where the caller knows them. */
export interface DownloadPipelineOptions {
  hls: boolean;
  upload: boolean;
  purge: boolean;
}

export function useDownloadViews(downloads: ReadonlyArray<Download>): DownloadView[] {
  return useMemo(() => toDownloadViews([...downloads]), [downloads]);
}

/**
 * The list plus each record's derived pipeline. The pipeline depends on the
 * options a download ran with, which the list endpoint does not report, so the
 * caller passes the current defaults; the derivation then only marks a stage
 * done when the record itself proves it.
 */
export function useDownloadViewsWithPipeline(
  downloads: ReadonlyArray<Download>,
  options: DownloadPipelineOptions,
): DownloadView[] {
  return useMemo(
    () =>
      toDownloadViews([...downloads]).map((v) => ({
        ...v,
        pipeline: pipelineStages(v, options),
      })),
    [downloads, options.hls, options.upload, options.purge],
  );
}

export type { CreateDownloadInput };
