import { chromium, devices } from 'playwright';

/*
 * Drive the console UI with *real browser-level touch* (CDP Input.dispatchTouchEvent),
 * which is what a finger actually produces. Synthetic DOM TouchEvents can be
 * ignored by React's root listener, so they are not a fair test.
 *
 * The target page displays its own touch count and scroll position, so the
 * screenshots say what happened rather than leaving it to inference.
 */

const CONSOLE = 'http://127.0.0.1:12056/';
const TARGET = process.argv[2] ?? 'http://10.2.90.6:12099/index.html';

const b = await chromium.launch({ args: ['--no-sandbox'], executablePath: '/usr/bin/chromium' });
const ctx = await b.newContext({ ...devices['Pixel 7'] });
const page = await ctx.newPage();
const errs = [];
page.on('pageerror', (e) => errs.push(`pageerror: ${e.message}`));
page.on('console', (m) => { if (m.type() === 'error') errs.push(`console: ${m.text()}`); });
await page.addInitScript(() => {
  localStorage.setItem('vyzorix.local-storage', JSON.stringify({ tier: 'memory' }));
});

const cdp = await ctx.newCDPSession(page);

await page.goto(CONSOLE, { waitUntil: 'networkidle' });
await page.getByRole('button', { name: 'Browse' }).click();
await page.waitForTimeout(800);
await page.getByRole('button', { name: 'View a page' }).click();
await page.waitForTimeout(500);
const address = page.getByLabel('Address');
await address.click();
await address.fill(TARGET);
await page.waitForTimeout(300);
await page.getByRole('button', { name: /Open live/ }).click();
await page.waitForSelector('.live__stage', { timeout: 45000 });
await page.waitForFunction(() => {
  const i = document.querySelector('.live__frame');
  return Boolean(i && i.getAttribute('src')?.startsWith('blob:'));
}, { timeout: 45000 });
await page.waitForTimeout(4500);
await page.locator('.live__stage').screenshot({ path: '/tmp/t1-loaded.png' });

const st = await page.locator('.live__stage').boundingBox();
const cx = st.x + st.width / 2;

async function swipe(fromY, toY, steps = 14) {
  await cdp.send('Input.dispatchTouchEvent', {
    type: 'touchStart',
    touchPoints: [{ x: cx, y: fromY, id: 1 }],
  });
  for (let i = 1; i <= steps; i++) {
    await cdp.send('Input.dispatchTouchEvent', {
      type: 'touchMove',
      touchPoints: [{ x: cx, y: fromY + ((toY - fromY) * i) / steps, id: 1 }],
    });
    await page.waitForTimeout(16);
  }
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
}

// Tap the TAP ME block first.
await cdp.send('Input.dispatchTouchEvent', {
  type: 'touchStart',
  touchPoints: [{ x: cx, y: st.y + st.height * 0.2, id: 1 }],
});
await page.waitForTimeout(60);
await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
await page.waitForTimeout(2500);
await page.locator('.live__stage').screenshot({ path: '/tmp/t2-tapped.png' });

// Then swipe up.
await swipe(st.y + st.height * 0.75, st.y + st.height * 0.2);
await page.waitForTimeout(3000);
await page.locator('.live__stage').screenshot({ path: '/tmp/t3-swiped.png' });

console.log('errors:', errs.length ? errs.join(' | ') : 'none');
await b.close();
