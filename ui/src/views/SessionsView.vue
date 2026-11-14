<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';

import { ApiError, call } from '../api';
import Findings from '../components/Findings.vue';
import Icon from '../components/Icon.vue';
import Modal from '../components/Modal.vue';
import PreviewPanel from '../components/PreviewPanel.vue';
import { t } from '../i18n';
import { app, attempt, describe, formatDuration, formatTime, loadProfiles, notify, refreshStatus, remaining } from '../store';
import type { Dashboard, Finding, LocalUser, Preview } from '../types';

const users = ref<LocalUser[]>([]);
const detail = ref<Dashboard>();
const profileId = ref('');
const guests = ref<string[]>([]);
const minutes = ref(60);
const custom = ref(false);
const onExpiry = ref<'disconnect' | 'logoff' | 'none'>('disconnect');
const auditOnly = ref(false);
const busy = ref(false);
const findings = ref<Finding[]>([]);
const confirmNeeded = ref(false);
const preview = ref<Preview>();
const previewing = ref(false);

const presets = [15, 30, 60, 120, 240, 480];

async function load() {
  await loadProfiles();
  profileId.value ||= app.profiles[0]?.id ?? '';
  detail.value = await attempt(() => call<Dashboard>('dashboard'));
  if (!app.status?.armed) users.value = (await attempt(() => call<LocalUser[]>('users.list'))) ?? [];
}
onMounted(load);

const valid = computed(() => profileId.value && guests.value.length > 0 && minutes.value >= 1 && minutes.value <= 10080);

async function start(confirmed = false) {
  busy.value = true;
  try {
    await call('session.start', {
      profile_id: profileId.value,
      guest_sids: guests.value,
      duration_minutes: minutes.value,
      on_expiry: onExpiry.value,
      audit_only: auditOnly.value,
      confirm_warnings: confirmed,
    });
    confirmNeeded.value = false;
    notify('ok', t('Protection is now active.'));
    await Promise.all([refreshStatus(), load()]);
  } catch (error) {
    if (error instanceof ApiError && (error.code === 'confirmation_required' || error.code === 'policy_invalid')) {
      findings.value = error.details.findings ?? [];
      confirmNeeded.value = error.code === 'confirmation_required';
      if (!confirmNeeded.value) notify('bad', t('The policy has errors and cannot be activated.'));
    } else {
      notify('bad', describe(error));
    }
  } finally {
    busy.value = false;
  }
}

async function runPreview() {
  previewing.value = true;
  preview.value = await attempt(() => call<Preview>('policy.preview', { profile_id: profileId.value, guest_sids: guests.value }));
  previewing.value = false;
}

async function stop() {
  await attempt(() => call('session.stop'), t('Protection stopped.'));
  await Promise.all([refreshStatus(), load()]);
}

async function extend(add: number) {
  await attempt(() => call('session.extend', { minutes: add }), t('Session extended.'));
  await Promise.all([refreshStatus(), load()]);
}
</script>

