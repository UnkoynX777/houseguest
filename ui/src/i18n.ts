import { ref } from 'vue';

import { pt } from './pt-BR';

export type Language = 'en' | 'pt-BR';

export const lang = ref<Language>(navigator.language.toLowerCase().startsWith('pt') ? 'pt-BR' : 'en');

export function t(text: string, vars: Record<string, string | number> = {}): string {
  const template = lang.value === 'pt-BR' ? (pt[text] ?? text) : text;
  return template.replace(/\{(\w+)\}/g, (_, name: string) => String(vars[name] ?? ''));
}
