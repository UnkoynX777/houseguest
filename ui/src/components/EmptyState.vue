<script setup lang="ts">
import Icon from './Icon.vue';

defineProps<{ icon: string; text: string }>();
</script>

<template>
  <div class="empty">
    <Icon :name="icon" :size="32" />
    <span>{{ text }}</span>
    <slot />
  </div>
</template>
