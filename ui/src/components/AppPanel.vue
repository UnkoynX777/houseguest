<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue';

import { ApiError, call } from '../api';
import { describeBlock, describeDecision, describeNote, recommendation, roleLabel, sourceLabel } from '../explain';
import { t } from '../i18n';
import { app, attempt, describe, formatTime, loadProfiles, notify, profileLabel, randomId } from '../store';
import type { Application, BlockEntry, Component, ComponentView, DecisionInfo, DependencyMode, Explanation, LibraryApp, PendingReview } from '../types';
import AppIcon from './AppIcon.vue';
import Icon from './Icon.vue';
import Modal from './Modal.vue';

const props = defineProps<{ entry: LibraryApp; profileId: string }>();
const emit = defineEmits<{ close: []; changed: []; observe: [] }>();

type Tab = 'overview' | 'dependencies' | 'blocks' | 'advanced' | 'simulation';
const tab = ref<Tab>('overview');
const explanation = ref<Explanation>();
const loading = ref(true);
const busy = ref(false);
const testPath = ref('');
const testResult = ref<{ decision: DecisionInfo; path: string }>();
const confirm = ref<{ title: string; run: () => Promise<void> }>();

const profile = computed(() => app.profiles.find((p) => p.id === props.profileId));
const application = computed<Application | undefined>(() => {
  const id = explanation.value?.application?.id ?? props.entry.listed;
  return profile.value?.apps?.find((a) => a.id === id);
});
const allowed = computed(() => explanation.value?.decision.action === 'allow');
const summary = computed(() => explanation.value?.summary);
const tabs = computed<{ id: Tab; label: string; icon: string }[]>(() => [
  { id: 'overview', label: t('Overview'), icon: 'about' },
  { id: 'dependencies', label: t('Dependencies'), icon: 'link' },
  { id: 'blocks', label: t('Blocks'), icon: 'history' },
  { id: 'advanced', label: t('Advanced'), icon: 'rules' },
  { id: 'simulation', label: t('Simulation'), icon: 'flask' },
]);

async function reload() {
  loading.value = true;
  await loadProfiles();
  explanation.value = await attempt(() => call<Explanation>('apps.explain', { profile_id: props.profileId, path: props.entry.path, package_family: props.entry.package_family }));
  loading.value = false;
}

onMounted(() => {
  reload();
  window.addEventListener('keydown', onKey);
});
onUnmounted(() => window.removeEventListener('keydown', onKey));
watch(() => [props.entry.path, props.profileId], reload);

function onKey(event: KeyboardEvent) {
  if (event.key === 'Escape' && !confirm.value) emit('close');
}

async function persist(next: Application, message = t('Saved.')) {
  busy.value = true;
  const saved = await attempt(() => call('apps.save', { profile_id: props.profileId, app: next }), message);
  busy.value = false;
  if (saved) {
    await reload();
    emit('changed');
  }
}

async function allowHere() {
  busy.value = true;
  const info = await attempt(() => call<{ sha256: string; publisher: string }>('apps.inspect', { path: props.entry.path }));
  if (info) {
    const main: Component = {
      id: randomId('c-'), label: props.entry.name, role: 'main', scope: 'independent', path: props.entry.path,
      sha256: info.sha256, publisher: info.publisher, package_family: props.entry.package_family, follow_updates: !!info.publisher,
    };
    await persist({ id: randomId('app-'), name: props.entry.name, mode: 'approved', unknown: 'review', priority: 50, components: [main] }, t('{name} is now allowed.', { name: props.entry.name }));
  }
  busy.value = false;
}

async function removeFromProfile() {
  if (!application.value) return;
  busy.value = true;
  const done = await attempt(() => call('apps.remove', { profile_id: props.profileId, app_id: application.value!.id }), t('{name} was removed from this profile.', { name: props.entry.name }));
  busy.value = false;
  if (done) {
    await reload();
    emit('changed');
  }
}

const setMode = (mode: DependencyMode) => application.value && persist({ ...application.value, mode });
const setUnknown = (unknown: 'deny' | 'review') => application.value && persist({ ...application.value, unknown });
const setPriority = (value: number) => application.value && persist({ ...application.value, priority: Math.max(-1000, Math.min(1000, Math.round(value) || 0)) });

