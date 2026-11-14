<script setup lang="ts" generic="T">
import { computed, nextTick, onActivated, onBeforeUnmount, onMounted, ref, watch } from 'vue';

// Renders only the rows that are on screen, plus a margin, inside the page's own scroll container. Every item has
// the same height, so the scrollbar and the layout are the same as if all of them were drawn. With a minimum width
// the items flow into as many columns as fit.
const props = withDefaults(
  defineProps<{ items: T[]; itemKey: (item: T) => string; itemHeight: number; minWidth?: number; gap?: number; overscan?: number }>(),
  { minWidth: 0, gap: 0, overscan: 3 },
);

const root = ref<HTMLElement>();
const width = ref(0);
const first = ref(0);
const last = ref(0);
let scroller: HTMLElement | undefined;
let frame = 0;
let observer: ResizeObserver | undefined;

const columns = computed(() => (props.minWidth > 0 && width.value > 0 ? Math.max(1, Math.floor((width.value + props.gap) / (props.minWidth + props.gap))) : 1));
const stride = computed(() => props.itemHeight + props.gap);
const rows = computed(() => Math.ceil(props.items.length / columns.value));
const total = computed(() => Math.max(0, rows.value * stride.value - props.gap));
const shown = computed(() => props.items.slice(first.value * columns.value, (last.value + 1) * columns.value));

function scrollParent(element: HTMLElement | null): HTMLElement {
  for (let node = element?.parentElement; node; node = node.parentElement) {
    const overflow = getComputedStyle(node).overflowY;
    if (overflow === 'auto' || overflow === 'scroll') return node;
  }
  return document.documentElement;
}

function update() {
  frame = 0;
  if (!root.value) return;
  scroller ??= scrollParent(root.value);
  const view = scroller === document.documentElement ? { top: 0, height: window.innerHeight } : { top: scroller.getBoundingClientRect().top, height: scroller.clientHeight };
  const into = view.top - root.value.getBoundingClientRect().top;
  const from = Math.max(0, Math.floor(into / stride.value) - props.overscan);
  const to = Math.min(rows.value - 1, Math.ceil((into + view.height) / stride.value) + props.overscan);
  first.value = from;
  last.value = Math.max(from, to);
}

function schedule() {
  if (!frame) frame = requestAnimationFrame(update);
}

function attach() {
  const parent = scrollParent(root.value ?? null);
  if (scroller === parent) return;
  scroller?.removeEventListener('scroll', schedule);
  scroller = parent;
  (scroller === document.documentElement ? window : scroller).addEventListener('scroll', schedule, { passive: true });
}

onMounted(() => {
  attach();
  observer = new ResizeObserver((entries) => {
    width.value = Math.floor(entries[0].contentRect.width);
    schedule();
  });
  if (root.value) observer.observe(root.value);
  window.addEventListener('resize', schedule);
  update();
});
onActivated(() => nextTick(() => (attach(), update())));
onBeforeUnmount(() => {
  observer?.disconnect();
  window.removeEventListener('resize', schedule);
  scroller?.removeEventListener('scroll', schedule);
  cancelAnimationFrame(frame);
});
watch([() => props.items.length, columns, () => props.itemHeight], () => nextTick(update));
</script>

<template>
  <div ref="root" class="vlist" :style="{ height: total + 'px' }">
    <div class="vlist-body" :style="{ transform: `translateY(${first * stride}px)`, gridTemplateColumns: `repeat(${columns}, minmax(0, 1fr))`, gap: gap + 'px' }">
      <template v-for="item in shown" :key="itemKey(item)"><slot :item="item" /></template>
    </div>
  </div>
</template>
