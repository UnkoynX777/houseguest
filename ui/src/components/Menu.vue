<script setup lang="ts">
import { onBeforeUnmount, ref } from 'vue';

import { t } from '../i18n';
import { enterLayer } from '../layers';
import Icon from './Icon.vue';

defineProps<{ label?: string; text?: string; icon?: string }>();

const open = ref(false);
const root = ref<HTMLElement>();
const up = ref(false);
let leave: (() => void) | undefined;

function away(event: Event) {
  if (root.value && !root.value.contains(event.target as Node)) close();
}
function close() {
  open.value = false;
  leave?.();
  leave = undefined;
  document.removeEventListener('pointerdown', away, true);
}
function toggle() {
  if (open.value) return close();
  const box = root.value?.getBoundingClientRect();
  up.value = !!box && window.innerHeight - box.bottom < 260;
  open.value = true;
  document.addEventListener('pointerdown', away, true);
  leave = enterLayer(root, close, { modal: false });
}
onBeforeUnmount(close);
</script>

<template>
  <span ref="root" class="menu-anchor">
    <button v-if="text" class="btn" :aria-label="label ?? text" aria-haspopup="menu" :aria-expanded="open" @click.stop="toggle"><Icon :name="icon ?? 'plus'" />{{ text }}<Icon name="chevron" :size="14" /></button>
    <button v-else class="btn sm icon ghost" :aria-label="label ?? t('More actions')" :title="label ?? t('More actions')" aria-haspopup="menu" :aria-expanded="open" @click.stop="toggle"><Icon name="more" /></button>
    <div v-if="open" class="menu" :class="{ up }" role="menu" @click="close"><slot /></div>
  </span>
</template>
