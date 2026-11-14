<script setup lang="ts">
import { computed, ref } from 'vue';

import { ApiError, host } from '../api';
import Icon from '../components/Icon.vue';
import Mark from '../components/Mark.vue';
import { t } from '../i18n';
import { app, describe, setup, unlock } from '../store';

const pin = ref('');
const confirmation = ref('');
const busy = ref(false);
const error = ref('');

const configured = computed(() => app.status?.configured ?? false);
const mismatch = computed(() => !configured.value && confirmation.value !== '' && pin.value !== confirmation.value);
const canSubmit = computed(() => pin.value.length >= 4 && !busy.value && (configured.value || pin.value === confirmation.value));

async function submit() {
  if (!canSubmit.value) return;
  busy.value = true;
  error.value = '';
  try {
    await (configured.value ? unlock(pin.value) : setup(pin.value));
  } catch (e) {
    error.value = e instanceof ApiError && e.code === 'invalid_pin' ? t('Incorrect PIN.') : describe(e);
    pin.value = '';
  } finally {
    busy.value = false;
  }
}

const elevate = () => host('elevate');
</script>

<template>
  <div class="gate">
    <form class="card" @submit.prevent="submit">
      <Mark class="mark" />
      <template v-if="!configured">
        <div>
          <h1>{{ t('Welcome to Houseguest') }}</h1>
          <p class="muted">{{ t('Let guests use only what you allow. Choose an owner PIN to get started.') }}</p>
        </div>
        <div v-if="!app.elevated" class="banner">
          <Icon name="shield" />
          <div>
            <strong>{{ t('Administrator approval needed') }}</strong>
            <p class="muted">{{ t('Setting up the owner account requires administrator permission, so nobody else can claim it.') }}</p>
            <button type="button" class="btn primary sm" style="margin-top: 10px" @click="elevate">{{ t('Continue as administrator') }}</button>
          </div>
        </div>
        <template v-else>
          <div class="field">
            <label for="pin">{{ t('Owner PIN') }}</label>
            <input id="pin" v-model="pin" class="input pin" type="password" autocomplete="off" maxlength="64" />
            <span class="hint">{{ t('4 to 64 characters. You will need it to change anything.') }}</span>
          </div>
          <div class="field">
            <label for="confirm">{{ t('Repeat PIN') }}</label>
            <input id="confirm" v-model="confirmation" class="input pin" type="password" autocomplete="off" maxlength="64" />
            <span v-if="mismatch" class="hint" style="color: var(--danger)">{{ t('The PINs do not match.') }}</span>
          </div>
          <button class="btn primary" :disabled="!canSubmit">{{ t('Create owner account') }}</button>
        </template>
      </template>
      <template v-else>
        <div>
          <h1>{{ t('Unlock Houseguest') }}</h1>
          <p class="muted">{{ app.status?.armed ? t('Protection is active.') : t('Enter your owner PIN.') }}</p>
        </div>
        <div class="field">
          <label for="pin">{{ t('Owner PIN') }}</label>
          <input id="pin" v-model="pin" class="input pin" type="password" autocomplete="off" maxlength="64" autofocus />
        </div>
        <button class="btn primary" :disabled="!canSubmit">
          <span v-if="busy" class="spinner" />{{ t('Unlock') }}
        </button>
      </template>
      <p v-if="error" class="banner bad" role="alert"><Icon name="alert" />{{ error }}</p>
    </form>
  </div>
</template>
