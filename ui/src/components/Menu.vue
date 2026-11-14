<script setup lang="ts">
import { onBeforeUnmount, ref } from 'vue';

import { t } from '../i18n';
import Icon from './Icon.vue';

defineProps<{ label?: string }>();

const open = ref(false);
const root = ref<HTMLElement>();
const up = ref(false);

function away(event: Event) {
  if (root.value && !root.value.contains(event.target as Node)) close();
}
function key(event: KeyboardEvent) {
  if (event.key === 'Escape') close();
}
function close() {
  open.value = false;
  document.removeEventListener('pointerdown', away, true);
  document.removeEventListener('keydown', key, true);
}
function toggle() {
  if (open.value) return close();
  const box = root.value?.getBoundingClientRect();
  up.value = !!box && window.innerHeight - box.bottom < 260;
  open.value = true;
  document.addEventListener('pointerdown', away, true);
  document.addEventListener('keydown', key, true);
}
onBeforeUnmount(close);
</script>

<template>
  <span ref="root" class="menu-anchor">
    <button class="btn sm icon ghost" :aria-label="label ?? t('More actions')" :title="label ?? t('More actions')" aria-haspopup="menu" :aria-expanded="open" @click.stop="toggle"><Icon name="more" /></button>
    <div v-if="open" class="menu" :class="{ up }" role="menu" @click="close"><slot /></div>
  </span>
</template>
