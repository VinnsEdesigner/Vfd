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
  await p.waitForTimeout(3000);
  // Scope the mode click to the "How to show the page" group.
  const group = p.locator('[aria-label="How to show the page"]').first();
  console.log("groups:", await p.locator('[aria-label="How to show the page"]').count());
  await group.getByRole("button", { name: "Video", exact: true }).click();
  await p.waitForTimeout(5000);
  const info = await p.evaluate(() => {
    const live = document.querySelector(".live");
    return {
      kids: live ? Array.from(live.children).map(c => c.tagName + "." + (c.className || "")) : null,
      stage: document.querySelectorAll(".live__stage").length,
      video: document.querySelectorAll("video").length,
      blank: document.querySelectorAll(".live__blank").length,
      note: document.querySelectorAll(".live__note").length,
    };
  });
  console.log("INFO " + JSON.stringify(info));
  await b.close();
})().catch(e => { console.error("fatal " + e.message); process.exit(1); });
