<script lang="ts">
  import { api, type Mood, type ScreenName, type Trigger } from '../api';
  import { attempt } from '../lib/ui.svelte';
  const screens: [ScreenName, string][] = [['boot', 'Boot'], ['wifi', 'Wi-Fi'], ['setup', 'Setup'], ['face', 'Face'], ['ledger', 'Ledger'], ['listening', 'Listening'], ['thinking', 'Thinking'], ['speaking', 'Speaking']];
  const moods: [Mood, string][] = [['auto', 'Auto'], ['neutral', 'Contempt'], ['happy', 'Satisfied'], ['excited', 'Amused'], ['curious', 'Scanning'], ['surprised', 'Alarmed'], ['sleepy', 'Standby'], ['worried', 'Rationing']];
  const fire = (t: Trigger) => attempt(api.trigger(t));
</script>

<h2>Try it</h2>
<div class="chips">
  <button class="primary" onclick={() => fire({ demo: 'ask' })}>▶ "How much Claude do I have left?"</button>
  <button onclick={() => fire({ demo: 'power-on' })}>⏻ Power on</button>
  <button onclick={() => fire({ demo: 'first-run' })}>⏻ First run</button>
</div>
<h2>Screen</h2>
<div class="chips">{#each screens as [v, l]}<button onclick={() => fire({ screen: v })}>{l}</button>{/each}</div>
<h2>Mood</h2>
<div class="chips">{#each moods as [v, l]}<button onclick={() => fire({ mood: v })}>{l}</button>{/each}</div>
<p class="muted">Auto: satisfied under 25 % used, rationing at 85 %+, standby after a minute with nobody and nothing moving in view. Otherwise, contempt.</p>
