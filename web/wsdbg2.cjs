const { chromium } = require('playwright-core');
(async () => {
  const b = await chromium.launch({ executablePath: '/usr/bin/chromium', args: ['--no-sandbox','--disable-dev-shm-usage'] });
  const ctx = await b.newContext({ viewport: { width: 1400, height: 950 }, hasTouch: true });
  const p = await ctx.newPage();
  p.on('websocket', (ws) => {
    ws.on('framesent', (f) => {
      const s = String(f.payload);
      if (s.includes('"watch"') || s.includes('"rescan"')) console.log('SENT', s.slice(0, 120));
    });
    ws.on('framereceived', (f) => {
      const s = String(f.payload);
      if (s.includes('"type":"media"')) {
        try { const d = JSON.parse(s); console.log('RECV MEDIA', d.items.length, 'items, picked', d.items.filter(i=>i.picked).length); } catch {}
      }
    });
  });
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
  await p.getByLabel('Address').first().fill('https://commons.wikimedia.org/wiki/File:Sunflower_from_Silesia2.jpg');
  const open = p.getByRole('button', { name: 'Open live', exact: true }).first();
  if (await open.count()) await open.click();
  await p.waitForTimeout(6000);
  console.log('--- opening HUD (should send watch) ---');
  await p.evaluate(() => { Array.from(document.querySelectorAll('button')).find(x => (x.textContent||'').trim().startsWith('Media'))?.click(); });
  await p.waitForTimeout(4000);
  await b.close();
})().catch(e => { console.error('fatal', e.message); process.exit(1); });
