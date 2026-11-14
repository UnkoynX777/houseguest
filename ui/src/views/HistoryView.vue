<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';

import { ApiError, call, host } from '../api';
import AppIcon from '../components/AppIcon.vue';
import BlockReview from '../components/BlockReview.vue';
import EmptyState from '../components/EmptyState.vue';
import Icon from '../components/Icon.vue';
import Modal from '../components/Modal.vue';
import PageHeader from '../components/PageHeader.vue';
import { describeBlock, describeDecision } from '../explain';
import { t } from '../i18n';
import { attempt, describe, formatTime, notify } from '../store';
import type { AuditEntry, BlockEntry } from '../types';
import { eventLabel, eventsOf, groupOf, type EventGroup } from './events';

const pageSize = 50;
const entries = ref<AuditEntry[]>([]);
const more = ref(false);
const loading = ref(true);
const loadingMore = ref(false);
const failure = ref('');
const filter = ref<'all' | EventGroup>('all');
const range = ref(0);
const query = ref('');
const sort = ref<'new' | 'old'>('new');
const selected = ref<AuditEntry>();
const reviewing = ref<BlockEntry>();

let generation = 0;
let debounce: ReturnType<typeof setTimeout> | undefined;

async function load(append = false) {
  const mine = ++generation;
  if (append) loadingMore.value = true;
  else loading.value = true;
  failure.value = '';
  try {
    const args: Record<string, unknown> = { limit: pageSize };
    if (filter.value !== 'all') args.events = eventsOf(filter.value);
    if (range.value) args.since = Math.floor(Date.now() / 1000) - range.value;
    if (query.value.trim()) args.text = query.value.trim();
    if (append && entries.value.length) args.before = entries.value[entries.value.length - 1].seq;
    const page = await call<{ entries: AuditEntry[]; more: boolean }>('audit.query', args);
    if (mine !== generation) return;
    entries.value = append ? [...entries.value, ...page.entries] : page.entries;
    more.value = page.more;
  } catch (error) {
    if (mine !== generation) return;
    failure.value = error instanceof ApiError ? describe(error) : String(error);
    if (!append) entries.value = [];
  } finally {
    if (mine === generation) {
      loading.value = false;
      loadingMore.value = false;
    }
  }
}

watch([filter, range], () => load());
watch(query, () => {
  clearTimeout(debounce);
  debounce = setTimeout(() => load(), 250);
});
onMounted(() => load());
onBeforeUnmount(() => clearTimeout(debounce));

const rows = computed(() => (sort.value === 'new' ? entries.value : [...entries.value].reverse()));

const isApp = (entry: AuditEntry) => ['app.blocked', 'app.would_block', 'app.block_failed', 'app.allowed', 'protection.spared'].includes(entry.event);
const detail = (entry: AuditEntry) => (entry.detail ?? {}) as Record<string, string>;
const appName = (entry: AuditEntry) => {
  const d = detail(entry);
  return d.process || String(d.path ?? '').split(/[\\/]/).pop() || '';
};
const outcome = (entry: AuditEntry): 'allowed' | 'blocked' | 'would' | 'neutral' =>
  entry.event === 'app.allowed' ? 'allowed' : entry.event === 'app.blocked' || entry.event === 'app.block_failed' ? 'blocked' : entry.event === 'app.would_block' ? 'would' : 'neutral';
const outcomeLabel = (entry: AuditEntry) =>
  ({ allowed: t('Allowed'), blocked: entry.event === 'app.block_failed' ? t('Not stopped') : t('Blocked'), would: t('Audit only'), neutral: '' })[outcome(entry)];
const outcomeBadge = (entry: AuditEntry) => ({ allowed: 'ok', blocked: 'bad', would: 'warn', neutral: '' })[outcome(entry)];
const glyph = (entry: AuditEntry) =>
  ({ blocked: 'ban', allowed: 'allowed', sessions: 'sessions', security: 'key', changes: 'edit' })[groupOf(entry)];

