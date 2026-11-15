<script setup lang="ts">
import { computed, onActivated, onBeforeUnmount, onDeactivated, ref, toRef, watch } from 'vue';

import { call, host, rememberIconSources } from '../api';
import AppIcon from '../components/AppIcon.vue';
import AppPanel from '../components/AppPanel.vue';
import DiscoveryModal from '../components/DiscoveryModal.vue';
import EntryResolver from '../components/EntryResolver.vue';
import EmptyState from '../components/EmptyState.vue';
import LibraryTools from '../components/LibraryTools.vue';
import Menu from '../components/Menu.vue';
import Icon from '../components/Icon.vue';
import PageHeader from '../components/PageHeader.vue';
import Skeleton from '../components/Skeleton.vue';
import VirtualList from '../components/VirtualList.vue';
import { categoryLabel, categoryOf, kindLabel, platformLabel, sourceLabel } from '../explain';
import { t } from '../i18n';
import { app, attempt, loadProfiles, notify, profileLabel, randomId } from '../store';
import type { Application, LibraryApp } from '../types';
import { preferences } from '../ui-state';
import { useResource } from '../use-resource';

type Library = { apps: LibraryApp[]; scanning: boolean };

const prefs = preferences.apps;
const profileId = toRef(prefs, 'profileId');
const view = toRef(prefs, 'view');
const status = toRef(prefs, 'status');
const category = toRef(prefs, 'category');
const sort = toRef(prefs, 'sort');
const kind = toRef(prefs, 'kind');
const platform = toRef(prefs, 'platform');
const volume = toRef(prefs, 'volume');
const showTools = toRef(prefs, 'showTools');
const query = ref('');
const search = ref('');
const selected = ref(new Set<string>());
const active = ref<LibraryApp>();
const observing = ref<LibraryApp>();
const working = ref(false);
const starting = ref(false);
const tools = ref<'sources' | 'added' | 'folder' | 'diagnose'>();
let poller: ReturnType<typeof setTimeout> | undefined;
let typing: ReturnType<typeof setTimeout> | undefined;
let handledProfile = '';
const recentWindow = 7 * 86400;

// The stored profile is kept until the profiles are known; only then is it checked against them.
function settleProfile() {
  if (!app.profiles.length || app.profiles.some((p) => p.id === profileId.value)) return;
  profileId.value = app.profiles.find((p) => p.id === app.ruleProfileId)?.id ?? app.profiles[0].id;
}
settleProfile();
watch(() => app.profiles, settleProfile);

const library = useResource<Library>(
  () => `apps.library:${profileId.value}`,
  async () => {
    if (!profileId.value) return { apps: [], scanning: false };
    const result = await call<Library>('apps.library', { profile_id: profileId.value });
    rememberIconSources(result.apps);
    return result;
  },
  ['policy', 'catalog'],
  300_000,
);
const entries = computed(() => library.data.value?.apps ?? []);
const scanning = computed(() => starting.value || (library.data.value?.scanning ?? false));
const loading = computed(() => library.initial.value || !profileId.value);
const failure = library.failed;

const profile = computed(() => app.profiles.find((p) => p.id === profileId.value));
const key = (entry: LibraryApp) => entry.id;
const attention = (entry: LibraryApp) => !!(entry.pending || entry.conflicts);
const resolved = (entry: LibraryApp) => entry.resolution === 'resolved' && !!entry.path;
const iconPath = (entry: LibraryApp) => entry.path || entry.candidates?.[0]?.path || '';
const stateText = (entry: LibraryApp) => (entry.status === 'allowed' ? t('Allowed') : entry.status === 'blocked' ? t('Blocked') : t('Choose program'));

const load = (_quiet = false) => library.load(true);

function poll() {
  clearTimeout(poller);
  if (scanning.value && library.active.value) {
    poller = setTimeout(async () => {
      await library.load(true);
      starting.value = false;
      poll();
    }, 1500);
  }
}
watch([scanning, library.active], poll);
watch(entries, (list) => {
  if (active.value) active.value = list.find((entry) => key(entry) === key(active.value!)) ?? active.value;
});
watch(query, (value) => {
  clearTimeout(typing);
  typing = setTimeout(() => (search.value = value), 120);
});
watch(profileId, () => (selected.value = new Set()));

