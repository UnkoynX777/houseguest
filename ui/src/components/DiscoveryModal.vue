<script setup lang="ts">
import { computed, onUnmounted, ref } from 'vue';

import { ApiError, call } from '../api';
import { riskLabel, roleLabel } from '../explain';
import { t } from '../i18n';
import { attempt, describe, formatDuration, notify } from '../store';
import type { DependencyMode, DiscoveryReport, LibraryApp, ObservedItem } from '../types';
import AppIcon from './AppIcon.vue';
import Icon from './Icon.vue';
import Modal from './Modal.vue';

const props = defineProps<{ entry: LibraryApp; profileId: string }>();
const emit = defineEmits<{ close: []; saved: [] }>();

type Step = 'start' | 'observing' | 'review';
const step = ref<Step>('start');
const minutes = ref(10);
const report = ref<DiscoveryReport>();
const picked = ref(new Set<string>());
const name = ref(props.entry.name);
const mode = ref<DependencyMode>('approved');
const keep = ref(false);
const busy = ref(false);
const now = ref(Date.now() / 1000);
const acknowledge = ref(false);
let timer: ReturnType<typeof setInterval> | undefined;

const key = (item: ObservedItem) => `${item.path.toLowerCase()}|${item.sha256}`;
const items = computed(() => (report.value?.items ?? []).filter((item) => item.verdict !== 'main'));
const groups = computed(() => [
  { id: 'recommended', title: t('Recommended'), hint: t('Started by the application and signed or kept in its folder.'), items: items.value.filter((i) => i.verdict === 'recommended') },
  { id: 'review', title: t('Look closely'), hint: t('Unsigned, outside the app folder, or not started by the application.'), items: items.value.filter((i) => i.verdict === 'review') },
  { id: 'avoid', title: t('Can run other programs'), hint: t('Shells and script hosts. Approving them weakens the profile.'), items: items.value.filter((i) => i.verdict === 'avoid') },
  { id: 'windows', title: t('Windows components'), hint: t('Already protected. No approval needed.'), items: items.value.filter((i) => i.verdict === 'windows') },
]);
const remaining = computed(() => Math.max(0, Math.floor((report.value?.ends_at ?? 0) - now.value)));
const needsAcknowledge = computed(() => items.value.some((i) => i.verdict === 'avoid' && picked.value.has(key(i))));

async function poll() {
  now.value = Date.now() / 1000;
  try {
    const next = await call<DiscoveryReport>('discovery.status');
    report.value = next;
    if (!next.running && step.value === 'observing') toReview();
  } catch {
    /* the next tick tries again */
  }
}

async function start() {
  busy.value = true;
  const started = await attempt(() => call<DiscoveryReport>('discovery.start', { path: props.entry.path, package_family: props.entry.package_family || undefined, label: props.entry.name, minutes: minutes.value }));
  busy.value = false;
  if (!started) return;
  report.value = started;
  step.value = 'observing';
  timer = setInterval(poll, 1500);
}

async function finish() {
  busy.value = true;
  const stopped = await attempt(() => call<DiscoveryReport>('discovery.stop'));
  busy.value = false;
  if (stopped) {
    report.value = stopped;
    toReview();
  }
}

function toReview() {
  if (timer) clearInterval(timer);
  timer = undefined;
  picked.value = new Set(items.value.filter((i) => i.verdict === 'recommended').map(key));
  step.value = 'review';
}

function toggle(item: ObservedItem) {
  const next = new Set(picked.value);
  if (!next.delete(key(item))) next.add(key(item));
  picked.value = next;
}

async function save() {
  busy.value = true;
  try {
    await call('discovery.apply', {
      profile_id: props.profileId,
      name: name.value.trim() || props.entry.name,
      mode: mode.value,
      save_to_library: keep.value,
      acknowledge_sensitive: acknowledge.value,
      items: items.value.filter((i) => picked.value.has(key(i))).map((i) => ({ path: i.path, sha256: i.sha256 })),
    });
    notify('ok', t('{name} and its dependencies are saved.', { name: name.value }));
    emit('saved');
    emit('close');
  } catch (error) {
    if (error instanceof ApiError && error.code === 'confirmation_required') notify('info', t('Tick the box to confirm that you want to approve tools that run other programs.'));
    else notify('bad', describe(error));
  } finally {
    busy.value = false;
  }
}

async function cancel() {
  if (step.value === 'observing') await call('discovery.stop').catch(() => undefined);
  emit('close');
}

onUnmounted(() => {
  if (timer) clearInterval(timer);
});
</script>

