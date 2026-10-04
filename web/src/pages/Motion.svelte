<script lang="ts">
  import { api, type MotionState } from '../api';
  import { attempt, toast } from '../lib/ui.svelte';
  let m = $state<MotionState | null>(null);
  let yaw = $state(0);
  let pitch = $state(25);
  let err = $state<string | null>(null);

  async function load() {
    try {
      m = await api.motion();
      err = null;
    } catch (e) {
      err = e instanceof Error ? e.message : String(e);
    }
  }
  $effect(() => {
    load().then(() => m && ((yaw = Math.round(m.yaw)), (pitch = Math.round(m.pitch))));
    const t = setInterval(load, 1500);
    return () => clearInterval(t);
  });

  let pending: ReturnType<typeof setTimeout> | undefined;
  function jog() {
    clearTimeout(pending);
    pending = setTimeout(() => attempt(api.jog(yaw, pitch)), 80);
  }
  async function zero() {
    if (!confirm('Make the current head pose the new centre?')) return;
    const z = await attempt(api.setZero());
    if (z) {
      toast(`Centre saved (raw ${z.yaw} / ${z.pitch}).`);
      yaw = 0;
      pitch = 0;
    }
  }
</script>

<h2>Head</h2>
{#if err}
  <p class="quip">{err}</p>
{:else if m}
  <div class="card">
    <div class="mono">yaw {m.yaw.toFixed(1)}° · pitch {m.pitch.toFixed(1)}° · torque {m.torque ? 'on' : 'off'}</div>
    <div class="mono muted">centre (raw): yaw {m.zero.yaw} · pitch {m.zero.pitch}</div>
  </div>
  <p class="muted">Moving the sliders holds the head for 15 s, then tracking takes over again.</p>
  <label class="field"><span class="label">Yaw {yaw}° (− is Femto's left)</span>
    <input type="range" min={-m.limits.yaw} max={m.limits.yaw} bind:value={yaw} oninput={jog} />
  </label>
  <label class="field"><span class="label">Pitch {pitch}° (0 = level)</span>
    <input type="range" min={m.limits.pitch_min} max={m.limits.pitch_max} bind:value={pitch} oninput={jog} />
  </label>
  <div class="chips">
    <button onclick={() => attempt(api.nod())}>Nod</button>
    <button onclick={() => ((yaw = 0), (pitch = 0), jog())}>Centre</button>
    <button onclick={() => attempt(api.setTorque(!m!.torque))}>{m.torque ? 'Go limp' : 'Torque on'}</button>
  </div>
  <h2>Smoothness</h2>
  <p class="muted">Live tuning for servo noise; resets on reboot.</p>
  <label class="field"><span class="label">Top speed {m.max_speed}°/s</span>
    <input type="range" min="10" max="400" value={m.max_speed} onchange={(e) => attempt(api.tuneMotion({ max_speed: +e.currentTarget.value }))} />
  </label>
  <label class="field"><span class="label">Stiffness {m.stiffness}</span>
    <input type="range" min="10" max="300" value={m.stiffness} onchange={(e) => attempt(api.tuneMotion({ stiffness: +e.currentTarget.value }))} />
  </label>
  <label class="field"><span class="label">Move time {m.move_ms} ms</span>
    <input type="range" min="20" max="200" value={m.move_ms} onchange={(e) => attempt(api.tuneMotion({ move_ms: +e.currentTarget.value }))} />
  </label>
  <label class="row"><input type="checkbox" checked={m.rest_pitch} onchange={(e) => attempt(api.tuneMotion({ rest_pitch: e.currentTarget.checked }))} /> Pitch rests too (torque off when idle; the head may sag)</label>
  <h2>Calibrate</h2>
  <p class="muted">Jog (or go limp and move the head by hand) until Femto looks straight ahead and level, then save it as the centre. Stored where M5's firmware keeps it.</p>
  <button class="danger" onclick={zero}>Save as centre</button>
{:else}
  <p class="mono muted">Reading servos…</p>
{/if}
