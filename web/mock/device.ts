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
  camera: true,
  brightness: null,
  volume: 60,
  tz: 'Europe/Berlin',
  voice_lang: 'auto',
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
    usage_url: 'https://trmnl.kirillorlov.pro',
    usage_token_set: false,
    token: '',
    voice_url: '',
  };
  let screen = 'face';
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