onActivated(() => {
  if (app.ruleProfileId && app.ruleProfileId !== handledProfile && app.profiles.some((p) => p.id === app.ruleProfileId)) {
    profileId.value = app.ruleProfileId;
  }
  handledProfile = app.ruleProfileId;
});
onDeactivated(() => {
  active.value = undefined;
  observing.value = undefined;
  tools.value = undefined;
});

async function refresh() {
  const started = await attempt(() => call('apps.refresh'));
  if (started) {
    starting.value = true;
    setTimeout(() => library.load(true).then(() => (starting.value = false)), 600);
  }
}

async function cancelRefresh() {
  await attempt(() => call('apps.refresh.cancel'));
}

async function addPrograms() {
  const picked = await attempt(() => host<{ chosen: boolean; paths?: string[] }>('pick_paths', { kind: 'executable' }));
  if (!picked?.chosen || !picked.paths?.length) return;
  const result = await attempt(() => call<{ added: number }>('apps.manual.add', { paths: picked.paths }));
  if (result) {
    notify('ok', t('{n} programs added.', { n: result.added }));
    await load(true);
  }
}

onBeforeUnmount(() => {
  clearTimeout(poller);
  clearTimeout(typing);
});

const listed = computed(() => entries.value.filter((entry) => showTools.value || entry.role === 'application'));
const platforms = computed(() => [...new Set(listed.value.map((entry) => entry.platform))].filter(Boolean).sort());
const volumes = computed(() => [...new Set(listed.value.map((entry) => entry.volume))].filter(Boolean).sort());
const hiddenCount = computed(() => entries.value.length - entries.value.filter((entry) => entry.role === 'application').length);
const isRecent = (entry: LibraryApp) => entry.first_seen > 0 && Date.now() / 1000 - entry.first_seen < recentWindow;

const categories = computed(() => {
  const counts = new Map<string, number>();
  for (const entry of listed.value) counts.set(categoryOf(entry), (counts.get(categoryOf(entry)) ?? 0) + 1);
  return [...counts.entries()].sort((a, b) => b[1] - a[1]);
});

function matchesKind(entry: LibraryApp): boolean {
  switch (kind.value) {
    case 'all': return true;
    case 'recent': return isRecent(entry);
    case 'dependencies': return !!entry.listed && (entry.dependencies ?? 0) > 0;
    case 'uncategorized': return categoryOf(entry) === 'other';
    case 'installed': return entry.kind === 'installed';
    default: return entry.kind === kind.value;
  }
}

const visible = computed(() => {
  const needle = search.value.trim().toLowerCase();
  const rows = listed.value.filter(
    (entry) =>
      matchesKind(entry) &&
      (!platform.value || entry.platform === platform.value) &&
      (!volume.value || entry.volume === volume.value) &&
      (status.value === 'all' || (status.value === 'attention' ? attention(entry) : status.value === 'unresolved' ? !resolved(entry) : entry.status === status.value)) &&
      (!category.value || categoryOf(entry) === category.value) &&
      (!needle || `${entry.name} ${entry.company} ${entry.path} ${entry.description}`.toLowerCase().includes(needle)),
  );
  const order: Record<typeof sort.value, (a: LibraryApp, b: LibraryApp) => number> = {
    recent: (a, b) => b.first_seen - a.first_seen || b.modified - a.modified || a.name.localeCompare(b.name),
    name: (a, b) => a.name.localeCompare(b.name),
    publisher: (a, b) => a.company.localeCompare(b.company) || a.name.localeCompare(b.name),
    status: (a, b) => a.status.localeCompare(b.status) || a.name.localeCompare(b.name),
    blocks: (a, b) => b.blocks - a.blocks || a.name.localeCompare(b.name),
  };
  return rows.sort(order[sort.value]);
});

