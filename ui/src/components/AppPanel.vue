<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue';

import { ApiError, call } from '../api';
import { describeBlock, describeDecision, describeNote, formatSize, kindLabel, platformLabel, recommendation, roleKindLabel, roleLabel, shortPath, signatureHint, signatureLabel, signatureTone, sourceLabel } from '../explain';
import { t } from '../i18n';
import { app, attempt, describe, formatTime, loadProfiles, notify, profileLabel, randomId } from '../store';
import { signerName } from '../certificate';
import type { Application, BlockEntry, Component, ComponentView, DecisionInfo, DependencyMode, Explanation, FileDetails, LibraryApp, PendingReview } from '../types';
import AppIcon from './AppIcon.vue';
import CopyText from './CopyText.vue';
import Icon from './Icon.vue';
import Modal from './Modal.vue';

const props = defineProps<{ entry: LibraryApp; profileId: string }>();
const emit = defineEmits<{ close: []; changed: []; observe: [] }>();

async function forgetManual() {
  busy.value = true;
  const done = await attempt(() => call('apps.manual.remove', { path: props.entry.path }), t('Removed from the library.'));
  busy.value = false;
  if (done) {
    emit('changed');
    emit('close');
  }
}

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

const details = ref<FileDetails>();
const detailsPhase = ref<'loading' | 'ready' | 'error'>('loading');
const detailsError = ref('');
let detailsToken = 0;

// The signature check and the version resource are read by the service while the rest of the panel is already
// usable, and the answer is remembered there until the file changes.
async function loadDetails() {
  const mine = ++detailsToken;
  detailsPhase.value = 'loading';
  try {
    const reply = await call<FileDetails>('apps.details', { path: props.entry.path });
    if (mine !== detailsToken) return;
    details.value = reply;
    detailsPhase.value = 'ready';
  } catch (error) {
    if (mine !== detailsToken) return;
    detailsError.value = describe(error);
    detailsPhase.value = 'error';
  }
}

const signature = computed(() => details.value?.signature);
const declared = computed(() => details.value?.declared);
const folderOf = (path: string) => path.replace(/[\\/][^\\/]*$/, '');
const dateOf = (seconds?: number) => (seconds ? formatTime(seconds) : '');

