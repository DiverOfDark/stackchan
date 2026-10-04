// A simulated Femto for `npm run dev` (PRD §6.7 developer experience).
import type { Plugin } from 'vite';
import type { IncomingMessage, ServerResponse } from 'node:http';
import type { Connections, Network, Settings, Status } from '../src/api';

const defaults: Settings = {
  name: 'Femto',
  honorific: 'sir',
  eyewear: 'exec-glasses',
  corp: true,
  corp_name: 'Aldgate Dynamics',
  accent: 'signal-red',
  fx: true,
  follow: true,
  head_motion: 'calm',
  camera: true,
  brightness: null,
  volume: 60,
  tz: 'Europe/Berlin',
  voice_lang: 'auto',
  led_mode: 'usage',
  led_brightness: 40,
  led_flip: false,
};

export function mockDevice(opts: { setup?: boolean } = {}): Plugin {
  const started = Date.now();
  let saved: Settings = { ...defaults };
  let live: Settings = { ...defaults };
  let setup = opts.setup ?? false;
  let password: string | null = null;
  let session: string | null = null;
  let wifi = { ssid: setup ? null : 'HomeNet', password: '' };
  let conn: Connections & { token: string } = {
    usage_url: 'https://trmnl.example.com',
    usage_token_set: false,
    token: '',
    voice_url: '',
  };
  let screen = 'face';
  let motion = { yaw: 0, pitch: 25, zero: { yaw: 478, pitch: 544 }, torque: true, move_ms: 60, stiffness: 80, max_speed: 60, rest_pitch: false, limits: { yaw: 60, pitch_min: 0, pitch_max: 60 } };

  // A small gradient BMP standing in for camera/screen frames.
  const bmp = (w: number, h: number, hue: number) => {
    const row = (w * 3 + 3) & ~3;
    const b = Buffer.alloc(54 + row * h);
    b.write('BM');
    b.writeUInt32LE(b.length, 2);
    b.writeUInt32LE(54, 10);
    b.writeUInt32LE(40, 14);
    b.writeInt32LE(w, 18);
    b.writeInt32LE(-h, 22);
    b.writeUInt16LE(1, 26);
    b.writeUInt16LE(24, 28);
    const t = Date.now() / 1000;
    for (let y = 0; y < h; y++)
      for (let x = 0; x < w; x++) {
        const o = 54 + y * row + x * 3;
        b[o] = (x * 255) / w;
        b[o + 1] = (y * 255) / h;
        b[o + 2] = 128 + 127 * Math.sin(t + hue);
      }
    return b;
  };
  let mood = 'Contempt';

  const networks: Network[] = [
    { ssid: 'HomeNet', rssi: -53, secure: true },
    { ssid: 'FRITZ!Box Gast', rssi: -67, secure: false },
    { ssid: 'Neighbour-5G', rssi: -81, secure: true },
  ];

  const status = (): Status => ({
    version: '0.1.0-mock',
    uptime_s: Math.round((Date.now() - started) / 1000),
    setup,
    auth_required: password !== null && !setup,
    mood,
    screen,
    panel: 'Ili9342e',
    wifi: { ssid: wifi.ssid, ip: wifi.ssid ? '192.168.1.42' : null, rssi: wifi.ssid ? -53 : null, connected: !!wifi.ssid },
    usage: { signed_in: true, session_pct: 38, week_pct: 61, session_reset_min: 134, stale: false, error: null },
    heap: { internal_kb: 196, psram_kb: 5214 },
    fps: 12.1,
    vision: { face: { x: Math.sin(Date.now() / 2000) * 0.5, y: -0.2 }, seen_s_ago: 0.3 },
    voice: { state: 'idle', mic: Math.abs(Math.sin(Date.now() / 700)) * 0.4 },
    dirty: JSON.stringify(saved) !== JSON.stringify(live),
  });

  const body = (req: IncomingMessage) =>
    new Promise<any>((resolve) => {
      let raw = '';
      req.on('data', (c) => (raw += c));
      req.on('end', () => {
        try {
          resolve(raw ? JSON.parse(raw) : {});
        } catch {
          resolve(raw);
        }
      });
    });

  const send = (res: ServerResponse, code: number, data?: unknown, headers: Record<string, string> = {}) => {
    res.writeHead(code, { 'Content-Type': 'application/json', ...headers });
    res.end(data === undefined ? '' : typeof data === 'string' ? data : JSON.stringify(data));
  };

  const authed = (req: IncomingMessage) =>
    setup || password === null || (session !== null && (req.headers.cookie ?? '').includes(`femto_session=${session}`));

  return {
    name: 'femto-mock-device',
    configureServer(server) {
      server.middlewares.use(async (req, res, next) => {
        const url = new URL(req.url ?? '/', 'http://x');
        if (!url.pathname.startsWith('/api/')) return next();
        const route = `${req.method} ${url.pathname.slice(4)}`;
        await new Promise((r) => setTimeout(r, 120)); // feel like Wi-Fi

        if (route === 'GET /status') return send(res, 200, status());
        if (route === 'POST /auth/login') {
          const b = await body(req);
          if (b.password !== password) return send(res, 401, 'wrong password');
          session = Math.random().toString(36).slice(2);
          return send(res, 204, undefined, { 'Set-Cookie': `femto_session=${session}; HttpOnly; SameSite=Strict; Path=/` });
        }
        if (!authed(req)) return send(res, 401, 'login required');

        switch (route) {
          case 'GET /settings':
            return send(res, 200, live);
          case 'PUT /settings':
            live = { ...live, ...(await body(req)) };
            if (live.name.trim().length === 0 || live.name.length > 12) return send(res, 422, 'name: 1–12 characters');
            return send(res, 200, live);
          case 'POST /settings/save':
            saved = { ...live };
            return send(res, 204);
          case 'POST /settings/revert':
            live = { ...saved };
            return send(res, 200, live);
          case 'GET /wifi/scan':
            await new Promise((r) => setTimeout(r, 1200));
            return send(res, 200, networks);
          case 'PUT /wifi': {
            const b = await body(req);
            wifi = { ssid: b.ssid, password: b.password };
            return send(res, 204);
          }
          case 'GET /connections': {
            const { token: _t, ...pub } = conn;
            return send(res, 200, pub);
          }
          case 'PUT /connections': {
            const b = await body(req);
            if (b.usage_url !== undefined) conn.usage_url = b.usage_url;
            if (b.voice_url !== undefined) conn.voice_url = b.voice_url;
            if (b.usage_token !== undefined) {
              conn.token = b.usage_token;
              conn.usage_token_set = b.usage_token !== '';
            }
            const { token: _t, ...pub } = conn;
            return send(res, 200, pub);
          }
          case 'POST /connections/test': {
            const which = url.searchParams.get('which');
            const target = which === 'voice' ? conn.voice_url : conn.usage_url;
            if (!target) return send(res, 200, { ok: false, status: null, latency_ms: null, detail: 'not configured' });
            return send(res, 200, { ok: true, status: 200, latency_ms: 84, detail: which === 'usage' ? 'signed in · session 38 %' : 'reachable' });
          }
          case 'POST /test': {
            const b = await body(req);
            if (b.screen) screen = b.screen;
            if (b.mood) mood = b.mood;
            if (b.demo) screen = b.demo === 'ask' ? 'listening' : 'boot';
            return send(res, 204);
          }
          case 'POST /auth/password':
            password = (await body(req)).password || null;
            return send(res, 204);
          case 'POST /setup/finish':
            saved = { ...live };
            setup = false;
            return send(res, 204);
          case 'POST /reboot':
          case 'POST /factory-reset':
            return send(res, 204);
          case 'GET /camera.bmp':
            res.writeHead(200, { 'Content-Type': 'image/bmp' });
            return res.end(bmp(160, 120, 0));
          case 'GET /screen.bmp':
            res.writeHead(200, { 'Content-Type': 'image/bmp' });
            return res.end(bmp(320, 240, 2));
          case 'GET /motion':
            return send(res, 200, motion);
          case 'PUT /motion': {
            const b = await body(req);
            if (b.jog) [motion.yaw, motion.pitch] = b.jog;
            if (b.torque !== undefined) motion.torque = b.torque;
            for (const k of ['move_ms', 'stiffness', 'max_speed', 'rest_pitch'] as const) if (b[k] !== undefined) (motion as any)[k] = b[k];
            return send(res, 204);
          }
          case 'POST /motion/zero':
            motion = { ...motion, yaw: 0, pitch: 0, zero: { yaw: 470, pitch: 560 } };
            return send(res, 200, motion.zero);
          case 'POST /voice/talk':
            return send(res, 204);
          case 'POST /ota':
            req.resume();
            req.on('end', () => send(res, 204));
            return;
        }
        send(res, 404, 'no such endpoint');
      });
    },
  };
}
