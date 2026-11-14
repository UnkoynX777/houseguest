<script setup lang="ts">
import { onMounted, ref } from 'vue';

import { call } from '../api';
import { lang, type Language } from '../i18n';
import { t } from '../i18n';
import { app, applyTheme, attempt, lockNow, type Theme } from '../store';
import Icon from '../components/Icon.vue';

const expiry = ref('disconnect');

onMounted(async () => {
  const settings = await attempt(() => call<{ default_on_expiry: string }>('settings.get'));
  if (settings) expiry.value = settings.default_on_expiry;
});

async function setLanguage(value: Language) {
  lang.value = value;
  await attempt(() => call('settings.set', { language: value }));
}

async function setExpiry() {
  await attempt(() => call('settings.set', { default_on_expiry: expiry.value }), t('Saved.'));
}
</script>

<template>
  <div class="page">
    <div class="page-head"><div class="grow"><h1>{{ t('Settings') }}</h1></div></div>
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
    <div class="card row"><div class="grow"><h2>{{ t('Lock Houseguest') }}</h2><p class="muted">{{ t('Require the PIN again. Protection keeps running.') }}</p></div><button class="btn" @click="lockNow"><Icon name="lock" />{{ t('Lock now') }}</button></div>
  </div>
</template>
