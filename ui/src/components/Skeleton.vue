<script setup lang="ts">
// Placeholders that have the same size as the content they stand for, so nothing moves when the data arrives.
withDefaults(defineProps<{ kind: 'app-grid' | 'app-list' | 'events' | 'cards' | 'kpis' | 'lines'; count?: number }>(), { count: 8 });
</script>

<template>
  <div v-if="kind === 'app-grid'" class="app-grid" aria-hidden="true">
    <div v-for="n in count" :key="n" class="app-card ghost-card">
      <span class="skeleton" style="width: 52px; height: 52px; border-radius: 12px" />
      <span class="skeleton" style="width: 70%; height: 14px" />
      <span class="skeleton" style="width: 45%; height: 11px" />
    </div>
  </div>
  <div v-else-if="kind === 'app-list'" class="app-list" aria-hidden="true">
    <div v-for="n in count" :key="n" class="app-row ghost-row">
      <span class="skeleton" style="width: 16px; height: 16px" />
      <span class="skeleton" style="width: 34px; height: 34px; border-radius: 9px" />
      <span class="stack" style="gap: 6px"><span class="skeleton" style="width: 60%; height: 13px" /><span class="skeleton" style="width: 35%; height: 10px" /></span>
      <span class="skeleton hide-narrow" style="width: 80%; height: 11px" />
      <span class="hide-narrow" />
      <span class="skeleton" style="width: 58px; height: 22px; border-radius: 99px" />
    </div>
  </div>
  <div v-else-if="kind === 'events'" class="history-list" aria-hidden="true">
    <div v-for="n in count" :key="n" class="event">
      <span class="bar" />
      <span class="skeleton" style="width: 34px; height: 34px; border-radius: 9px" />
      <span class="stack grow" style="gap: 6px"><span class="skeleton" style="width: 40%; height: 13px" /><span class="skeleton" style="width: 65%; height: 11px" /></span>
      <span class="skeleton" style="width: 84px; height: 11px" />
    </div>
  </div>
  <div v-else-if="kind === 'cards'" class="cards" aria-hidden="true">
    <div v-for="n in count" :key="n" class="card ghost-profile">
      <div class="row" style="gap: 10px"><span class="skeleton" style="width: 34px; height: 34px; border-radius: 9px" /><span class="skeleton" style="width: 50%; height: 15px" /></div>
      <span class="skeleton" style="width: 90%; height: 11px" />
      <span class="skeleton" style="width: 60%; height: 11px" />
      <div class="row" style="gap: 8px; margin-top: auto"><span class="skeleton" style="width: 78px; height: 28px; border-radius: 8px" /><span class="skeleton" style="width: 78px; height: 28px; border-radius: 8px" /></div>
    </div>
  </div>
  <div v-else-if="kind === 'kpis'" class="grid cols-3" aria-hidden="true">
    <div v-for="n in count" :key="n" class="card ghost-kpi">
      <span class="skeleton" style="width: 45%; height: 11px" />
      <span class="skeleton" style="width: 30%; height: 26px" />
    </div>
  </div>
  <div v-else class="stack" aria-hidden="true">
    <span v-for="n in count" :key="n" class="skeleton" :style="{ height: '46px', opacity: 1 - n * 0.06 }" />
  </div>
</template>
