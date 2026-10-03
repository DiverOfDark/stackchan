<script lang="ts">
  import { api, type Settings, type Status } from '../api';
  import { attempt } from '../lib/ui.svelte';
  import SettingsForm from '../lib/SettingsForm.svelte';
  let { status }: { status: Status } = $props();
  let s = $state<Settings | null>(null);

  $effect(() => {
    api.settings().then((v) => (s = v));
  });

  // Applied live on the device; persisted only on Save (PRD §6.7).
  async function change(p: Partial<Settings>) {
    if (!s) return;
    const prev = s;
    s = { ...s, ...p };
    const v = await attempt(api.patchSettings(p));
    s = v ?? prev;
  }
</script>

{#if s}
  <SettingsForm {s} onchange={change} />
{:else}
  <p class="muted mono">Reading settings…</p>
{/if}

{#if status.dirty}
  <div class="savebar"><div class="inner">
    <span class="grow muted">Applied live, not saved.</span>
    <button onclick={async () => (s = (await attempt(api.revertSettings())) ?? s)}>Revert</button>
    <button class="primary" onclick={() => attempt(api.saveSettings(), 'Saved. Noted, sir.')}>Save</button>
  </div></div>
{/if}
