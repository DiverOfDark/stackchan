<script lang="ts">
  // Every §6.6 setting; used by the Settings page and the setup wizard.
  import type { Settings } from '../api';
  let { s, onchange, only }: { s: Settings; onchange: (p: Partial<Settings>) => void; only?: 'character' } = $props();

  const accents = [
    ['signal-red', 'oklch(0.6 0.23 25)'],
    ['toxic-green', 'oklch(0.75 0.2 140)'],
    ['ice-blue', 'oklch(0.72 0.14 225)'],
  ] as const;
  const eyewear = [
    ['exec-glasses', 'Exec glasses'],
    ['ar-half-lens', 'AR half-lens'],
    ['reticle', 'Reticle'],
    ['none', 'None'],
  ] as const;
  const zones = ['Europe/Berlin', 'Europe/London', 'Europe/Moscow', 'Europe/Kyiv', 'America/New_York', 'America/Los_Angeles', 'Asia/Tokyo', 'UTC'];
</script>

<h2>Character</h2>
<label class="field"><span class="label">Name (shown on boot, in captions)</span>
  <input type="text" maxlength="12" value={s.name} onchange={(e) => onchange({ name: e.currentTarget.value })} />
</label>
<div class="label">Honorific</div>
<div class="chips">
  {#each ['sir', 'madam', 'guv'] as const as h}
    <button class:on={s.honorific === h} onclick={() => onchange({ honorific: h })}>{h}</button>
  {/each}
</div>
<div class="label" style="margin-top:12px">Accent</div>
<div class="row">
  {#each accents as [a, css]}
    <button class="swatch" class:on={s.accent === a} style:background={css} title={a} aria-label={a} onclick={() => onchange({ accent: a })}></button>
  {/each}
</div>
<div class="label" style="margin-top:12px">Eyewear</div>
<div class="chips">
  {#each eyewear as [v, label]}
    <button class:on={s.eyewear === v} onclick={() => onchange({ eyewear: v })}>{label}</button>
  {/each}
</div>

{#if only !== 'character'}
  <h2>Corporate</h2>
  <label class="row"><input type="checkbox" checked={s.corp} onchange={(e) => onchange({ corp: e.currentTarget.checked })} /> Property of a corporation</label>
  {#if s.corp}
    <label class="field"><span class="label">Corporation</span>
      <input type="text" maxlength="24" value={s.corp_name} onchange={(e) => onchange({ corp_name: e.currentTarget.value })} />
    </label>
  {/if}

  <h2>Display</h2>
  <label class="row"><input type="checkbox" checked={s.fx} onchange={(e) => onchange({ fx: e.currentTarget.checked })} /> Scanlines, vignette, glitches</label>
  <label class="row" style="margin-top:10px"><input type="checkbox" checked={s.brightness === null} onchange={(e) => onchange({ brightness: e.currentTarget.checked ? null : 60 })} /> Auto brightness</label>
  {#if s.brightness !== null}
    <label class="field"><span class="label">Brightness {s.brightness}%</span>
      <input type="range" min="10" max="100" value={s.brightness} onchange={(e) => onchange({ brightness: +e.currentTarget.value })} />
    </label>
  {/if}

  <h2>Body LEDs</h2>
  <div class="chips">
    {#each [['usage', 'Usage meters'], ['mood', 'Moods only'], ['off', 'Off']] as const as [v, label]}
      <button class:on={s.led_mode === v} onclick={() => onchange({ led_mode: v })}>{label}</button>
    {/each}
  </div>
  <p class="muted">Usage meters: left strip = 5-hour session, right = week (bone / toxic / red like the status band). Moods animate either way: VU while listening, chase while thinking, voice while speaking.</p>
  {#if s.led_mode !== 'off'}
    <label class="field"><span class="label">Brightness {s.led_brightness}%</span>
      <input type="range" min="5" max="100" value={s.led_brightness} onchange={(e) => onchange({ led_brightness: +e.currentTarget.value })} />
    </label>
    <label class="row"><input type="checkbox" checked={s.led_flip} onchange={(e) => onchange({ led_flip: e.currentTarget.checked })} /> Meters fill the other way</label>
  {/if}

  <h2>Behaviour</h2>
  <label class="row"><input type="checkbox" checked={s.follow} onchange={(e) => onchange({ follow: e.currentTarget.checked })} /> Follow me (eyes + head)</label>
  <label class="row" style="margin-top:10px"><input type="checkbox" checked={s.camera} onchange={(e) => onchange({ camera: e.currentTarget.checked })} /> Camera (face tracking, standby when alone)</label>

  <h2>Voice</h2>
  <label class="field"><span class="label">Volume {s.volume}%</span>
    <input type="range" min="0" max="100" value={s.volume} onchange={(e) => onchange({ volume: +e.currentTarget.value })} />
  </label>
  <div class="label">Language</div>
  <div class="chips">
    {#each [['auto', 'Auto'], ['ru', 'Русский'], ['en', 'English']] as const as [v, label]}
      <button class:on={s.voice_lang === v} onclick={() => onchange({ voice_lang: v })}>{label}</button>
    {/each}
  </div>

  <h2>Time</h2>
  <label class="field"><span class="label">Timezone (reset labels)</span>
    <select value={s.tz} onchange={(e) => onchange({ tz: e.currentTarget.value })}>
      {#each zones as z}<option value={z}>{z}</option>{/each}
    </select>
  </label>
{/if}
