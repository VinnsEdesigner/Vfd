const { chromium } = require("playwright-core");
(async () => {
  const b = await chromium.launch({ executablePath: "/usr/bin/chromium", args: ["--no-sandbox","--disable-dev-shm-usage"] });
  const ctx = await b.newContext({ viewport: { width: 412, height: 915 }, hasTouch: true, isMobile: true });
  const p = await ctx.newPage();
  await p.goto("https://vyzorix-file-downloader.pages.dev/", { waitUntil: "domcontentloaded" });
  await p.evaluate((eng) => {
    localStorage.setItem("vyzorix.media.connection", JSON.stringify({ baseUrl: eng, authToken: "" }));
    localStorage.setItem("vyzorix.local-storage", JSON.stringify({ tier: "opfs" }));
  }, process.env.ENGINE);
  await p.reload({ waitUntil: "domcontentloaded" });
  await p.waitForTimeout(2500);
  const txt = await p.locator("body").innerText();
  console.log("=== BODY (first 700) ===");
  console.log(txt.slice(0, 700));
  const ls = await p.evaluate(() => ({ conn: localStorage.getItem("vyzorix.media.connection"), store: localStorage.getItem("vyzorix.local-storage") }));
  console.log("=== localStorage ===", JSON.stringify(ls));
  await b.close();
})().catch(e => { console.error("fatal", e.message); process.exit(1); });