function updateComponent(component: Component, change: Partial<Component>) {
  if (!application.value) return;
  const components = application.value.components.map((c) => (c.id === component.id ? { ...c, ...change } : c));
  return persist({ ...application.value, components });
}

function removeComponent(component: Component) {
  if (!application.value) return;
  return persist({ ...application.value, components: application.value.components.filter((c) => c.id !== component.id) }, t('Dependency removed.'));
}

async function reauthorize(component: ComponentView) {
  busy.value = true;
  const done = await attempt(() => call('apps.reauthorize', { profile_id: props.profileId, app_id: application.value?.id, component_id: component.id, expect_sha256: component.on_disk?.sha256 }), t('The new file is approved.'));
  busy.value = false;
  if (done) {
    await reload();
    emit('changed');
  }
}

async function ask(action: () => Promise<unknown>, retry: () => Promise<void>, success: string) {
  busy.value = true;
  try {
    await action();
    notify('ok', success);
    await reload();
    emit('changed');
  } catch (error) {
    if (error instanceof ApiError && error.code === 'confirmation_required') askSensitive(retry);
    else notify('bad', describe(error));
  } finally {
    busy.value = false;
  }
}

function approve(review: PendingReview, minutes: number, acknowledge = false): Promise<void> {
  const args: Record<string, unknown> = { id: review.id, minutes, acknowledge_sensitive: acknowledge };
  if (review.sha256) args.expect_sha256 = review.sha256;
  return ask(() => call('reviews.approve', args), () => approve(review, minutes, true), t('Approved.'));
}

function approveBlock(block: BlockEntry, minutes: number, acknowledge = false): Promise<void> {
  if (!block.related_app) return Promise.resolve();
  const args = { profile_id: props.profileId, app_id: block.related_app, path: block.path, minutes, acknowledge_sensitive: acknowledge };
  return ask(() => call('apps.component.add', args), () => approveBlock(block, minutes, true), t('Approved.'));
}

async function dismiss(review: PendingReview) {
  await attempt(() => call('reviews.dismiss', { id: review.id }));
  await reload();
  emit('changed');
}

function askSensitive(run: () => Promise<void>) {
  confirm.value = { title: t('This program can run other programs'), run };
}

async function test() {
  if (!testPath.value.trim()) return;
  const result = await attempt(() => call<Explanation>('apps.explain', { profile_id: props.profileId, path: testPath.value.trim() }));
  if (result) testResult.value = { decision: result.decision, path: result.path };
}

const pin = (c: Component) => (c.sha256 ? t('Exact file') : c.publisher ? t('Signer') : c.package_family ? t('Package') : t('Path only'));
const stateBadge = (state: ComponentView['state']) =>
  ({ approved: ['ok', t('Approved')], updated: ['info', t('Updated by publisher')], changed: ['bad', t('Changed — needs review')], missing: ['warn', t('Not found')], unpinned: ['warn', t('Not pinned')], expired: ['warn', t('Expired')], pattern: ['info', t('Folder rule')], unknown: ['info', '–'] } as const)[state];
const fileName = (path: string) => path.split(/[\\/]/).pop() ?? path;
const expiry = (c: Component) => (c.expires_at ? t('Until {time}', { time: formatTime(c.expires_at) }) : '');
const modes = computed<{ id: DependencyMode; label: string; hint: string }[]>(() => [
  { id: 'selected_only', label: t('Only this program'), hint: t('Nothing it starts is allowed unless a rule allows it.') },
  { id: 'approved', label: t('Approved dependencies'), hint: t('It may start the programs listed under Dependencies, and only those.') },
  { id: 'related', label: t('Related programs'), hint: t('Also allows unlisted programs it starts when they share its signer or live in its folder. Never tools that run other programs.') },
]);
const modeHint = computed(() => modes.value.find((m) => m.id === application.value?.mode)?.hint ?? '');
</script>

