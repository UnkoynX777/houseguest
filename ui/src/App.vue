<script setup lang="ts">
import { computed, onMounted, type Component } from 'vue';

import Icon from './components/Icon.vue';
import Mark from './components/Mark.vue';
import { t } from './i18n';
import { app, boot, toasts, unlocked, type Route } from './store';
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
import SettingsView from './views/SettingsView.vue';
import { lockNow } from './store';

const views: Record<Route, Component> = {
  dashboard: DashboardView, apps: AppsView, profiles: ProfilesView, rules: RulesView, sessions: SessionsView,
  history: HistoryView, security: SecurityView, settings: SettingsView, diagnostics: DiagnosticsView, about: AboutView,
};

const items = computed<{ route: Route; label: string }[]>(() => [
  { route: 'dashboard', label: t('Dashboard') },
  { route: 'sessions', label: t('Sessions') },
  { route: 'apps', label: t('Applications') },
  { route: 'profiles', label: t('Profiles') },
  { route: 'rules', label: t('Rules') },
  { route: 'history', label: t('History') },
  { route: 'security', label: t('Security') },
  { route: 'diagnostics', label: t('Diagnostics') },
  { route: 'settings', label: t('Settings') },
  { route: 'about', label: t('About') },
]);

onMounted(boot);
</script>

<template>
  <div v-if="!app.ready" class="gate"><span class="spinner" /></div>
  <GateView v-else-if="!unlocked" />
  <div v-else class="shell">
    <nav class="sidebar" :aria-label="t('Main navigation')">
      <div class="brand"><Mark /><span>Houseguest</span></div>
      <button v-for="item in items" :key="item.route" class="nav" :aria-current="app.route === item.route ? 'page' : undefined" @click="app.route = item.route">
        <Icon :name="item.route" /><span>{{ item.label }}</span>
      </button>
      <div class="spacer" />
      <button class="nav" @click="lockNow"><Icon name="lock" /><span>{{ t('Lock') }}</span></button>
    </nav>
    <main class="main"><component :is="views[app.route]" :key="app.route" /></main>
  </div>

  <div class="toasts" role="status" aria-live="polite">
    <div v-for="toast in toasts" :key="toast.id" class="toast" :class="toast.kind">
      <Icon :name="toast.kind === 'ok' ? 'check' : toast.kind === 'bad' ? 'alert' : 'about'" />
      <span>{{ toast.text }}</span>
    </div>
  </div>
</template>
