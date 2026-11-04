const { chromium } = require("playwright-core");
(async () => {
  const b = await chromium.launch({ executablePath: "/usr/bin/chromium", args: ["--no-sandbox","--disable-dev-shm-usage"] });
  const ctx = await b.newContext({ viewport: { width: 412, height: 915 }, hasTouch: true, isMobile: true });
  const p = await ctx.newPage();
  await p.goto("http://127.0.0.1:8099/", { waitUntil: "domcontentloaded" });
  await p.evaluate(() => {
    localStorage.setItem("vyzorix.media.connection", JSON.stringify({ baseUrl: "http://127.0.0.1:12001", authToken: "" }));
    localStorage.setItem("vyzorix.local-storage", JSON.stringify({ tier: "opfs" }));
  });
  await p.reload({ waitUntil: "domcontentloaded" });
  await p.waitForTimeout(2000);
  // What did capabilities return, as the app saw it?
  const caps = await p.evaluate(async () => {
    const r = await fetch("http://127.0.0.1:12001/api/v1/capabilities");
    const d = await r.json();
    return { rtc: d.rtc, interactive: d.interactive, render: d.render };
  });
  console.log("CAPS", JSON.stringify(caps));
  await p.getByRole("button", { name: "Browse", exact: true }).first().click();
  await p.waitForTimeout(1200);
  await p.getByRole("button", { name: "View a page", exact: true }).first().click();
  await p.waitForTimeout(1000);
  await p.getByLabel("Address").first().fill("https://example.com/");
  await p.waitForTimeout(2000);
  console.log("MODES:", (await p.locator(".browse__modes").allInnerTexts()).join(" || "));
  console.log("pressed:", await p.evaluate(() => Array.from(document.querySelectorAll("[aria-pressed=true]")).map(e => e.textContent.trim())));
  await b.close();
})().catch(e => { console.error("fatal", e.message); process.exit(1); });
