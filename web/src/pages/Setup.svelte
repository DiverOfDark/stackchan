<script lang="ts">
  // First-run wizard (PRD §6.7): Wi-Fi → backends → character → password → done.
  import { api, type Settings, type Status } from '../api';
  import { attempt } from '../lib/ui.svelte';
  import WifiPicker from '../lib/WifiPicker.svelte';
  import ConnectionsForm from '../lib/ConnectionsForm.svelte';
  import SettingsForm from '../lib/SettingsForm.svelte';
  let { status }: { status: Status } = $props();

  let step = $state(0);
  let joined = $state<string | null>(null);
  let s = $state<Settings | null>(null);
  let pw = $state('');
  let done = $state(false);
  const steps = ['Wi-Fi', 'Backends', 'Character', 'Password', 'Finish'];

  $effect(() => {
    api.settings().then((v) => (s = v));
  });

  async function change(p: Partial<Settings>) {
    if (!s) return;
    s = { ...s, ...p };
    s = (await attempt(api.patchSettings(p))) ?? s;
  }

  async function finish() {
    if (pw && (await attempt(api.setPassword(pw))) === undefined) return;
    if ((await attempt(api.finishSetup())) !== undefined) done = true;
  }
</script>

<h2>Connect me. I don't enjoy waiting.</h2>
<div class="steps">{#each steps as _, i}<i class:done={i < step} class:now={i === step}></i>{/each}</div>
<div class="label">{String(step + 1).padStart(2, '0')} // {steps[step]}</div>

{#if done}
  <div class="card">
    <p class="quip">Registered. Joining {joined ?? 'the grid'} now.</p>
    <p>Reconnect your phone to your usual Wi-Fi, then open <a href="http://femto.local">http://femto.local</a>
      (or the IP shown on Femto's screen).</p>
  </div>
{:else if step === 0}
  {#if status.wifi.connected && !joined}<p class="muted">Currently on {status.wifi.ssid}.</p>{/if}
  <WifiPicker saveLabel="Use this network" onsaved={(ssid) => ((joined = ssid), (step = 1))} />
  {#if status.wifi.connected}<p><button onclick={() => (step = 1)}>Keep current network</button></p>{/if}
{:else if step === 1}
  <ConnectionsForm />
  <div class="row"><button onclick={() => (step = 0)}>Back</button><button class="primary" onclick={() => (step = 2)}>Next</button></div>
{:else if step === 2}
  {#if s}<SettingsForm {s} onchange={change} only="character" />{/if}
  <p class="muted">Everything else lives under Settings later.</p>
  <div class="row"><button onclick={() => (step = 1)}>Back</button><button class="primary" onclick={() => (step = 3)}>Next</button></div>
{:else if step === 3}
  <p>Protects this page once Femto is on your network. Leave empty for none.</p>
  <label class="field"><span class="label">Admin password</span><input type="password" bind:value={pw} autocomplete="new-password" /></label>
  <div class="row"><button onclick={() => (step = 2)}>Back</button><button class="primary" onclick={() => (step = 4)}>Next</button></div>
{:else}
  <div class="card">
    <div class="mono">network · {joined ?? status.wifi.ssid ?? 'none'}</div>
    <div class="mono">name · {s?.name} · {s?.honorific}</div>
    <div class="mono">password · {pw ? 'set' : 'none'}</div>
  </div>
  <p class="quip">Unregistered assets are recycled.</p>
  <div class="row"><button onclick={() => (step = 3)}>Back</button><button class="primary" onclick={finish}>Register & reboot</button></div>
{/if}
