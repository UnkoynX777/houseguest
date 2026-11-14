<script setup lang="ts">
import EmptyState from '../components/EmptyState.vue';
import PageHeader from '../components/PageHeader.vue';
import { computed, onMounted, ref, watch } from 'vue';

import { call } from '../api';
import Icon from '../components/Icon.vue';
import Modal from '../components/Modal.vue';
import PreviewPanel from '../components/PreviewPanel.vue';
import { t } from '../i18n';
import { app, attempt, formatTime, loadProfiles, profileLabel, randomId } from '../store';
import type { Action, MatchType, Preview, Profile, Rule } from '../types';

const draft = ref<Profile>();
const dirty = ref(false);
const editing = ref<Rule>();
const preview = ref<Preview>();
const previewing = ref(false);
const saving = ref(false);

onMounted(async () => {
  await attempt(loadProfiles);
  if (!app.ruleProfileId || !app.profiles.some((p) => p.id === app.ruleProfileId)) app.ruleProfileId = app.profiles[0]?.id ?? '';
  select();
});
watch(() => app.ruleProfileId, select);

function select() {
  const source = app.profiles.find((p) => p.id === app.ruleProfileId);
  draft.value = source ? structuredClone(JSON.parse(JSON.stringify(source))) : undefined;
  dirty.value = false;
}

const rules = computed(() => [...(draft.value?.rules ?? [])].sort((a, b) => b.priority - a.priority));

function edit(rule?: Rule) {
  editing.value = rule ? { ...rule, match: { ...rule.match } } : { id: randomId('r-'), action: 'allow', priority: 50, match: { type: 'path', value: '' } };
}

function commit() {
  const rule = editing.value;
  if (!rule || !draft.value) return;
  const index = draft.value.rules.findIndex((r) => r.id === rule.id);
  if (index >= 0) draft.value.rules[index] = rule;
  else draft.value.rules.push(rule);
  editing.value = undefined;
  dirty.value = true;
}

function remove(rule: Rule) {
  if (!draft.value) return;
  draft.value.rules = draft.value.rules.filter((r) => r.id !== rule.id);
  dirty.value = true;
}

async function save() {
  if (!draft.value) return;
  saving.value = true;
  const saved = await attempt(() => call('profiles.save', { profile: draft.value }), t('Profile saved.'));
  saving.value = false;
  if (saved) {
    dirty.value = false;
    await loadProfiles();
  }
}

async function runPreview() {
  previewing.value = true;
  preview.value = await attempt(() => call<Preview>('policy.preview', { profile: draft.value }));
  previewing.value = false;
}

const typeLabel = (type: MatchType) => ({ path: t('Path'), hash: t('Hash'), publisher: t('Publisher'), package: t('Package') })[type];
const hints: Record<MatchType, string> = { path: 'Supports * (within a folder) and ** (any depth), and %SystemRoot%, %ProgramFiles%.', hash: '64 hexadecimal characters (SHA-256).', publisher: 'The exact certificate subject from the file’s signature.', package: 'Package family name of a packaged app.' };
const setAction = (action: Action) => editing.value && (editing.value.action = action);
</script>

