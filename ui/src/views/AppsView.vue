<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';

import { call } from '../api';
import Icon from '../components/Icon.vue';
import Modal from '../components/Modal.vue';
import { t } from '../i18n';
import { app, attempt, loadProfiles, notify, randomId } from '../store';
import type { Action, AppEntry, MatchType } from '../types';

const apps = ref<AppEntry[]>([]);
const loading = ref(true);
const query = ref('');
const company = ref('');
const target = ref<AppEntry>();
const profileId = ref('');
const action = ref<Action>('allow');
const matchType = ref<MatchType>('path');
const saving = ref(false);
const details = ref<{ sha256: string; publisher: string; size: number }>();

async function load() {
  loading.value = true;
  apps.value = (await attempt(() => call<AppEntry[]>('apps.discover'))) ?? [];
  await loadProfiles();
  loading.value = false;
}
onMounted(load);

const companies = computed(() => [...new Set(apps.value.map((a) => a.company).filter(Boolean))].sort());
const filtered = computed(() =>
  apps.value.filter(
    (a) =>
      (!company.value || a.company === company.value) &&
      `${a.name} ${a.company} ${a.path}`.toLowerCase().includes(query.value.toLowerCase()),
  ),
);

const palette = ['#e8a33d', '#6aa7f5', '#5fcf9b', '#e57fa0', '#a98af0', '#f08a5d'];
const tint = (name: string) => palette[[...name].reduce((sum, c) => sum + c.charCodeAt(0), 0) % palette.length];

function choose(entry: AppEntry) {
  target.value = entry;
  profileId.value = app.profiles[0]?.id ?? '';
  action.value = 'allow';
  matchType.value = 'path';
  details.value = undefined;
}

async function inspect(entry: AppEntry) {
  target.value = undefined;
  details.value = await attempt(() => call('apps.inspect', { path: entry.path }));
  if (details.value) target.value = entry;
}

async function addRule() {
  const entry = target.value;
  const profile = app.profiles.find((p) => p.id === profileId.value);
  if (!entry || !profile) return;
  saving.value = true;
  const info = matchType.value === 'path' ? undefined : await attempt(() => call<{ sha256: string; publisher: string }>('apps.inspect', { path: entry.path }));
  const value = matchType.value === 'path' ? entry.path : matchType.value === 'hash' ? info?.sha256 : info?.publisher;
  if (!value) {
    saving.value = false;
    notify('bad', matchType.value === 'hash' ? t('The file could not be hashed.') : t('This file has no trusted digital signature.'));
    return;
  }
  const updated = { ...profile, rules: [...profile.rules, { id: randomId('r-'), action: action.value, priority: 50, match: { type: matchType.value, value } }] };
  const saved = await attempt(() => call('profiles.save', { profile: updated }), t('Rule added to “{name}”.', { name: profile.name }));
  saving.value = false;
  if (saved) {
    target.value = undefined;
    await loadProfiles();
  }
}
</script>

<template>
  <div class="page">
    <div class="page-head">
      <div class="grow"><h1>{{ t('Applications') }}</h1><p>{{ t('Programs found on this computer. Pick one to allow or block it in a profile.') }}</p></div>
      <button class="btn" :disabled="loading" @click="load"><Icon name="refresh" />{{ t('Rescan') }}</button>
    </div>

    <div class="row">
      <div class="search grow"><Icon name="search" /><input v-model="query" class="input" :placeholder="t('Search applications')" :aria-label="t('Search applications')" /></div>
      <select v-model="company" class="select" style="width: 220px" :aria-label="t('Publisher')">
        <option value="">{{ t('All publishers') }}</option>
        <option v-for="name in companies" :key="name" :value="name">{{ name }}</option>
      </select>
    </div>

    <div v-if="loading" class="stack"><div v-for="n in 6" :key="n" class="skeleton" style="height: 54px" /></div>
    <div v-else-if="!filtered.length" class="card empty"><Icon name="apps" />{{ t('No applications match your search.') }}</div>
    <div v-else class="table-wrap">
      <table>
        <tbody>
          <tr v-for="entry in filtered" :key="entry.path">
            <td style="width: 1%"><div class="avatar" :style="{ background: tint(entry.name) }">{{ entry.name.slice(0, 2).toUpperCase() }}</div></td>
            <td style="max-width: 0">
              <div class="truncate"><strong>{{ entry.name }}</strong> <span class="faint">{{ entry.company }}</span></div>
              <div class="hint truncate mono">{{ entry.path }}</div>
            </td>
            <td style="width: 1%; white-space: nowrap">
              <button class="btn sm ghost" @click="inspect(entry)"><Icon name="eye" />{{ t('Details') }}</button>
              <button class="btn sm" @click="choose(entry)"><Icon name="plus" />{{ t('Add rule') }}</button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <Modal v-if="target" :title="target.name" @close="target = undefined">
      <template v-if="details">
        <div class="stack" style="gap: 6px">
          <div><span class="label">SHA-256</span><div class="mono" style="word-break: break-all">{{ details.sha256 || '–' }}</div></div>
          <div><span class="label">{{ t('Digital signature') }}</span><div>{{ details.publisher || t('Not signed or not trusted') }}</div></div>
          <div><span class="label">{{ t('Size') }}</span><div>{{ (details.size / 1048576).toFixed(1) }} MB</div></div>
        </div>
        <div class="banner"><Icon name="about" /><span>{{ t('Hash and publisher rules keep working if the file is renamed. Path rules do not.') }}</span></div>
      </template>
      <template v-else>
        <div class="field"><label>{{ t('Profile') }}</label>
          <select v-model="profileId" class="select"><option v-for="p in app.profiles" :key="p.id" :value="p.id">{{ p.name }}</option></select></div>
        <div class="field"><label>{{ t('Action') }}</label>
          <div class="seg"><button :aria-pressed="action === 'allow'" @click="action = 'allow'">{{ t('Allow') }}</button><button :aria-pressed="action === 'deny'" @click="action = 'deny'">{{ t('Block') }}</button></div></div>
        <div class="field"><label>{{ t('Match by') }}</label>
          <select v-model="matchType" class="select">
            <option value="path">{{ t('Path (this exact location)') }}</option>
            <option value="hash">{{ t('File hash (this exact version)') }}</option>
            <option value="publisher">{{ t('Publisher (all signed versions)') }}</option>
          </select></div>
      </template>
      <template #footer>
        <button class="btn" @click="target = undefined">{{ t('Close') }}</button>
        <button v-if="!details" class="btn primary" :disabled="saving || !profileId" @click="addRule"><span v-if="saving" class="spinner" />{{ t('Add rule') }}</button>
      </template>
    </Modal>
  </div>
</template>
