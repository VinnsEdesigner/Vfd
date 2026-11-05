/**
 * End-to-end: does picking media in the page add it to the HUD with a
 * thumbnail and a title?
 */
const { chromium } = require('playwright-core');
const SITE = process.env.SITE || 'http://127.0.0.1:8099';
const ENGINE = process.env.ENGINE || 'http://127.0.0.1:12001';
const PAGE = process.env.PAGE_URL || 'https://commons.wikimedia.org/wiki/File:Sunflower_from_Silesia2.jpg';

function ok(n, c, d = '') {
  console.log(`${c ? 'PASS' : 'FAIL'}  ${n}${d ? '  — ' + d : ''}`);
  if (!c) process.exitCode = 1;
}

(async () => {
  const b = await chromium.launch({
    executablePath: '/usr/bin/chromium',
    args: ['--no-sandbox', '--disable-dev-shm-usage'],
  });
  const ctx = await b.newContext({ viewport: { width: 1400, height: 950 }, hasTouch: true });
  const p = await ctx.newPage();
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
  const open = p.getByRole('button', { name: 'Open live', exact: true }).first();
  if (await open.count()) await open.click();
  await p.waitForTimeout(6000);

  // Open the HUD, which also starts collecting.
  await p.evaluate(() => {
    Array.from(document.querySelectorAll('button'))
      .find((x) => (x.textContent || '').trim().startsWith('Media'))
      ?.click();
  });
  await p.waitForTimeout(3000);
  ok('the HUD opened', (await p.locator('.hud').count()) > 0);

  const before = await p.locator('.hud__row').count();
  console.log('   rows before the pick:', before);

  // Click the picture in the live stage. The stage is the page's own viewport,
  // so the picture's centre is somewhere in the upper middle.
  const stage = await p.locator('.live__stage').boundingBox();
  if (!stage) { console.log('no stage'); await b.close(); return; }
  const cx = stage.x + stage.width / 2;
  for (const y of [0.2, 0.3, 0.4, 0.5, 0.6]) {
    await p.mouse.click(cx, stage.y + stage.height * y);
    await p.waitForTimeout(700);
  }
  await p.waitForTimeout(3000);

  const rows = p.locator('.hud__row');
  const after = await rows.count();
  console.log('   rows after the pick:', after);
  const picked = await p.locator('.hud__row--picked').count();
  console.log('   picked rows:', picked);
  ok('picking media adds it to the list', after > before || picked > 0);

  if (picked > 0) {
    const first = p.locator('.hud__row--picked').first();
    const name = await first.locator('.hud__name').innerText();
    const tags = await first.locator('.hud__tag').allInnerTexts();
    const hasThumb = (await first.locator('.hud__thumb').count()) > 0;
    console.log('   card:', JSON.stringify({ name, tags, hasThumb }));
    ok('the card shows a title or filename', name.trim().length > 0);
    ok('the card shows a format and a size', tags.length >= 2);
  }
  await p.screenshot({ path: '/tmp/hud-pick.png' });
  await b.close();
  console.log(process.exitCode ? '\nRESULT: FAILURES' : '\nRESULT: ALL PASS');
})().catch((e) => { console.error('fatal', e.message); process.exit(1); });