const totals = computed(() => ({
  allowed: listed.value.filter((e) => e.status === 'allowed').length,
  blocked: listed.value.filter((e) => e.status === 'blocked').length,
  withDependencies: listed.value.filter((e) => e.listed && (e.dependencies ?? 0) > 0).length,
  attention: listed.value.filter(attention).length,
  unresolved: listed.value.filter((e) => !resolved(e)).length,
}));

const kindCounts = computed(() => ({
  installed: listed.value.filter((e) => e.kind === 'installed').length,
  portable: listed.value.filter((e) => e.kind === 'portable').length,
  store: listed.value.filter((e) => e.kind === 'store').length,
  manual: listed.value.filter((e) => e.kind === 'manual').length,
  recent: listed.value.filter(isRecent).length,
  dependencies: totals.value.withDependencies,
  uncategorized: listed.value.filter((e) => categoryOf(e) === 'other').length,
}));

const chosen = computed(() => entries.value.filter((e) => selected.value.has(key(e))));

function toggle(entry: LibraryApp) {
  const next = new Set(selected.value);
  if (!next.delete(key(entry))) next.add(key(entry));
  selected.value = next;
}

function open(entry: LibraryApp) {
  if (selected.value.size) toggle(entry);
  else active.value = entry;
}

function selectAllVisible() {
  selected.value = new Set(visible.value.map(key));
}

async function allowSelected() {
  const targets = chosen.value.filter((e) => !e.listed && resolved(e) && !e.offline);
  if (!targets.length) return;
  working.value = true;
  let done = 0;
  for (const entry of targets) {
    const info = await attempt(() => call<{ sha256: string; publisher: string }>('apps.inspect', { path: entry.path }));
    if (!info) continue;
    const next: Application = {
      id: randomId('app-'), name: entry.name, mode: 'approved', unknown: 'review', priority: 50,
      components: [{ id: randomId('c-'), label: entry.name, role: 'main', scope: 'independent', path: entry.path, sha256: info.sha256, publisher: info.publisher, package_family: entry.package_family, follow_updates: !!info.publisher }],
    };
    if (await attempt(() => call('apps.save', { profile_id: profileId.value, app: next }))) done++;
  }
  working.value = false;
  notify('ok', t('{n} programs allowed in {profile}.', { n: done, profile: profileLabel(profile.value) }));
  selected.value = new Set();
  await load();
}

async function blockSelected() {
  const current = profile.value;
  if (!current || !chosen.value.length) return;
  working.value = true;
  const listed = new Set(chosen.value.map((e) => e.listed).filter(Boolean));
  const rules = [...current.rules];
  for (const entry of chosen.value.filter((e) => !e.listed && resolved(e) && e.status === 'allowed')) {
    rules.push({ id: randomId('r-'), action: 'deny', priority: 90, match: { type: 'path', value: entry.path } });
  }
  const next = { ...current, rules, apps: (current.apps ?? []).filter((a) => !listed.has(a.id)) };
  const saved = await attempt(() => call('profiles.save', { profile: next }), t('{n} programs blocked in {profile}.', { n: chosen.value.length, profile: profileLabel(current) }));
  working.value = false;
  if (saved) {
    selected.value = new Set();
    await loadProfiles();
    await load();
  }
}

function afterObservation() {
  observing.value = undefined;
  load();
}
</script>

