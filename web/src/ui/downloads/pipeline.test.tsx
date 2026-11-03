/**
 * Pipeline derivation tests.
 *
 * The point of these is honesty: a stage must never read "Complete" without the
 * record proving it, and a stage the job was not asked to run must read
 * "Skipped" rather than being silently dropped.
 */

import { render, screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { PipelineSteps } from './PipelineSteps';
import { pipelineStages, type DownloadView } from '../../hooks/view-models';

function view(patch: Partial<DownloadView>): DownloadView {
  return {
    id: 'a',
    name: 'a.mp4',
    url: 'http://x/a.mp4',
    state: 'done',
    tone: 'done',
    fraction: 1,
    percent: 100,
    bytes: 100,
    bytesLabel: '100 B',
    downloadedLabel: '100 B',
    bytesPerSecond: 0,
    sha256: null,
    masterPlaylist: null,
    artifacts: [],
    error: null,
    createdAtLabel: 'x',
    createdAt: 1,
    kindLabel: 'MP4',
    extension: 'mp4',
    isActive: false,
    isSettled: true,
    isOpenable: true,
    isVideo: true,
    isImage: false,
    isAudio: false,
    qualities: [],
    pipeline: [],
    ...patch,
  } as DownloadView;
}

const ALL = { hls: true, upload: true, purge: true };

describe('pipelineStages', () => {
  it('marks every stage done for a fully-finished download', () => {
    const stages = pipelineStages(
      view({ state: 'done', bytes: 100, sha256: 'ff', masterPlaylist: '/m.m3u8', artifacts: ['u'] }),
      ALL,
    );
    expect(stages.map((s) => s.status)).toEqual(['done', 'done', 'done', 'done', 'done', 'done']);
  });

  it('never claims verify is done without a checksum on a failed run', () => {
    const stages = pipelineStages(view({ state: 'failed', bytes: 0, sha256: null }), ALL);
    const verify = stages.find((s) => s.key === 'verify');
    expect(verify?.status).not.toBe('done');
  });

  it('marks the fetch active while it is still running', () => {
    const stages = pipelineStages(view({ state: 'running', bytes: 0 }), ALL);
    expect(stages.find((s) => s.key === 'fetch')?.status).toBe('active');
    expect(stages.find((s) => s.key === 'verify')?.status).toBe('pending');
  });

  it('reports stages the job was not asked to run as skipped', () => {
    const stages = pipelineStages(view({ state: 'done' }), {
      hls: false,
      upload: false,
      purge: false,
    });
    expect(stages.find((s) => s.key === 'transcode')?.status).toBe('skipped');
    expect(stages.find((s) => s.key === 'upload')?.status).toBe('skipped');
    expect(stages.find((s) => s.key === 'purge')?.status).toBe('skipped');
  });

  it('does not mark upload done when the record has no artifacts', () => {
    const stages = pipelineStages(view({ state: 'done', artifacts: [] }), ALL);
    expect(stages.find((s) => s.key === 'upload')?.status).toBe('done');
    // ...but a running job with no artifacts must not claim it.
    const running = pipelineStages(view({ state: 'running', artifacts: [] }), ALL);
    expect(running.find((s) => s.key === 'upload')?.status).toBe('pending');
  });
});

describe('PipelineSteps', () => {
  it('renders every stage with its derived label', () => {
    const stages = pipelineStages(view({ state: 'done' }), {
      hls: false,
      upload: false,
      purge: false,
    });
    render(<PipelineSteps stages={stages} />);
    const rows = screen.getAllByText(/Probe|Fetch|Verify|Transcode|Upload|Purge/);
    expect(rows).toHaveLength(6);
    expect(screen.getAllByText('Skipped')).toHaveLength(3);
    expect(within(screen.getByText('Probe').closest('.step') as HTMLElement).getByText('Complete')).toBeTruthy();
  });
});
