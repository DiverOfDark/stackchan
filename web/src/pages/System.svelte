<script lang="ts">
  import { api, type Status } from '../api';
  import { attempt, toast } from '../lib/ui.svelte';
  let { status }: { status: Status } = $props();
  let progress = $state<number | null>(null);
  let wipeText = $state('');
  let pw = $state('');
  let pw2 = $state('');

  async function ota(e: Event) {
    const file = (e.currentTarget as HTMLInputElement).files?.[0];
    if (!file) return;
    progress = 0;
    await attempt(api.ota(file, (p) => (progress = p)), 'Flashed. Rebooting into the new firmware.');
    progress = null;
  }

  async function exportSettings() {
    const s = await attempt(api.settings());
    if (!s) return;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([JSON.stringify(s, null, 2)], { type: 'application/json' }));
    a.download = `femto-settings.json`;
    a.click();
  }

  async function importSettings(e: Event) {
    const file = (e.currentTarget as HTMLInputElement).files?.[0];
    if (!file) return;
    try {
      const s = JSON.parse(await file.text());
      if ((await attempt(api.patchSettings(s))) && (await attempt(api.saveSettings(), 'Imported and saved.')) !== undefined) return;
    } catch {
      toast('not a settings file', true);
    }
  }

  async function setPassword() {
    if (pw !== pw2) return toast('passwords differ', true);
    if (pw.length < 6) return toast('at least 6 characters', true);
    if ((await attempt(api.setPassword(pw), 'Password set.')) !== undefined) pw = pw2 = '';
  }
</script>

<h2>Firmware</h2>
<div class="card">
  <div class="mono">running {status.version} · panel {status.panel}</div>
  <label class="field"><span class="label">Upload firmware (.bin)</span>
    <input type="file" accept=".bin,application/octet-stream" onchange={ota} disabled={progress !== null} />
  </label>
  {#if progress !== null}
    <div class="meter">{#each Array(20) as _, i}<i style:background={i < progress / 5 ? 'var(--bone)' : undefined}></i>{/each}</div>
    <div class="mono">{progress}%</div>
  {/if}
</div>

<h2>Settings file</h2>
<div class="row">
  <button onclick={exportSettings}>Export</button>
  <label class="grow"><span class="label">Import</span> <input type="file" accept=".json" onchange={importSettings} /></label>
</div>

<h2>Admin password</h2>
<label class="field"><span class="label">New password</span><input type="password" bind:value={pw} autocomplete="new-password" /></label>
<label class="field"><span class="label">Again</span><input type="password" bind:value={pw2} autocomplete="new-password" /></label>
<button onclick={setPassword}>Set password</button>

<h2>Maintenance</h2>
<div class="row"><button onclick={() => attempt(api.reboot(), 'Rebooting.')}>Reboot</button></div>
<div class="card alert" style="margin-top:16px">
  <div class="label hot">Factory wipe · Wi-Fi, tokens, settings, password</div>
  <p class="quip">Everything I knew about you. Gone.</p>
  <label class="field"><span class="label">Type WIPE</span><input type="text" bind:value={wipeText} autocomplete="off" /></label>
  <button class="danger" disabled={wipeText !== 'WIPE'} onclick={() => attempt(api.factoryReset(), 'Wiped. Rebooting into setup.')}>Wipe</button>
</div>
