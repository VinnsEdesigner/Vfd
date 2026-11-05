/**
 * PipelineSteps — the stage list for one download.
 *
 * The statuses are derived in the data layer from the record's own facts, so
 * this component only renders them. It never asserts a stage is done: a stage
 * with no evidence reads "Pending", and a stage the job was not asked to run
 * reads "Skipped" rather than being hidden, so the list keeps its shape.
 */

import { IconCheck, IconX, IconMinus } from '../primitives/Icons';
import type { PipelineStageKey, PipelineStageStatus } from '../../hooks/view-models';

export interface PipelineStageMeta {
  key: PipelineStageKey;
  name: string;
  meta: string;
}

/** Display copy for the stages, in Vyzor order. */
export const PIPELINE_STAGES: ReadonlyArray<PipelineStageMeta> = [
  { key: 'probe', name: 'Probe', meta: 'HEAD + byte sniff' },
  { key: 'fetch', name: 'Fetch', meta: 'Ranged, multi-connection' },
  { key: 'verify', name: 'Verify', meta: 'SHA-256 checksum' },
  { key: 'transcode', name: 'Transcode', meta: 'HLS adaptive ladder' },
  { key: 'upload', name: 'Upload', meta: 'S3-compatible PUT' },
  { key: 'purge', name: 'Purge', meta: 'Edge cache invalidation' },
];

export interface PipelineStepsProps {
  /** Derived status per stage, in the same order as PIPELINE_STAGES. */
  stages: ReadonlyArray<{ key: PipelineStageKey; status: PipelineStageStatus }>;
  /** Copy overrides; defaults to PIPELINE_STAGES. */
  meta?: ReadonlyArray<PipelineStageMeta>;
}

const STATUS_LABEL: Record<PipelineStageStatus, string> = {
  done: 'Complete',
  active: 'Running',
  fault: 'Failed',
  pending: 'Pending',
  skipped: 'Skipped',
};

function glyph(status: PipelineStageStatus, index: number) {
  if (status === 'done') return <IconCheck />;
  if (status === 'fault') return <IconX />;
  if (status === 'skipped') return <IconMinus />;
  return String(index + 1);
}

export function PipelineSteps({ stages, meta = PIPELINE_STAGES }: PipelineStepsProps) {
  const byKey = new Map(stages.map((s) => [s.key, s.status]));
  return (
    <div className="steps">
      {meta.map((m, i) => {
        const status = byKey.get(m.key) ?? 'pending';
        return (
          <div key={m.key} className={`step step--${status}`}>
            <span className="step__dot">{glyph(status, i)}</span>
            <div className="step__body">
              <div className="step__name">{m.name}</div>
              <div className="step__meta">{m.meta}</div>
            </div>
            <span className="step__right">{STATUS_LABEL[status]}</span>
          </div>
        );
      })}
    </div>
  );
}
