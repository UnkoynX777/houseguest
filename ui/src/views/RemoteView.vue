<script setup lang="ts">
import { computed, onActivated, onBeforeUnmount, onDeactivated, onMounted, ref } from 'vue';

import { call } from '../api';
import CopyText from '../components/CopyText.vue';
import Modal from '../components/Modal.vue';
import Icon from '../components/Icon.vue';
import PageHeader from '../components/PageHeader.vue';
import { t } from '../i18n';
import { attempt, formatDuration, formatTime } from '../store';

interface Remote {
  state: string;
  configured: boolean;
  enabled: boolean;
  hostname: string;
  url: string;
  emails: string[];
  connected_seconds: number;
  last_connected: number;
  restarts: number;
  last_error: string;
  access_policy: boolean;
  component_installed: boolean;
}
interface Device {
  id: string;
  name: string;
  scope: 'read' | 'write';
  created: number;
  expires: number;
  last_seen: number;
  revoked: boolean;
}

const status = ref<Remote | null>(null);
const devices = ref<Device[]>([]);
const apiRunning = ref(false);
const logs = ref<string[]>([]);
const showLogs = ref(false);
const busy = ref('');
const exposure = ref<{ protected: boolean; reachable: boolean; message: string } | null>(null);

const token = ref('');
const found = ref<{ accounts: { id: string; name: string }[]; zones: { id: string; name: string; account: string }[] } | null>(null);
const accountId = ref('');
const zoneId = ref('');
const subdomain = ref('houseguest');
const emails = ref('');

const deviceName = ref('');
const deviceScope = ref<'read' | 'write'>('read');
const issued = ref('');

const zones = computed(() => found.value?.zones.filter((z) => !accountId.value || z.account === accountId.value) ?? []);
const zoneName = computed(() => zones.value.find((z) => z.id === zoneId.value)?.name ?? '');
const hostname = computed(() => (zoneName.value ? `${subdomain.value.trim().toLowerCase()}.${zoneName.value}` : ''));
const live = computed(() => status.value?.state === 'connected');
const stateLabel = computed(() => {
  const s = status.value;
  if (!s) return '';
  if (!s.configured) return t('Not set up');
  return { stopped: t('Off'), starting: t('Starting'), connected: t('Connected'), reconnecting: t('Reconnecting'), failed: t('Stopped after errors') }[s.state] ?? s.state;
});
const stateKind = computed(() => ({ connected: 'ok', failed: 'bad', reconnecting: 'warn', starting: 'warn' })[status.value?.state ?? ''] ?? '');

async function load() {
  try {
    status.value = await call<Remote>('remote.status');
    const info = await call<{ running: boolean; devices: Device[] }>('api.info');
    apiRunning.value = info.running;
    devices.value = info.devices.filter((d) => !d.revoked);
    if (showLogs.value) logs.value = await call<string[]>('remote.logs');
  } catch {
    status.value = null;
  }
}

let timer: ReturnType<typeof setInterval> | undefined;
const start = () => { void load(); clearInterval(timer); timer = setInterval(() => void load(), 4000); };
const stop = () => clearInterval(timer);
onMounted(start);
onActivated(start);
onDeactivated(stop);
onBeforeUnmount(stop);

async function run<T>(name: string, action: () => Promise<T>, ok?: string) {
  busy.value = name;
  try {
    return await attempt(action, ok);
  } finally {
    busy.value = '';
    await load();
  }
}

async function check() {
  const reply = await run('check', () => call('remote.discover', { api_token: token.value.trim() }));
  if (!reply) return;
  found.value = reply;
  accountId.value = reply.accounts[0]?.id ?? '';
  zoneId.value = reply.zones.find((z: { account: string }) => z.account === accountId.value)?.id ?? '';
}

async function create() {
  const list = emails.value.split(/[\s,;]+/).filter(Boolean);
  const done = await run('setup', () => call('remote.setup', { api_token: token.value.trim(), account_id: accountId.value, zone_id: zoneId.value, hostname: hostname.value, emails: list }), t('Remote access is set up. It is still off.'));
  if (done) {
    token.value = '';
    found.value = null;
  }
}

const install = () => run('install', () => call('remote.install'), t('The cloudflared component is installed.'));
const enable = () => run('enable', () => call('remote.enable'), t('Remote access is on.'));
const disable = () => run('disable', () => call('remote.disable'), t('Remote access is off.'));

async function test() {
  exposure.value = (await run('test', () => call('remote.test'))) ?? null;
}

const removing = ref(false);
async function reset() {
  removing.value = false;
  await run('reset', () => call('remote.reset', { api_token: token.value.trim() }), t('Remote access was removed.'));
}