<template>
  <div class="page">
    <PageHeader :title="t('Rules')" :subtitle="t('Higher priority wins. At equal priority, Block wins over Allow.')">
      <select v-model="app.ruleProfileId" class="select" style="width: 220px" :aria-label="t('Profile')">
      <option v-for="p in app.profiles" :key="p.id" :value="p.id">{{ profileLabel(p) }}</option>
      </select>
    </PageHeader>

    <EmptyState v-if="!draft" icon="rules" :text="t('Create a profile first.')" />
    <template v-else>
      <div class="card row wrap">
        <div class="grow"><h2>{{ t('Default for anything not listed') }}</h2><p class="muted">{{ t('Essential Windows components are always allowed so the computer stays usable.') }}</p></div>
        <div class="seg">
          <button :aria-pressed="draft.default_action === 'deny'" @click="(draft.default_action = 'deny'), (dirty = true)">{{ t('Block') }}</button>
          <button :aria-pressed="draft.default_action === 'allow'" @click="(draft.default_action = 'allow'), (dirty = true)">{{ t('Allow') }}</button>
        </div>
      </div>

      <div class="row">
        <h2 class="grow">{{ t('{n} rules', { n: rules.length }) }}</h2>
        <button class="btn" :disabled="previewing" @click="runPreview"><span v-if="previewing" class="spinner" /><Icon v-else name="eye" />{{ t('Preview result') }}</button>
        <button class="btn primary" @click="edit()"><Icon name="plus" />{{ t('Add rule') }}</button>
      </div>

      <EmptyState v-if="!rules.length" icon="rules" :text="t('No rules yet. Add one, or pick an app from Applications.')" />
      <div v-else class="table-wrap">
        <table>
          <thead><tr><th>{{ t('Action') }}</th><th>{{ t('Match') }}</th><th>{{ t('Priority') }}</th><th /></tr></thead>
          <tbody>
            <tr v-for="rule in rules" :key="rule.id">
              <td style="width: 1%"><span class="badge" :class="rule.action === 'allow' ? 'ok' : 'bad'">{{ rule.action === 'allow' ? t('Allow') : t('Block') }}</span></td>
              <td style="max-width: 0">
                <span class="badge info" style="margin-right: 8px">{{ typeLabel(rule.match.type) }}</span><span class="mono truncate">{{ rule.match.value }}</span>
                <div v-if="rule.expires_at" class="hint">{{ t('Expires {time}', { time: formatTime(rule.expires_at) }) }}</div>
              </td>
              <td style="width: 1%" class="faint">{{ rule.priority }}</td>
              <td style="width: 1%; white-space: nowrap">
                <button class="btn sm icon ghost" :aria-label="t('Edit')" @click="edit(rule)"><Icon name="edit" /></button>
                <button class="btn sm icon ghost" :aria-label="t('Delete')" @click="remove(rule)"><Icon name="trash" /></button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>

      <div v-if="dirty" class="card row" style="position: sticky; bottom: 0; border-color: var(--accent)">
        <span class="grow"><strong>{{ t('Unsaved changes') }}</strong></span>
        <button class="btn" @click="select">{{ t('Discard') }}</button>
        <button class="btn primary" :disabled="saving" @click="save"><span v-if="saving" class="spinner" />{{ t('Save') }}</button>
      </div>
    </template>

    <Modal v-if="editing" :title="t('Rule')" @close="editing = undefined">
      <div class="field"><label>{{ t('Action') }}</label>
        <div class="seg"><button :aria-pressed="editing.action === 'allow'" @click="setAction('allow')">{{ t('Allow') }}</button><button :aria-pressed="editing.action === 'deny'" @click="setAction('deny')">{{ t('Block') }}</button></div></div>
      <div class="field"><label>{{ t('Match by') }}</label>
        <select v-model="editing.match.type" class="select">
          <option value="path">{{ t('Path') }}</option><option value="hash">{{ t('Hash') }}</option><option value="publisher">{{ t('Publisher') }}</option><option value="package">{{ t('Package') }}</option>
        </select></div>
      <div class="field"><label for="rule-value">{{ t('Value') }}</label><input id="rule-value" v-model="editing.match.value" class="input mono" /><span class="hint">{{ t(hints[editing.match.type]) }}</span></div>
      <div class="field"><label for="rule-priority">{{ t('Priority (-1000 to 1000)') }}</label><input id="rule-priority" v-model.number="editing.priority" class="input" type="number" min="-1000" max="1000" /></div>
      <template #footer><button class="btn" @click="editing = undefined">{{ t('Cancel') }}</button><button class="btn primary" :disabled="!editing.match.value.trim()" @click="commit">{{ t('Done') }}</button></template>
    </Modal>

    <Modal v-if="preview" :title="t('Policy preview')" wide @close="preview = undefined">
      <PreviewPanel :preview="preview" />
      <template #footer><button class="btn" @click="preview = undefined">{{ t('Close') }}</button></template>
    </Modal>
  </div>
</template>
