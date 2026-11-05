const { chromium } = require('playwright-core');
(async () => {
  const b = await chromium.launch({ executablePath: '/usr/bin/chromium',
    args: ['--no-sandbox','--disable-dev-shm-usage','--autoplay-policy=no-user-gesture-required'] });
  const ctx = await b.newContext({ viewport: { width: 1400, height: 950 } });
  const p = await ctx.newPage();
  await p.goto('http://127.0.0.1:8099/', { waitUntil: 'domcontentloaded' });
  await p.evaluate(() => {
    localStorage.setItem('vyzorix.media.connection', JSON.stringify({ baseUrl: 'http://127.0.0.1:12001', authToken: '' }));
    localStorage.setItem('vyzorix.local-storage', JSON.stringify({ tier: 'opfs' }));
  });
  await p.reload({ waitUntil: 'domcontentloaded' });
  await p.waitForTimeout(1500);
  await p.getByRole('button', { name: 'Browse', exact: true }).first().click();
  await p.waitForTimeout(1000);
  await p.getByRole('button', { name: 'View a page', exact: true }).first().click();
  await p.waitForTimeout(800);
  // A page with real video, so the list shows more than images.
  await p.getByLabel('Address').first().fill('https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8');
  await p.getByRole('button', { name: 'Open live', exact: true }).first().click();
  await p.waitForTimeout(6000);
  await p.evaluate(() => {
    Array.from(document.querySelectorAll('button')).find((x) => (x.textContent||'').trim().startsWith('Media'))?.click();
  });
  await p.waitForTimeout(7000);
  await p.waitForTimeout(800);
  await p.screenshot({ path: '/tmp/hud-shot.png' });
  const summary = await p.evaluate(() => ({
    rows: document.querySelectorAll('.hud__row').length,
    first: Array.from(document.querySelectorAll('.hud__row')).slice(0, 3).map((r) => ({
      name: r.querySelector('.hud__name')?.textContent,
      tags: Array.from(r.querySelectorAll('.hud__tag')).map((t) => t.textContent),
      variants: Array.from(r.querySelectorAll('.hud__variant')).map((v) => v.textContent.trim()),
    })),
  }));
  console.log(JSON.stringify(summary, null, 1));
  await b.close();
})().catch(e => { console.error('fatal', e.message); process.exit(1); });
