<script setup lang="ts">
import { host } from '../api';
import { t } from '../i18n';
import Icon from './Icon.vue';
import Mark from './Mark.vue';

// The window has no system caption: this bar is the caption. The host marks the left part as draggable through
// CSS app regions, and the two buttons call the host so closing and minimizing keep their tray behaviour.
const act = (action: 'minimize' | 'close' | 'menu') => host('window', { action }).catch(() => undefined);
</script>

<template>
  <header class="titlebar" @contextmenu.prevent="act('menu')">
    <div class="titlebar-drag"><Mark /><span>Houseguest</span></div>
    <div class="titlebar-buttons">
      <button class="tb" :title="t('Minimize')" :aria-label="t('Minimize')" @click="act('minimize')"><Icon name="minimize" :size="15" /></button>
      <button class="tb close" :title="t('Close')" :aria-label="t('Close')" @click="act('close')"><Icon name="x" :size="15" /></button>
    </div>
  </header>
</template>
