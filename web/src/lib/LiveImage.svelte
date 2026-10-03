<script lang="ts">
  // Polls an image endpoint (BMP) while mounted and visible.
  let { src, every = 1000, alt, children }: { src: string; every?: number; alt: string; children?: import('svelte').Snippet } = $props();
  let url = $state<string | null>(null);
  let failed = $state(false);

  $effect(() => {
    let stop = false;
    let prev: string | null = null;
    async function tick() {
      while (!stop) {
        if (!document.hidden) {
          try {
            const r = await fetch(`${src}?t=${Date.now()}`, { cache: 'no-store' });
            if (!r.ok) throw new Error(String(r.status));
            const next = URL.createObjectURL(await r.blob());
            url = next;
            if (prev) URL.revokeObjectURL(prev);
            prev = next;
            failed = false;
          } catch {
            failed = true;
          }
        }
        await new Promise((res) => setTimeout(res, every));
      }
    }
    tick();
    return () => {
      stop = true;
      if (prev) URL.revokeObjectURL(prev);
    };
  });
</script>

<div class="live">
  {#if url}<img src={url} {alt} />{:else}<div class="mono muted">{failed ? 'no picture' : 'loading…'}</div>{/if}
  {@render children?.()}
</div>

<style>
  .live { position: relative; background: var(--panel); border: 1px solid var(--dim); line-height: 0; }
  img { width: 100%; image-rendering: pixelated; }
  div.mono { line-height: 1.4; padding: 40px 12px; text-align: center; }
</style>
