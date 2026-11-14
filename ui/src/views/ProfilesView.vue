<script setup lang="ts">
import { onMounted, ref } from 'vue';

import { call, host } from '../api';
import Icon from '../components/Icon.vue';
import Modal from '../components/Modal.vue';
import { t } from '../i18n';
import { app, attempt, go, loadProfiles, notify, randomId } from '../store';
import type { Profile } from '../types';

const creating = ref(false);
const name = ref('');
const base = ref('');
const deleting = ref<Profile>();

onMounted(() => attempt(loadProfiles));

const builtin = (profile: Profile) => profile.id.startsWith('builtin-');
const describeBuiltin = (profile: Profile) =>
  ({ 'builtin-guest': t('Only what you pick. Admin tools are blocked.'), 'builtin-gaming': t('For game nights: add your launchers and games.'),
     'builtin-work': t('Everything allowed except admin tools.'), 'builtin-restricted': t('Like Guest, and installers are blocked too.') })[profile.id] ?? '';

async function create() {
  const source = app.profiles.find((p) => p.id === base.value);
  const profile: Profile = {
    id: randomId('p-'),
    name: name.value.trim(),
    default_action: source?.default_action ?? 'deny',
    rules: source ? source.rules.map((r) => ({ ...r, id: randomId('r-') })) : [],
  };
  if (await attempt(() => call('profiles.save', { profile }), t('Profile created.'))) {
    creating.value = false;
    name.value = '';
    await loadProfiles();
    go('rules', profile.id);
  }
}

async function duplicate(profile: Profile) {
  const copy = { ...profile, id: randomId('p-'), name: t('{name} (copy)', { name: profile.name }), rules: profile.rules.map((r) => ({ ...r, id: randomId('r-') })) };
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
    <div class="page-head">
      <div class="grow"><h1>{{ t('Profiles') }}</h1><p>{{ t('A profile is a reusable set of rules for a kind of guest.') }}</p></div>
      <button class="btn" @click="importProfile"><Icon name="upload" />{{ t('Import') }}</button>
      <button class="btn primary" @click="creating = true"><Icon name="plus" />{{ t('New profile') }}</button>
    </div>

    <div class="grid cols-3">
      <div v-for="profile in app.profiles" :key="profile.id" class="card stack" style="gap: 10px">
        <div class="row">
          <h2 class="grow truncate">{{ profile.name }}</h2>
          <span v-if="builtin(profile)" class="badge">{{ t('Template') }}</span>
        </div>
        <p class="muted" style="min-height: 42px">{{ describeBuiltin(profile) || t('Custom profile.') }}</p>
        <div class="row wrap">
          <span class="badge" :class="profile.default_action === 'deny' ? 'bad' : 'ok'">{{ profile.default_action === 'deny' ? t('Blocks by default') : t('Allows by default') }}</span>
          <span class="badge">{{ t('{n} rules', { n: profile.rules.length }) }}</span>
        </div>
        <div class="row" style="margin-top: 4px">
          <button class="btn sm primary" @click="go('rules', profile.id)"><Icon name="edit" />{{ t('Edit rules') }}</button>
          <button class="btn sm icon ghost right" :aria-label="t('Duplicate')" :title="t('Duplicate')" @click="duplicate(profile)"><Icon name="copy" /></button>
          <button class="btn sm icon ghost" :aria-label="t('Export')" :title="t('Export')" @click="exportProfile(profile)"><Icon name="download" /></button>
          <button class="btn sm icon ghost" :aria-label="t('Delete')" :title="t('Delete')" @click="deleting = profile"><Icon name="trash" /></button>
        </div>
      </div>
    </div>

    <Modal v-if="creating" :title="t('New profile')" @close="creating = false">
      <div class="field"><label for="pname">{{ t('Name') }}</label><input id="pname" v-model="name" class="input" maxlength="60" @keyup.enter="name.trim() && create()" /></div>
      <div class="field"><label>{{ t('Start from') }}</label>
        <select v-model="base" class="select"><option value="">{{ t('Empty profile') }}</option><option v-for="p in app.profiles" :key="p.id" :value="p.id">{{ p.name }}</option></select></div>
      <template #footer><button class="btn" @click="creating = false">{{ t('Cancel') }}</button><button class="btn primary" :disabled="!name.trim()" @click="create">{{ t('Create') }}</button></template>
    </Modal>

    <Modal v-if="deleting" :title="t('Delete profile?')" @close="deleting = undefined">
      <p>{{ t('“{name}” and its {n} rules will be removed. This cannot be undone.', { name: deleting.name, n: deleting.rules.length }) }}</p>
      <template #footer><button class="btn" @click="deleting = undefined">{{ t('Cancel') }}</button><button class="btn danger" @click="remove">{{ t('Delete') }}</button></template>
    </Modal>
  </div>
</template>