async function reload() {
  loading.value = true;
  loadDetails();
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
      <AppIcon :path="entry.path" :sources="entry.icons" :name="entry.name" :size="52" />
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
          <button v-if="entry.manual" class="btn" :class="{ right: !application }" :disabled="busy" @click="forgetManual"><Icon name="trash" />{{ t('Remove from library') }}</button>
        </div>

        <dl class="kv">
          <dt>{{ t('Type') }}</dt><dd>{{ kindLabel(entry.kind) }}<template v-if="entry.scope"> · {{ entry.scope === 'user' ? t('Only for this user') : t('All users') }}</template></dd>
          <dt>{{ t('Version') }}</dt><dd><template v-if="entry.version || declared?.file_version">{{ entry.version || declared?.file_version }}</template><span v-else-if="detailsPhase === 'loading'" class="skeleton" style="display: inline-block; width: 80px; height: 12px" /><span v-else class="absent">{{ t('Not declared') }}</span></dd>
          <dt>{{ t('Location') }}</dt><dd><CopyText :value="entry.path" :display="shortPath(entry.install_dir || folderOf(entry.path))" reveal :label="t('path')" /></dd>
          <dt>{{ t('Signature') }}</dt>
          <dd>
            <div v-if="detailsPhase === 'loading'" class="skeleton" style="height: 18px; width: 70%" />
            <div v-else-if="detailsPhase === 'error'" class="signature-line"><span class="muted">{{ t('The signature could not be read.') }}</span><button class="btn sm" @click="loadDetails">{{ t('Try again') }}</button></div>
            <div v-else-if="signature" class="signature-line">
              <span class="badge" :class="signatureTone(signature.state)" :title="signatureHint(signature.state)"><span class="dot" />{{ signatureLabel(signature.state) }}</span>
              <span v-if="signature.signer && signature.state !== 'unsigned'">{{ t('Signed by') }}: <strong>{{ signature.signer }}</strong></span>
            </div>
          </dd>
        </dl>

        <details class="fold">
          <summary><Icon name="chevron" :size="16" />{{ t('Technical details') }}</summary>
          <div class="fold-body">
            <section>
              <h4>{{ t('File') }}</h4>
              <dl class="kv">
                <dt>{{ t('Executable') }}</dt><dd><CopyText :value="entry.path" mono reveal :label="t('path')" /></dd>
                <dt v-if="entry.install_dir">{{ t('Folder') }}</dt><dd v-if="entry.install_dir"><CopyText :value="entry.install_dir" mono reveal :label="t('path')" /></dd>
                <dt v-if="details?.size !== undefined">{{ t('Size') }}</dt><dd v-if="details?.size !== undefined">{{ formatSize(details.size) }}</dd>
                <dt>SHA-256</dt><dd><CopyText v-if="explanation.sha256" :value="explanation.sha256" mono label="SHA-256" /><span v-else class="absent">{{ t('Could not be calculated') }}</span></dd>
                <dt v-if="entry.package_family">{{ t('Package') }}</dt><dd v-if="entry.package_family"><CopyText :value="entry.package_family" mono :label="t('package')" /></dd>
              </dl>
            </section>

            <section>
              <h4>{{ t('Found by Houseguest') }}</h4>
              <dl class="kv">
                <dt>{{ t('Found by') }}</dt><dd>{{ (entry.sources?.length ? entry.sources : [entry.source]).map(sourceLabel).join(', ') }}</dd>
                <dt>{{ t('Publisher in the installation record') }}</dt><dd><template v-if="entry.company">{{ entry.company }}</template><span v-else class="absent">{{ t('Not recorded') }}</span></dd>
                <dt v-if="entry.platform">{{ t('Platform') }}</dt><dd v-if="entry.platform">{{ platformLabel(entry.platform) }}</dd>
                <dt v-if="entry.role !== 'application'">{{ t('Classified as') }}</dt><dd v-if="entry.role !== 'application'">{{ roleKindLabel(entry.role) }}</dd>
              </dl>
            </section>

            <section>
              <h4>{{ t('Declared by the program') }}</h4>
              <div v-if="detailsPhase === 'loading'" class="skeleton" style="height: 70px" />
              <template v-else-if="declared">
                <dl class="kv">
                  <dt>{{ t('Company') }}</dt><dd><template v-if="declared.company">{{ declared.company }}</template><span v-else class="absent">{{ t('Not declared') }}</span></dd>
                  <dt>{{ t('Product') }}</dt><dd><template v-if="declared.product">{{ declared.product }}</template><span v-else class="absent">{{ t('Not declared') }}</span></dd>
                  <dt>{{ t('Description') }}</dt><dd><template v-if="declared.description">{{ declared.description }}</template><span v-else class="absent">{{ t('Not declared') }}</span></dd>
                  <dt>{{ t('File version') }}</dt><dd><template v-if="declared.file_version">{{ declared.file_version }}</template><span v-else class="absent">{{ t('Not declared') }}</span></dd>
                  <dt v-if="declared.product_version && declared.product_version !== declared.file_version">{{ t('Product version') }}</dt><dd v-if="declared.product_version && declared.product_version !== declared.file_version">{{ declared.product_version }}</dd>
                  <dt v-if="declared.original_name">{{ t('Original file name') }}</dt><dd v-if="declared.original_name">{{ declared.original_name }}</dd>
                  <dt v-if="declared.copyright">{{ t('Copyright') }}</dt><dd v-if="declared.copyright">{{ declared.copyright }}</dd>
                </dl>
                <p class="note">{{ t('These values are written by whoever built the file. They are not checked and do not affect what is allowed.') }}</p>
              </template>
              <p v-else class="absent">{{ t('Not available') }}</p>
            </section>

            <section>
              <h4>{{ t('Digital signature') }}</h4>
              <div v-if="detailsPhase === 'loading'" class="skeleton" style="height: 90px" />
              <p v-else-if="detailsPhase === 'error'" class="absent">{{ detailsError }}</p>
              <template v-else-if="signature">
                <p class="note" style="margin: 0 0 8px">{{ signatureHint(signature.state) }}</p>
                <dl v-if="signature.signer" class="kv">
                  <dt>{{ t('Signer') }}</dt><dd>{{ signature.signer }}</dd>
                  <dt>{{ t('Issued by') }}</dt><dd>{{ signature.issuer.common_name || signature.issuer.organization || t('Unknown') }}</dd>
                  <dt>{{ t('Valid') }}</dt><dd>{{ dateOf(signature.not_before) }} – {{ dateOf(signature.not_after) }}<template v-if="signature.certificate_expired"> · {{ signature.timestamped ? t('expired, but signed with a trusted timestamp') : t('expired') }}</template></dd>
                  <dt>{{ t('Source') }}</dt><dd>{{ signature.via_catalog ? t('Windows security catalog') : t('Signature inside the file') }}<template v-if="signature.timestamped"> · {{ t('timestamped') }}</template></dd>
                  <dt>{{ t('Algorithm') }}</dt><dd>{{ signature.algorithm }}</dd>
                  <dt>{{ t('Serial number') }}</dt><dd><CopyText :value="signature.serial" mono :label="t('serial number')" /></dd>
                  <dt>SHA-1</dt><dd><CopyText :value="signature.thumbprint_sha1" mono :label="t('thumbprint')" /></dd>
                  <dt>SHA-256</dt><dd><CopyText :value="signature.thumbprint_sha256" mono :label="t('thumbprint')" /></dd>
                </dl>
                <details v-if="signature.subject.fields.length" class="fold" style="margin-top: 10px">
                  <summary><Icon name="chevron" :size="16" />{{ t('Certificate subject') }}</summary>
                  <div class="fold-body"><dl class="kv"><template v-for="(field, i) in signature.subject.fields" :key="i"><dt :title="field.oid">{{ field.label }}</dt><dd>{{ field.value }}</dd></template></dl></div>
                </details>
                <details v-if="signature.issuer.fields.length" class="fold" style="margin-top: 8px">
                  <summary><Icon name="chevron" :size="16" />{{ t('Certificate issuer') }}</summary>
                  <div class="fold-body"><dl class="kv"><template v-for="(field, i) in signature.issuer.fields" :key="i"><dt :title="field.oid">{{ field.label }}</dt><dd>{{ field.value }}</dd></template></dl></div>
                </details>
                <p class="note">{{ t('Windows checked the signature and the chain of trust. Revocation lists are not downloaded, so a certificate revoked very recently may still appear valid.') }}</p>
                <dl v-if="explanation.publisher" class="kv" style="margin-top: 10px">
                  <dt>{{ t('Pinned by Houseguest') }}</dt><dd><CopyText :value="explanation.publisher" mono :lines="2" :label="t('signer identity')" /></dd>
                </dl>
              </template>
            </section>
          </div>
        </details>
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
                <span v-if="review.publisher" class="mini" :title="review.publisher">{{ signerName(review.publisher) || t('Signed') }}</span>
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
