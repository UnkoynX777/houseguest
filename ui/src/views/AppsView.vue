<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';

import { call, host } from '../api';
import AppIcon from '../components/AppIcon.vue';
import AppPanel from '../components/AppPanel.vue';
import DiscoveryModal from '../components/DiscoveryModal.vue';
import EmptyState from '../components/EmptyState.vue';
import LibraryTools from '../components/LibraryTools.vue';
import Menu from '../components/Menu.vue';
import Icon from '../components/Icon.vue';
import PageHeader from '../components/PageHeader.vue';
import { categoryLabel, categoryOf, kindLabel, sourceLabel } from '../explain';
import { t } from '../i18n';
import { app, attempt, describe, loadProfiles, notify, profileLabel, randomId } from '../store';
import type { Application, LibraryApp } from '../types';

type View = 'grid' | 'list';
type Status = 'all' | 'allowed' | 'blocked' | 'attention';
type Sort = 'name' | 'publisher' | 'status' | 'blocks' | 'recent';
type Kind = 'all' | 'installed' | 'portable' | 'store' | 'manual' | 'recent' | 'dependencies' | 'uncategorized';

const library = ref<LibraryApp[]>([]);
const loading = ref(true);
const profileId = ref('');
const view = ref<View>(readView());
const query = ref('');
const status = ref<Status>('all');
const category = ref('');
const sort = ref<Sort>('name');
const selected = ref(new Set<string>());
const active = ref<LibraryApp>();
const observing = ref<LibraryApp>();
const working = ref(false);
const kind = ref<Kind>('all');
const showTools = ref(false);
const scanning = ref(false);
const failure = ref('');
const tools = ref<'sources' | 'added' | 'folder' | 'diagnose'>();
let poller: ReturnType<typeof setTimeout> | undefined;
const recentWindow = 7 * 86400;

function readView(): View {
  try {
    return localStorage.getItem('apps.view') === 'list' ? 'list' : 'grid';
  } catch {
    return 'grid';
  }
}

watch(view, (value) => {
  try {
    localStorage.setItem('apps.view', value);
  } catch {
    /* storage can be unavailable */
  }
});

const profile = computed(() => app.profiles.find((p) => p.id === profileId.value));
const key = (entry: LibraryApp) => entry.path.toLowerCase();
const attention = (entry: LibraryApp) => !!(entry.pending || entry.conflicts);

async function load(quiet = false) {
  if (!profileId.value) return;
  if (!quiet) loading.value = true;
  try {
    const result = await call<{ apps: LibraryApp[]; scanning: boolean }>('apps.library', { profile_id: profileId.value });
    library.value = result.apps;
    scanning.value = result.scanning;
    failure.value = '';
  } catch (error) {
    failure.value = describe(error);
    scanning.value = false;
  }
  loading.value = false;
  if (active.value) active.value = library.value.find((entry) => key(entry) === key(active.value!)) ?? active.value;
  clearTimeout(poller);
  if (scanning.value) poller = setTimeout(() => load(true), 1500);
}

