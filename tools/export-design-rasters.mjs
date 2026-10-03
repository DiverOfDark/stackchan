// Exports the design's "On device" canvases (15 screens, 320x240) to design/raster/.
// Usage: (cd design && python3 -m http.server 18765) & node tools/export-design-rasters.mjs
// Needs: npm i playwright && npx playwright install chromium
import { chromium } from 'playwright';
import fs from 'fs';
const b = await chromium.launch();
const p = await b.newPage({ viewport: { width: 1400, height: 1000 } });
await p.goto('http://localhost:18765/Femto%20Dystopia.dc.html');
await p.waitForTimeout(8000);
const urls = await p.evaluate(() => [...document.querySelectorAll('canvas')].map(c => c.toDataURL('image/png')));
const dir = 'design/raster';
fs.mkdirSync(dir, { recursive: true });
urls.forEach((u, i) => fs.writeFileSync(`${dir}/${String(i).padStart(2, '0')}.png`, Buffer.from(u.split(',')[1], 'base64')));
console.log(urls.length);
await b.close();
