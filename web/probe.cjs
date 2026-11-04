const { chromium } = require('playwright-core');
(async () => {
  const b = await chromium.launch({ executablePath: '/usr/bin/chromium',
    args: ['--no-sandbox','--disable-dev-shm-usage','--autoplay-policy=no-user-gesture-required'] });
  const ctx = await b.newContext({ viewport: { width: 412, height: 915 }, hasTouch: true, isMobile: true });
  const p = await ctx.newPage();
  p.on('pageerror', e => console.log('PAGEERROR', String(e).slice(0,300)));
  await p.goto('http://127.0.0.1:8099/', { waitUntil: 'domcontentloaded' });
  await p.evaluate((eng) => {
    localStorage.setItem('vyzorix.media.connection', JSON.stringify({ baseUrl: eng, authToken: '' }));
    localStorage.setItem('vyzorix.local-storage', JSON.stringify({ tier: 'opfs' }));
  }, process.env.ENGINE);
  await p.reload({ waitUntil: 'domcontentloaded' });
  await p.waitForTimeout(1500);
  await p.getByRole('button', { name: 'Browse', exact: true }).first().click();
  await p.waitForTimeout(1200);
  await p.getByRole('button', { name: 'View a page', exact: true }).first().click();
  await p.waitForTimeout(800);
  await p.getByLabel('Address').first().fill('https://example.com/');
  await p.getByRole('button', { name: 'Open live', exact: true }).first().click();
  await p.waitForTimeout(3500);
  await p.getByRole('button', { name: 'Video', exact: true }).first().click();
  await p.waitForTimeout(4000);
  const html = await p.evaluate(() => {
    const s = document.querySelector('.live__stage');
    const outer = document.querySelector('.live');
    return {
      stageExists: Boolean(s),
      stageChildren: s ? Array.from(s.children).map(c => c.tagName + '.' + c.className) : null,
      stageHTML: s ? s.outerHTML.slice(0, 900) : null,
      outerHTML: outer ? outer.outerHTML.slice(0, 1600) : null,
    };
  });
  console.log(JSON.stringify(html, null, 1));
  await p.screenshot({ path: '/tmp/shot.png' });
  await b.close();
})().catch(e => { console.error('fatal', e.message); process.exit(1); });
