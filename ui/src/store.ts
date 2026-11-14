import { computed, reactive, ref, watch } from 'vue';

import { ApiError, auth, call, host, onHostEvent } from './api';
import { clearAll, markStale, resetGenerations, resource, syncGenerations } from './cache';
import { lang, t } from './i18n';
import type { Profile, Status } from './types';
import { preferences, type RouteName } from './ui-state';

export type Route = RouteName;

export type Theme = 'system' | 'light' | 'dark';

export interface HostPrefs {
  minimize_to_tray: boolean;
  start_with_windows: boolean;
  theme?: Theme;
}

export interface Toast {
  id: number;
  kind: 'ok' | 'bad' | 'info';
  text: string;
}

export const app = reactive({
  ready: false,
  elevated: false,
  status: null as Status | null,
  fetchedAt: 0,
  profiles: [] as Profile[],
  route: preferences.route as Route,
  ruleProfileId: preferences.rulesProfileId,
  clock: Date.now(),
  theme: 'system' as Theme,
  prefs: { minimize_to_tray: true, start_with_windows: false } as HostPrefs,
});

const statusInterval = 5000;

export const toasts = ref<Toast[]>([]);
let toastId = 0;

export function notify(kind: Toast['kind'], text: string) {
  const id = ++toastId;
  toasts.value.push({ id, kind, text });
  setTimeout(() => (toasts.value = toasts.value.filter((toast) => toast.id !== id)), kind === 'bad' ? 7000 : 3500);
}

export function describe(error: unknown): string {
  if (error instanceof ApiError) {
    if (error.code === 'rate_limited') return t('Too many attempts. Try again in {s} seconds.', { s: error.details.retry_after ?? 30 });
    if (error.code === 'service_unavailable') return t('The Houseguest service is not running.');
    return error.message;
  }
  return String(error);
}

export async function attempt<T>(action: () => Promise<T>, success?: string): Promise<T | undefined> {
  try {
    const result = await action();
    if (success) notify('ok', success);
    return result;
  } catch (error) {
    notify('bad', describe(error));
    return undefined;
  }
}

export function profileLabel(profile?: { id?: string; name: string }): string {
  if (!profile) return '';
  return profile.id?.startsWith('builtin-') ? t(profile.name) : profile.name;
}

export const unlocked = computed(() => !!auth.token);

export const serverNow = computed(() => (app.status ? app.status.now + (app.clock - app.fetchedAt) / 1000 : 0));

export const remaining = computed(() =>
  app.status?.armed && app.status.ends_at ? Math.max(0, Math.floor(app.status.ends_at - serverNow.value)) : 0,
);

let sessionSignature = '';

export async function refreshStatus() {
  const status = await call<Status>('status');
  const signature = `${status.armed}|${status.ends_at ?? ''}|${status.profile ?? ''}|${status.ending ?? ''}`;
  if (sessionSignature && signature !== sessionSignature) markStale('session');
  sessionSignature = signature;
  syncGenerations(status.generations);
  app.status = status;
  app.fetchedAt = Date.now();
  app.clock = app.fetchedAt;
  if (app.status.language === 'en' || app.status.language === 'pt-BR') lang.value = app.status.language;
}

const profiles = resource<Profile[]>('profiles', () => call<Profile[]>('profiles.list'), ['policy'], 300_000);

// Profiles are read by most pages, so they live in the store once. A page that only needs them to be there asks
// without forcing; anything that just changed them forces a read.
export async function loadProfiles(force = true) {
  await profiles.load(force);
  if (profiles.slot.data && profiles.slot.data !== app.profiles) app.profiles = profiles.slot.data;
}

export const ensureProfiles = () => loadProfiles(false);

watch(
  () => profiles.slot.stale,
  (stale) => stale && unlocked.value && void loadProfiles(false).catch(() => undefined),
);
watch(
  () => app.route,
  (route) => {
    preferences.route = route;
  },
);
watch(
  () => app.ruleProfileId,
  (id) => {
    preferences.rulesProfileId = id;
  },
);

export function go(route: Route, profileId?: string) {
  if (profileId) app.ruleProfileId = profileId;
  app.route = route;
}

export function formatDuration(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  const two = (n: number) => String(n).padStart(2, '0');
  return h > 0 ? `${h}:${two(m)}:${two(s)}` : `${two(m)}:${two(s)}`;
}

export function formatTime(seconds: number): string {
  return new Date(seconds * 1000).toLocaleString(lang.value, { dateStyle: 'medium', timeStyle: 'short' });
}

export function randomId(prefix: string): string {
  const bytes = crypto.getRandomValues(new Uint8Array(5));
  return prefix + Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

function showTheme(theme: Theme) {
  app.theme = theme;
  if (theme === 'system') document.documentElement.removeAttribute('data-theme');
  else document.documentElement.dataset.theme = theme;
  try {
    localStorage.setItem('theme', theme);
  } catch {
    /* storage can be unavailable */
  }
}

export async function applyTheme(theme: Theme) {
  showTheme(theme);
  await host('set_prefs', { theme }).catch(() => undefined);
}

export async function setPrefs(change: Partial<HostPrefs>) {
  app.prefs = { ...app.prefs, ...(await host<HostPrefs>('set_prefs', change)) };
}

async function loadPrefs() {
  const prefs = await host<HostPrefs>('get_prefs').catch(() => undefined);
  if (!prefs) return;
  app.prefs = prefs;
  if (prefs.theme) showTheme(prefs.theme);
}

export async function boot() {
  onHostEvent((event) => {
    if (event.event === 'navigate' && unlocked.value) app.route = event.route as Route;
    if (event.event === 'prefs') app.prefs = event.prefs as HostPrefs;
  });
  try {
    app.elevated = (await host<{ elevated: boolean }>('info')).elevated;
    await Promise.all([refreshStatus(), loadPrefs()]);
    host('set_prefs', { language: lang.value }).catch(() => undefined);
  } catch (error) {
    notify('bad', describe(error));
  }
  app.ready = true;
  setInterval(() => {
    if (!document.hidden && app.status?.armed) app.clock = Date.now();
  }, 1000);
  window.addEventListener('focus', () => app.status && refreshStatus().catch(() => undefined));
  setInterval(() => {
    if (!document.hidden && unlocked.value) refreshStatus().catch(() => undefined);
  }, statusInterval);
  watch(remaining, (value) => {
    if (value === 0 && app.status?.armed) setTimeout(() => refreshStatus().catch(() => undefined), 1500);
  });
}

export async function unlock(pin: string) {
  const { token } = await call<{ token: string }>('unlock', { pin });
  auth.token = token;
  await Promise.all([refreshStatus(), loadProfiles()]);
}

export async function setup(pin: string) {
  const { token } = await call<{ token: string }>('setup', { pin });
  auth.token = token;
  await Promise.all([refreshStatus(), loadProfiles()]);
}

export async function lockNow() {
  if (auth.token) await call('lock').catch(() => undefined);
  auth.token = '';
  app.route = 'dashboard';
  app.profiles = [];
  sessionSignature = '';
  clearAll();
  resetGenerations();
}
