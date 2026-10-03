<script lang="ts">
  import { api, type Connections, type TestResult } from '../api';
  import { attempt } from './ui.svelte';
  let conn = $state<Connections | null>(null);
  let url = $state('');
  let token = $state('');
  let voice = $state('');
  let results = $state<Record<string, TestResult | 'busy'>>({});

  $effect(() => {
    api.connections().then((c) => {
      conn = c;
      url = c.usage_url;
      voice = c.voice_url;
    });
  });

  async function save() {
    const patch = { usage_url: url.trim(), voice_url: voice.trim(), ...(token ? { usage_token: token } : {}) };
    const c = await attempt(api.setConnections(patch), 'Connections stored.');
    if (c) {
      conn = c;
      token = '';
    }
  }
  async function clearToken() {
    conn = (await attempt(api.setConnections({ usage_token: '' }), 'Token cleared.')) ?? conn;
  }
  async function test(which: 'usage' | 'voice') {
    await save();
    results[which] = 'busy';
    results[which] = (await attempt(api.testConnection(which))) ?? { ok: false, status: null, latency_ms: null, detail: 'request failed' };
  }
</script>

{#snippet result(which: string)}
  {@const r = results[which]}
  {#if r === 'busy'}<span class="mono muted">testing…</span>
  {:else if r}<span class="mono {r.ok ? 'ok' : 'hot'}">{r.ok ? '✓' : '✗'} {r.status ?? ''} {r.latency_ms !== null ? `${r.latency_ms} ms` : ''} · {r.detail}</span>{/if}
{/snippet}

{#if conn}
  <label class="field"><span class="label">Usage server (trmnl-cyberpunk)</span>
    <input type="url" bind:value={url} placeholder="https://trmnl.example.com" />
  </label>
  <label class="field"><span class="label">Token {conn.usage_token_set ? '· set' : '· not set'}</span>
    <input type="password" bind:value={token} placeholder={conn.usage_token_set ? '••••••••  (leave empty to keep)' : 'STACKCHAN_TOKEN, if the server has one'} autocomplete="off" />
  </label>
  <div class="row">
    <button onclick={() => test('usage')}>Test</button>
    {#if conn.usage_token_set}<button class="danger" onclick={clearToken}>Clear token</button>{/if}
    <span class="grow">{@render result('usage')}</span>
  </div>
  <label class="field"><span class="label">Voice backend (pipecat)</span>
    <input type="url" bind:value={voice} placeholder="https://voice.example.com" />
  </label>
  <div class="row">
    <button onclick={() => test('voice')}>Test</button>
    <span class="grow">{@render result('voice')}</span>
  </div>
  <p><button class="primary" onclick={save}>Save connections</button></p>
{:else}
  <p class="muted mono">Reading connections…</p>
{/if}
