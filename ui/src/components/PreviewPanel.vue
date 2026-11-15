<script setup lang="ts">
import { computed, ref } from 'vue';

import { t } from '../i18n';
import type { Preview } from '../types';
import Findings from './Findings.vue';
import Icon from './Icon.vue';

const props = defineProps<{ preview: Preview }>();
const filter = ref<'all' | 'allow' | 'deny'>('all');
const query = ref('');

const rows = computed(() =>
  props.preview.apps.filter(
    (app) => (filter.value === 'all' || app.action === filter.value) && app.name.toLowerCase().includes(query.value.toLowerCase()),
  ),
);
</script>

<template>
  <div class="stack">
    <Findings :findings="preview.findings" />
    <div v-if="preview.continuity" class="banner" :class="preview.continuity.install_trusted ? '' : 'warn'">
      <Icon :name="preview.continuity.install_trusted ? 'check' : 'alert'" />
      <span>{{ preview.continuity.install_trusted ? t('Houseguest, its window and the Windows essentials stay available under this profile.') : t('Houseguest cannot recognize its own files under this profile. See the warnings above.') }}</span>
    </div>
    <div class="row wrap">
      <span class="badge ok">{{ t('{n} would run', { n: preview.allowed }) }}</span>
      <span class="badge bad">{{ t('{n} would be blocked', { n: preview.denied }) }}</span>
      <div class="seg right" role="group">
        <button :aria-pressed="filter === 'all'" @click="filter = 'all'">{{ t('All') }}</button>
        <button :aria-pressed="filter === 'allow'" @click="filter = 'allow'">{{ t('Allowed') }}</button>
        <button :aria-pressed="filter === 'deny'" @click="filter = 'deny'">{{ t('Blocked') }}</button>
      </div>
      <div class="search" style="width: 200px">
        <Icon name="search" />
        <input v-model="query" class="input" :placeholder="t('Search')" :aria-label="t('Search')" />
      </div>
    </div>
    <div class="table-wrap" style="max-height: 320px; overflow: auto">
      <table>
        <tbody>
          <tr v-for="app in rows" :key="app.path">
            <td>
              <div>{{ app.name }}</div>
              <div class="hint truncate mono" style="max-width: 460px" :title="app.path">{{ app.path }}</div>
            </td>
            <td style="width: 1%">
              <span class="badge" :class="app.action === 'allow' ? 'ok' : 'bad'">{{ app.action === 'allow' ? t('Allowed') : t('Blocked') }}</span>
            </td>
            <td class="faint" style="width: 1%; white-space: nowrap">{{ ({ protected: t('System essential'), continuity: t('Houseguest'), rule: t('Rule'), application: t('Application'), dependency: t('Dependency'), related: t('Related'), default: t('Default'), unresolved: t('Unidentified') } as Record<string, string>)[app.source] ?? app.source }}</td>
          </tr>
          <tr v-if="!rows.length"><td class="faint">{{ t('Nothing to show.') }}</td></tr>
        </tbody>
      </table>
    </div>
  </div>
</template>
