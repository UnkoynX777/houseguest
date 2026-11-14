<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue';

import { call, host } from '../api';
import { evidenceLabel, platformLabel, reasonLabel, sourceLabel } from '../explain';
import { t } from '../i18n';
import { attempt } from '../store';
import type { LibraryApp } from '../types';
import AppIcon from './AppIcon.vue';
import Icon from './Icon.vue';

const props = defineProps<{ entry: LibraryApp }>();
const emit = defineEmits<{ close: []; changed: [] }>();

const busy = ref(false);
const fileName = (path: string) => path.split(/[\\/]/).pop() ?? path;
const relative = (path: string) => (props.entry.install_dir && path.toLowerCase().startsWith(props.entry.install_dir.toLowerCase()) ? path.slice(props.entry.install_dir.length).replace(/^[\\/]/, '') : path);

async function choose(path: string) {
  busy.value = true;
  const done = await attempt(() => call('apps.resolve', { id: props.entry.id, path }), t('Executable chosen. The library is updating.'));
  busy.value = false;
  if (done) {
    emit('changed');
    emit('close');
  }
}

async function browse() {
  const picked = await attempt(() => host<{ chosen: boolean; paths?: string[] }>('pick_paths', { kind: 'executable' }));
  if (picked?.chosen && picked.paths?.[0]) await choose(picked.paths[0]);
}

function onKey(event: KeyboardEvent) {
  if (event.key === 'Escape') emit('close');
}
onMounted(() => window.addEventListener('keydown', onKey));
onUnmounted(() => window.removeEventListener('keydown', onKey));
</script>

<template>
  <div class="drawer-scrim" @mousedown="emit('close')" />
  <aside class="drawer" role="dialog" aria-modal="true" :aria-label="entry.name">
    <div class="drawer-head">
      <AppIcon :path="entry.candidates[0]?.path ?? ''" :name="entry.name" :size="52" />
      <div class="grow">
        <h2 class="truncate">{{ entry.name }}</h2>
        <div class="muted truncate">{{ platformLabel(entry.platform) }}<template v-if="entry.volume"> · {{ entry.volume }}</template><template v-if="entry.company"> · {{ entry.company }}</template></div>
      </div>
      <span class="badge warn"><span class="dot" />{{ entry.resolution === 'candidates' ? t('Needs a decision') : t('No program found') }}</span>
      <button class="btn icon ghost" :aria-label="t('Close')" @click="emit('close')"><Icon name="x" /></button>
    </div>

    <div class="drawer-body">
      <div class="banner" :class="entry.resolution === 'candidates' ? 'warn' : ''">
        <Icon name="alert" />
        <span v-if="entry.resolution === 'candidates'">{{ t('Houseguest found this installation but could not tell with confidence which program starts it. Choose it below so it can be allowed or blocked.') }}</span>
        <span v-else>{{ t('Houseguest found this installation but no program that a person would start. It is kept here in case you know which file to use.') }}</span>
      </div>

      <template v-if="entry.candidates.length">
        <h3>{{ t('Programs in this installation') }}</h3>
        <div v-for="candidate in entry.candidates" :key="candidate.path" class="component">
          <div class="grow" style="min-width: 0">
            <strong>{{ fileName(candidate.path) }}</strong>
            <div class="hint mono truncate" :title="candidate.path">{{ relative(candidate.path) }}</div>
          </div>
          <div class="actions"><button class="btn sm primary" :disabled="busy" @click="choose(candidate.path)">{{ t('Use this program') }}</button></div>
          <div class="meta">
            <span v-for="reason in candidate.reasons" :key="reason" class="mini">{{ reasonLabel(reason) }}</span>
            <span class="mini">{{ t('Score {n}', { n: candidate.score }) }}</span>
          </div>
        </div>
      </template>
      <button class="btn" :disabled="busy" @click="browse"><Icon name="apps" />{{ t('Choose another file…') }}</button>

      <dl class="kv">
        <dt v-if="entry.install_dir">{{ t('Folder') }}</dt><dd v-if="entry.install_dir" class="mono">{{ entry.install_dir }}</dd>
        <dt>{{ t('Found by') }}</dt><dd>{{ (entry.sources ?? []).map(sourceLabel).join(', ') }}</dd>
        <template v-if="entry.evidence?.length">
          <dt>{{ t('Evidence') }}</dt>
          <dd><div v-for="item in entry.evidence" :key="item.kind + item.detail" class="truncate" :title="item.detail">{{ evidenceLabel(item.kind) }}</div></dd>
        </template>
      </dl>
    </div>
  </aside>
</template>