<template>
  <div class="page" :class="{ selecting: selected.size > 0 }">
    <PageHeader :title="t('Applications')" :subtitle="t('Choose what runs in a profile. Allowing a program also lets you decide what it may start.')">
      <button v-if="scanning" class="btn" @click="cancelRefresh"><span class="spinner" />{{ t('Updating library…') }}</button>
      <button v-else class="btn" :disabled="loading" @click="refresh"><Icon name="refresh" />{{ t('Refresh library') }}</button>
      <Menu :text="t('Add')" :label="t('Add programs')">
        <button role="menuitem" @click="addPrograms"><Icon name="plus" />{{ t('Add application…') }}</button>
        <button role="menuitem" @click="tools = 'folder'"><Icon name="apps" />{{ t('Add by folder…') }}</button>
        <hr />
        <button role="menuitem" @click="tools = 'sources'"><Icon name="tool" />{{ t('Sources and tools…') }}</button>
        <button role="menuitem" @click="tools = 'diagnose'"><Icon name="search" />{{ t('Why is a program missing?') }}</button>
      </Menu>
    </PageHeader>

    <div v-if="failure" class="banner bad" role="alert"><Icon name="alert" /><div class="grow"><strong>{{ t('The library could not be loaded.') }}</strong><div>{{ failure }}</div></div><button class="btn sm" @click="load()">{{ t('Try again') }}</button></div>

    <div class="toolbar">
      <div class="field" style="flex-direction: row; align-items: center; gap: 8px">
        <label for="apps-profile" style="white-space: nowrap">{{ t('Profile') }}</label>
        <select id="apps-profile" v-model="profileId" class="select"><option v-for="p in app.profiles" :key="p.id" :value="p.id">{{ profileLabel(p) }}</option></select>
      </div>
      <div class="search grow" style="min-width: 220px"><Icon name="search" /><input v-model="query" class="input" :placeholder="t('Search by name, publisher or path')" :aria-label="t('Search applications')" /></div>
      <select v-if="platforms.length" v-model="platform" class="select" :aria-label="t('Platform')">
        <option value="">{{ t('Any platform') }}</option>
        <option v-for="item in platforms" :key="item" :value="item">{{ platformLabel(item) }}</option>
      </select>
      <select v-if="volumes.length > 1" v-model="volume" class="select" :aria-label="t('Drive')">
        <option value="">{{ t('Any drive') }}</option>
        <option v-for="item in volumes" :key="item" :value="item">{{ item }}</option>
      </select>
      <select v-model="sort" class="select" :aria-label="t('Sort')">
        <option value="name">{{ t('Sort: name') }}</option>
        <option value="publisher">{{ t('Sort: publisher') }}</option>
        <option value="status">{{ t('Sort: status') }}</option>
        <option value="blocks">{{ t('Sort: most blocked') }}</option>
        <option value="recent">{{ t('Sort: recently found') }}</option>
      </select>
      <div class="seg" role="group" :aria-label="t('View')">
        <button :aria-pressed="view === 'grid'" :title="t('Grid')" @click="view = 'grid'"><Icon name="grid" :size="16" /></button>
        <button :aria-pressed="view === 'list'" :title="t('List')" @click="view = 'list'"><Icon name="list" :size="16" /></button>
      </div>
    </div>

    <div class="row wrap">
      <div class="chips" role="group" :aria-label="t('Status')">
        <button class="chip" :aria-pressed="status === 'all'" @click="status = 'all'">{{ t('All') }} · {{ listed.length }}</button>
        <button class="chip" :aria-pressed="status === 'allowed'" @click="status = 'allowed'">{{ t('Allowed') }} · {{ totals.allowed }}</button>
        <button class="chip" :aria-pressed="status === 'blocked'" @click="status = 'blocked'">{{ t('Blocked') }} · {{ totals.blocked }}</button>
        <button v-if="totals.attention" class="chip" :aria-pressed="status === 'attention'" @click="status = 'attention'">{{ t('Needs attention') }} · {{ totals.attention }}</button>
        <button v-if="totals.unresolved" class="chip" :aria-pressed="status === 'unresolved'" @click="status = 'unresolved'">{{ t('Choose program') }} · {{ totals.unresolved }}</button>
      </div>
      <div class="chips" role="group" :aria-label="t('Category')">
        <button class="chip" :aria-pressed="category === ''" @click="category = ''">{{ t('Any category') }}</button>
        <button v-for="[id, count] in categories" :key="id" class="chip" :aria-pressed="category === id" @click="category = category === id ? '' : id">{{ categoryLabel(id) }} · {{ count }}</button>
      </div>
    </div>

    <div class="row wrap">
      <div class="chips" role="group" :aria-label="t('Type')">
        <button class="chip" :aria-pressed="kind === 'all'" @click="kind = 'all'">{{ t('Any type') }}</button>
        <button class="chip" :aria-pressed="kind === 'installed'" @click="kind = kind === 'installed' ? 'all' : 'installed'">{{ t('Installed') }} · {{ kindCounts.installed }}</button>
        <button class="chip" :aria-pressed="kind === 'portable'" @click="kind = kind === 'portable' ? 'all' : 'portable'">{{ t('Portable') }} · {{ kindCounts.portable }}</button>
        <button class="chip" :aria-pressed="kind === 'store'" @click="kind = kind === 'store' ? 'all' : 'store'">{{ t('Microsoft Store') }} · {{ kindCounts.store }}</button>
        <button v-if="kindCounts.manual" class="chip" :aria-pressed="kind === 'manual'" @click="kind = kind === 'manual' ? 'all' : 'manual'">{{ t('Added by hand') }} · {{ kindCounts.manual }}</button>
        <button v-if="kindCounts.recent" class="chip" :aria-pressed="kind === 'recent'" @click="kind = kind === 'recent' ? 'all' : 'recent'">{{ t('Found recently') }} · {{ kindCounts.recent }}</button>
        <button v-if="kindCounts.dependencies" class="chip" :aria-pressed="kind === 'dependencies'" @click="kind = kind === 'dependencies' ? 'all' : 'dependencies'">{{ t('With dependencies') }} · {{ kindCounts.dependencies }}</button>
        <button v-if="kindCounts.uncategorized" class="chip" :aria-pressed="kind === 'uncategorized'" @click="kind = kind === 'uncategorized' ? 'all' : 'uncategorized'">{{ t('Uncategorized') }} · {{ kindCounts.uncategorized }}</button>
      </div>
      <label v-if="hiddenCount" class="check" style="padding: 2px 8px"><input v-model="showTools" type="checkbox" />{{ t('Show tools and helpers ({n})', { n: hiddenCount }) }}</label>
    </div>

    <div class="counters">
      <span><strong>{{ totals.allowed }}</strong> {{ t('allowed') }}</span>
      <span><strong>{{ totals.blocked }}</strong> {{ t('blocked') }}</span>
      <span><strong>{{ totals.withDependencies }}</strong> {{ t('with dependencies') }}</span>
      <span v-if="totals.attention"><strong>{{ totals.attention }}</strong> {{ t('need attention') }}</span>
      <span class="faint">{{ t('Showing {n}', { n: visible.length }) }}</span>
      <span v-if="library.refreshing.value && !loading" class="spinner" role="status" :aria-label="t('Updating library…')" />
      <button v-if="visible.length" class="btn sm ghost" @click="selectAllVisible">{{ t('Select all shown') }}</button>
    </div>

    <Skeleton v-if="loading || (scanning && !entries.length)" :kind="view === 'grid' ? 'app-grid' : 'app-list'" :count="view === 'grid' ? 12 : 9" />
    <EmptyState v-else-if="!visible.length" icon="apps" :text="t('No applications match your filters.')" />

    <VirtualList v-else-if="view === 'grid'" :items="visible" :item-key="key" :item-height="188" :min-width="172" :gap="12" v-slot="{ item: entry }">
      <div class="app-card" role="button" tabindex="0" :aria-selected="selected.has(key(entry))" @click="open(entry)" @keydown.enter="open(entry)">
        <input class="pick" type="checkbox" :checked="selected.has(key(entry))" :aria-label="t('Select {name}', { name: entry.name })" @click.stop @change="toggle(entry)" />
        <span class="state">
          <span v-if="entry.pending" class="mini warn" :title="t('Waiting for your decision')"><Icon name="alert" />{{ entry.pending }}</span>
          <span v-if="entry.conflicts" class="mini bad" :title="t('A rule conflicts with this program')"><Icon name="alert" /></span>
          <span class="state-dot" :class="entry.status" :title="stateText(entry)" />
        </span>
        <AppIcon :path="iconPath(entry)" :sources="entry.icons" :name="entry.name" :size="52" />
        <div class="name truncate" :title="entry.name">{{ entry.name }}</div>
        <div class="sub truncate" :title="entry.company">{{ entry.company || sourceLabel(entry.source) }}</div>
        <div class="minis">
          <span v-if="entry.offline" class="mini warn">{{ t('Drive offline') }}</span>
          <span v-else-if="!resolved(entry)" class="mini warn">{{ t('Choose program') }}</span>
          <span v-if="entry.platform && entry.platform !== 'store'" class="mini">{{ platformLabel(entry.platform) }}</span>
          <span v-if="entry.volume" class="mini">{{ entry.volume }}</span>
          <span v-if="entry.listed" class="mini"><Icon name="link" />{{ t('{n} dependencies', { n: entry.dependencies ?? 0 }) }}</span>
          <span v-if="entry.kind !== 'installed' && !entry.platform" class="mini">{{ kindLabel(entry.kind) }}</span>
        </div>
      </div>
    </VirtualList>

    <div v-else class="app-list">
     <VirtualList :items="visible" :item-key="key" :item-height="56" v-slot="{ item: entry }">
      <div class="app-row" role="button" tabindex="0" :aria-selected="selected.has(key(entry))" @click="open(entry)" @keydown.enter="open(entry)">
        <input class="pick" type="checkbox" :checked="selected.has(key(entry))" :aria-label="t('Select {name}', { name: entry.name })" @click.stop @change="toggle(entry)" />
        <AppIcon :path="iconPath(entry)" :sources="entry.icons" :name="entry.name" :size="34" />
        <div style="min-width: 0"><strong class="truncate" style="display: block" :title="entry.name">{{ entry.name }}</strong><span class="hint truncate" style="display: block" :title="entry.company">{{ entry.company || sourceLabel(entry.source) }}</span></div>
        <div class="hint mono truncate hide-narrow" :title="entry.path || entry.install_dir">{{ entry.path || entry.install_dir }}</div>
        <div class="row hide-narrow" style="gap: 6px">
          <span v-if="entry.kind !== 'installed'" class="mini">{{ kindLabel(entry.kind) }}</span>
          <span v-if="entry.listed" class="mini"><Icon name="link" />{{ entry.dependencies ?? 0 }}</span>
          <span v-if="entry.pending" class="mini warn"><Icon name="alert" />{{ entry.pending }}</span>
          <span v-if="entry.conflicts" class="mini bad"><Icon name="alert" /></span>
          <span v-if="entry.blocks" class="mini">{{ t('{n} blocks', { n: entry.blocks }) }}</span>
        </div>
        <span class="badge" :class="entry.status === 'allowed' ? 'ok' : entry.status === 'blocked' ? 'bad' : 'warn'">{{ stateText(entry) }}</span>
      </div>
     </VirtualList>
    </div>

    <div v-if="selected.size" class="bulkbar" role="toolbar" :aria-label="t('Selected programs')">
      <strong>{{ t('{n} selected', { n: selected.size }) }}</strong>
      <button class="btn sm primary" :disabled="working" @click="allowSelected"><Icon name="check" />{{ t('Allow in {profile}', { profile: profileLabel(profile)  }) }}</button>
      <button class="btn sm danger" :disabled="working" @click="blockSelected"><Icon name="x" />{{ t('Block') }}</button>
      <button class="btn sm ghost" @click="selected = new Set()">{{ t('Clear') }}</button>
    </div>

    <EntryResolver v-if="active && !resolved(active)" :key="active.id" :entry="active" @close="active = undefined" @changed="load(true)" />
    <AppPanel v-else-if="active" :key="active.id + profileId" :entry="active" :profile-id="profileId" @close="active = undefined" @changed="load()" @observe="observing = active" />
    <LibraryTools v-if="tools" :start="tools" @close="tools = undefined" @changed="load(true)" />
    <DiscoveryModal v-if="observing" :entry="observing" :profile-id="profileId" @close="observing = undefined" @saved="afterObservation" />
  </div>
</template>
