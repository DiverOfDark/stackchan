<script lang="ts">
  import { api } from '../api';
  let lines = $state<string[]>([]);
  let open = $state(false);
  let paused = $state(false);
  let filter = $state('');
  let box: HTMLPreElement | undefined = $state();
  let partial = '';

  $effect(() => {
    const close = api.logs(
      (t) => {
        if (paused) return;
        const parts = (partial + t.replace(/\x1b\[[0-9;]*m/g, '')).split('\n');
        partial = parts.pop() ?? '';
        lines = [...lines, ...parts].slice(-800);
        queueMicrotask(() => box && (box.scrollTop = box.scrollHeight));
      },
      (o) => (open = o),
    );
    return close;
  });
  const shown = $derived(filter ? lines.filter((l) => l.toLowerCase().includes(filter.toLowerCase())) : lines);
</script>

<h2>Logs <span class="tag" class:bone={!open}>{open ? 'live' : 'offline'}</span></h2>
<div class="row">
  <input class="grow" type="text" placeholder="filter" bind:value={filter} />
  <button onclick={() => (paused = !paused)}>{paused ? 'Resume' : 'Pause'}</button>
  <button onclick={() => (lines = [])}>Clear</button>
</div>
<pre bind:this={box}>{#each shown as l}<span class:err={/^E \(/.test(l)} class:warn={/^W \(/.test(l)}>{l}
</span>{/each}</pre>

<style>
  pre { background: var(--panel); border: 1px solid var(--dim); padding: 8px; height: 65vh; overflow: auto; font: 11px/1.35 var(--mono); white-space: pre-wrap; word-break: break-all; margin-top: 10px; }
  .err { color: var(--a); }
  .warn { color: var(--toxic); }
</style>
