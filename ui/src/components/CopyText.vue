<script setup lang="ts">
import { onBeforeUnmount, ref } from 'vue';

import { host } from '../api';
import { t } from '../i18n';
import { notify } from '../store';
import Icon from './Icon.vue';

// A value that may be long (a path, a hash, a thumbprint). It wraps instead of overflowing, can be limited to a few
// lines with the full text in the tooltip, and carries the actions people want with it. The text shown is the
// value itself; only the layout adapts.
const props = withDefaults(defineProps<{ value: string; display?: string; mono?: boolean; lines?: number; reveal?: boolean; label?: string }>(), { lines: 0 });

const copied = ref(false);
let timer: ReturnType<typeof setTimeout> | undefined;

async function copy() {
  try {
    await navigator.clipboard.writeText(props.value);
  } catch {
    const field = document.createElement('textarea');
    field.value = props.value;
    field.setAttribute('readonly', '');
    field.style.position = 'fixed';
    field.style.opacity = '0';
    document.body.appendChild(field);
    field.select();
    const done = document.execCommand('copy');
    field.remove();
    if (!done) return notify('bad', t('Could not copy.'));
  }
  copied.value = true;
  clearTimeout(timer);
  timer = setTimeout(() => (copied.value = false), 1600);
}

async function open() {
  const reply = await host<{ opened: boolean }>('reveal', { path: props.value }).catch(() => ({ opened: false }));
  if (!reply.opened) notify('bad', t('The folder could not be opened.'));
}

onBeforeUnmount(() => clearTimeout(timer));
</script>

<template>
  <span class="copytext">
    <span class="value" :class="{ mono, clamp: lines > 0 }" :style="lines > 0 ? { '--lines': lines } : undefined" :title="value">{{ display ?? value }}</span>
    <span class="tools">
      <button class="btn icon ghost sm" :aria-label="label ? t('Copy {what}', { what: label }) : t('Copy')" :title="t('Copy')" @click="copy"><Icon :name="copied ? 'check' : 'copy'" :size="14" /></button>
      <button v-if="reveal" class="btn icon ghost sm" :aria-label="t('Show in folder')" :title="t('Show in folder')" @click="open"><Icon name="external" :size="14" /></button>
    </span>
  </span>
</template>
