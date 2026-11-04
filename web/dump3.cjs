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
  await p.getByRole("button", { name: "Browse", exact: true }).first().click();
  await p.waitForTimeout(1200);
  await p.getByRole("button", { name: "View a page", exact: true }).first().click();
  await p.waitForTimeout(1000);
  await p.getByLabel("Address").first().fill("https://example.com/");
  await p.waitForTimeout(1500);
  await p.getByRole("button", { name: "Video", exact: true }).first().click();
  await p.waitForTimeout(4000);
  const html = await p.evaluate(() => {
    const live = document.querySelector(".live");
    return { len: live ? live.outerHTML.length : 0, html: live ? live.outerHTML : "none" };
  });
  console.log("LEN", html.len);
  console.log(html.html);
  await b.close();
})().catch(e => { console.error("fatal", e.message); process.exit(1); });