<template>
  <div class="page">
    <div class="page-head"><div class="grow"><h1>{{ t('Sessions') }}</h1><p>{{ t('Lend your computer for a set time, with only the apps you choose.') }}</p></div></div>

    <div v-if="app.status?.armed && detail?.session" class="card hero stack">
      <div class="row">
        <div class="grow">
          <div class="status-pill" style="color: var(--success)"><span class="pulse" />{{ t('Protection active') }}</div>
          <p class="muted">{{ t('Profile “{name}” · ends {time}', { name: detail.session.profile_name, time: formatTime(detail.session.ends_at) }) }}</p>
        </div>
        <strong style="font-size: 32px; font-variant-numeric: tabular-nums">{{ formatDuration(remaining) }}</strong>
      </div>
      <div class="row wrap">
        <button v-for="n in [15, 30, 60]" :key="n" class="btn sm" @click="extend(n)">+{{ n }} {{ t('min') }}</button>
        <button class="btn sm danger right" @click="stop"><Icon name="stop" />{{ t('Stop protection') }}</button>
      </div>
      <div v-if="detail.exceptions?.length">
        <h3>{{ t('Temporary exceptions') }}</h3>
        <div class="timeline">
          <div v-for="rule in detail.exceptions" :key="rule.id">
            <span class="badge info">{{ rule.match.type }}</span>
            <span class="mono grow truncate">{{ rule.match.value }}</span>
            <span class="faint">{{ rule.expires_at ? t('until {time}', { time: formatTime(rule.expires_at) }) : '' }}</span>
          </div>
        </div>
      </div>
    </div>

    <div v-else class="grid cols-2" style="align-items: start">
      <div class="card stack">
        <h2>{{ t('1. Choose a profile') }}</h2>
        <select v-model="profileId" class="select" :aria-label="t('Profile')">
          <option v-for="profile in app.profiles" :key="profile.id" :value="profile.id">{{ profile.name }}</option>
        </select>
        <h2 style="margin-top: 8px">{{ t('2. Who is the guest?') }}</h2>
        <div v-if="!users.length" class="skeleton" />
        <div v-else class="stack" style="gap: 2px">
          <label v-for="user in users" :key="user.sid" class="check">
            <input v-model="guests" type="checkbox" :value="user.sid" />
            <span class="grow">{{ user.name }}</span>
            <span v-if="user.owner" class="badge accent">{{ t('Your account') }}</span>
          </label>
        </div>
        <p class="hint">{{ t('Tip: create a separate standard account for guests (Windows Settings → Accounts).') }}</p>
      </div>

      <div class="card stack">
        <h2>{{ t('3. How long?') }}</h2>
        <div class="row wrap" style="gap: 8px">
          <button v-for="n in presets" :key="n" class="btn sm" :class="{ primary: !custom && minutes === n }" @click="(minutes = n), (custom = false)">
            {{ n < 60 ? `${n} ${t('min')}` : `${n / 60} h` }}
          </button>
          <button class="btn sm" :class="{ primary: custom }" @click="custom = true">{{ t('Custom') }}</button>
        </div>
        <div v-if="custom" class="field">
          <label for="minutes">{{ t('Minutes (1–10080)') }}</label>
          <input id="minutes" v-model.number="minutes" class="input" type="number" min="1" max="10080" />
        </div>
        <h2 style="margin-top: 8px">{{ t('4. When time is up') }}</h2>
        <select v-model="onExpiry" class="select" :aria-label="t('When time is up')">
          <option value="disconnect">{{ t('Lock the guest out (disconnect)') }}</option>
          <option value="logoff">{{ t('Sign the guest out') }}</option>
          <option value="none">{{ t('Only remove restrictions') }}</option>
        </select>
        <label class="row" style="margin-top: 4px">
          <span class="switch"><input v-model="auditOnly" type="checkbox" /></span>
          <span class="grow"><strong>{{ t('Audit mode') }}</strong><br /><span class="hint">{{ t('Only record what would be blocked. Nothing is stopped.') }}</span></span>
        </label>
        <div class="row" style="margin-top: 8px">
          <button class="btn" :disabled="!valid || previewing" @click="runPreview"><span v-if="previewing" class="spinner" /><Icon v-else name="eye" />{{ t('Preview') }}</button>
          <button class="btn primary right" :disabled="!valid || busy" @click="start()"><Icon name="shield" />{{ t('Start protection') }}</button>
        </div>
      </div>
    </div>

    <Modal v-if="preview" :title="t('Policy preview')" wide @close="preview = undefined">
      <PreviewPanel :preview="preview" />
      <template #footer><button class="btn" @click="preview = undefined">{{ t('Close') }}</button></template>
    </Modal>

    <Modal v-if="findings.length" :title="confirmNeeded ? t('Review before activating') : t('Policy problems')" @close="findings = []">
      <Findings :findings="findings" />
      <template #footer>
        <button class="btn" @click="findings = []">{{ t('Cancel') }}</button>
        <button v-if="confirmNeeded" class="btn primary" @click="(findings = []), start(true)">{{ t('Activate anyway') }}</button>
      </template>
    </Modal>
  </div>
</template>
