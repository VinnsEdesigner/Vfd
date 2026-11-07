/**
 * SiteAccessPanel tests.
 *
 * The identity control used to be a text box: a user typed a target and hoped
 * they spelled it right. "Chrome131" is refused where "chrome131" is accepted,
 * and the failure was silent — the fetch just fell back to libcurl — so the
 * field is now a picker built from the list the host reports. These tests pin
 * that: the options come from the host, a choice is saved, and no free text is
 * asked for.
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { SiteAccessPanel, type SiteAccessValue } from './SiteAccessPanel';

const base: SiteAccessValue = {
  cookieConfigured: false,
  cookieNames: [],
  proxy: '',
  impersonate: '',
  impersonateTargets: ['chrome131', 'firefox133', 'safari180'],
  identityRetries: 0,
  captchaProvider: 'stub',
  captchaProviders: ['stub', '2captcha'],
  captchaKeySet: false,
  captchaBaseUrl: '',
  frontierDbUrl: '',
  frontierDbPath: '',
  frontierTokenSet: false,
};

function setup(patch: Partial<SiteAccessValue> = {}) {
  const onSave = vi.fn();
  render(
    <SiteAccessPanel
      value={{ ...base, ...patch }}
      onSave={onSave}
      onClearCookies={vi.fn()}
    />,
  );
  return { onSave };
}

describe('SiteAccessPanel identity picker', () => {
  it('offers the host-reported targets rather than a text box', () => {
    setup();
    // The trigger shows the default family; opening it lists the host's targets.
    fireEvent.click(screen.getByRole('combobox', { name: 'Impersonate' }));
    expect(screen.getByRole('option', { name: /chrome131/ })).toBeDefined();
    expect(screen.getByRole('option', { name: /firefox133/ })).toBeDefined();
    expect(screen.getByRole('option', { name: /safari180/ })).toBeDefined();
  });

  it('saves the chosen target', () => {
    const { onSave } = setup();
    fireEvent.click(screen.getByRole('combobox', { name: 'Impersonate' }));
    fireEvent.click(screen.getByRole('option', { name: /chrome131/ }));
    fireEvent.click(screen.getByRole('button', { name: 'Save identity' }));
    expect(onSave).toHaveBeenCalledWith(
      expect.objectContaining({ impersonate: 'chrome131' }),
    );
  });

  it('falls back to a family list when the host reports none', () => {
    setup({ impersonateTargets: [] });
    fireEvent.click(screen.getByRole('combobox', { name: 'Impersonate' }));
    // The fallback always includes the bare families both tools expand.
    expect(screen.getByRole('option', { name: /firefox/ })).toBeDefined();
  });
});
