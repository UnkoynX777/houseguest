import { reactive, watch } from 'vue';

// Preferences that outlive the window: where the owner was, how Applications is filtered and sorted, which
// profile they were looking at. Nothing here grants anything. There is no token, PIN, search text, selection,
// scroll position or open dialog, and the blob is validated field by field when it is read, so damaged or
// edited storage falls back to defaults instead of breaking the interface. The blob carries a version and every
// older shape is migrated explicitly.

export const routes = [
  'dashboard', 'apps', 'profiles', 'rules', 'sessions', 'history', 'security', 'settings', 'diagnostics', 'about',
] as const;
export type RouteName = (typeof routes)[number];

export interface AppsPrefs {
  view: 'grid' | 'list';
  sort: 'name' | 'publisher' | 'status' | 'blocks' | 'recent';
  status: 'all' | 'allowed' | 'blocked' | 'attention' | 'unresolved';
  kind: 'all' | 'installed' | 'portable' | 'store' | 'manual' | 'recent' | 'dependencies' | 'uncategorized';
  category: string;
  platform: string;
  volume: string;
  showTools: boolean;
  profileId: string;
}

export interface HistoryPrefs {
  filter: 'all' | 'blocked' | 'allowed' | 'sessions' | 'security' | 'changes';
  range: number;
  sort: 'new' | 'old';
}

export interface Preferences {
  version: number;
  route: RouteName;
  apps: AppsPrefs;
  history: HistoryPrefs;
  rulesProfileId: string;
  sessionsProfileId: string;
}

const key = 'houseguest.ui';
export const currentVersion = 1;

export const defaults = (): Preferences => ({
  version: currentVersion,
  route: 'dashboard',
  apps: { view: 'grid', sort: 'name', status: 'all', kind: 'all', category: '', platform: '', volume: '', showTools: false, profileId: '' },
  history: { filter: 'all', range: 0, sort: 'new' },
  rulesProfileId: '',
  sessionsProfileId: '',
});

const oneOf = <T extends string>(value: unknown, allowed: readonly T[], fallback: T): T =>
  typeof value === 'string' && (allowed as readonly string[]).includes(value) ? (value as T) : fallback;
const text = (value: unknown, fallback = '') => (typeof value === 'string' && value.length < 200 ? value : fallback);
const flag = (value: unknown, fallback = false) => (typeof value === 'boolean' ? value : fallback);
const object = (value: unknown): Record<string, unknown> =>
  value !== null && typeof value === 'object' && !Array.isArray(value) ? (value as Record<string, unknown>) : {};

export function sanitize(raw: unknown): Preferences {
  const base = defaults();
  const source = object(raw);
  const apps = object(source.apps);
  const history = object(source.history);
  return {
    version: currentVersion,
    route: oneOf(source.route, routes, base.route),
    apps: {
      view: oneOf(apps.view, ['grid', 'list'] as const, base.apps.view),
      sort: oneOf(apps.sort, ['name', 'publisher', 'status', 'blocks', 'recent'] as const, base.apps.sort),
      status: oneOf(apps.status, ['all', 'allowed', 'blocked', 'attention', 'unresolved'] as const, base.apps.status),
      kind: oneOf(apps.kind, ['all', 'installed', 'portable', 'store', 'manual', 'recent', 'dependencies', 'uncategorized'] as const, base.apps.kind),
      category: text(apps.category),
      platform: text(apps.platform),
      volume: text(apps.volume),
      showTools: flag(apps.showTools),
      profileId: text(apps.profileId),
    },
    history: {
      filter: oneOf(history.filter, ['all', 'blocked', 'allowed', 'sessions', 'security', 'changes'] as const, base.history.filter),
      range: [0, 3600, 86400, 604800, 2592000].includes(history.range as number) ? (history.range as number) : 0,
      sort: oneOf(history.sort, ['new', 'old'] as const, base.history.sort),
    },
    rulesProfileId: text(source.rulesProfileId),
    sessionsProfileId: text(source.sessionsProfileId),
  };
}

// Each step takes the shape of one version to the next. There is one version so far; the legacy keys that earlier
// releases wrote one by one are folded in the first time.
export function migrate(raw: unknown, legacyView?: string | null): Preferences {
  const source = object(raw);
  const version = typeof source.version === 'number' ? source.version : 0;
  if (version > currentVersion) return defaults();
  const result = sanitize(raw);
  if (version === 0 && (legacyView === 'list' || legacyView === 'grid')) result.apps.view = legacyView;
  return result;
}

function read(): Preferences {
  try {
    const stored = localStorage.getItem(key);
    return migrate(stored ? JSON.parse(stored) : undefined, localStorage.getItem('apps.view'));
  } catch {
    return defaults();
  }
}

export const preferences = reactive<Preferences>(read());

let timer: ReturnType<typeof setTimeout> | undefined;
function persist() {
  clearTimeout(timer);
  timer = setTimeout(() => {
    try {
      localStorage.setItem(key, JSON.stringify(sanitize(preferences)));
    } catch {
      /* storage can be unavailable */
    }
  }, 300);
}

watch(preferences, persist, { deep: true });

export function resetPreferences() {
  Object.assign(preferences, defaults());
}
