<script lang="ts">
  import type { Status } from '../api';
  import LiveImage from '../lib/LiveImage.svelte';
  let { status }: { status: Status } = $props();
  const face = $derived(status.vision?.face ?? null);
  const fresh = $derived((status.vision?.seen_s_ago ?? 99) < 1.5);
</script>

<h2>Camera</h2>
<p class="muted">What Femto sees (160×120, mirrored). Frames stay on your network; this page only fetches while open.</p>
<LiveImage src="/api/camera.bmp" every={700} alt="camera">
  {#if face && fresh}
    <div class="mark" style:left={`${(face.x + 1) * 50}%`} style:top={`${(face.y + 1) * 50}%`}></div>
  {/if}
</LiveImage>
<div class="mono" style="margin-top:8px">
  {#if face && fresh}face at x {face.x.toFixed(2)} · y {face.y.toFixed(2)}{:else}no face{#if status.vision?.seen_s_ago != null} · last seen {status.vision.seen_s_ago.toFixed(0)} s ago{/if}{/if}
</div>

<style>
  .mark { position: absolute; width: 28px; height: 28px; margin: -14px 0 0 -14px; border: 2px solid var(--a); border-radius: 50%; box-shadow: 0 0 0 1px var(--panel); }
</style>
