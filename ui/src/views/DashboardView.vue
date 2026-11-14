<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';

import { call } from '../api';
import Icon from '../components/Icon.vue';
import { t } from '../i18n';
import { app, attempt, formatDuration, formatTime, go, refreshStatus, remaining } from '../store';
import type { Dashboard } from '../types';
import { eventLabel } from './events';

const data = ref<Dashboard>();
const loading = ref(true);

async function load() {
  data.value = await attempt(() => call<Dashboard>('dashboard'));
  loading.value = false;
}
onMounted(load);

const total = computed(() => (data.value?.session ? data.value.session.ends_at - data.value.session.started_at : 1));
const circumference = 2 * Math.PI * 62;
const offset = computed(() => circumference * (1 - Math.min(1, remaining.value / total.value)));

async function extend(minutes: number) {
  await attempt(() => call('session.extend', { minutes }), t('Session extended.'));
  await Promise.all([refreshStatus(), load()]);
}

async function stop() {
  await attempt(() => call('session.stop'), t('Protection stopped.'));
  await Promise.all([refreshStatus(), load()]);
}
</script>

<template>
  <div class="page">
    <div class="page-head">
      <div class="grow">
        <h1>{{ t('Dashboard') }}</h1>
        <p>{{ t('Your computer, your rules.') }}</p>
      </div>
      <button class="btn primary" @click="go('sessions')"><Icon name="play" />{{ app.status?.armed ? t('Manage session') : t('Start a guest session') }}</button>
    </div>

    <div v-if="loading" class="grid cols-3"><div v-for="n in 3" :key="n" class="card"><div class="skeleton" style="height: 70px" /></div></div>

    <template v-else-if="data">
      <div v-if="data.enforcement === 'unavailable'" class="banner bad" role="alert">
        <Icon name="alert" />
        <div><strong>{{ t('Enforcement is unavailable') }}</strong><p>{{ t('The service could not start process monitoring. See Diagnostics.') }}</p></div>
      </div>
      <div v-if="data.store_tampered" class="banner bad" role="alert">
        <Icon name="alert" /><div><strong>{{ t('Settings failed their integrity check') }}</strong><p>{{ t('Houseguest started from a clean state. Review Security and Diagnostics.') }}</p></div>
      </div>

      <div class="card hero row" style="gap: 32px">
        <div v-if="data.session" class="ring" :aria-label="t('Time remaining')">
          <svg viewBox="0 0 148 148"><circle class="track" cx="74" cy="74" r="62" /><circle class="bar" cx="74" cy="74" r="62" :stroke-dasharray="circumference" :stroke-dashoffset="offset" /></svg>
          <div class="center"><strong>{{ formatDuration(remaining) }}</strong><span class="faint">{{ t('remaining') }}</span></div>
        </div>
        <div class="grow stack" style="gap: 8px">
          <div class="status-pill" :style="{ color: data.session ? 'var(--success)' : 'var(--text)' }">
            <span v-if="data.session" class="pulse" />
            {{ data.session ? (data.session.audit_only ? t('Observing (audit mode)') : t('Protection active')) : t('Protection is off') }}
          </div>
          <p v-if="data.session" class="muted">
            {{ t('Profile “{name}” · {n} guest account(s) · ends {time}', { name: data.session.profile_name, n: data.session.guest_sids.length, time: formatTime(data.session.ends_at) }) }}
          </p>
          <p v-else class="muted">{{ t('No guest session is running. Start one to restrict an account to the apps you choose.') }}</p>
          <div v-if="data.session" class="row wrap" style="margin-top: 8px">
            <button class="btn sm" @click="extend(15)">+15 {{ t('min') }}</button>
            <button class="btn sm" @click="extend(30)">+30 {{ t('min') }}</button>
            <button class="btn sm" @click="extend(60)">+60 {{ t('min') }}</button>
            <button class="btn sm danger" @click="stop"><Icon name="stop" />{{ t('Stop protection') }}</button>
          </div>
        </div>
      </div>

      <div class="grid cols-4">
        <div class="card kpi"><div class="value">{{ data.blocked }}</div><div class="label">{{ t('Launches blocked this session') }}</div></div>
        <div class="card kpi"><div class="value">{{ data.session ? data.allowed_rules ?? 0 : '–' }}</div><div class="label">{{ t('Allow rules in profile') }}</div></div>
        <div class="card kpi"><div class="value">{{ data.profiles }}</div><div class="label">{{ t('Profiles') }}</div></div>
        <div class="card kpi">
          <div class="value" style="font-size: 20px; padding: 5px 0"><span class="badge" :class="data.enforcement === 'supervised' ? 'ok' : 'bad'">{{ data.enforcement === 'supervised' ? t('Supervised') : t('Unavailable') }}</span></div>
          <div class="label">{{ t('Enforcement level') }}</div>
        </div>
      </div>

      <div class="card">
        <div class="row"><h2 class="grow">{{ t('Recent events') }}</h2><button class="btn sm ghost" @click="go('history')">{{ t('See all') }}</button></div>
        <div v-if="!data.recent.length" class="empty"><Icon name="history" />{{ t('Nothing has happened yet.') }}</div>
        <div v-else class="timeline" style="margin-top: 8px">
          <div v-for="entry in data.recent" :key="entry.seq">
            <span class="faint" style="width: 150px; flex: none">{{ formatTime(entry.ts) }}</span>
            <span>{{ eventLabel(entry) }}</span>
          </div>
        </div>
      </div>
    </template>
  </div>
</template>
