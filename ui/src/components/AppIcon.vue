<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';

import { icon, iconPixels, iconSourcesFor, type IconSource } from '../api';
import { catalogEpoch } from '../cache';

const props = defineProps<{ path: string; name: string; size?: number; sources?: IconSource[] }>();

const box = ref<HTMLElement>();
const url = ref('');
const dimension = computed(() => props.size ?? 44);
const palette = ['#d98a1f', '#4f8fe8', '#2fa97a', '#d4658c', '#8c6fe0', '#e0744a', '#3fa3b8'];
const tint = computed(() => palette[[...props.name].reduce((sum, c) => sum + c.charCodeAt(0), 0) % palette.length]);
const initials = computed(() => props.name.replace(/[^\p{L}\p{N} ]/gu, '').trim().slice(0, 2).toUpperCase() || '?');
let observer: IntersectionObserver | undefined;
let wanted = false;

let request = 0;
async function load() {
  if (!wanted || (!props.path && !props.sources?.length)) return;
  const mine = ++request;
  const picture = await icon(props.path || props.sources![0].path, iconPixels(dimension.value), props.sources ?? iconSourcesFor(props.path), catalogEpoch.value);
  if (mine === request) url.value = picture;
}

onMounted(() => {
  observer = new IntersectionObserver(
    (entries) => {
      if (!entries.some((entry) => entry.isIntersecting)) return;
      wanted = true;
      observer?.disconnect();
      load();
    },
    { rootMargin: '200px' },
  );
  if (box.value) observer.observe(box.value);
});
onBeforeUnmount(() => observer?.disconnect());
watch(
  () => props.path,
  () => {
    url.value = '';
    load();
  },
);
watch(catalogEpoch, load);
</script>

<template>
  <span ref="box" class="app-icon" :style="{ width: dimension + 'px', height: dimension + 'px', fontSize: dimension * 0.36 + 'px' }">
    <img v-if="url" :src="url" alt="" draggable="false" />
    <span v-else class="fallback" :style="{ background: tint }">{{ initials }}</span>
  </span>
</template>
