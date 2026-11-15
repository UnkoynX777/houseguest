<script setup lang="ts">
import { computed, onActivated, ref } from 'vue';

import { call } from '../api';
import Icon from '../components/Icon.vue';
import PageHeader from '../components/PageHeader.vue';
import { t } from '../i18n';
import { app, attempt, formatTime } from '../store';
import type { Diagnostics } from '../types';
import { useResource } from '../use-resource';

const report = useResource<Diagnostics>(() => 'diagnostics', () => call<Diagnostics>('diagnostics'), ['policy', 'session'], 30_000);
const saved = useResource<string[]>(() => 'backups', () => call<string[]>('backup.list'), ['policy'], 120_000);
const info = report.data;
const backups = computed(() => saved.data.value ?? []);

const remote = ref<{ configured: boolean; enabled: boolean; state: string } | null>(null);
const phones = ref<{ scope: string; revoked: boolean }[]>([]);
async function loadRemote() {
  remote.value = await call<{ configured: boolean; enabled: boolean; state: string }>('remote.status').catch(() => null);
  phones.value = await call<{ devices: { scope: string; revoked: boolean }[] }>('api.info').then((r) => r.devices.filter((d) => !d.revoked), () => []);
}
onActivated(loadRemote);
void loadRemote();

const oldPin = ref('');
const newPin = ref('');
const busy = ref(false);
const mode = computed(() => info.value?.supervisor?.mode);
const modeBadge = computed(() => (mode.value ? { supervised: 'ok', polling: 'warn', unavailable: 'bad' }[mode.value] : ''));
const modeText = computed(() => (mode.value ? { supervised: t('Supervised'), polling: t('Polling (reduced)'), unavailable: t('Unavailable') }[mode.value] : '—'));
const remoteText = computed(() => {
  const r = remote.value;
  if (!r) return '—';
  if (!r.configured) return t('Not set up');
  return r.enabled ? (r.state === 'connected' ? t('Connected') : t('On')) : t('Off');
});
const remoteBadge = computed(() => (remote.value?.enabled ? (remote.value.state === 'connected' ? 'ok' : 'warn') : ''));
const writers = computed(() => phones.value.filter((p) => p.scope === 'write').length);

async function load() {
  await Promise.all([report.load(true), saved.load(true), loadRemote()]);
}

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
  'Package rules for Microsoft Store apps apply to running programs; the preview cannot show them.',
  'Remote access and the assistant send data to Cloudflare and Google only when you turn them on. Cloudflare can technically read requests it forwards.',
];
</script>

