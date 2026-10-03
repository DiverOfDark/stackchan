// Device HTTP API contract (PRD §6.7). The firmware implements exactly
// these shapes; `mock/device.ts` simulates them for `npm run dev`.

export type Honorific = 'sir' | 'madam' | 'guv';
export type Eyewear = 'exec-glasses' | 'ar-half-lens' | 'reticle' | 'none';
export type Accent = 'signal-red' | 'toxic-green' | 'ice-blue';
export type VoiceLang = 'auto' | 'ru' | 'en';

/** Mirrors `femto_core::Settings`. */
export interface Settings {
  name: string;
  honorific: Honorific;
  eyewear: Eyewear;
  corp: boolean;
  corp_name: string;
  accent: Accent;
  fx: boolean;
  follow: boolean;
  camera: boolean;
  /** 10–100, or null for auto. */
  brightness: number | null;
  volume: number;
  tz: string;
  voice_lang: VoiceLang;
}

export interface Status {
  version: string;
  uptime_s: number;
  /** Setup mode: SoftAP up, no auth required. */
  setup: boolean;
  /** Admin password set (login needed outside setup). */
  auth_required: boolean;
  mood: string;
  screen: string;
  panel: string;
  wifi: { ssid: string | null; ip: string | null; rssi: number | null; connected: boolean };
  usage: {
    signed_in: boolean;
    session_pct: number | null;
    week_pct: number | null;
    session_reset_min: number | null;
    stale: boolean;
    error: string | null;
  };
  heap: { internal_kb: number; psram_kb: number };
  fps: number;
  /** Settings differ from what is saved in flash. */
  dirty: boolean;
}

export interface Network {
  ssid: string;
  rssi: number;
  secure: boolean;
}

export interface Connections {
  usage_url: string;
  usage_token_set: boolean;
  voice_url: string;
}

export interface ConnectionsPatch {
  usage_url?: string;
  /** Empty string clears it. Never sent back by the device. */
  usage_token?: string;
  voice_url?: string;
}

export interface TestResult {
  ok: boolean;
  status: number | null;
  latency_ms: number | null;
  detail: string;
}

export type ScreenName = 'boot' | 'wifi' | 'setup' | 'face' | 'ledger' | 'listening' | 'thinking' | 'speaking';
export type Mood = 'auto' | 'neutral' | 'happy' | 'excited' | 'curious' | 'surprised' | 'sleepy' | 'worried';

export interface Trigger {
  screen?: ScreenName;
  mood?: Mood;
  demo?: 'ask' | 'power-on' | 'first-run';
}

export class ApiError extends Error {
  constructor(public status: number, message: string) {
    super(message);
  }
}

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(`/api${path}`, {
    method,
    headers: body === undefined ? {} : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
    credentials: 'same-origin',
  });
  if (!res.ok) throw new ApiError(res.status, (await res.text()) || res.statusText);
  const text = await res.text();
  return (text ? JSON.parse(text) : undefined) as T;
}

export const api = {
  status: () => call<Status>('GET', '/status'),
  settings: () => call<Settings>('GET', '/settings'),
  /** Partial merge, applied live (not persisted until save). */
  patchSettings: (p: Partial<Settings>) => call<Settings>('PUT', '/settings', p),
  saveSettings: () => call<void>('POST', '/settings/save'),
  revertSettings: () => call<Settings>('POST', '/settings/revert'),
  scan: () => call<Network[]>('GET', '/wifi/scan'),
  setWifi: (ssid: string, password: string) => call<void>('PUT', '/wifi', { ssid, password }),
  connections: () => call<Connections>('GET', '/connections'),
  setConnections: (p: ConnectionsPatch) => call<Connections>('PUT', '/connections', p),
  testConnection: (which: 'usage' | 'voice') => call<TestResult>('POST', `/connections/test?which=${which}`),
  trigger: (t: Trigger) => call<void>('POST', '/test', t),
  login: (password: string) => call<void>('POST', '/auth/login', { password }),
  setPassword: (password: string) => call<void>('POST', '/auth/password', { password }),
  reboot: () => call<void>('POST', '/reboot'),
  factoryReset: () => call<void>('POST', '/factory-reset', { confirm: 'WIPE' }),
  /** Finishes setup: saves everything and reboots into station mode. */
  finishSetup: () => call<void>('POST', '/setup/finish'),
  async ota(file: File, onProgress: (pct: number) => void): Promise<void> {
    await new Promise<void>((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota');
      xhr.upload.onprogress = (e) => e.lengthComputable && onProgress(Math.round((e.loaded * 100) / e.total));
      xhr.onload = () => (xhr.status < 300 ? resolve() : reject(new ApiError(xhr.status, xhr.responseText)));
      xhr.onerror = () => reject(new ApiError(0, 'upload failed'));
      xhr.send(file);
    });
  },
};
