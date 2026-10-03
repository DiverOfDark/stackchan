// Gzips dist/index.html for the firmware and enforces the 80 KB budget (PRD §6.7).
import { readFileSync, writeFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';

const html = readFileSync('dist/index.html');
const gz = gzipSync(html, { level: 9 });
writeFileSync('dist/index.html.gz', gz);
const kb = (n) => (n / 1024).toFixed(1);
console.log(`web UI: ${kb(html.length)} KB → ${kb(gz.length)} KB gzipped (budget 80 KB)`);
if (gz.length > 80 * 1024) {
  console.error('over the 80 KB budget');
  process.exit(1);
}