const reason = (entry: AuditEntry) => {
  if (entry.event === 'app.allowed') return describeDecision({ reason: detail(entry).reason, source: detail(entry).source, rule_id: detail(entry).rule } as never, detail(entry).profile);
  if (isApp(entry)) return describeBlock(asBlock(entry));
  return '';
};
const asBlock = (entry: AuditEntry): BlockEntry => ({ ...(entry.detail as unknown as BlockEntry), event: entry.event, ts: entry.ts });
const reviewable = (entry: AuditEntry) => ['app.blocked', 'app.would_block', 'app.block_failed'].includes(entry.event);

const details = computed(() => {
  const entry = selected.value;
  if (!entry) return [];
  const d = detail(entry);
  const parent = d.parent ? d.parent.split(/[\\/]/).pop() : '';
  const lines: [string, string][] = [
    [t('Time'), new Date(entry.ts * 1000).toLocaleString()],
    [t('Event'), eventLabel(entry)],
  ];
  if (isApp(entry)) {
    lines.push([t('Program'), d.path ?? ''], [t('Started by'), parent ? `${parent} (${d.parent})` : t('Unknown')], [t('Profile'), d.profile || ''], [t('Rule'), d.rule || d.source || ''], [t('Publisher'), d.publisher || t('Unsigned')], [t('SHA-256'), d.sha256 || ''], [t('Technical reason'), `${d.source ?? ''} · ${d.reason ?? ''}${d.note ? ` · ${d.note}` : ''}`]);
  } else {
    for (const [key, value] of Object.entries(d)) lines.push([key, typeof value === 'string' ? value : JSON.stringify(value)]);
  }
  lines.push([t('Actor'), entry.actor], [t('Sequence'), String(entry.seq)]);
  return lines.filter(([, value]) => value !== '');
});

async function verify() {
  const result = await attempt(() => call<{ valid: boolean }>('audit.verify'));
  if (result) notify(result.valid ? 'ok' : 'bad', result.valid ? t('The audit log is intact.') : t('The audit log has been modified.'));
}

async function exportCsv() {
  const quote = (value: string) => `"${value.replaceAll('"', '""')}"`;
  const lines = ['time,event,program,profile,rule,detail', ...rows.value.map((e) => [new Date(e.ts * 1000).toISOString(), eventLabel(e), detail(e).path ?? '', detail(e).profile ?? '', detail(e).rule ?? '', JSON.stringify(e.detail ?? {})].map(quote).join(','))];
  const result = await attempt(() => host<{ saved: boolean }>('save_text', { name: 'houseguest-history.csv', content: lines.join('\r\n') }));
  if (result?.saved) notify('ok', t('History exported.'));
}

const groups = computed(() => [
  ['all', t('All')], ['blocked', t('Blocked')], ['allowed', t('Allowed')], ['sessions', t('Sessions')], ['security', t('Security')], ['changes', t('Changes')],
] as const);

const filtered = computed(() => filter.value !== 'all' || !!query.value.trim() || range.value !== 0);
</script>

