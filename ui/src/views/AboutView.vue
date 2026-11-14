<script setup lang="ts">
import { ref } from 'vue';

import Icon from '../components/Icon.vue';
import Mark from '../components/Mark.vue';
import { t } from '../i18n';
import { app, notify } from '../store';

const checking = ref(false);
const repository = 'UnkoynX777/houseguest';

function newer(latest: string, current: string): boolean {
  const parse = (v: string) => v.replace(/^v/, '').split('.').map((n) => Number.parseInt(n, 10) || 0);
  const [a, b] = [parse(latest), parse(current)];
  for (let i = 0; i < 3; i++) if ((a[i] ?? 0) !== (b[i] ?? 0)) return (a[i] ?? 0) > (b[i] ?? 0);
  return false;
}

async function check() {
  checking.value = true;
  try {
    const response = await fetch(`https://api.github.com/repos/${repository}/releases/latest`, { headers: { Accept: 'application/vnd.github+json' } });
    if (response.status === 404) notify('info', t('No release has been published yet.'));
    else if (!response.ok) throw new Error(String(response.status));
    else {
      const release = (await response.json()) as { tag_name: string; html_url: string };
      notify(newer(release.tag_name, app.status?.version ?? '0.0.0') ? 'info' : 'ok', newer(release.tag_name, app.status?.version ?? '0.0.0') ? t('Version {v} is available on GitHub.', { v: release.tag_name }) : t('You are up to date.'));
    }
  } catch {
    notify('bad', t('Could not reach GitHub.'));
  } finally {
    checking.value = false;
  }
}
</script>

<template>
  <div class="page">
    <div class="card hero row" style="gap: 24px">
      <Mark style="width: 72px; height: 72px" />
      <div class="grow">
        <h1>Houseguest</h1>
        <p class="muted">{{ t('Version {v}', { v: app.status?.version ?? '' }) }} · Apache-2.0</p>
        <p style="margin-top: 8px">{{ t('Lend your PC without lending your life. Open source, auditable, and honest about its limits.') }}</p>
      </div>
      <button class="btn" :disabled="checking" @click="check"><span v-if="checking" class="spinner" /><Icon v-else name="refresh" />{{ t('Check for updates') }}</button>
    </div>
    <div class="card stack">
      <h2>{{ t('Privacy') }}</h2>
      <p class="muted">{{ t('Houseguest never sends data anywhere. Checking for updates contacts GitHub only when you press the button.') }}</p>
    </div>
    <div class="card stack">
      <h2>{{ t('Open source') }}</h2>
      <p class="muted">{{ t('Source code, issues and security reports are on GitHub. See the repository for the threat model and architecture.') }}</p>
      <a :href="`https://github.com/${repository}`" class="row" style="gap: 6px"><Icon name="external" style="width: 16px; height: 16px" />github.com/{{ repository }}</a>
    </div>
  </div>
</template>
