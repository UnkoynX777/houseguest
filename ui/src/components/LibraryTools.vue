<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';

import { call, host } from '../api';
import { kindLabel, sourceLabel } from '../explain';
import { t } from '../i18n';
import { attempt, formatTime, notify } from '../store';
import type { Diagnosis, LibraryApp, SourcesReport } from '../types';
import AppIcon from './AppIcon.vue';
import Icon from './Icon.vue';
import Modal from './Modal.vue';

type Tab = 'sources' | 'added' | 'folder' | 'diagnose';

const props = defineProps<{ start?: Tab }>();
const emit = defineEmits<{ close: []; changed: [] }>();

const tab = ref<Tab>(props.start ?? 'sources');
const report = ref<SourcesReport>();
const failure = ref('');
const busy = ref(false);
const folder = ref('');
const candidates = ref<LibraryApp[]>([]);
const picked = ref(new Set<string>());
const scanned = ref(false);
const path = ref('');
const diagnosis = ref<Diagnosis>();

async function deepScan() {
  if (await attempt(() => call('apps.refresh', { deep: true }), t('Deep scan started. It runs in the background and can be cancelled.'))) {
    emit('changed');
    await loadSources();
  }
}

async function setRemovable(value: boolean) {
  if (await attempt(() => call('apps.settings', { include_removable: value }))) {
    emit('changed');
    await loadSources();
  }
}

async function loadSources() {
  try {
    report.value = await call<SourcesReport>('apps.sources');
    failure.value = '';
  } catch (error) {
    failure.value = String((error as Error).message ?? error);
  }
}
onMounted(loadSources);

async function choose(kind: 'folder' | 'executable') {
  const result = await attempt(() => host<{ chosen: boolean; paths?: string[] }>('pick_paths', { kind }));
  return result?.chosen ? (result.paths ?? []) : [];
}

async function addPrograms() {
  const paths = await choose('executable');
  if (!paths.length) return;
  busy.value = true;
  const result = await attempt(() => call<{ added: number }>('apps.manual.add', { paths }));
  busy.value = false;
  if (result) {
    notify('ok', t('{n} programs added.', { n: result.added }));
    emit('changed');
    await loadSources();
  }
}

async function browseFolder() {
  const [chosen] = await choose('folder');
  if (!chosen) return;
  folder.value = chosen;
  await scanFolder();
}

async function scanFolder() {
  if (!folder.value.trim()) return;
  busy.value = true;
  scanned.value = false;
  const result = await attempt(() => call<{ apps: LibraryApp[] }>('apps.folder.scan', { path: folder.value.trim() }));
  busy.value = false;
  candidates.value = result?.apps ?? [];
  picked.value = new Set(candidates.value.map((app) => app.path));
  scanned.value = !!result;
}

function toggle(path: string) {
  const next = new Set(picked.value);
  if (!next.delete(path)) next.add(path);
  picked.value = next;
}

async function addPicked() {
  const paths = [...picked.value];
  if (!paths.length) return;
  busy.value = true;
  const result = await attempt(() => call<{ added: number }>('apps.manual.add', { paths }));
  busy.value = false;
  if (result) {
    notify('ok', t('{n} programs added.', { n: result.added }));
    emit('changed');
    candidates.value = [];
    scanned.value = false;
    await loadSources();
    tab.value = 'added';
  }
}

async function registerFolder() {
  if (!folder.value.trim()) return;
  const result = await attempt(() => call<{ added: boolean }>('apps.portable.add', { path: folder.value.trim() }));
  if (result) {
    notify('ok', result.added ? t('Folder registered. Its programs appear as portable.') : t('That folder is already registered.'));
    emit('changed');
    await loadSources();
  }
}

async function addRoot() {
  const [chosen] = await choose('folder');
  if (!chosen) return;
  const result = await attempt(() => call<{ added: boolean }>('apps.portable.add', { path: chosen }), t('Folder registered. Its programs appear as portable.'));
  if (result) {
    emit('changed');
    await loadSources();
  }
}

async function removeRoot(root: string) {
  if (await attempt(() => call('apps.portable.remove', { path: root }), t('Folder removed.'))) {
    emit('changed');
    await loadSources();
  }
}

async function removeManual(entry: string) {
  if (await attempt(() => call('apps.manual.remove', { path: entry }), t('Removed from the library.'))) {
    emit('changed');
    await loadSources();
  }
}

async function relocate(entry: string) {
  const [chosen] = await choose('executable');
  if (!chosen) return;
  if (await attempt(() => call('apps.manual.relocate', { path: entry, new_path: chosen }), t('Path updated.'))) {
    emit('changed');
    await loadSources();
  }
}

async function browseDiagnosis() {
  const [chosen] = await choose('executable');
  if (!chosen) return;
  path.value = chosen;
  await diagnose();
}

async function diagnose() {
  if (!path.value.trim()) return;
  busy.value = true;
  diagnosis.value = await attempt(() => call<Diagnosis>('apps.diagnose', { path: path.value.trim() }));
  busy.value = false;
}

