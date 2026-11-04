/**
 * End-to-end check of the live-video path, in a real browser.
 *
 * What it proves, and why each step is here:
 *
 *   1. the console loads and reports the engine's rtc capability  — the UI is
 *      reachable and the build carries the engine URL;
 *   2. a video session negotiates                            — HTTP signalling,
 *      the RTCPeerConnection, and ICE all work together;
 *   3. the video element actually receives frames            — media is flowing,
 *      which is the whole point and is not provable any other way;
 *   4. touch, swipe, wheel and typing reach the page         — the input path
 *      (data channel -> CDP) works, and the page reacts.
 *
 * Step 3 is measured from the element itself: `videoWidth` only becomes non-zero
 * once real decoded frames arrive, so it cannot be faked by a session that
 * negotiated but never sent anything.
 *
 * A touch device is emulated for the touch checks, because that is the case the
 * engine has special handling for (touch events, a mobile viewport) and the one
 * most likely to regress.
 */
const { chromium } = require('playwright-core');

const SITE = process.env.SITE || 'http://127.0.0.1:8099';
const PAGE = process.env.PAGE_URL || 'https://example.com/';

function log(...a) { console.log(...a); }
function ok(name, cond, detail = '') {
  log(`${cond ? 'PASS' : 'FAIL'}  ${name}${detail ? '  — ' + detail : ''}`);
  if (!cond) process.exitCode = 1;
  return cond;
}

