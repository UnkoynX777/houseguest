<script setup lang="ts">
import { ref } from 'vue';

import { t } from '../i18n';
import { useLayer } from '../layers';
import Icon from './Icon.vue';

defineProps<{ title: string; wide?: boolean }>();
const emit = defineEmits<{ close: [] }>();
const dialog = ref<HTMLElement>();

useLayer(dialog, () => emit('close'), { focus: 'input, select, button.primary' });
</script>

<template>
  <Teleport to="body">
    <div class="overlay" @mousedown.self="emit('close')">
      <div ref="dialog" class="modal" :class="{ wide }" role="dialog" aria-modal="true" :aria-label="title" tabindex="-1">
        <div class="modal-head">
          <h2 class="grow">{{ title }}</h2>
          <button class="btn icon ghost" :aria-label="t('Close')" @click="emit('close')"><Icon name="x" /></button>
        </div>
        <div class="modal-body"><slot /></div>
        <div v-if="$slots.footer" class="modal-foot"><slot name="footer" /></div>
      </div>
    </div>
  </Teleport>
</template>
