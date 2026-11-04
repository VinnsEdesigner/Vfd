const { chromium } = require("playwright-core");
(async () => {
  const b = await chromium.launch({ executablePath: "/usr/bin/chromium", args: ["--no-sandbox","--disable-dev-shm-usage"] });
  const ctx = await b.newContext({ viewport: { width: 412, height: 915 }, hasTouch: true, isMobile: true });
  const p = await ctx.newPage();
  p.on("pageerror", e => console.log("PAGEERROR", String(e).slice(0,200)));
  await p.goto("http://127.0.0.1:8099/", { waitUntil: "domcontentloaded" });
  await p.evaluate((eng) => {
    localStorage.setItem("vyzorix.media.connection", JSON.stringify({ baseUrl: eng, authToken: "" }));
    localStorage.setItem("vyzorix.local-storage", JSON.stringify({ tier: "opfs" }));
  }, process.env.ENGINE);
  await p.reload({ waitUntil: "domcontentloaded" });
  await p.waitForTimeout(2000);
  console.log("=== after load, buttons ===");
  console.log((await p.getByRole("button").allInnerTexts()).slice(0,25).join(" | "));
  await p.getByRole("button", { name: "Browse", exact: true }).first().click();
  await p.waitForTimeout(1500);
  console.log("=== on Browse, buttons ===");
  console.log((await p.getByRole("button").allInnerTexts()).slice(0,30).join(" | "));
  await p.getByRole("button", { name: "View a page", exact: true }).first().click();
  await p.waitForTimeout(1000);
  console.log("=== View a page, buttons ===");
  console.log((await p.getByRole("button").allInnerTexts()).slice(0,30).join(" | "));
  console.log("=== inputs ===");
  for (const el of await p.locator("input").all()) {
    console.log("  input:", await el.getAttribute("id"), "|", await el.getAttribute("placeholder"));
  }
  await p.getByLabel("Address").first().fill("https://example.com/");
  await p.waitForTimeout(500);
  console.log("=== after typing, buttons ===");
  console.log((await p.getByRole("button").allInnerTexts()).slice(0,30).join(" | "));
  await b.close();
})().catch(e => { console.error("fatal", e.message); process.exit(1); });
