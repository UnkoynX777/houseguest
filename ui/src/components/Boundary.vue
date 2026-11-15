<script setup lang="ts">
import { defineComponent, onErrorCaptured, ref, watch } from 'vue';

import { t } from '../i18n';
import { describe } from '../store';
import Icon from './Icon.vue';

// A section that cannot be drawn is replaced by a message and a retry; whatever surrounds it keeps working.
// `reset` clears the failure when the surrounding context changes (another tab, another page).
const props = defineProps<{ reset?: unknown; label?: string }>();
const emit = defineEmits<{ retry: [] }>();

const failure = ref('');
const generation = ref(0);

// Errors thrown while a slot is drawn belong to the component drawing it, so the slot is drawn by a child.
const Section = defineComponent({ setup: (_, { slots }) => () => slots.default?.() });

onErrorCaptured((error) => {
  console.error(error);
  failure.value = describe(error);
  return false;
});

function clear() {
  failure.value = '';
  generation.value++;
}
function retry() {
  clear();
  emit('retry');
}

// A section that is fine is left alone when the context changes; only a failed one is drawn again.
watch(
  () => props.reset,
  () => {
    if (failure.value) clear();
  },
);
</script>

<template>
  <div v-if="failure" class="failure" role="alert">
    <h3><Icon name="alert" :size="16" /> {{ label ?? t('This section could not be displayed.') }}</h3>
    <p>{{ failure }}</p>
    <button class="btn sm" @click="retry">{{ t('Try again') }}</button>
  </div>
  <Section v-else :key="generation"><slot /></Section>
</template>
