<script setup lang="ts">
import { computed, nextTick, onMounted, ref, watch, type Component } from 'vue';

import Boundary from './components/Boundary.vue';
import Icon from './components/Icon.vue';
import TitleBar from './components/TitleBar.vue';
import { t } from './i18n';
import { app, boot, formatDuration, remaining, toasts, unlocked, type Route } from './store';
import AboutView from './views/AboutView.vue';
import AppsView from './views/AppsView.vue';
import DashboardView from './views/DashboardView.vue';
import DiagnosticsView from './views/DiagnosticsView.vue';
import GateView from './views/GateView.vue';
import HistoryView from './views/HistoryView.vue';
import ProfilesView from './views/ProfilesView.vue';
import RulesView from './views/RulesView.vue';
import SecurityView from './views/SecurityView.vue';
import SessionsView from './views/SessionsView.vue';
import AssistantView from './views/AssistantView.vue';
import RemoteView from './views/RemoteView.vue';
import SettingsView from './views/SettingsView.vue';
import { lockNow } from './store';

const views: Record<Route, Component> = {
  dashboard: DashboardView, apps: AppsView, profiles: ProfilesView, rules: RulesView, sessions: SessionsView,
  history: HistoryView, security: SecurityView, settings: SettingsView, diagnostics: DiagnosticsView, about: AboutView,
  remote: RemoteView, assistant: AssistantView,
};

const groups = computed<{ route: Route; label: string }[][]>(() => [
  [
    { route: 'dashboard', label: t('Dashboard') },
    { route: 'sessions', label: t('Sessions') },
  ],
  [
    { route: 'apps', label: t('Applications') },
    { route: 'profiles', label: t('Profiles') },
    { route: 'rules', label: t('Rules') },
  ],
  [
    { route: 'history', label: t('History') },
    { route: 'security', label: t('Security') },
    { route: 'diagnostics', label: t('Diagnostics') },
  ],
  [
    { route: 'remote', label: t('Remote access') },
    { route: 'assistant', label: t('Assistant') },
  ],
]);

const footer = computed<{ route: Route; label: string }[]>(() => [
  { route: 'settings', label: t('Settings') },
  { route: 'about', label: t('About') },
]);

const protection = computed(() => {
  if (!app.status?.armed) return { active: false, text: t('No active session') };
  return { active: true, text: t('Protection active') + ' · ' + formatDuration(remaining.value) };
});

onMounted(boot);

// The page area is one scroll container shared by every view. Each view keeps its own position while it is away.
const mainElement = ref<HTMLElement>();
const scrollTops = new Map<Route, number>();
watch(
  () => app.route,
  (_next, previous) => {
    if (mainElement.value) scrollTops.set(previous, mainElement.value.scrollTop);
  },
  { flush: 'pre' },
);
watch(
  () => app.route,
  async (next) => {
    await nextTick();
    requestAnimationFrame(() => {
      if (mainElement.value) mainElement.value.scrollTop = scrollTops.get(next) ?? 0;
    });
  },
);
</script>

<template>
  <div class="frame">
    <TitleBar />
    <div v-if="!app.ready" class="gate"><span class="spinner" /></div>
    <GateView v-else-if="!unlocked" />
    <div v-else class="shell">
      <nav class="sidebar" :aria-label="t('Main navigation')">
        <template v-for="(group, index) in groups" :key="index">
          <div v-if="index > 0" class="nav-divider" />
          <button v-for="item in group" :key="item.route" class="nav" :title="item.label" :aria-label="item.label" :aria-current="app.route === item.route ? 'page' : undefined" @click="app.route = item.route">
            <Icon :name="item.route" /><span>{{ item.label }}</span>
          </button>
        </template>
        <div class="spacer" />
        <button class="side-status" :class="{ active: protection.active }" :title="protection.text" :aria-label="protection.text" @click="app.route = 'dashboard'">
          <span class="dot" />
        </button>
        <button v-for="item in footer" :key="item.route" class="nav" :title="item.label" :aria-label="item.label" :aria-current="app.route === item.route ? 'page' : undefined" @click="app.route = item.route">
          <Icon :name="item.route" /><span>{{ item.label }}</span>
        </button>
        <button class="nav" :title="t('Lock')" :aria-label="t('Lock')" @click="lockNow"><Icon name="lock" /><span>{{ t('Lock') }}</span></button>
      </nav>
      <main ref="mainElement" class="main"><Boundary :reset="app.route" :label="t('This page could not be displayed.')"><KeepAlive><component :is="views[app.route]" /></KeepAlive></Boundary></main>
    </div>
  </div>

  <Teleport to="body">
    <div class="toasts" role="status" aria-live="polite">
      <div v-for="toast in toasts" :key="toast.id" class="toast" :class="toast.kind">
        <Icon :name="toast.kind === 'ok' ? 'check' : toast.kind === 'bad' ? 'alert' : 'about'" />
        <span>{{ toast.text }}</span>
      </div>
    </div>
  </Teleport>
</template>