<template>
  <div class="drawer-scrim" @mousedown="emit('close')" />
  <aside class="drawer" role="dialog" aria-modal="true" :aria-label="entry.name">
    <div class="drawer-head">
      <AppIcon :path="entry.path" :name="entry.name" :size="52" />
      <div class="grow">
        <h2 class="truncate">{{ entry.name }}</h2>
        <div class="muted truncate">{{ entry.company || t('Unknown publisher') }}<template v-if="entry.version"> · {{ entry.version }}</template></div>
      </div>
      <span v-if="explanation" class="badge" :class="allowed ? 'ok' : 'bad'"><span class="dot" />{{ allowed ? t('Allowed') : t('Blocked') }}</span>
      <button class="btn icon ghost" :aria-label="t('Close')" @click="emit('close')"><Icon name="x" /></button>
    </div>

    <div class="tabs" role="tablist">
      <button v-for="item in tabs" :key="item.id" class="tab" role="tab" :aria-selected="tab === item.id" @click="tab = item.id">
        <Icon :name="item.icon" :size="15" />{{ item.label }}
        <span v-if="item.id === 'dependencies' && summary?.pending" class="mini warn">{{ summary.pending }}</span>
      </button>
    </div>

    <div v-if="loading || !explanation" class="drawer-body"><div v-for="n in 4" :key="n" class="skeleton" style="height: 56px" /></div>

    <div v-else class="drawer-body">
      <template v-if="tab === 'overview'">
        <div class="diagnosis" :class="allowed ? 'allowed' : 'blocked'">
          <dl class="kv">
            <dt>{{ t('Application') }}</dt><dd><strong>{{ entry.name }}</strong></dd>
            <dt>{{ t('Decision') }}</dt><dd>{{ allowed ? t('Allowed') : t('Blocked') }}</dd>
            <dt>{{ t('Reason') }}</dt><dd>{{ describeDecision(explanation.decision, explanation.application?.name ?? entry.name) }}<template v-if="describeNote(explanation.decision.note)"> {{ describeNote(explanation.decision.note) }}</template></dd>
            <template v-if="application">
              <dt>{{ t('Profile') }}</dt><dd>{{ profileLabel(profile) }}</dd>
              <dt>{{ t('Dependencies') }}</dt>
              <dd>
                {{ t('{n} approved', { n: summary?.approved ?? 0 }) }}<template v-if="summary?.pending">, {{ t('{n} pending', { n: summary.pending }) }}</template><template v-if="summary?.changed">, {{ t('{n} changed', { n: summary.changed }) }}</template><template v-if="summary?.missing">, {{ t('{n} not found', { n: summary.missing }) }}</template>
              </dd>
            </template>
          </dl>
        </div>

        <div v-if="application && explanation.findings.length" class="banner warn">
          <Icon name="alert" />
          <div class="stack" style="gap: 4px"><span v-for="finding in explanation.findings" :key="finding.code + finding.message">{{ finding.message }}</span></div>
        </div>

        <div class="row">
          <button v-if="!application" class="btn primary" :disabled="busy" @click="allowHere"><Icon name="check" />{{ t('Allow in {profile}', { profile: profileLabel(profile) }) }}</button>
          <template v-else>
            <button class="btn" :disabled="busy" @click="tab = 'dependencies'"><Icon name="link" />{{ t('Manage dependencies') }}</button>
            <button class="btn" @click="emit('observe')"><Icon name="flask" />{{ t('Observe a run') }}</button>
            <button class="btn danger right" :disabled="busy" @click="removeFromProfile"><Icon name="trash" />{{ t('Remove') }}</button>
          </template>
        </div>

        <dl class="kv">
          <dt>{{ t('Executable') }}</dt><dd class="mono">{{ entry.path }}</dd>
          <dt>{{ t('Source') }}</dt><dd>{{ sourceLabel(entry.source) }}</dd>
          <dt v-if="explanation.publisher">{{ t('Signed by') }}</dt><dd v-if="explanation.publisher">{{ explanation.publisher }}</dd>
          <dt v-if="explanation.sha256">SHA-256</dt><dd v-if="explanation.sha256" class="mono">{{ explanation.sha256 }}</dd>
          <dt v-if="entry.package_family">{{ t('Package') }}</dt><dd v-if="entry.package_family" class="mono">{{ entry.package_family }}</dd>
        </dl>
        <p v-if="!application" class="hint">{{ t('Allowing a program pins its exact file and signer. Windows and Houseguest components are always protected and do not need to be listed.') }}</p>
      </template>

      <template v-else-if="tab === 'dependencies'">
        <p v-if="!application" class="muted">{{ t('Allow this program first, then approve what it needs.') }}</p>
        <template v-else>
          <div class="field">
            <label>{{ t('What the program may start') }}</label>
            <div class="seg" role="group">
              <button v-for="mode in modes" :key="mode.id" :aria-pressed="application.mode === mode.id" :disabled="busy" @click="setMode(mode.id)">{{ mode.label }}</button>
            </div>
            <span class="hint">{{ modeHint }}</span>
          </div>
          <div class="field">
            <label>{{ t('Unknown programs it starts') }}</label>
            <div class="seg" role="group">
              <button :aria-pressed="application.unknown === 'review'" :disabled="busy" @click="setUnknown('review')">{{ t('Block and ask me') }}</button>
              <button :aria-pressed="application.unknown === 'deny'" :disabled="busy" @click="setUnknown('deny')">{{ t('Block quietly') }}</button>
            </div>
          </div>

          <div v-if="explanation.pending.length" class="stack">
            <h3>{{ t('Waiting for your decision') }}</h3>
            <div v-for="review in explanation.pending" :key="review.id" class="component">
              <div class="grow">
                <strong>{{ fileName(review.path) }}</strong>
                <div class="hint mono truncate">{{ review.path }}</div>
              </div>
              <div class="actions">
                <button class="btn sm primary" :disabled="busy" @click="approve(review, 0)">{{ t('Approve') }}</button>
                <button class="btn sm" :disabled="busy" @click="approve(review, 60)">{{ t('1 hour') }}</button>
                <button class="btn sm ghost" :disabled="busy" @click="dismiss(review)">{{ t('Dismiss') }}</button>
              </div>
              <div class="meta">
                <span class="mini bad">{{ t('Blocked {n} times', { n: review.count }) }}</span>
                <span v-if="review.kind === 'component_changed'" class="mini warn">{{ t('File changed') }}</span>
                <span v-if="review.publisher" class="mini">{{ review.publisher }}</span>
                <span v-else class="mini warn">{{ t('Unsigned') }}</span>
                <span v-if="review.parent" class="mini">{{ t('Started by {name}', { name: fileName(review.parent) }) }}</span>
              </div>
            </div>
          </div>

          <h3>{{ t('Known components') }}</h3>
          <div v-for="component in explanation.components" :key="component.id" class="component">
            <div class="grow" style="min-width: 0">
              <strong>{{ component.label || fileName(component.path) }}</strong>
              <div class="hint mono truncate" :title="component.path">{{ component.path || component.package_family }}</div>
            </div>
            <div class="actions">
              <button v-if="component.state === 'changed'" class="btn sm primary" :disabled="busy" @click="reauthorize(component)">{{ t('Approve new file') }}</button>
              <button v-if="component.role !== 'main'" class="btn sm icon ghost" :aria-label="t('Remove')" :disabled="busy" @click="removeComponent(component)"><Icon name="trash" :size="15" /></button>
            </div>
            <div class="meta">
              <span class="badge" :class="stateBadge(component.state)[0]">{{ stateBadge(component.state)[1] }}</span>
              <span class="mini">{{ roleLabel(component.role) }}</span>
              <span class="mini">{{ component.role === 'main' ? t('Starts on its own') : component.scope === 'descendant' ? t('Only below the application') : t('Can be started by Windows') }}</span>
              <span class="mini">{{ pin(component) }}</span>
              <span v-if="component.expires_at" class="mini warn">{{ expiry(component) }}</span>
            </div>
          </div>
          <button class="btn" @click="emit('observe')"><Icon name="flask" />{{ t('Find dependencies by observing a run') }}</button>
        </template>
      </template>

      <template v-else-if="tab === 'blocks'">
        <p v-if="!explanation.history.length" class="muted">{{ t('Nothing related to this program was blocked recently.') }}</p>
        <div v-for="block in explanation.history" :key="block.ts + block.path" class="component">
          <div class="grow" style="min-width: 0">
            <strong>{{ block.process || fileName(block.path) }}</strong>
            <div class="hint">{{ formatTime(block.ts) }} · {{ block.profile }}</div>
          </div>
          <div class="actions">
            <button v-if="block.related_app && application" class="btn sm" :disabled="busy" @click="approveBlock(block, 0)">{{ t('Review and approve') }}</button>
          </div>
          <div class="meta"><span class="mini bad">{{ describeBlock(block) }}</span></div>
          <div v-if="block.parent" class="hint" style="grid-column: 1 / -1">{{ t('Started by {name}', { name: fileName(block.parent) }) }}</div>
          <div v-if="recommendation(block)" class="hint" style="grid-column: 1 / -1"><Icon name="about" :size="13" /> {{ recommendation(block) }}</div>
        </div>
      </template>

      <template v-else-if="tab === 'advanced'">
        <p v-if="!application" class="muted">{{ t('Allow this program first to tune how it is treated.') }}</p>
        <template v-else>
          <div class="field">
            <label>{{ t('Priority') }}</label>
            <input class="input" type="number" min="-1000" max="1000" :value="application.priority" style="max-width: 140px" @change="setPriority(Number(($event.target as HTMLInputElement).value))" />
            <span class="hint">{{ t('A rule with a higher priority that blocks this program wins. Protected Windows components are not affected.') }}</span>
          </div>
          <h3>{{ t('Per component') }}</h3>
          <div v-for="component in application.components" :key="component.id" class="component">
            <div class="grow" style="min-width: 0"><strong>{{ component.label || fileName(component.path) }}</strong><div class="hint mono truncate">{{ component.path }}</div></div>
            <div class="meta" style="grid-column: 1 / -1">
              <label v-if="component.publisher" class="check" style="padding: 0"><input type="checkbox" :checked="component.follow_updates" :disabled="busy" @change="updateComponent(component, { follow_updates: ($event.target as HTMLInputElement).checked })" />{{ t('Accept updates signed by the same publisher') }}</label>
              <label v-if="component.role !== 'main'" class="check" style="padding: 0"><input type="checkbox" :checked="component.scope === 'independent'" :disabled="busy || (!component.sha256 && !component.publisher)" @change="updateComponent(component, { scope: ($event.target as HTMLInputElement).checked ? 'independent' : 'descendant' })" />{{ t('May also be started by Windows or a broker') }}</label>
              <select v-if="component.role !== 'main'" class="select" style="width: auto; height: 30px" :disabled="busy" :value="component.expires_at ? 'timed' : 'always'" @change="($event.target as HTMLSelectElement).value === 'always' ? updateComponent(component, { expires_at: undefined }) : updateComponent(component, { expires_at: Math.floor(Date.now() / 1000) + 86400 })">
                <option value="always">{{ t('Approved until removed') }}</option>
                <option value="timed">{{ t('Approved for 24 hours') }}</option>
              </select>
            </div>
          </div>
        </template>
      </template>

      <template v-else>
        <p class="muted">{{ t('What the current profile does, based on the real files on this computer.') }}</p>
        <div v-if="application" class="stack" style="gap: 8px">
          <h3>{{ t('If it is started by {name}', { name: entry.name }) }}</h3>
          <div v-for="component in explanation.components.filter((c) => c.effective)" :key="component.id" class="row">
            <span class="badge" :class="component.effective?.action === 'allow' ? 'ok' : 'bad'">{{ component.effective?.action === 'allow' ? t('Allowed') : t('Blocked') }}</span>
            <div class="grow" style="min-width: 0"><strong>{{ component.label || fileName(component.path) }}</strong><div class="hint">{{ component.effective ? describeDecision(component.effective, entry.name) : '' }}</div></div>
          </div>
        </div>
        <div class="field">
          <label>{{ t('Test any program') }}</label>
          <div class="row">
            <input v-model="testPath" class="input grow mono" :placeholder="'C:\\Games\\Example\\game.exe'" @keydown.enter="test" />
            <button class="btn" :disabled="!testPath.trim()" @click="test">{{ t('Check') }}</button>
          </div>
          <div v-if="testResult" class="diagnosis" :class="testResult.decision.action === 'allow' ? 'allowed' : 'blocked'">
            <strong>{{ testResult.decision.action === 'allow' ? t('Allowed') : t('Blocked') }}</strong> — {{ describeDecision(testResult.decision) }}
            <div class="hint mono">{{ testResult.path }}</div>
          </div>
          <span class="hint">{{ t('Started on its own, outside any application, in this profile.') }}</span>
        </div>
      </template>
    </div>
  </aside>

  <Modal v-if="confirm" :title="confirm.title" @close="confirm = undefined">
    <div class="banner warn"><Icon name="alert" /><span>{{ t('Approving it lets the application start other programs through it. Only continue if the application cannot work without it.') }}</span></div>
    <template #footer>
      <button class="btn" @click="confirm = undefined">{{ t('Cancel') }}</button>
      <button class="btn primary" @click="confirm.run().finally(() => (confirm = undefined))">{{ t('Approve anyway') }}</button>
    </template>
  </Modal>
</template>
