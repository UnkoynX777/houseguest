<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue';

import { t } from '../i18n';
import Icon from './Icon.vue';

defineProps<{ title: string; wide?: boolean }>();
const emit = defineEmits<{ close: [] }>();
const dialog = ref<HTMLElement>();

function onKey(event: KeyboardEvent) {
  if (event.key === 'Escape') emit('close');
}

onMounted(() => {
  window.addEventListener('keydown', onKey);
  dialog.value?.querySelector<HTMLElement>('input, select, button.primary')?.focus();
});
onUnmounted(() => window.removeEventListener('keydown', onKey));
</script>

<template>
  <div class="overlay" @mousedown.self="emit('close')">
    <div ref="dialog" class="modal" :class="{ wide }" role="dialog" aria-modal="true" :aria-label="title">
      <div class="modal-head">
        <h2 class="grow">{{ title }}</h2>
        <button class="btn icon ghost" :aria-label="t('Close')" @click="emit('close')"><Icon name="x" /></button>
      </div>
      <div class="modal-body"><slot /></div>
      <div v-if="$slots.footer" class="modal-foot"><slot name="footer" /></div>
    </div>
  </div>
</template>
