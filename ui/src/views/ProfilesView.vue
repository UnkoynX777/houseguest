<script setup lang="ts">
import { onActivated, onDeactivated, ref } from 'vue';

import { call, host } from '../api';
import Icon from '../components/Icon.vue';
import Menu from '../components/Menu.vue';
import Modal from '../components/Modal.vue';
import PageHeader from '../components/PageHeader.vue';
import Skeleton from '../components/Skeleton.vue';
import { t } from '../i18n';
import { app, attempt, ensureProfiles, go, loadProfiles, notify, profileLabel, randomId } from '../store';
import type { Profile } from '../types';

const creating = ref(false);
const name = ref('');
const base = ref('');
const deleting = ref<Profile>();
const renaming = ref<Profile>();
const newName = ref('');

function startRename(profile: Profile) {
  renaming.value = profile;
  newName.value = profileLabel(profile);
}

async function rename() {
  const profile = renaming.value;
  renaming.value = undefined;
  if (!profile || !newName.value.trim()) return;
  await attempt(() => call('profiles.save', { profile: { ...profile, name: newName.value.trim() } }), t('Profile renamed.'));
  await loadProfiles();
}

const waiting = ref(app.profiles.length === 0);
onActivated(async () => {
  await attempt(ensureProfiles);
  waiting.value = false;
});
onDeactivated(() => {
  creating.value = false;
  deleting.value = undefined;
  renaming.value = undefined;
});

const builtin = (profile: Profile) => profile.id.startsWith('builtin-');
const describeBuiltin = (profile: Profile) =>
  ({ 'builtin-guest': t('Only what you pick. Admin tools are blocked.'), 'builtin-gaming': t('For game nights: add your launchers and games.'),
     'builtin-work': t('Everything allowed except admin tools.'), 'builtin-restricted': t('Like Guest, and installers are blocked too.'), 'builtin-custom': t('A clean base for your own choices.') })[profile.id] ?? '';

const cloneApps = (profile: Profile) =>
  (profile.apps ?? []).map((a) => ({ ...a, id: randomId('app-'), components: a.components.map((c) => ({ ...c, id: randomId('c-') })) }));
const dependencies = (profile: Profile) => (profile.apps ?? []).reduce((sum, a) => sum + Math.max(0, a.components.length - 1), 0);

async function create() {
  const source = app.profiles.find((p) => p.id === base.value);
  const profile: Profile = {
    id: randomId('p-'),
    name: name.value.trim(),
    default_action: source?.default_action ?? 'deny',
    rules: source ? source.rules.map((r) => ({ ...r, id: randomId('r-') })) : [],
    apps: source ? cloneApps(source) : [],
  };
  if (await attempt(() => call('profiles.save', { profile }), t('Profile created.'))) {
    creating.value = false;
    name.value = '';
    await loadProfiles();
    go('rules', profile.id);
  }
}

async function duplicate(profile: Profile) {
  const copy = { ...profile, id: randomId('p-'), name: t('{name} (copy)', { name: profile.name }), rules: profile.rules.map((r) => ({ ...r, id: randomId('r-') })), apps: cloneApps(profile) };
  await attempt(() => call('profiles.save', { profile: copy }), t('Profile duplicated.'));
  await loadProfiles();
}

async function exportProfile(profile: Profile) {
  const result = await attempt(() => host<{ saved: boolean }>('save_text', { name: `${profile.name}.json`, content: JSON.stringify(profile, null, 2) }));
  if (result?.saved) notify('ok', t('Profile exported.'));
}

async function importProfile() {
  const file = await attempt(() => host<{ opened: boolean; content?: string }>('open_text'));
  if (!file?.opened || !file.content) return;
  let profile: unknown;
  try {
    profile = JSON.parse(file.content);
  } catch {
    notify('bad', t('That file is not valid JSON.'));
    return;
  }
  await attempt(() => call('profiles.import', { profile }), t('Profile imported.'));
  await loadProfiles();
}

async function remove() {
  const profile = deleting.value;
  deleting.value = undefined;
  if (!profile) return;
  await attempt(() => call('profiles.delete', { id: profile.id }), t('Profile deleted.'));
  await loadProfiles();
}
</script>