<template>
  <div class="page">
    <PageHeader :title="t('History')" :subtitle="t('Everything Houseguest allowed, blocked or changed, in tamper-evident order.')">
      <button class="btn" @click="load()"><Icon name="refresh" />{{ t('Refresh') }}</button>
      <button class="btn" @click="verify"><Icon name="shield" />{{ t('Verify log') }}</button>
      <button class="btn" :disabled="!rows.length" @click="exportCsv"><Icon name="download" />{{ t('Export CSV') }}</button>
    </PageHeader>

    <div class="toolbar-row">
      <div class="seg" role="group" :aria-label="t('Event type')">
        <button v-for="[key, label] in groups" :key="key" :aria-pressed="filter === key" @click="filter = key">{{ label }}</button>
      </div>
      <div class="search"><Icon name="search" /><input v-model="query" class="input" :placeholder="t('Search programs, profiles or reasons')" :aria-label="t('Search history')" /></div>
      <select v-model.number="range" class="select" :aria-label="t('Period')">
        <option :value="0">{{ t('Any time') }}</option>
        <option :value="3600">{{ t('Last hour') }}</option>
        <option :value="86400">{{ t('Last 24 hours') }}</option>
        <option :value="604800">{{ t('Last 7 days') }}</option>
      </select>
      <select v-model="sort" class="select" :aria-label="t('Order')">
        <option value="new">{{ t('Newest first') }}</option>
        <option value="old">{{ t('Oldest first') }}</option>
      </select>
    </div>

    <div v-if="failure" class="banner bad" role="alert">
      <Icon name="alert" />
      <div class="grow"><strong>{{ t('The history could not be loaded.') }}</strong><div>{{ failure }}</div></div>
      <button class="btn sm" @click="load()">{{ t('Try again') }}</button>
    </div>
    <div v-else-if="loading" class="history-list"><div v-for="n in 7" :key="n" class="event"><span class="bar" /><span class="skeleton" style="height: 34px; width: 34px" /><span class="skeleton" style="height: 30px" /><span /></div></div>
    <EmptyState v-else-if="!rows.length" :icon="filtered ? 'search' : 'history'" :text="filtered ? t('No events match these filters.') : t('Nothing has been recorded yet. Programs that Houseguest allows or blocks during a session will appear here.')">
      <button v-if="filtered" class="btn sm" @click="filter = 'all'; query = ''; range = 0">{{ t('Clear filters') }}</button>
    </EmptyState>
    <template v-else>
      <div class="history-list" role="list">
        <div v-for="entry in rows" :key="entry.seq" class="event" :class="outcome(entry)" role="listitem" tabindex="0" @click="selected = entry" @keydown.enter="selected = entry">
          <span class="bar" />
          <AppIcon v-if="isApp(entry) && detail(entry).path" :path="detail(entry).path" :name="appName(entry)" :size="34" />
          <span v-else class="glyph"><Icon :name="glyph(entry)" /></span>
          <div class="grow">
            <div class="main-line">
              <strong class="truncate">{{ isApp(entry) ? appName(entry) : eventLabel(entry) }}</strong>
              <span v-if="outcomeLabel(entry)" class="badge" :class="outcomeBadge(entry)">{{ outcomeLabel(entry) }}</span>
            </div>
            <div class="sub-line truncate">
              <template v-if="isApp(entry)">{{ reason(entry) }}<template v-if="detail(entry).profile"> · {{ detail(entry).profile }}</template></template>
              <template v-else>{{ t('by {actor}', { actor: entry.actor }) }}</template>
            </div>
          </div>
          <span class="when">{{ formatTime(entry.ts) }}</span>
        </div>
      </div>
      <div v-if="more" class="pager"><button class="btn" :disabled="loadingMore" @click="load(true)"><span v-if="loadingMore" class="spinner" />{{ t('Load more') }}</button></div>
      <p v-else class="hint" style="text-align: center">{{ t('{n} events shown.', { n: rows.length }) }}</p>
    </template>

    <Modal v-if="selected" :title="isApp(selected) ? appName(selected) : eventLabel(selected)" @close="selected = undefined">
      <p v-if="isApp(selected)" class="muted">{{ reason(selected) }}</p>
      <dl class="detail-grid"><template v-for="[label, value] in details" :key="label"><dt>{{ label }}</dt><dd :class="{ mono: label === t('SHA-256') || label === t('Program') }">{{ value }}</dd></template></dl>
      <template #footer>
        <button v-if="reviewable(selected)" class="btn" @click="reviewing = asBlock(selected); selected = undefined">{{ t('Review') }}</button>
        <button class="btn primary" @click="selected = undefined">{{ t('Close') }}</button>
      </template>
    </Modal>
    <BlockReview v-if="reviewing" :block="reviewing" @close="reviewing = undefined" @done="load()" />
  </div>
</template>