async function pair() {
  const reply = await run('pair', () => call<{ token: string }>('api.devices.create', { name: deviceName.value.trim(), scope: deviceScope.value }));
  if (reply) {
    issued.value = reply.token;
    deviceName.value = '';
  }
}

const revoke = (device: Device) => run('revoke', () => call('api.devices.revoke', { id: device.id }), t('The device was removed.'));
const toggleLogs = async () => {
  showLogs.value = !showLogs.value;
  if (showLogs.value) logs.value = await call<string[]>('remote.logs').catch(() => []);
};
</script>

<template>
  <div class="page">
    <PageHeader :title="t('Remote access')" :subtitle="t('Check on Houseguest from your phone. Local protection never depends on this.')" />

    <div v-if="!status" class="card"><span class="skeleton" style="width: 60%" /></div>

    <template v-else>
      <section class="card stack">
        <div class="row">
          <div class="grow">
            <div class="row" style="gap: 8px"><h2>{{ t('Connection') }}</h2><span class="badge" :class="stateKind">{{ stateLabel }}</span></div>
            <p v-if="status.configured" class="muted">{{ status.hostname }}<template v-if="live"> · {{ t('connected for') }} {{ formatDuration(status.connected_seconds) }}</template></p>
            <p v-else class="muted">{{ t('Not set up yet. Remote access stays off until you finish the steps below.') }}</p>
          </div>
          <template v-if="status.configured">
            <button v-if="!status.component_installed" class="btn" :disabled="!!busy" @click="install"><Icon name="download" />{{ busy === 'install' ? t('Installing…') : t('Install cloudflared') }}</button>
            <button v-else-if="status.enabled" class="btn" :disabled="!!busy" @click="disable"><Icon name="stop" />{{ t('Turn off') }}</button>
            <button v-else class="btn primary" :disabled="!!busy || !apiRunning" @click="enable"><Icon name="play" />{{ t('Turn on') }}</button>
          </template>
        </div>
        <p v-if="status.last_error" class="badge bad" style="height: auto; padding: 6px 10px; white-space: normal">{{ status.last_error }}</p>
        <div v-if="status.configured" class="row wrap">
          <CopyText :value="status.url" mono />
          <span class="grow" />
          <button class="btn sm" :disabled="!!busy" @click="test"><Icon name="shield" />{{ t('Check that it is protected') }}</button>
          <button class="btn sm ghost" @click="toggleLogs"><Icon name="list" />{{ t('Log') }}</button>
        </div>
        <p v-if="exposure" class="badge" :class="exposure.protected ? 'ok' : 'bad'" style="height: auto; padding: 6px 10px; white-space: normal">{{ exposure.message }}</p>
        <pre v-if="showLogs" class="logbox">{{ logs.join('\n') || t('Nothing logged yet.') }}</pre>
      </section>

      <section v-if="!status.configured" class="card stack">
        <div><h2>{{ t('Set up with Cloudflare') }}</h2>
          <p class="muted">{{ t('Houseguest creates a tunnel, a DNS name and a sign-in policy in your Cloudflare account. Only the e-mail addresses you list can open it, and every request is checked again by Houseguest.') }}</p></div>
        <ol class="steps">
          <li>{{ t('You need a Cloudflare account with a domain, and Zero Trust turned on (the free plan is enough).') }}</li>
          <li>{{ t('Create an API token at dash.cloudflare.com/profile/api-tokens with these permissions: Account · Cloudflare Tunnel · Edit; Account · Access: Apps and Policies · Edit; Account · Access: Organizations · Read; Zone · DNS · Edit; Zone · Zone · Read.') }}</li>
          <li>{{ t('Paste it here. It is used once for the setup and is never saved.') }}</li>
        </ol>
        <div class="row"><input v-model="token" class="input" type="password" autocomplete="off" spellcheck="false" :placeholder="t('Cloudflare API token')" :aria-label="t('Cloudflare API token')" />
          <button class="btn" :disabled="!!busy || token.trim().length < 20" @click="check">{{ busy === 'check' ? t('Checking…') : t('Check token') }}</button></div>
        <template v-if="found">
          <div class="row wrap">
            <select v-model="accountId" class="select" style="width: 200px" :aria-label="t('Account')"><option v-for="a in found.accounts" :key="a.id" :value="a.id">{{ a.name }}</option></select>
            <select v-model="zoneId" class="select" style="width: 200px" :aria-label="t('Domain')"><option v-for="z in zones" :key="z.id" :value="z.id">{{ z.name }}</option></select>
          </div>
          <div class="row"><input v-model="subdomain" class="input" style="width: 160px" :aria-label="t('Name')" /><span class="muted">.{{ zoneName }}</span></div>
          <input v-model="emails" class="input" :placeholder="t('E-mail addresses allowed to sign in, separated by commas')" :aria-label="t('Allowed e-mail addresses')" />
          <div class="row"><span class="muted grow">{{ hostname }}</span>
            <button class="btn primary" :disabled="!!busy || !hostname || !emails.trim()" @click="create">{{ busy === 'setup' ? t('Creating…') : t('Create') }}</button></div>
        </template>
      </section>

      <section class="card stack">
        <div class="row">
          <div class="grow"><h2>{{ t('Paired phones') }}</h2><p class="muted">{{ t('Each phone has its own key. Read-only keys can look but not change.') }}</p></div>
          <span v-if="!apiRunning" class="badge warn">{{ t('Local API is not running') }}</span>
        </div>
        <div v-if="issued" class="card flat stack" style="background: var(--accent-soft)">
          <strong>{{ t('Copy this key now. It will not be shown again.') }}</strong>
          <CopyText :value="issued" mono :lines="2" />
          <p class="muted">{{ t('Open the address of your tunnel on the phone and paste the key when asked.') }}</p>
          <button class="btn sm" @click="issued = ''">{{ t('I copied it') }}</button>
        </div>
        <div v-if="devices.length === 0" class="muted">{{ t('No phones are paired.') }}</div>
        <div v-for="device in devices" :key="device.id" class="row">
          <Icon name="phone" />
          <div class="grow"><strong>{{ device.name }}</strong><div class="faint">{{ t('Last used') }} {{ device.last_seen ? formatTime(device.last_seen) : '—' }}</div></div>
          <span class="badge" :class="device.scope === 'write' ? 'warn' : ''">{{ device.scope === 'write' ? t('can change') : t('read only') }}</span>
          <button class="btn sm danger" :disabled="!!busy" @click="revoke(device)">{{ t('Remove') }}</button>
        </div>
        <div class="row wrap">
          <input v-model="deviceName" class="input" style="width: 200px" maxlength="64" :placeholder="t('Phone name')" :aria-label="t('Phone name')" />
          <div class="seg"><button :aria-pressed="deviceScope === 'read'" @click="deviceScope = 'read'">{{ t('Read only') }}</button><button :aria-pressed="deviceScope === 'write'" @click="deviceScope = 'write'">{{ t('Can change') }}</button></div>
          <button class="btn" :disabled="!!busy || !deviceName.trim() || !apiRunning" @click="pair"><Icon name="plus" />{{ t('Add phone') }}</button>
        </div>
      </section>

      <details class="card">
        <summary><strong>{{ t('What this does and does not protect') }}</strong></summary>
        <ul class="notes">
          <li>{{ t('Traffic between your phone and Cloudflare, and between Cloudflare and this computer, is encrypted. Cloudflare can technically see requests while it forwards them.') }}</li>
          <li>{{ t('Sign-in is checked twice: by Cloudflare Access, and by Houseguest, which verifies the signed identity itself.') }}</li>
          <li>{{ t('Changes that matter need the owner PIN on the phone, and the phone key can be removed here at any time.') }}</li>
          <li>{{ t('Someone with administrator control of this computer can still change or remove Houseguest. Turning remote access off does not affect local protection.') }}</li>
        </ul>
      </details>

      <div v-if="status.configured" class="row"><span class="grow" /><button class="btn sm ghost danger" :disabled="!!busy" @click="removing = true">{{ t('Remove remote access') }}</button></div>
    <Modal v-if="removing" :title="t('Remove remote access?')" @close="removing = false">
      <p class="muted">{{ t('The tunnel stops and this computer forgets it. To also delete the tunnel, DNS name and sign-in policy from Cloudflare, paste an API token with the same permissions as in the setup.') }}</p>
      <input v-model="token" class="input" type="password" autocomplete="off" spellcheck="false" :placeholder="t('Cloudflare API token (optional)')" :aria-label="t('Cloudflare API token (optional)')" style="margin-top: 12px" />
      <template #footer><button class="btn" @click="removing = false">{{ t('Cancel') }}</button><button class="btn danger" @click="reset">{{ t('Remove') }}</button></template>
    </Modal>
    </template>
  </div>
</template>

<style scoped>
.steps { margin: 0; padding-left: 20px; color: var(--text-2); display: grid; gap: 4px; }
.logbox { margin: 0; max-height: 180px; overflow: auto; padding: 8px 10px; background: var(--sunken); border-radius: var(--radius-s); font: 11.5px/1.5 var(--mono); white-space: pre-wrap; word-break: break-all; }
.notes { margin: 8px 0 0; padding-left: 18px; color: var(--text-2); display: grid; gap: 4px; }
details summary { cursor: pointer; }
</style>
