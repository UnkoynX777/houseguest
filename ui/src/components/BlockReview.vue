<script setup lang="ts">
import { computed, ref } from 'vue';

import { ApiError, call } from '../api';
import { describeBlock, recommendation } from '../explain';
import { t } from '../i18n';
import { app, describe, formatTime, notify, refreshStatus } from '../store';
import type { BlockEntry } from '../types';
import Icon from './Icon.vue';
import Modal from './Modal.vue';

const props = defineProps<{ block: BlockEntry }>();
const emit = defineEmits<{ close: []; done: [] }>();

const busy = ref(false);
const acknowledge = ref(false);
const needsAcknowledge = ref(false);
const profileId = computed(() => app.profiles.find((p) => p.name === props.block.profile)?.id ?? '');
const fileName = (path: string) => path.split(/[\\/]/).pop() ?? path;
const dependency = computed(() => !!props.block.related_app && !!profileId.value);

async function run(action: () => Promise<unknown>, message: string) {
  busy.value = true;
  try {
    await action();
    notify('ok', message);
    emit('done');
    emit('close');
  } catch (error) {
    if (error instanceof ApiError && error.code === 'confirmation_required') needsAcknowledge.value = true;
    else notify('bad', describe(error));
  } finally {
    busy.value = false;
  }
}

const approve = (minutes: number) =>
  run(() => call('apps.component.add', { profile_id: profileId.value, app_id: props.block.related_app, path: props.block.path, minutes, acknowledge_sensitive: acknowledge.value }), t('Approved.'));

async function allowTemporarily(minutes: number) {
  await refreshStatus();
  if (!app.status?.armed) {
    notify('info', t('Start a session first; exceptions apply to the running session.'));
    return;
  }
  await run(() => call('exceptions.add', { app: { path: props.block.path }, minutes, match: 'path' }), t('Allowed for {n} minutes.', { n: minutes }));
}
</script>

<template>
  <Modal :title="t('Review blocked program')" @close="emit('close')">
    <dl class="kv">
      <dt>{{ t('Program') }}</dt><dd><strong>{{ block.process || fileName(block.path) }}</strong></dd>
      <dt>{{ t('When') }}</dt><dd>{{ formatTime(block.ts) }}</dd>
      <dt>{{ t('Profile') }}</dt><dd>{{ block.profile }}</dd>
      <dt>{{ t('Why') }}</dt><dd>{{ describeBlock(block) }}</dd>
      <dt v-if="block.rule">{{ t('Rule') }}</dt><dd v-if="block.rule" class="mono">{{ block.rule }}</dd>
      <dt v-if="block.parent">{{ t('Started by') }}</dt><dd v-if="block.parent" class="mono">{{ block.parent }}</dd>
      <dt>{{ t('Executable') }}</dt><dd class="mono">{{ block.path }}</dd>
      <dt>{{ t('Signed by') }}</dt><dd>{{ block.publisher || t('Not signed or not trusted') }}</dd>
      <dt v-if="block.sha256">SHA-256</dt><dd v-if="block.sha256" class="mono">{{ block.sha256 }}</dd>
    </dl>
    <div v-if="recommendation(block)" class="banner"><Icon name="about" /><span>{{ recommendation(block) }}</span></div>
    <label v-if="needsAcknowledge" class="check"><input v-model="acknowledge" type="checkbox" />{{ t('I understand that approved tools can run other programs.') }}</label>
    <template #footer>
      <button class="btn" @click="emit('close')">{{ t('Close') }}</button>
      <template v-if="dependency">
        <button class="btn" :disabled="busy || (needsAcknowledge && !acknowledge)" @click="approve(60)">{{ t('Approve for 1 hour') }}</button>
        <button class="btn primary" :disabled="busy || (needsAcknowledge && !acknowledge)" @click="approve(0)">{{ t('Approve as dependency') }}</button>
      </template>
      <template v-else>
        <button class="btn" :disabled="busy" @click="allowTemporarily(15)">{{ t('Allow 15 min') }}</button>
        <button class="btn primary" :disabled="busy" @click="allowTemporarily(60)">{{ t('Allow 1 h') }}</button>
      </template>
    </template>
  </Modal>
</template>
