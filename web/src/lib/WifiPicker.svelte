<script lang="ts">
  import { api, type Network } from '../api';
  import { attempt } from './ui.svelte';
  let { onsaved, saveLabel = 'Join' }: { onsaved?: (ssid: string) => void; saveLabel?: string } = $props();
  let nets = $state<Network[] | null>(null);
  let ssid = $state('');
  let password = $state('');
  let manual = $state(false);
  let busy = $state(false);

  async function scan() {
    nets = null;
    nets = (await attempt(api.scan())) ?? [];
  }
  $effect(() => {
    scan();
  });

  async function save() {
    busy = true;
    if ((await attempt(api.setWifi(ssid, password), 'Credentials stored.')) !== undefined) onsaved?.(ssid);
    busy = false;
  }
  const bars = (rssi: number) => (rssi > -60 ? '▮▮▮' : rssi > -72 ? '▮▮▯' : '▮▯▯');
</script>

<div class="row"><span class="label grow">Networks in range</span><button onclick={scan}>Scan</button></div>
{#if nets === null}
  <p class="mono muted">Searching the grid…</p>
{:else}
  {#each nets as n}
    <button class="net" class:on={ssid === n.ssid && !manual} onclick={() => ((ssid = n.ssid), (manual = false))}>
      <span>{n.ssid}{n.secure ? '' : ' · open'}</span><span class="mono muted">{bars(n.rssi)} {n.rssi}</span>
    </button>
  {/each}
  <button class="net" class:on={manual} onclick={() => ((manual = true), (ssid = ''))}><span>Other network…</span><span></span></button>
{/if}
{#if manual}
  <label class="field"><span class="label">SSID</span><input type="text" bind:value={ssid} maxlength="32" /></label>
{/if}
{#if ssid}
  <label class="field"><span class="label">Password for {ssid}</span>
    <input type="password" bind:value={password} maxlength="64" autocomplete="off" />
  </label>
  <button class="primary" disabled={busy} onclick={save}>{saveLabel}</button>
{/if}
