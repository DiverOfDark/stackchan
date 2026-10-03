<script lang="ts">
  import type { Status } from '../api';
  import { levelClass, fmtMin } from '../lib/ui.svelte';
  let { status }: { status: Status } = $props();
  const u = $derived(status.usage);
  const segs = (p: number | null) => Array.from({ length: 10 }, (_, i) => p !== null && i < Math.round(p / 10));
  const uptime = $derived(`${Math.floor(status.uptime_s / 3600)}h ${String(Math.floor((status.uptime_s % 3600) / 60)).padStart(2, '0')}m`);
</script>

<h2>Status <span class="tag">{status.mood}</span></h2>

<div class="card" class:alert={u.stale && u.signed_in}>
  <div class="label">The ledger {#if u.stale && u.signed_in}· <span class="hot">stale</span>{/if}</div>
  {#if !u.signed_in}
    <p class="quip">Nobody has authorised my ledger.</p>
  {:else}
    <div class="row">
      {#each [['Session // 5h', u.session_pct], ['Weekly // quota', u.week_pct]] as [label, pct]}
        <div class="grow">
          <div class="label">{label}</div>
          <div class="big {levelClass(pct as number | null)}">{pct ?? '--'}%</div>
          <div class="meter">{#each segs(pct as number | null) as on}<i style:background={on ? 'currentColor' : undefined} class={levelClass(pct as number | null)}></i>{/each}</div>
        </div>
      {/each}
    </div>
    {#if u.session_reset_min !== null}<div class="mono muted">session resets in {fmtMin(u.session_reset_min)}</div>{/if}
  {/if}
  {#if u.error}<div class="mono hot">{u.error}</div>{/if}
</div>

<div class="card">
  <div class="label">Network</div>
  <div class="mono">{status.wifi.connected ? `${status.wifi.ssid} · ${status.wifi.ip} · ${status.wifi.rssi} dBm` : 'not connected'}</div>
</div>

<div class="card">
  <div class="label">Machine</div>
  <div class="mono">fw {status.version} · up {uptime} · {status.fps.toFixed(1)} fps · panel {status.panel}</div>
  <div class="mono muted">heap {status.heap.internal_kb} KB internal · {status.heap.psram_kb} KB psram · screen {status.screen}</div>
</div>
