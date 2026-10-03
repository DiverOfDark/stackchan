<script lang="ts">
  import type { Status } from '../api';
  import WifiPicker from '../lib/WifiPicker.svelte';
  import ConnectionsForm from '../lib/ConnectionsForm.svelte';
  let { status }: { status: Status } = $props();
  let changing = $state(false);
</script>

<h2>Wi-Fi</h2>
<div class="card">
  <div class="row">
    <span class="grow mono">{status.wifi.connected ? `${status.wifi.ssid} · ${status.wifi.rssi} dBm` : 'not connected'}</span>
    <button onclick={() => (changing = !changing)}>{changing ? 'Cancel' : 'Change'}</button>
  </div>
  {#if changing}
    <p class="muted">Femto reboots onto the new network. If it can't join, it falls back to setup mode.</p>
    <WifiPicker saveLabel="Join & reboot" onsaved={() => (changing = false)} />
  {/if}
</div>

<h2>Backends</h2>
<ConnectionsForm />
