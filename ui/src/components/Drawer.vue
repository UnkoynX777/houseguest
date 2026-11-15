<script setup lang="ts">
import { ref } from 'vue';

import { useLayer } from '../layers';

defineProps<{ label: string }>();
const emit = defineEmits<{ close: [] }>();
const panel = ref<HTMLElement>();

useLayer(panel, () => emit('close'));
</script>

<template>
  <Teleport to="body">
    <div class="drawer-layer">
      <div class="drawer-scrim" @mousedown="emit('close')" />
      <aside ref="panel" class="drawer" role="dialog" aria-modal="true" :aria-label="label" tabindex="-1"><slot /></aside>
    </div>
  </Teleport>
</template>
