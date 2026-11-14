<script setup lang="ts">
import PageHeader from '../components/PageHeader.vue';
import { onMounted, ref } from 'vue';

import { call } from '../api';
import Icon from '../components/Icon.vue';
import { t } from '../i18n';
import { attempt, formatTime, notify } from '../store';
import type { Diagnostics } from '../types';

const info = ref<Diagnostics>();
const backups = ref<string[]>([]);
const oldPin = ref('');
const newPin = ref('');
const busy = ref(false);

async function load() {
  info.value = await attempt(() => call<Diagnostics>('diagnostics'));
  backups.value = (await attempt(() => call<string[]>('backup.list'))) ?? [];
}
onMounted(load);

async function changePin() {
  busy.value = true;
  if (await attempt(() => call('pin.change', { old: oldPin.value, new: newPin.value }), t('PIN changed.'))) {
    oldPin.value = '';
    newPin.value = '';
  }
  busy.value = false;
}

async function backup() {
  await attempt(() => call('backup.create'), t('Backup created.'));
  await load();
}

async function restore(name: string) {
  if (await attempt(() => call('backup.restore', { name }), t('Backup restored.'))) await load();
}

const stamp = (name: string) => {
  const seconds = Number(name.replace(/\D/g, ''));
  return seconds ? formatTime(seconds) : name;
};

const limits = [
  'Enforcement is reactive: a blocked program can run for a moment before it is stopped.',
  'Programs that are allowed can start other programs. Review launchers and browsers.',
  'A Windows administrator can always remove Houseguest. That is by design, for recovery.',
  'Physical access to the computer is outside what Houseguest can protect. Use BitLocker and a firmware password.',
  'Microsoft Store apps are matched by their executable path or publisher, not by package.',
];
</script>

<template>
  <div class="page">
    <PageHeader :title="t('Security')" :subtitle="t('What Houseguest protects, what it does not, and how to keep it healthy.')" />

    <div class="grid cols-2" style="align-items: start">
      <div class="card stack">
        <div class="row"><Icon name="shield" /><h2>{{ t('Protection level') }}</h2></div>
        <template v-if="info">
          <div class="row"><span class="badge" :class="info.supervisor_running ? 'warn' : 'bad'">{{ info.supervisor_running ? t('Supervised (reactive)') : t('Unavailable') }}</span><span class="muted">{{ info.capabilities.edition }} · build {{ info.capabilities.build }}</span></div>
          <p class="muted">{{ t('Houseguest watches every program the guest account starts and stops the ones your rules do not allow. It works on every Windows edition.') }}</p>
          <div class="banner"><Icon name="about" /><span>{{ info.capabilities.applocker_enforced ? t('This edition can also enforce AppLocker, but Houseguest does not use it yet.') : t('Preventive enforcement (AppLocker) is not supported on this Windows edition.') }}</span></div>
        </template>
        <div v-else class="skeleton" style="height: 80px" />
      </div>

      <div class="card stack">
        <div class="row"><Icon name="alert" /><h2>{{ t('Known limits') }}</h2></div>
        <ul class="stack muted" style="padding-left: 18px; margin: 0; gap: 8px"><li v-for="item in limits" :key="item">{{ t(item) }}</li></ul>
      </div>

      <div class="card stack">
        <div class="row"><Icon name="lock" /><h2>{{ t('Change PIN') }}</h2></div>
        <div class="field"><label for="old">{{ t('Current PIN') }}</label><input id="old" v-model="oldPin" class="input" type="password" autocomplete="off" /></div>
        <div class="field"><label for="new">{{ t('New PIN') }}</label><input id="new" v-model="newPin" class="input" type="password" autocomplete="off" maxlength="64" /></div>
        <button class="btn primary" :disabled="busy || !oldPin || newPin.length < 4" @click="changePin">{{ t('Change PIN') }}</button>
        <p class="hint">{{ t('After three wrong attempts Houseguest makes the next try wait longer each time.') }}</p>
      </div>

      <div class="card stack">
        <div class="row"><Icon name="download" /><h2 class="grow">{{ t('Backups') }}</h2><button class="btn sm" @click="backup"><Icon name="plus" />{{ t('Create backup') }}</button></div>
        <p class="muted">{{ t('A backup holds your profiles and settings. The owner PIN is never changed by a restore.') }}</p>
        <div v-if="!backups.length" class="hint">{{ t('No backups yet.') }}</div>
        <div v-else class="timeline">
          <div v-for="name in [...backups].reverse().slice(0, 6)" :key="name"><span class="grow">{{ stamp(name) }}</span><button class="btn sm" @click="restore(name)">{{ t('Restore') }}</button></div>
        </div>
      </div>
    </div>

    <div class="card stack">
      <h2>{{ t('Recommendations') }}</h2>
      <label class="check"><Icon name="check" /><span>{{ t('Give guests a standard (non-administrator) account. An administrator can bypass any restriction.') }}</span></label>
      <label class="check"><Icon name="check" /><span>{{ t('Turn on BitLocker so your files stay private if someone boots another system.') }}</span></label>
      <label class="check"><Icon name="check" /><span>{{ t('Use a PIN you do not reuse elsewhere, and do not share it.') }}</span></label>
      <button class="btn sm" style="align-self: flex-start" @click="notify('info', t('Recovery: an administrator can run houseguest-cli recover from an elevated prompt.'))"><Icon name="about" />{{ t('How do I recover access?') }}</button>
    </div>
  </div>
</template>