async function refresh() {
  const started = await attempt(() => call('apps.refresh'));
  if (started) {
    scanning.value = true;
    clearTimeout(poller);
    poller = setTimeout(() => load(true), 600);
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

onMounted(async () => {
  await attempt(loadProfiles);
  profileId.value = app.profiles.find((p) => p.id === app.ruleProfileId)?.id ?? app.profiles[0]?.id ?? '';
  await load();
});
onBeforeUnmount(() => clearTimeout(poller));
watch(profileId, () => {
  selected.value = new Set();
  load();
});

const listed = computed(() => library.value.filter((entry) => showTools.value || entry.role === 'application'));
const hiddenCount = computed(() => library.value.length - library.value.filter((entry) => entry.role === 'application').length);
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
  const needle = query.value.trim().toLowerCase();
  const rows = listed.value.filter(
    (entry) =>
      matchesKind(entry) &&
      (status.value === 'all' || (status.value === 'attention' ? attention(entry) : entry.status === status.value)) &&
      (!category.value || categoryOf(entry) === category.value) &&
      (!needle || `${entry.name} ${entry.company} ${entry.path} ${entry.description}`.toLowerCase().includes(needle)),
  );
  const order: Record<Sort, (a: LibraryApp, b: LibraryApp) => number> = {
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

const chosen = computed(() => library.value.filter((e) => selected.value.has(key(e))));

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
  const targets = chosen.value.filter((e) => !e.listed);
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
  for (const entry of chosen.value.filter((e) => !e.listed && e.status === 'allowed')) {
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
      <button v-if="visible.length" class="btn sm ghost" @click="selectAllVisible">{{ t('Select all shown') }}</button>
    </div>

    <div v-if="loading || (scanning && !library.length)" class="app-grid"><div v-for="n in 12" :key="n" class="skeleton" style="height: 150px" /></div>
    <EmptyState v-else-if="!visible.length" icon="apps" :text="t('No applications match your filters.')" />

    <div v-else-if="view === 'grid'" class="app-grid">
      <div v-for="entry in visible" :key="entry.path" class="app-card" role="button" tabindex="0" :aria-selected="selected.has(key(entry))" @click="open(entry)" @keydown.enter="open(entry)">
        <input class="pick" type="checkbox" :checked="selected.has(key(entry))" :aria-label="t('Select {name}', { name: entry.name })" @click.stop @change="toggle(entry)" />
        <span class="state">
          <span v-if="entry.pending" class="mini warn" :title="t('Waiting for your decision')"><Icon name="alert" />{{ entry.pending }}</span>
          <span v-if="entry.conflicts" class="mini bad" :title="t('A rule conflicts with this program')"><Icon name="alert" /></span>
          <span class="state-dot" :class="entry.status" :title="entry.status === 'allowed' ? t('Allowed') : t('Blocked')" />
        </span>
        <AppIcon :path="entry.path" :name="entry.name" :size="52" />
        <div class="name truncate" :title="entry.name">{{ entry.name }}</div>
        <div class="sub truncate" :title="entry.company">{{ entry.company || sourceLabel(entry.source) }}</div>
        <span v-if="entry.listed" class="mini"><Icon name="link" />{{ t('{n} dependencies', { n: entry.dependencies ?? 0 }) }}</span>
        <span v-if="entry.kind !== 'installed'" class="mini">{{ kindLabel(entry.kind) }}</span>
      </div>
    </div>

    <div v-else class="app-list">
      <div v-for="entry in visible" :key="entry.path" class="app-row" role="button" tabindex="0" :aria-selected="selected.has(key(entry))" @click="open(entry)" @keydown.enter="open(entry)">
        <input class="pick" type="checkbox" :checked="selected.has(key(entry))" :aria-label="t('Select {name}', { name: entry.name })" @click.stop @change="toggle(entry)" />
        <AppIcon :path="entry.path" :name="entry.name" :size="34" />
        <div style="min-width: 0"><strong class="truncate" style="display: block">{{ entry.name }}</strong><span class="hint truncate" style="display: block">{{ entry.company || sourceLabel(entry.source) }}</span></div>
        <div class="hint mono truncate hide-narrow" :title="entry.path">{{ entry.path }}</div>
        <div class="row hide-narrow" style="gap: 6px">
          <span v-if="entry.kind !== 'installed'" class="mini">{{ kindLabel(entry.kind) }}</span>
          <span v-if="entry.listed" class="mini"><Icon name="link" />{{ entry.dependencies ?? 0 }}</span>
          <span v-if="entry.pending" class="mini warn"><Icon name="alert" />{{ entry.pending }}</span>
          <span v-if="entry.conflicts" class="mini bad"><Icon name="alert" /></span>
          <span v-if="entry.blocks" class="mini">{{ t('{n} blocks', { n: entry.blocks }) }}</span>
        </div>
        <span class="badge" :class="entry.status === 'allowed' ? 'ok' : 'bad'">{{ entry.status === 'allowed' ? t('Allowed') : t('Blocked') }}</span>
      </div>
    </div>

    <div v-if="selected.size" class="bulkbar" role="toolbar" :aria-label="t('Selected programs')">
      <strong>{{ t('{n} selected', { n: selected.size }) }}</strong>
      <button class="btn sm primary" :disabled="working" @click="allowSelected"><Icon name="check" />{{ t('Allow in {profile}', { profile: profileLabel(profile)  }) }}</button>
      <button class="btn sm danger" :disabled="working" @click="blockSelected"><Icon name="x" />{{ t('Block') }}</button>
      <button class="btn sm ghost" @click="selected = new Set()">{{ t('Clear') }}</button>
    </div>

    <AppPanel v-if="active" :key="active.path + profileId" :entry="active" :profile-id="profileId" @close="active = undefined" @changed="load()" @observe="observing = active" />
    <LibraryTools v-if="tools" :start="tools" @close="tools = undefined" @changed="load(true)" />
    <DiscoveryModal v-if="observing" :entry="observing" :profile-id="profileId" @close="observing = undefined" @saved="afterObservation" />
  </div>
</template>