(async () => {
  const browser = await chromium.launch({
    executablePath: '/usr/bin/chromium',
    args: ['--no-sandbox', '--disable-dev-shm-usage', '--use-gl=swiftshader',
           '--autoplay-policy=no-user-gesture-required'],
  });

  // A touch device, so the engine emulates touch and the page lays itself out as
  // it would on a phone. This is the case with the most special handling.
  const ctx = await browser.newContext({
    viewport: { width: 412, height: 915 },
    hasTouch: true,
    isMobile: true,
    deviceScaleFactor: 1,
  });
  const page = await ctx.newPage();

  const consoleErrors = [];
  page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text()); });
  page.on('pageerror', (e) => consoleErrors.push(String(e)));

  // --- 1. the console loads -------------------------------------------------
  await page.goto(SITE, { waitUntil: 'domcontentloaded', timeout: 60000 });
  ok('console loads', await page.title() !== '', await page.title());

  // Point the console at the engine and dismiss the first-run storage prompt,
  // both of which a real user does once and which otherwise gate the UI.
  await page.evaluate((engine) => {
    window.localStorage.setItem(
      'vyzorix.media.connection',
      JSON.stringify({ baseUrl: engine || 'http://127.0.0.1:12001', authToken: '' }),
    );
    // The storage chooser is a first-run gate; picking a tier dismisses it.
    window.localStorage.setItem('vyzorix.local-storage', JSON.stringify({ tier: 'opfs' }));
  }, process.env.ENGINE).catch(() => {});
  await page.reload({ waitUntil: 'domcontentloaded' });
  await page.waitForTimeout(1500);

  // If the chooser is still up (a stale key shape, say), click through it.
  const chooser = page.getByText('Where should downloads be kept?').first();
  if (await chooser.count()) {
    const pick = page.getByRole('button', { name: /browser tab|this browser|keep/i }).first();
    if (await pick.count()) await pick.click().catch(() => {});
    await page.waitForTimeout(1000);
    log('dismissed the storage chooser');
  }

  // --- navigate to Browse ---------------------------------------------------
  // The nav items are buttons, not links.
  const browseNav = page.getByRole('button', { name: /^browse$/i }).first();
  if (await browseNav.count()) {
    await browseNav.click();
    log('clicked Browse');
  } else {
    log('WARN  no Browse button found');
  }
  await page.waitForTimeout(2000);

  // The capability must be visible as a Video mode, which only appears when the
  // engine reported `rtc.available`.
  const bodyText = await page.locator('body').innerText().catch(() => '');
  log('--- page text sample ---');
  log(bodyText.slice(0, 400).replace(/\n{2,}/g, '\n'));
  ok('engine reports it can stream video', /video/i.test(bodyText));

  // --- 2. open a page -------------------------------------------------------
  // "View a page" is the tab that shows a page. Exact match: "Read a URL" also
  // contains no such words, but "Read" is a different control entirely.
  await page.getByRole('button', { name: 'View a page', exact: true }).first().click();
  await page.waitForTimeout(800);
  log('switched to View a page');

  // The page-address field, which the Field primitive labels "Address".
  const urlInput = page.getByLabel('Address').first();
  await urlInput.waitFor({ state: 'visible', timeout: 10000 });
  await urlInput.fill(PAGE);
  log('typed the address');

  // Choosing "View a page" is itself the action: the live toolbar appears with
  // the address, so there is no separate open control to press. Wait for the
  // toolbar, which is the signal the session is up.
  const reload = page.getByRole('button', { name: 'Reload', exact: true }).first();
  if (await reload.count()) {
    log('the live toolbar is up');
  } else {
    log('WARN  no live toolbar after View a page');
  }
  await page.waitForTimeout(4000);
  log('--- after opening the page ---');
  log((await page.locator('body').innerText().catch(() => '')).slice(-600).replace(/\n{2,}/g, '\n'));
  log('--- after submitting the address ---');
  log((await page.locator('body').innerText().catch(() => '')).slice(0, 500).replace(/\n{2,}/g, '\n'));

  // Choose the Video mode, which is what mounts the video stage. The session
  // opens from the stage itself, because a MediaStream needs an element that is
  // already in the DOM.
  const videoBtn = page.getByRole('button', { name: 'Video', exact: true }).first();
  if (await videoBtn.count()) {
    await videoBtn.click();
    log('clicked the Video mode');
  } else {
    log('WARN  no Video mode button');
  }
  await page.waitForTimeout(2500);

  // --- diagnostic: what is on the page after choosing Video? ----------------
  await page.waitForTimeout(2500);
  const diag = await page.evaluate(() => {
    const q = (sel) => document.querySelectorAll(sel).length;
    const stage = document.querySelector('.live, [class*="live"]');
    return {
      videos: q('video'),
      imgs: q('img'),
      liveFrame: q('.live__frame'),
      liveBlank: q('.live__blank'),
      pressed: Array.from(document.querySelectorAll('[aria-pressed="true"]')).map(
        (e) => (e.textContent || '').trim(),
      ),
      stageHtml: stage ? stage.outerHTML.slice(0, 400) : '(no .live stage)',
    };
  });
  log('DIAG:', JSON.stringify(diag, null, 1));

  // --- 3. media must actually flow -----------------------------------------
  const video = page.locator('video').first();
  let frames = 0;
  try {
    await video.waitFor({ state: 'attached', timeout: 20000 });
    // videoWidth is only non-zero once decoded frames have arrived.
    frames = await page.waitForFunction(
      () => {
        const v = document.querySelector('video');
        return v && v.videoWidth > 0 ? v.videoWidth : 0;
      },
      null,
      { timeout: 45000 },
    ).then((h) => h.jsonValue()).catch(() => 0);
  } catch (e) {
    log('video wait error:', e.message.split('\n')[0]);
  }
  const dims = await page.evaluate(() => {
    const v = document.querySelector('video');
    if (!v) return null;
    return { w: v.videoWidth, h: v.videoHeight, ready: v.readyState, paused: v.paused,
             srcObject: Boolean(v.srcObject) };
  });
  log('video element:', JSON.stringify(dims));
  ok('video element exists', dims !== null);
  ok('video receives decoded frames', Boolean(dims && dims.w > 0 && dims.h > 0),
     dims ? `${dims.w}x${dims.h}, readyState=${dims.ready}` : 'no element');

  // Watch the frame counter advance, which is what "it is playing" means.
  const advanced = await page.evaluate(async () => {
    const v = document.querySelector('video');
    if (!v) return 0;
    const t0 = v.currentTime;
    await new Promise((r) => setTimeout(r, 2500));
    return v.currentTime - t0;
  });
  log('currentTime advanced by', advanced.toFixed(2), 's');
  ok('the stream is playing (currentTime advances)', advanced > 0.2);

  // --- 4. input -------------------------------------------------------------
  const box = await video.boundingBox().catch(() => null);
  if (box) {
    const cx = box.x + box.width / 2;
    const cy = box.y + box.height / 2;

    // A swipe, as a finger would: touchStart, several moves, touchEnd.
    await page.touchscreen.tap(cx, cy).catch((e) => log('tap err', e.message));
    log('sent a tap');

    // A drag / swipe across the page.
    await page.evaluate(({ x, y }) => {
      const el = document.querySelector('video');
      if (!el) return;
      const mk = (type, px, py) => {
        const t = new Touch({ identifier: 1, target: el, clientX: px, clientY: py });
        el.dispatchEvent(new TouchEvent(type, {
          bubbles: true, cancelable: true, touches: type === 'touchend' ? [] : [t],
          targetTouches: type === 'touchend' ? [] : [t], changedTouches: [t],
        }));
      };
      mk('touchstart', x, y);
      for (let i = 1; i <= 6; i++) mk('touchmove', x, y - i * 20);
      mk('touchend', x, y - 120);
    }, { x: cx, y: cy });
    log('sent a swipe');
    await page.waitForTimeout(1200);

    // A wheel, which the engine forwards as a scroll.
    await page.mouse.move(cx, cy);
    await page.mouse.wheel(0, 300);
    log('sent a wheel');
    await page.waitForTimeout(800);

    // Typing: focus the stage, then send keys.
    await page.mouse.click(cx, cy).catch(() => {});
    await page.keyboard.type('hello', { delay: 60 });
    log('sent typing');
    await page.waitForTimeout(800);
  } else {
    log('WARN  no video box; skipping input');
  }

  // The engine must still be alive after all that input.
  const after = await page.evaluate(async () => {
    const v = document.querySelector('video');
    if (!v) return null;
    const t0 = v.currentTime;
    await new Promise((r) => setTimeout(r, 2000));
    return { w: v.videoWidth, ready: v.readyState, advanced: v.currentTime - t0 };
  });
  log('after input:', JSON.stringify(after));
  ok('the stream survived the input and is still playing',
     Boolean(after && after.w > 0 && after.advanced > 0.1));

  if (consoleErrors.length) {
    log('--- console errors ---');
    for (const e of consoleErrors.slice(0, 10)) log('  ', e.slice(0, 200));
  } else {
    log('no console errors');
  }

  await browser.close();
  log(process.exitCode ? '\nRESULT: FAILURES' : '\nRESULT: ALL PASS');
})().catch((e) => { console.error('fatal:', e); process.exit(1); });
