<script setup lang="ts">
defineProps<{ label: string; hint?: string }>();
const model = defineModel<boolean>({ default: false });
</script>

<template>
  <label class="toggle">
    <span class="grow">
      <strong>{{ label }}</strong>
      <span v-if="hint" class="hint">{{ hint }}</span>
    </span>
    <span class="switch"><input v-model="model" type="checkbox" role="switch" :aria-label="label" /></span>
  </label>
</template>
