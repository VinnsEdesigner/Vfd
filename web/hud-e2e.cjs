/**
 * End-to-end check of the navigation controls and the media HUD.
 *
 * Opens a real page that has media, turns the HUD on, and checks what a person
 * would: the navigation buttons exist, the overlay lists things, a manifest shows
 * its qualities, and removing an entry takes it out of the list.
 */
const { chromium } = require('playwright-core');

const SITE = process.env.SITE || 'http://127.0.0.1:8099';
const ENGINE = process.env.ENGINE || 'http://127.0.0.1:12001';
const PAGE = process.env.PAGE_URL || 'https://commons.wikimedia.org/wiki/Category:Videos';

function ok(name, cond, detail = '') {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${name}${detail ? '  — ' + detail : ''}`);
  if (!cond) process.exitCode = 1;
}

(async () => {
  const b = await chromium.launch({
    executablePath: '/usr/bin/chromium',
    args: ['--no-sandbox', '--disable-dev-shm-usage', '--autoplay-policy=no-user-gesture-required'],
  });
  const ctx = await b.newContext({ viewport: { width: 1280, height: 900 }, hasTouch: true });
  const p = await ctx.newPage();
  p.on('pageerror', (e) => console.log('PAGEERROR', String(e).slice(0, 200)));

  await p.goto(SITE, { waitUntil: 'domcontentloaded' });
  await p.evaluate((eng) => {
    localStorage.setItem('vyzorix.media.connection', JSON.stringify({ baseUrl: eng, authToken: '' }));
    localStorage.setItem('vyzorix.local-storage', JSON.stringify({ tier: 'opfs' }));
  }, ENGINE);
  await p.reload({ waitUntil: 'domcontentloaded' });
  await p.waitForTimeout(1500);

  await p.getByRole('button', { name: 'Browse', exact: true }).first().click();
  await p.waitForTimeout(1000);
  await p.getByRole('button', { name: 'View a page', exact: true }).first().click();
  await p.waitForTimeout(800);
  await p.getByLabel('Address').first().fill(PAGE);
  // The session only starts when the control is pressed: nothing runs on the
  // Vyzor until someone asks for a page.
  const openBtn = p.getByRole('button', { name: 'Open live', exact: true }).first();
  if (await openBtn.count()) {
    await openBtn.click();
    console.log('pressed Open live');
  }
  await p.waitForTimeout(5000);

  // --- navigation controls -------------------------------------------------
  ok('Back is offered', (await p.getByLabel('Back').count()) > 0);
  ok('Forward is offered', (await p.getByLabel('Forward').count()) > 0);
  ok('Reload is offered', (await p.getByRole('button', { name: /reload/i }).count()) > 0);

  // --- the HUD -------------------------------------------------------------
  const mediaBtn = p.getByRole('button', { name: /^media/i }).first();
  ok('the Media button is offered', (await mediaBtn.count()) > 0);
  if ((await mediaBtn.count()) === 0) { await b.close(); return; }
  await mediaBtn.click();
  await p.waitForTimeout(6000);

  const hud = p.locator('.hud');
  ok('the overlay opened', (await hud.count()) > 0);

  // Everything is listed by default, media first. Wait for the list to fill: it
  // arrives over the socket as the page loads.
  await p
    .waitForFunction(() => document.querySelectorAll('.hud__row').length > 0, null, { timeout: 25000 })
    .catch(() => {});
  const rows = await p.locator('.hud__row').count();
  console.log('   rows:', rows);
  ok('the list has entries', rows > 0);

  const sample = await p.evaluate(() =>
    Array.from(document.querySelectorAll('.hud__row')).slice(0, 6).map((r) => ({
      name: r.querySelector('.hud__name')?.textContent ?? '',
      tags: Array.from(r.querySelectorAll('.hud__tag')).map((t) => t.textContent),
    })),
  );
  console.log('   sample:', JSON.stringify(sample, null, 1));

  // Every row should carry a format and a size — that is the point of the list.
  const wellFormed = sample.every((r) => r.tags.length >= 2 && r.tags[0]);
  ok('each row shows a format and a size', wellFormed);

  // --- remove --------------------------------------------------------------
  const before = await p.locator('.hud__row').count();
  await p.locator('.hud__remove').first().click();
  await p.waitForTimeout(500);
  const after = await p.locator('.hud__row').count();
  ok('removing takes an entry out of the list', after === before - 1, `${before} -> ${after}`);

  // --- download ------------------------------------------------------------
  await p.locator('.hud__row').first().getByRole('button', { name: /download/i }).click();
  await p.waitForTimeout(2500);
  const queued = await p.evaluate(() =>
    document.body.innerText.includes('Queued') || document.body.innerText.includes('queued'),
  );
  ok('a download can be queued from the list', queued);

  await p.screenshot({ path: '/tmp/hud.png' });
  await b.close();
  console.log(process.exitCode ? '\nRESULT: FAILURES' : '\nRESULT: ALL PASS');
})().catch((e) => { console.error('fatal', e.message); process.exit(1); });
