<script setup lang="ts">
import { t } from '../i18n';
import type { Finding } from '../types';
import Icon from './Icon.vue';

defineProps<{ findings: Finding[] }>();
</script>

<template>
  <div class="stack" style="gap: 8px">
    <div v-for="(finding, index) in findings" :key="index" class="banner" :class="{ warn: finding.severity === 'warning', bad: finding.severity === 'error' }">
      <Icon :name="finding.severity === 'info' ? 'about' : 'alert'" />
      <div>
        <strong>{{ finding.severity === 'error' ? t('Error') : finding.severity === 'warning' ? t('Warning') : t('Note') }}</strong>
        <span> · {{ t(finding.message) }}</span>
        <div v-if="finding.rules.length" class="hint mono">{{ finding.rules.join(', ') }}</div>
      </div>
    </div>
  </div>
</template>
