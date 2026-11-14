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

const titles: Record<string, string> = {
  windows_build: 'Windows version',
  cryptography: 'Cryptography',
  process_inspection: 'Process inspection',
  process_events: 'Process creation events',
  signature_verification: 'Publisher verification',
  session_control: 'Session control',
  local_accounts: 'Local accounts',
  webview2_runtime: 'WebView2 runtime',
  defender_independent: 'Independence from Windows security features',
};

const checks = computed(() => {
  const d = info.value;
  if (!d) return [];
  const own = [
    { ok: d.supervisor.mode !== 'unavailable', soft: d.supervisor.mode === 'polling', label: t('Process monitoring'), detail: { supervised: t('Running'), polling: t('Running in polling mode'), unavailable: t('Not running') }[d.supervisor.mode] },
    { ok: d.audit_chain_valid, label: t('Audit log integrity'), detail: d.audit_chain_valid ? t('Chain verified') : t('Chain broken') },
    { ok: d.state === 'loaded' || d.state === 'fresh', label: t('Settings integrity'), detail: { loaded: t('Verified'), fresh: t('New installation'), recovered: t('Recovered from snapshot'), tampered: t('Failed verification') }[d.state] },
    { ok: d.backups > 0, label: t('Backups'), detail: d.backups > 0 ? t('{n} available', { n: d.backups }) : t('None yet'), soft: true },
  ];
  const probes = d.capabilities.checks.map((check) => ({
    ok: check.health === 'ok',
    soft: check.health === 'degraded' || (check.health === 'missing' && !check.critical),
    label: t(titles[check.id] ?? check.id),
    detail: check.detail,
  }));
  return [...own, ...probes];
});

const latency = computed(() => info.value?.supervisor.latency_ms);

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
        <h2>{{ t('Monitoring performance') }}</h2>
        <dl class="grid cols-4" style="margin: 0">
          <div><dt class="label">{{ t('Programs inspected') }}</dt><dd class="kpi-number">{{ info.supervisor.inspected }}</dd></div>
          <div><dt class="label">{{ t('Programs stopped') }}</dt><dd class="kpi-number">{{ info.supervisor.terminated }}</dd></div>
          <div><dt class="label">{{ t('Median-to-95th stop delay') }}</dt><dd class="kpi-number">{{ latency && latency.samples ? Math.round(latency.average) + ' / ' + Math.round(latency.p95) + ' ms' : '—' }}</dd></div>
          <div><dt class="label">{{ t('Slowest stop') }}</dt><dd class="kpi-number">{{ latency && latency.samples ? Math.round(latency.max) + ' ms' : '—' }}</dd></div>
        </dl>
        <p v-if="info.supervisor.degraded_reason" class="hint">{{ info.supervisor.degraded_reason }}</p>
        <p class="hint">{{ t('Houseguest stops a program after it starts, so a blocked program can run for a short moment. These figures are measured on this computer.') }}</p>
      </div>
      <div class="card stack">
        <h2>{{ t('System') }}</h2>
        <dl class="grid cols-2" style="margin: 0">
          <div><dt class="label">{{ t('Windows') }}</dt><dd style="margin: 0">{{ info.capabilities.product_name }} {{ info.capabilities.display_version }} · {{ info.capabilities.edition }} · build {{ info.capabilities.build }}</dd></div>
          <div><dt class="label">{{ t('Service version') }}</dt><dd style="margin: 0">{{ info.version }}</dd></div>
          <div><dt class="label">{{ t('Data folder') }}</dt><dd class="mono" style="margin: 0">{{ info.data_directory }}</dd></div>
          <div><dt class="label">{{ t('Install folder') }}</dt><dd class="mono" style="margin: 0">{{ info.install_directory }}</dd></div>
        </dl>
      </div>
    </template>
  </div>
</template>