<template>
  <div class="page">
    <PageHeader :title="t('Profiles')" :subtitle="t('A profile is a reusable set of rules for a kind of guest.')">
      <button class="btn" @click="importProfile"><Icon name="upload" />{{ t('Import') }}</button>
      <button class="btn primary" @click="creating = true"><Icon name="plus" />{{ t('New profile') }}</button>
    </PageHeader>

    <Skeleton v-if="waiting && !app.profiles.length" kind="cards" :count="3" />
    <div v-else class="cards">
      <article v-for="profile in app.profiles" :key="profile.id" class="card profile-card">
        <div class="title">
          <span class="profile-glyph"><Icon :name="profile.default_action === 'deny' ? 'shield' : 'allowed'" /></span>
          <h2 class="grow truncate" :title="profileLabel(profile)">{{ profileLabel(profile) }}</h2>
          <span v-if="builtin(profile)" class="badge">{{ t('Template') }}</span>
        </div>
        <p class="desc">{{ describeBuiltin(profile) || t('Custom profile.') }}</p>
        <div class="facts">
          <span class="badge" :class="profile.default_action === 'deny' ? 'bad' : 'ok'">{{ profile.default_action === 'deny' ? t('Blocks by default') : t('Allows by default') }}</span>
          <span class="badge info">{{ t('{n} applications', { n: profile.apps?.length ?? 0 }) }}</span>
          <span class="badge">{{ t('{n} rules', { n: profile.rules.length }) }}</span>
          <span v-if="dependencies(profile)" class="badge">{{ t('{n} dependencies', { n: dependencies(profile) }) }}</span>
        </div>
        <div class="foot">
          <button class="btn primary" @click="go('apps', profile.id)"><Icon name="apps" />{{ t('Applications') }}</button>
          <button class="btn" @click="go('rules', profile.id)"><Icon name="edit" />{{ t('Rules') }}</button>
          <span class="grow" />
          <Menu :label="t('Actions for {name}', { name: profileLabel(profile) })">
            <button role="menuitem" @click="startRename(profile)"><Icon name="edit" />{{ t('Rename') }}</button>
            <button role="menuitem" @click="duplicate(profile)"><Icon name="copy" />{{ t('Duplicate') }}</button>
            <button role="menuitem" @click="exportProfile(profile)"><Icon name="download" />{{ t('Export') }}</button>
            <hr />
            <button role="menuitem" class="danger" @click="deleting = profile"><Icon name="trash" />{{ t('Delete') }}</button>
          </Menu>
        </div>
      </article>
    </div>

    <Modal v-if="renaming" :title="t('Rename profile')" @close="renaming = undefined">
      <div class="field"><label for="rname">{{ t('Name') }}</label><input id="rname" v-model="newName" class="input" maxlength="60" @keyup.enter="rename" /></div>
      <template #footer><button class="btn" @click="renaming = undefined">{{ t('Cancel') }}</button><button class="btn primary" :disabled="!newName.trim()" @click="rename">{{ t('Save') }}</button></template>
    </Modal>

    <Modal v-if="creating" :title="t('New profile')" @close="creating = false">
      <div class="field"><label for="pname">{{ t('Name') }}</label><input id="pname" v-model="name" class="input" maxlength="60" @keyup.enter="name.trim() && create()" /></div>
      <div class="field"><label>{{ t('Start from') }}</label>
        <select v-model="base" class="select"><option value="">{{ t('Empty profile') }}</option><option v-for="p in app.profiles" :key="p.id" :value="p.id">{{ profileLabel(p) }}</option></select></div>
      <template #footer><button class="btn" @click="creating = false">{{ t('Cancel') }}</button><button class="btn primary" :disabled="!name.trim()" @click="create">{{ t('Create') }}</button></template>
    </Modal>

    <Modal v-if="deleting" :title="t('Delete profile?')" @close="deleting = undefined">
      <p>{{ t('“{name}” and its {n} rules will be removed. This cannot be undone.', { name: deleting.name, n: deleting.rules.length }) }}</p>
      <template #footer><button class="btn" @click="deleting = undefined">{{ t('Cancel') }}</button><button class="btn danger" @click="remove">{{ t('Delete') }}</button></template>
    </Modal>
  </div>
</template>