async function addDiagnosed() {
  const result = await attempt(() => call<{ added: number }>('apps.manual.add', { path: path.value.trim() }), t('Program added.'));
  if (result) {
    emit('changed');
    await diagnose();
  }
}

const volumeType = (type: string) =>
  ({ fixed: t('Fixed disk'), removable: t('Removable'), network: t('Network'), optical: t('Optical'), ramdisk: t('RAM disk'), unknown: t('Unknown') } as Record<string, string>)[type] ?? type;

const canAdd = computed(() => !!diagnosis.value && ['not_detected', 'discarded', 'component', 'volume_skipped'].includes(diagnosis.value.verdict) && diagnosis.value.verdict !== 'volume_skipped');
</script>

<template>
  <Modal :title="t('Library tools')" wide @close="emit('close')">
    <div class="tabs" role="tablist">
      <button v-for="[id, label] in ([['sources', t('Sources')], ['added', t('Added by hand')], ['folder', t('Add by folder')], ['diagnose', t('Missing program')]] as const)" :key="id" class="tab" role="tab" :aria-selected="tab === id" @click="tab = id">{{ label }}</button>
    </div>

    <template v-if="tab === 'sources'">
      <p class="muted">{{ t('Houseguest combines independent sources. One that is unavailable on this Windows is skipped without affecting the others, and what each one contributed is shown below.') }}</p>
      <div v-if="failure" class="banner bad" role="alert"><Icon name="alert" /><span>{{ failure }}</span></div>
      <div v-else-if="!report" class="stack"><div v-for="n in 5" :key="n" class="skeleton" style="height: 36px" /></div>
      <template v-else>
        <div class="counters">
          <span><strong>{{ report.total }}</strong> {{ t('in the library') }}</span>
          <span><strong>{{ report.resolved }}</strong> {{ t('with a program') }}</span>
          <span v-if="report.candidates"><strong>{{ report.candidates }}</strong> {{ t('need a choice') }}</span>
          <span v-if="report.unresolved"><strong>{{ report.unresolved }}</strong> {{ t('without a program') }}</span>
          <span v-if="report.offline"><strong>{{ report.offline }}</strong> {{ t('on offline drives') }}</span>
          <span v-if="report.discarded"><strong>{{ report.discarded }}</strong> {{ t('set aside') }}</span>
        </div>
        <h3>{{ t('Sources') }}</h3>
        <div class="source-list">
          <div v-for="source in report.sources" :key="source.name" class="source-row">
            <span class="state-dot" :class="source.available ? 'allowed' : 'blocked'" />
            <div class="grow" style="min-width: 0">
              <strong>{{ sourceLabel(source.name) }}</strong>
              <div class="hint">{{ source.available ? source.note : source.error }}</div>
            </div>
            <span v-if="source.scanned && source.available" class="hint" style="text-align: right">{{ t('{n} found · {unique} only here · {merged} merged · {ms} ms', { n: source.found, unique: source.unique, merged: source.merged, ms: source.milliseconds }) }}</span>
          </div>
        </div>
        <h3>{{ t('Drives') }}</h3>
        <div class="source-list">
          <div v-for="volume in report.volumes" :key="volume.root" class="source-row">
            <span class="state-dot" :class="volume.included && volume.ready ? 'allowed' : 'blocked'" />
            <div class="grow"><strong>{{ volume.root }} {{ volume.label }}</strong><div class="hint">{{ volumeType(volume.type) }}<template v-if="volume.filesystem"> · {{ volume.filesystem }}</template><template v-if="volume.reason"> · {{ volume.reason }}</template></div></div>
          </div>
        </div>
        <label class="check"><input type="checkbox" :checked="report.include_removable" @change="setRemovable(($event.target as HTMLInputElement).checked)" />{{ t('Include removable drives') }}</label>
        <p class="hint">{{ t('Network locations are never opened, because the service runs with high privileges.') }}</p>
        <div class="row wrap">
          <button class="btn" :disabled="report.scanning" @click="deepScan"><Icon name="search" />{{ t('Deep scan drives') }}</button>
          <span class="hint">{{ t('Looks through the fixed drives for programs that have no installer or registration. It can take a few minutes and never runs on its own.') }}</span>
        </div>
        <p class="hint">{{ t('Last refreshed {time}.', { time: formatTime(report.updated_at) }) }}<template v-if="report.scanning">{{ ' ' }}{{ report.deep ? t('Deep scan running…') : t('Refreshing now…') }}</template></p>
      </template>
    </template>

    <template v-else-if="tab === 'added'">
      <div class="row wrap">
        <button class="btn" :disabled="busy" @click="addPrograms"><Icon name="plus" />{{ t('Add application') }}</button>
        <button class="btn" @click="addRoot"><Icon name="plus" />{{ t('Register portable folder') }}</button>
      </div>
      <h3>{{ t('Portable folders') }}</h3>
      <p v-if="!report?.portable_roots.length" class="hint">{{ t('No portable folders yet. Registered folders are searched when the library refreshes, never continuously.') }}</p>
      <div v-for="root in report?.portable_roots ?? []" :key="root" class="source-row">
        <Icon name="apps" /><span class="grow mono truncate" :title="root">{{ root }}</span>
        <button class="btn sm icon ghost" :aria-label="t('Remove')" @click="removeRoot(root)"><Icon name="trash" :size="15" /></button>
      </div>
      <h3>{{ t('Programs added by hand') }}</h3>
      <p v-if="!report?.manual.length" class="hint">{{ t('Nothing was added by hand.') }}</p>
      <div v-for="entry in report?.manual ?? []" :key="entry.path" class="source-row">
        <AppIcon :path="entry.path" :name="entry.name" :size="28" />
        <div class="grow" style="min-width: 0"><strong class="truncate" style="display: block">{{ entry.name }}</strong><span class="hint mono truncate" style="display: block" :title="entry.path">{{ entry.path }}</span></div>
        <span v-if="entry.missing" class="badge warn">{{ t('File not found') }}</span>
        <button v-if="entry.missing" class="btn sm" @click="relocate(entry.path)">{{ t('Locate') }}</button>
        <button class="btn sm icon ghost" :aria-label="t('Remove')" @click="removeManual(entry.path)"><Icon name="trash" :size="15" /></button>
      </div>
    </template>

    <template v-else-if="tab === 'folder'">
      <p class="muted">{{ t('Pick a folder and Houseguest looks for programs inside it, without running anything.') }}</p>
      <div class="row">
        <input v-model="folder" class="input grow" :placeholder="t('Folder path')" :aria-label="t('Folder path')" @keyup.enter="scanFolder" />
        <button class="btn" @click="browseFolder"><Icon name="apps" />{{ t('Browse') }}</button>
        <button class="btn primary" :disabled="busy || !folder.trim()" @click="scanFolder"><span v-if="busy" class="spinner" /><Icon v-else name="search" />{{ t('Search') }}</button>
      </div>
      <p v-if="scanned && !candidates.length" class="muted">{{ t('No programs were found in that folder.') }}</p>
      <div v-if="candidates.length" class="source-list">
        <label v-for="app in candidates" :key="app.path" class="source-row" style="cursor: pointer">
          <input type="checkbox" :checked="picked.has(app.path)" @change="toggle(app.path)" />
          <AppIcon :path="app.path" :name="app.name" :size="28" />
          <div class="grow" style="min-width: 0"><strong class="truncate" style="display: block">{{ app.name }}</strong><span class="hint mono truncate" style="display: block" :title="app.path">{{ app.path }}</span></div>
          <span class="mini">{{ kindLabel(app.kind) }}</span>
        </label>
      </div>
      <div v-if="folder.trim()" class="row wrap">
        <button v-if="candidates.length" class="btn primary" :disabled="busy || !picked.size" @click="addPicked"><Icon name="check" />{{ t('Add {n} selected', { n: picked.size }) }}</button>
        <button class="btn" @click="registerFolder"><Icon name="plus" />{{ t('Keep watching this folder') }}</button>
      </div>
    </template>

    <template v-else>
      <p class="muted">{{ t('Enter the path of a program that does not appear and Houseguest explains why.') }}</p>
      <div class="row">
        <input v-model="path" class="input grow mono" :placeholder="t('C:\\Program Files\\App\\app.exe')" :aria-label="t('Program path')" @keyup.enter="diagnose" />
        <button class="btn" @click="browseDiagnosis"><Icon name="apps" />{{ t('Browse') }}</button>
        <button class="btn primary" :disabled="busy || !path.trim()" @click="diagnose"><span v-if="busy" class="spinner" /><Icon v-else name="search" />{{ t('Check') }}</button>
      </div>
      <div v-if="diagnosis" class="diagnosis" :class="diagnosis.verdict === 'listed' ? 'allowed' : 'blocked'">
        <ul class="checks">
          <li v-for="check in diagnosis.checks" :key="check.step + check.detail"><Icon :name="check.ok ? 'check' : 'alert'" :size="15" :class="check.ok ? 'ok-icon' : 'bad-icon'" /><span>{{ check.detail }}</span></li>
        </ul>
        <p v-if="diagnosis.advice" style="margin-top: 8px"><strong>{{ diagnosis.advice }}</strong></p>
        <div v-if="diagnosis.app" class="row" style="margin-top: 8px"><AppIcon :path="diagnosis.app.path" :name="diagnosis.app.name" :size="28" /><span>{{ diagnosis.app.name }} · {{ (diagnosis.app.sources ?? []).map(sourceLabel).join(', ') }}</span></div>
        <button v-if="canAdd" class="btn primary" style="margin-top: 10px" :disabled="busy" @click="addDiagnosed"><Icon name="plus" />{{ t('Add this program') }}</button>
      </div>
    </template>
  </Modal>
</template>
