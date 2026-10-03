<script lang="ts">
  import { api } from '../api';
  import { attempt } from '../lib/ui.svelte';
  let { onDone }: { onDone: () => void } = $props();
  let password = $state('');
  let busy = $state(false);

  async function submit(e: SubmitEvent) {
    e.preventDefault();
    busy = true;
    await attempt(api.login(password));
    onDone();
    busy = false;
  }
</script>

<h2>Identify yourself</h2>
<p class="quip">Unregistered personnel are recycled.</p>
<form onsubmit={submit}>
  <label class="field"><span class="label">Admin password</span>
    <input type="password" bind:value={password} autocomplete="current-password" required />
  </label>
  <button class="primary" disabled={busy}>Log in</button>
</form>
