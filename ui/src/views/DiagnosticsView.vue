<script setup lang="ts">
import PageHeader from '../components/PageHeader.vue';
import { computed, onMounted, ref } from 'vue';

import { call } from '../api';
import Icon from '../components/Icon.vue';
import { t } from '../i18n';
import { attempt, notify } from '../store';
import type { Diagnostics } from '../types';

const info = ref<Diagnostics>();
const loading = ref(true);

async function load() {
  loading.value = true;
  info.value = await attempt(() => call<Diagnostics>('diagnostics'));
  loading.value = false;
}
onMounted(load);

const checks = computed(() => {
  const d = info.value;
  if (!d) return [];
  return [
    { ok: d.supervisor_running, label: t('Process monitoring'), detail: d.supervisor_running ? t('Running') : d.supervisor_error || t('Not running') },
    { ok: d.audit_chain_valid, label: t('Audit log integrity'), detail: d.audit_chain_valid ? t('Chain verified') : t('Chain broken') },
    { ok: d.state === 'loaded' || d.state === 'fresh', label: t('Settings integrity'), detail: { loaded: t('Verified'), fresh: t('New installation'), recovered: t('Recovered from snapshot'), tampered: t('Failed verification') }[d.state] },
    { ok: d.capabilities.webview2, label: t('WebView2 runtime'), detail: d.capabilities.webview2 ? t('Installed') : t('Missing') },
    { ok: d.backups > 0, label: t('Backups'), detail: d.backups > 0 ? t('{n} available', { n: d.backups }) : t('None yet'), soft: true },
  ];
});

async function copy() {
  if (!info.value) return;
  await navigator.clipboard.writeText(JSON.stringify(info.value, null, 2)).then(() => notify('ok', t('Report copied.')), () => notify('bad', t('Copy failed.')));
}
</script>

<template>
  <div class="page">
    <PageHeader :title="t('Diagnostics')" :subtitle="t('Check that everything Houseguest depends on is healthy.')">
      <button class="btn" :disabled="!info" @click="copy"><Icon name="copy" />{{ t('Copy report') }}</button>
      <button class="btn primary" :disabled="loading" @click="load"><Icon name="refresh" />{{ t('Run checks') }}</button>
    </PageHeader>
    <div v-if="loading" class="stack"><div v-for="n in 5" :key="n" class="skeleton" style="height: 52px" /></div>
    <template v-else-if="info">
      <div class="table-wrap">
        <table><tbody>
          <tr v-for="check in checks" :key="check.label">
            <td style="width: 1%"><span class="badge" :class="check.ok ? 'ok' : check.soft ? 'warn' : 'bad'"><Icon :name="check.ok ? 'check' : 'alert'" style="width: 14px; height: 14px" /></span></td>
            <td>{{ check.label }}</td><td class="muted">{{ check.detail }}</td>
          </tr>
        </tbody></table>
      </div>
      <div class="card stack">
        <h2>{{ t('System') }}</h2>
        <dl class="grid cols-2" style="margin: 0">
          <div><dt class="label">{{ t('Windows') }}</dt><dd style="margin: 0">{{ info.capabilities.edition }} · {{ info.capabilities.build }}</dd></div>
          <div><dt class="label">{{ t('Service version') }}</dt><dd style="margin: 0">{{ info.version }}</dd></div>
          <div><dt class="label">{{ t('Data folder') }}</dt><dd class="mono" style="margin: 0">{{ info.data_directory }}</dd></div>
          <div><dt class="label">{{ t('Install folder') }}</dt><dd class="mono" style="margin: 0">{{ info.install_directory }}</dd></div>
        </dl>
      </div>
    </template>
  </div>
</template>