<template>
  <div class="page">
    <PageHeader :title="t('Security')" :subtitle="t('What Houseguest protects, what it does not, and how to keep it healthy.')" />

    <section class="card">
      <div class="status-row">
        <div><span class="faint">{{ t('Protection') }}</span><div class="row" style="gap: 6px"><span class="badge" :class="modeBadge">{{ modeText }}</span></div></div>
        <div><span class="faint">{{ t('Session') }}</span><div><strong>{{ app.status?.armed ? t('Active') : t('None') }}</strong></div></div>
        <div><span class="faint">{{ t('Remote access') }}</span><div class="row" style="gap: 6px"><span class="badge" :class="remoteBadge">{{ remoteText }}</span></div></div>
        <div><span class="faint">{{ t('Paired phones') }}</span><div><strong>{{ phones.length }}</strong><span v-if="writers" class="faint"> · {{ writers }} {{ t('can change') }}</span></div></div>
        <div><span class="faint">{{ t('Backups') }}</span><div><strong>{{ backups.length }}</strong></div></div>
      </div>
    </section>

    <div class="grid cols-2" style="align-items: start">
      <section class="card stack">
        <div class="row"><Icon name="shield" /><h2>{{ t('Local protection') }}</h2></div>
        <template v-if="info">
          <p class="muted">{{ t('Houseguest watches every program the guest account starts and stops the ones your rules do not allow.') }} {{ info.capabilities?.product_name }} · build {{ info.capabilities?.build }}</p>
          <details><summary>{{ t('Details') }}</summary>
            <p class="muted" style="margin-top: 6px">{{ t('Houseguest uses its own process supervision. It does not depend on Windows Defender, AppLocker or App Control, so it behaves the same on every edition.') }}</p>
          </details>
        </template>
        <div v-else class="skeleton" style="height: 48px" />
      </section>

      <section class="card stack">
        <div class="row"><Icon name="lock" /><h2>{{ t('Authentication') }}</h2></div>
        <details>
          <summary>{{ t('Change PIN') }}</summary>
          <div class="stack" style="margin-top: 10px">
            <div class="field"><label for="old">{{ t('Current PIN') }}</label><input id="old" v-model="oldPin" class="input" type="password" autocomplete="off" /></div>
            <div class="field"><label for="new">{{ t('New PIN') }}</label><input id="new" v-model="newPin" class="input" type="password" autocomplete="off" maxlength="64" /></div>
            <button class="btn primary" style="align-self: flex-start" :disabled="busy || !oldPin || newPin.length < 4" @click="changePin">{{ t('Change PIN') }}</button>
          </div>
        </details>
        <p class="hint">{{ t('After three wrong attempts Houseguest makes the next try wait longer each time.') }}</p>
      </section>

      <section class="card stack">
        <div class="row"><Icon name="remote" /><h2 class="grow">{{ t('Remote access') }}</h2><button class="btn sm" @click="app.route = 'remote'">{{ t('Manage') }}</button></div>
        <p class="muted">{{ t('Off unless you set it up. Phones sign in through Cloudflare Access, and changes need your PIN.') }}</p>
        <p class="hint">{{ t('Paired phones and their rights are listed on the Remote access page, where each can be removed.') }}</p>
      </section>

      <section class="card stack">
        <div class="row"><Icon name="download" /><h2 class="grow">{{ t('Backup and recovery') }}</h2><button class="btn sm" @click="backup"><Icon name="plus" />{{ t('Create backup') }}</button></div>
        <p class="muted">{{ t('A backup holds your profiles and settings. The owner PIN is never changed by a restore.') }}</p>
        <div v-if="!backups.length" class="hint">{{ t('No backups yet.') }}</div>
        <div v-else class="timeline">
          <div v-for="name in [...backups].reverse().slice(0, 4)" :key="name"><span class="grow">{{ stamp(name) }}</span><button class="btn sm" @click="restore(name)">{{ t('Restore') }}</button></div>
        </div>
        <details><summary>{{ t('How do I recover access?') }}</summary>
          <p class="muted" style="margin-top: 6px">{{ t('Recovery: an administrator can run houseguest-cli recover from an elevated prompt.') }}</p>
        </details>
      </section>
    </div>

    <section class="card stack">
      <details>
        <summary><strong>{{ t('Known limits') }}</strong></summary>
        <ul class="stack muted" style="padding-left: 18px; margin: 8px 0 0; gap: 6px"><li v-for="item in limits" :key="item">{{ t(item) }}</li></ul>
      </details>
      <details>
        <summary><strong>{{ t('Recommendations') }}</strong></summary>
        <div class="stack" style="margin-top: 8px">
          <label class="check"><Icon name="check" /><span>{{ t('Give guests a standard (non-administrator) account. An administrator can bypass any restriction.') }}</span></label>
          <label class="check"><Icon name="check" /><span>{{ t('Turn on BitLocker so your files stay private if someone boots another system.') }}</span></label>
          <label class="check"><Icon name="check" /><span>{{ t('Use a PIN you do not reuse elsewhere, and do not share it.') }}</span></label>
        </div>
      </details>
    </section>
  </div>
</template>

<style scoped>
.status-row { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 12px; }
.status-row .faint { font-size: 12px; }
details summary { cursor: pointer; }
@media (max-width: 760px) { .status-row { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
</style>
