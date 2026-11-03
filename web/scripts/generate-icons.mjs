/**
 * Rasterise the logo at every size the app needs, from the vector source.
 *
 * Chromium does the rendering: it is already here for the tests, it antialiases
 * properly, and it reads the SVG's gradients the same way the app does. Each
 * icon is rendered at its native size rather than downscaled from one bitmap,
 * which is what makes a 16px favicon and a 512px app icon both sharp.
 */
import { chromium } from 'playwright';
import { readFileSync, writeFileSync } from 'node:fs';

const svg = readFileSync('public/logo.svg', 'utf8');

// The sizes that matter: the .ico set, the iOS touch icon, the manifest set,
// and a 1024 for high-density displays and store listings.
const SIZES = [16, 32, 48, 96, 180, 192, 256, 384, 512, 1024];

const browser = await chromium.launch();

/** Render the SVG into a PNG of exactly `size`×`size`, transparent outside the plate. */
async function render(size) {
  const page = await browser.newPage({
    viewport: { width: size, height: size },
    deviceScaleFactor: 1,
  });
  await page.setContent(
    `<!doctype html><html><head><style>
       html,body{margin:0;padding:0;background:transparent;width:${size}px;height:${size}px;overflow:hidden}
       svg{display:block;width:${size}px;height:${size}px}
     </style></head><body>${svg}</body></html>`,
    { waitUntil: 'load' },
  );
  const png = await page.screenshot({ omitBackground: true, type: 'png' });
  await page.close();
  return png;
}

const rendered = {};
for (const size of SIZES) {
  rendered[size] = await render(size);
  console.log(`rendered ${size}x${size} (${rendered[size].length} bytes)`);
}

/** Build an .ico from PNG entries. PNG-compressed entries are valid since Vista
 *  and are what every modern browser expects. */
function buildIco(pngs) {
  const count = pngs.length;
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0); // reserved
  header.writeUInt16LE(1, 2); // type: icon
  header.writeUInt16LE(count, 4);

  const dir = Buffer.alloc(16 * count);
  let offset = 6 + 16 * count;
  pngs.forEach((png, i) => {
    const w = png.size >= 256 ? 0 : png.size;
    const b = i * 16;
    dir.writeUInt8(w, b + 0);
    dir.writeUInt8(w, b + 1);
    dir.writeUInt8(0, b + 2); // palette
    dir.writeUInt8(0, b + 3); // reserved
    dir.writeUInt16LE(1, b + 4); // planes
    dir.writeUInt16LE(32, b + 6); // bits per pixel
    dir.writeUInt32LE(png.data.length, b + 8);
    dir.writeUInt32LE(offset, b + 12);
    offset += png.data.length;
  });

  return Buffer.concat([header, dir, ...pngs.map((p) => p.data)]);
}

// Files the app actually references.
writeFileSync('public/favicon-16.png', rendered[16]);
writeFileSync('public/favicon-32.png', rendered[32]);
writeFileSync('public/apple-touch-icon.png', rendered[180]);
writeFileSync('public/icon-192.png', rendered[192]);
writeFileSync('public/icon-512.png', rendered[512]);
writeFileSync('public/icon-1024.png', rendered[1024]);
writeFileSync(
  'public/favicon.ico',
  buildIco([16, 32, 48].map((size) => ({ size, data: rendered[size] }))),
);

console.log('wrote favicon.ico (16/32/48), favicon-16/32, apple-touch-icon(180), icon-192/512/1024');
await browser.close();
