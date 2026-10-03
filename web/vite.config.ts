import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { mockDevice } from './mock/device';

// `npm run dev`                 → simulated device (mock/device.ts)
// `DEVICE=femto.local npm run dev` → proxy /api to a real device
// `SETUP=1 npm run dev`          → simulated device in first-run setup mode
const device = process.env.DEVICE;

export default defineConfig({
  plugins: [svelte(), viteSingleFile(), ...(device ? [] : [mockDevice({ setup: !!process.env.SETUP })])],
  server: device ? { proxy: { '/api': `http://${device}` } } : {},
  build: { target: 'es2020', cssCodeSplit: false, assetsInlineLimit: 100_000_000 },
});
