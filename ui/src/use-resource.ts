import { computed, onActivated, onDeactivated, onMounted, ref, watch } from 'vue';

import { resource, type Dep } from './cache';

// Connects a page to a cached slot. The page shows whatever the slot already holds, asks for fresh data when it
// becomes visible or when the service reports that the data changed, and only shows a loading placeholder when
// there is nothing to show yet.
export function useResource<T>(key: () => string, loader: () => Promise<T>, deps: Dep[] = [], maxAge = 120_000) {
  const current = computed(() => resource<T>(key(), loader, deps, maxAge));
  const active = ref(false);

  const load = (force = false) => current.value.load(force);
  const slot = computed(() => current.value.slot);

  const start = () => {
    active.value = true;
    void load();
  };
  onMounted(() => !active.value && start());
  onActivated(start);
  onDeactivated(() => (active.value = false));

  watch(
    () => current.value.slot.stale,
    (stale) => stale && active.value && void load(),
  );
  watch(key, () => active.value && void load());

  return {
    slot,
    data: computed(() => slot.value.data),
    load,
    initial: computed(() => slot.value.data === undefined && (slot.value.phase === 'loading' || slot.value.phase === 'empty')),
    refreshing: computed(() => slot.value.phase === 'refreshing' || (slot.value.phase === 'loading' && slot.value.data !== undefined)),
    failed: computed(() => (slot.value.phase === 'error' ? slot.value.error : '')),
    active,
  };
}
