<script lang="ts">
  import { api, ApiError, type Status } from './api';
  import { ui, go } from './lib/ui.svelte';
  import Login from './pages/Login.svelte';
  import Dashboard from './pages/Dashboard.svelte';
  import SettingsPage from './pages/Settings.svelte';
  import Connections from './pages/Connections.svelte';
  import Test from './pages/Test.svelte';
  import System from './pages/System.svelte';
  import Setup from './pages/Setup.svelte';
  import Camera from './pages/Camera.svelte';
  import Motion from './pages/Motion.svelte';
  import Logs from './pages/Logs.svelte';

  let status = $state<Status | null>(null);
  let needLogin = $state(false);
  let offline = $state(false);

  async function poll() {
    try {
      status = await api.status();
      offline = false;
      // Setup mode always lands in the wizard (captive portal, PRD FR-4).
      if (status.setup && !ui.route.startsWith('/setup')) go('/setup');
      if (status.auth_required) {
        // A protected call tells us whether this browser has a session.
        await api.settings().then(
          () => (needLogin = false),
          (e) => (needLogin = e instanceof ApiError && e.status === 401),
        );
      } else needLogin = false;
    } catch {
      offline = true;
    }
  }

  $effect(() => {
    poll();
    const t = setInterval(poll, 3000);
    return () => clearInterval(t);
  });

  const tabs = [
    ['/', 'Status'],
    ['/settings', 'Settings'],
    ['/connections', 'Connections'],
    ['/camera', 'Camera'],
    ['/motion', 'Motion'],
    ['/test', 'Test'],
    ['/logs', 'Logs'],
    ['/system', 'System'],
  ] as const;
</script>

<header class="top">
  <div class="bar">
    <span class="brand">FEMTO</span>
    <span class="jp">フェムト · 監視</span>
    <span class="unit">{offline ? 'link lost' : status ? `unit 07 · ${status.version}` : '…'}</span>
  </div>
  {#if status && !status.setup && !needLogin}
    <nav>
      {#each tabs as [href, label]}
        <a href={`#${href}`} class:on={ui.route === href}>{label}</a>
      {/each}
    </nav>
  {/if}
</header>

<main class="wrap">
  {#if !status}
    <p class="muted mono">{offline ? 'Femto is not answering. Same network?' : 'Waking the machine…'}</p>
  {:else if status.setup || ui.route.startsWith('/setup')}
    <Setup {status} />
  {:else if needLogin}
    <Login onDone={poll} />
  {:else if ui.route === '/settings'}
    <SettingsPage {status} />
  {:else if ui.route === '/connections'}
    <Connections {status} />
  {:else if ui.route === '/camera'}
    <Camera {status} />
  {:else if ui.route === '/motion'}
    <Motion />
  {:else if ui.route === '/logs'}
    <Logs />
  {:else if ui.route === '/test'}
    <Test />
  {:else if ui.route === '/system'}
    <System {status} />
  {:else}
    <Dashboard {status} />
  {/if}
</main>

{#if ui.toast}
  <div class="toast" class:err={ui.toast.err}>{ui.toast.text}</div>
{/if}
