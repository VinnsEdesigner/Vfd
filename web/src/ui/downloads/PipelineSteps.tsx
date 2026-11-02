/**
 * PipelineSteps — the six-stage progress view for one download.
 *
 * Stage state is derived from the download's own state and stage index, so the
 * component is a pure function of its props.
 */

import { IconCheck, IconX } from '../primitives/Icons';
import type { DownloadState } from '../../hooks/view-models';

export interface PipelineStage {
  key: string;
  name: string;
  meta: string;
}

export const PIPELINE_STAGES: ReadonlyArray<PipelineStage> = [
  { key: 'probe', name: 'Probe', meta: 'HEAD + byte sniff' },
  { key: 'fetch', name: 'Fetch', meta: 'Ranged, 8 connections' },
  { key: 'verify', name: 'Verify', meta: 'SHA-256 checksum' },
  { key: 'transcode', name: 'Transcode', meta: 'HLS adaptive ladder' },
  { key: 'upload', name: 'Upload', meta: 'S3-compatible PUT' },
  { key: 'purge', name: 'Purge', meta: 'Edge cache invalidation' },
];

export interface PipelineStepsProps {
  state: DownloadState;
  /** Index of the stage currently in flight (or where it stopped). */
  stage: number;
  stages?: ReadonlyArray<PipelineStage>;
}

type StepStatus = 'done' | 'active' | 'fault' | 'pending';

function stepStatus(index: number, stage: number, state: DownloadState): StepStatus {
  if (index < stage) return 'done';
  if (index !== stage) return 'pending';
  if (state === 'running' || state === 'queued') return 'active';
  if (state === 'failed') return 'fault';
  if (state === 'done') return 'done';
  return 'pending';
}

const STATUS_LABEL: Record<StepStatus, string> = {
  done: 'Complete',
  active: 'Running',
  fault: 'Failed',
  pending: 'Pending',
};

export function PipelineSteps({
  state,
  stage,
  stages = PIPELINE_STAGES,
}: PipelineStepsProps) {
  return (
    <div className="steps">
      {stages.map((s, i) => {
        const status = stepStatus(i, stage, state);
        const glyph =
          status === 'done' ? <IconCheck /> : status === 'fault' ? <IconX /> : String(i + 1);
        return (
          <div key={s.key} className={`step step--${status}`}>
            <span className="step__dot">{glyph}</span>
            <div className="step__body">
              <div className="step__name">{s.name}</div>
              <div className="step__meta">{s.meta}</div>
            </div>
            <span className="step__right">{STATUS_LABEL[status]}</span>
          </div>
        );
      })}
    </div>
  );
}
