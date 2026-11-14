<script setup lang="ts">
import EmptyState from '../components/EmptyState.vue';
import PageHeader from '../components/PageHeader.vue';
import { computed, onMounted, ref } from 'vue';

import { call, host } from '../api';
import Icon from '../components/Icon.vue';
import { t } from '../i18n';
import { app, attempt, formatTime, notify, refreshStatus } from '../store';
import type { AuditEntry } from '../types';
import { eventLabel, groupOf, type EventGroup } from './events';

const entries = ref<AuditEntry[]>([]);
const loading = ref(true);
const filter = ref<'all' | EventGroup>('all');
const query = ref('');

async function load() {
  loading.value = true;
  entries.value = (await attempt(() => call<AuditEntry[]>('audit.list', { limit: 400 }))) ?? [];
  loading.value = false;
}
onMounted(load);

const rows = computed(() =>
  entries.value.filter((e) => (filter.value === 'all' || groupOf(e) === filter.value) && `${eventLabel(e)} ${JSON.stringify(e.detail)}`.toLowerCase().includes(query.value.toLowerCase())),
);

async function allow(entry: AuditEntry, minutes: number) {
  await refreshStatus();
  if (!app.status?.armed) {
    notify('info', t('Start a session first; exceptions apply to the running session.'));
    return;
  }
  const app_ = { path: String(entry.detail.path), sha256: String(entry.detail.sha256 ?? ''), publisher: String(entry.detail.publisher ?? '') };
  await attempt(() => call('exceptions.add', { app: app_, minutes, match: 'path' }), t('Allowed for {n} minutes.', { n: minutes }));
  await load();
}

async function verify() {
  const result = await attempt(() => call<{ valid: boolean }>('audit.verify'));
  if (result) notify(result.valid ? 'ok' : 'bad', result.valid ? t('The audit log is intact.') : t('The audit log has been modified.'));
}

async function exportCsv() {
  const quote = (value: string) => `"${value.replaceAll('"', '""')}"`;
  const lines = ['time,event,actor,detail', ...rows.value.map((e) => [new Date(e.ts * 1000).toISOString(), eventLabel(e), e.actor, JSON.stringify(e.detail)].map(quote).join(','))];
  await attempt(() => host('save_text', { name: 'houseguest-report.csv', content: lines.join('\r\n') }));
}
</script>

<template>
  <div class="page">
    <PageHeader :title="t('History')" :subtitle="t('Everything Houseguest did or blocked, in tamper-evident order.')">
      <button class="btn" @click="verify"><Icon name="shield" />{{ t('Verify log') }}</button>
      <button class="btn" :disabled="!rows.length" @click="exportCsv"><Icon name="download" />{{ t('Export CSV') }}</button>
    </PageHeader>
    <div class="row wrap">
      <div class="seg" role="group">
        <button v-for="g in ['all', 'blocked', 'sessions', 'security', 'changes'] as const" :key="g" :aria-pressed="filter === g" @click="filter = g">
          {{ { all: t('All'), blocked: t('Blocked'), sessions: t('Sessions'), security: t('Security'), changes: t('Changes') }[g] }}
        </button>
      </div>
      <div class="search grow"><Icon name="search" /><input v-model="query" class="input" :placeholder="t('Search events')" :aria-label="t('Search events')" /></div>
    </div>
    <div v-if="loading" class="stack"><div v-for="n in 6" :key="n" class="skeleton" style="height: 40px" /></div>
    <EmptyState v-else-if="!rows.length" icon="history" :text="t('No events match.')" />
    <div v-else class="table-wrap">
      <table>
        <tbody>
          <tr v-for="entry in rows" :key="entry.seq">
            <td class="faint" style="width: 1%; white-space: nowrap">{{ formatTime(entry.ts) }}</td>
            <td style="max-width: 0">
              <div class="truncate">{{ eventLabel(entry) }}</div>
              <div v-if="entry.event === 'app.blocked'" class="hint truncate mono">{{ entry.detail.path }}</div>
            </td>
            <td style="width: 1%; white-space: nowrap">
              <template v-if="entry.event === 'app.blocked'">
                <button class="btn sm" @click="allow(entry, 15)">{{ t('Allow 15 min') }}</button>
                <button class="btn sm" @click="allow(entry, 60)">{{ t('Allow 1 h') }}</button>
              </template>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>
