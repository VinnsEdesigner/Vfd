const { chromium } = require("playwright-core");
(async () => {
  const b = await chromium.launch({ executablePath: "/usr/bin/chromium", args: ["--no-sandbox","--disable-dev-shm-usage"] });
  const ctx = await b.newContext({ viewport: { width: 412, height: 915 }, hasTouch: true, isMobile: true });
  const p = await ctx.newPage();
  p.on("console", m => { if (m.type()==="error") console.log("CONSOLE-ERR", m.text().slice(0,150)); });
  p.on("requestfailed", r => console.log("REQFAIL", r.url().slice(0,90), r.failure()?.errorText));
  p.on("response", r => { if (r.url().includes("/rtc")) console.log("RTC-RESP", r.status(), r.url().slice(0,90)); });
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
  await p.getByRole("button", { name: "Video", exact: true }).first().click();
  await p.waitForTimeout(6000);
  const info = await p.evaluate(() => ({
    stage: document.querySelectorAll(".live__stage").length,
    video: document.querySelectorAll("video").length,
    blank: document.querySelectorAll(".live__blank").length,
    waiting: document.querySelectorAll(".live__waiting").length,
    liveHTMLlen: (document.querySelector(".live")||{outerHTML:""}).outerHTML.length,
  }));
  console.log("DOM", JSON.stringify(info));
  await b.close();
})().catch(e => { console.error("fatal", e.message); process.exit(1); });
