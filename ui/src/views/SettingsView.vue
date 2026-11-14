<script setup lang="ts">
import PageHeader from '../components/PageHeader.vue';
import Toggle from '../components/Toggle.vue';
import { onMounted, ref } from 'vue';

import { call } from '../api';
import { lang, type Language } from '../i18n';
import { t } from '../i18n';
import { host } from '../api';
import { app, applyTheme, attempt, lockNow, setPrefs, type Theme } from '../store';
import Icon from '../components/Icon.vue';

const expiry = ref('disconnect');

onMounted(async () => {
  const settings = await attempt(() => call<{ default_on_expiry: string }>('settings.get'));
  if (settings) expiry.value = settings.default_on_expiry;
});

async function setLanguage(value: Language) {
  lang.value = value;
  host('set_prefs', { language: value }).catch(() => undefined);
  await attempt(() => call('settings.set', { language: value }));
}

async function setExpiry() {
  await attempt(() => call('settings.set', { default_on_expiry: expiry.value }), t('Saved.'));
}
</script>

<template>
  <div class="page">
    <PageHeader :title="t('Settings')" />
    <div class="card stack">
      <div class="row"><div class="grow"><h2>{{ t('Language') }}</h2></div>
        <div class="seg"><button :aria-pressed="lang === 'en'" @click="setLanguage('en')">English</button><button :aria-pressed="lang === 'pt-BR'" @click="setLanguage('pt-BR')">Português</button></div></div>
      <div class="row"><div class="grow"><h2>{{ t('Appearance') }}</h2></div>
        <div class="seg"><button v-for="theme in (['system', 'light', 'dark'] as Theme[])" :key="theme" :aria-pressed="app.theme === theme" @click="applyTheme(theme)">{{ { system: t('System'), light: t('Light'), dark: t('Dark') }[theme] }}</button></div></div>
      <div class="row"><div class="grow"><h2>{{ t('Default when time is up') }}</h2><p class="muted">{{ t('Used by new sessions.') }}</p></div>
        <select v-model="expiry" class="select" style="width: 260px" :aria-label="t('Default when time is up')" @change="setExpiry">
          <option value="disconnect">{{ t('Lock the guest out (disconnect)') }}</option><option value="logoff">{{ t('Sign the guest out') }}</option><option value="none">{{ t('Only remove restrictions') }}</option>
        </select></div>
    </div>
    <div class="card stack">
      <Toggle :model-value="app.prefs.minimize_to_tray" :label="t('Keep in the notification area when closed')" :hint="t('Closing the window keeps Houseguest in the notification area. Protection always runs in the service.')" @update:model-value="setPrefs({ minimize_to_tray: $event })" />
      <Toggle :model-value="app.prefs.start_with_windows" :label="t('Start with Windows')" :hint="t('Starts the interface hidden in the notification area when you sign in. Protection does not depend on it.')" @update:model-value="setPrefs({ start_with_windows: $event })" />
    </div>
    <div class="card row"><div class="grow"><h2>{{ t('Lock Houseguest') }}</h2><p class="muted">{{ t('Require the PIN again. Protection keeps running.') }}</p></div><button class="btn" @click="lockNow"><Icon name="lock" />{{ t('Lock now') }}</button></div>
  </div>
</template>
