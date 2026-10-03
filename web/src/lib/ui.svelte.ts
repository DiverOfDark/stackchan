// Small shared UI state: toasts and the current route.
export const ui = $state({ toast: null as { text: string; err: boolean } | null, route: location.hash.slice(1) || '/' });

let timer: ReturnType<typeof setTimeout> | undefined;
export function toast(text: string, err = false) {
  ui.toast = { text, err };
  clearTimeout(timer);
  timer = setTimeout(() => (ui.toast = null), err ? 5000 : 2500);
}

export function go(route: string) {
  location.hash = route;
}

window.addEventListener('hashchange', () => (ui.route = location.hash.slice(1) || '/'));

/** Wrap an API call: toast errors, return undefined on failure. */
export async function attempt<T>(p: Promise<T>, okText?: string): Promise<T | undefined> {
  try {
    const v = await p;
    if (okText) toast(okText);
    return v;
  } catch (e) {
    toast(e instanceof Error ? e.message : String(e), true);
    return undefined;
  }
}

export const levelClass = (pct: number | null) => (pct === null ? 'muted' : pct >= 85 ? 'hot' : pct >= 60 ? 'caution' : 'ok');
export const fmtMin = (m: number) => `${Math.floor(m / 60)}h ${String(m % 60).padStart(2, '0')}m`;