<template>
  <Modal :title="t('Find what {name} needs', { name: entry.name })" wide @close="cancel">
    <template v-if="step === 'start'">
      <div class="row"><AppIcon :path="entry.path" :name="entry.name" :size="48" /><div class="grow"><strong>{{ entry.name }}</strong><div class="hint mono truncate">{{ entry.path }}</div></div></div>
      <p>{{ t('Houseguest watches the programs your account starts while you use this application, then lists them so you can approve the ones it really needs. Nothing is allowed automatically.') }}</p>
      <div class="field">
        <label>{{ t('How long to watch') }}</label>
        <select v-model.number="minutes" class="select" style="max-width: 200px">
          <option :value="5">{{ t('{n} minutes', { n: 5 }) }}</option>
          <option :value="10">{{ t('{n} minutes', { n: 10 }) }}</option>
          <option :value="20">{{ t('{n} minutes', { n: 20 }) }}</option>
          <option :value="30">{{ t('{n} minutes', { n: 30 }) }}</option>
        </select>
      </div>
      <div class="banner"><Icon name="about" /><span>{{ t('Only your own programs are recorded, and only those started by this application or kept in its folder. Open the application after pressing Start.') }}</span></div>
    </template>

    <template v-else-if="step === 'observing'">
      <div class="row"><span class="spinner" /><strong>{{ t('Watching… open {name} and use it normally.', { name: entry.name }) }}</strong><span class="faint right">{{ formatDuration(remaining) }}</span></div>
      <p class="muted">{{ t('Start a game, sign in, let it update — whatever the person who will use it needs. Press Finish when you are done.') }}</p>
      <div v-if="report && items.length === 0" class="empty"><Icon name="flask" /><span>{{ t('Nothing seen yet.') }}</span></div>
      <div v-else class="table-wrap">
        <div v-for="item in report?.items ?? []" :key="key(item)" class="observed" style="grid-template-columns: minmax(0, 1fr) auto">
          <div style="min-width: 0"><strong>{{ item.name }}</strong><div class="hint mono truncate">{{ item.path }}</div></div>
          <span class="mini">{{ roleLabel(item.role) }}</span>
        </div>
      </div>
    </template>

    <template v-else>
      <div class="field"><label>{{ t('Name') }}</label><input v-model="name" class="input" maxlength="80" /></div>
      <p v-if="!items.length" class="muted">{{ t('No other programs were seen. {name} will be allowed on its own.', { name: entry.name }) }}</p>
      <template v-for="group in groups" :key="group.id">
        <div v-if="group.items.length" class="stack" style="gap: 6px">
          <div><h3>{{ group.title }} <span class="faint">({{ group.items.length }})</span></h3><span class="hint">{{ group.hint }}</span></div>
          <div class="table-wrap">
            <label v-for="item in group.items" :key="key(item)" class="observed" :style="group.id === 'windows' ? 'opacity: 0.6' : ''">
              <input type="checkbox" :checked="picked.has(key(item))" :disabled="group.id === 'windows'" @change="toggle(item)" />
              <div style="min-width: 0">
                <strong>{{ item.name }}</strong> <span class="mini">{{ roleLabel(item.role) }}</span>
                <div class="hint mono truncate" :title="item.path">{{ item.path }}</div>
                <div v-if="item.publisher" class="hint truncate">{{ item.publisher }}</div>
                <div class="row wrap" style="gap: 6px; margin-top: 4px"><span v-for="risk in item.risks" :key="risk" class="risk">{{ riskLabel(risk) }}</span></div>
              </div>
              <span class="faint">×{{ item.runs }}</span>
            </label>
          </div>
        </div>
      </template>
      <div class="field">
        <label>{{ t('What the application may start') }}</label>
        <select v-model="mode" class="select" style="max-width: 320px">
          <option value="approved">{{ t('Approved dependencies') }}</option>
          <option value="related">{{ t('Related programs (same signer or folder)') }}</option>
          <option value="selected_only">{{ t('Only this program') }}</option>
        </select>
      </div>
      <label class="check"><input v-model="keep" type="checkbox" />{{ t('Keep this set in the library so other profiles can reuse it') }}</label>
      <label v-if="needsAcknowledge" class="check"><input v-model="acknowledge" type="checkbox" />{{ t('I understand that approved tools can run other programs.') }}</label>
    </template>

    <template #footer>
      <button class="btn" @click="cancel">{{ t('Cancel') }}</button>
      <button v-if="step === 'start'" class="btn primary" :disabled="busy" @click="start"><Icon name="play" />{{ t('Start watching') }}</button>
      <button v-else-if="step === 'observing'" class="btn primary" :disabled="busy" @click="finish"><Icon name="check" />{{ t('Finish') }}</button>
      <button v-else class="btn primary" :disabled="busy || (needsAcknowledge && !acknowledge)" @click="save"><span v-if="busy" class="spinner" /><Icon v-else name="check" />{{ t('Save application') }}</button>
    </template>
  </Modal>
</template>
