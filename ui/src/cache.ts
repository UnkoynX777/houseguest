import { ref, shallowReactive } from 'vue';

// One place for data that is shown but never decides anything. Every slot belongs to a page, remembers when it
// was fetched and which kind of change makes it stale, and is refreshed in the background so a page that was
// visited before opens with its last data instead of an empty state. Writes and privileged reads do not go
// through here: they call the service directly and then mark the affected slots stale.

export type Phase = 'empty' | 'loading' | 'ready' | 'refreshing' | 'error';
export type Dep = 'policy' | 'catalog' | 'history' | 'session';

export interface Slot<T> {
  data: T | undefined;
  phase: Phase;
  error: string;
  fetchedAt: number;
  stale: boolean;
}

interface Item {
  slot: Slot<any>;
  deps: Dep[];
  token: number;
  signature: string;
  flight?: Promise<void>;
  used: number;
}

const maxSlots = 48;
const items = new Map<string, Item>();

function fingerprint(value: unknown): string {
  try {
    return JSON.stringify(value) ?? '';
  } catch {
    return String(Math.random());
  }
}

function evict() {
  if (items.size <= maxSlots) return;
  const idle = [...items.entries()].filter(([, item]) => !item.flight).sort((a, b) => a[1].used - b[1].used);
  for (const [key] of idle.slice(0, items.size - maxSlots)) items.delete(key);
}

function ensure(key: string, deps: Dep[]): Item {
  let item = items.get(key);
  if (!item) {
    item = {
      slot: shallowReactive({ data: undefined, phase: 'empty', error: '', fetchedAt: 0, stale: false } as Slot<any>),
      deps,
      token: 0,
      signature: '',
      used: 0,
    };
    items.set(key, item);
    evict();
  }
  item.used = Date.now();
  return item;
}

export interface Resource<T> {
  slot: Slot<T>;
  load(force?: boolean): Promise<void>;
  invalidate(): void;
}

export function resource<T>(key: string, loader: () => Promise<T>, deps: Dep[] = [], maxAge = 120_000): Resource<T> {
  const item = ensure(key, deps);
  const slot = item.slot as Slot<T>;

  const load = (force = false): Promise<void> => {
    const fresh = slot.data !== undefined && !slot.stale && Date.now() - slot.fetchedAt < maxAge;
    if (fresh && !force) return Promise.resolve();
    if (item.flight && !force) return item.flight;
    const token = ++item.token;
    slot.phase = slot.data === undefined ? 'loading' : 'refreshing';
    const flight = loader()
      .then((value) => {
        if (token !== item.token) return;
        const signature = fingerprint(value);
        if (signature !== item.signature) {
          item.signature = signature;
          slot.data = value;
        }
        slot.fetchedAt = Date.now();
        slot.stale = false;
        slot.error = '';
        slot.phase = 'ready';
      })
      .catch((error: unknown) => {
        if (token !== item.token) return;
        slot.error = error instanceof Error ? error.message : String(error);
        slot.phase = slot.data === undefined ? 'error' : 'ready';
      })
      .finally(() => {
        if (item.flight === flight) item.flight = undefined;
      });
    item.flight = flight;
    return flight;
  };

  return { slot, load, invalidate: () => (slot.stale = true) };
}

export function markStale(dep: Dep) {
  for (const item of items.values()) {
    if (item.deps.includes(dep)) item.slot.stale = true;
  }
}

export function forget(prefix: string) {
  for (const key of [...items.keys()]) {
    if (key.startsWith(prefix)) items.delete(key);
  }
}

// Locking the interface drops everything that was shown, so nothing from a previous owner session is in memory.
export function clearAll() {
  items.clear();
}

export function cacheSize(): number {
  return items.size;
}

const generations: Partial<Record<Dep, number>> = {};

// The service reports a counter per kind of data in every status. A counter that moved since the last look is
// the event that makes the matching slots stale.
// Moves whenever the library changed on the machine, so pictures that were read from files are asked for again.
export const catalogEpoch = ref(0);

export function syncGenerations(reported: Partial<Record<Dep, number>> | undefined) {
  if (!reported) return;
  for (const dep of ['policy', 'catalog', 'history'] as const) {
    const value = reported[dep];
    if (value === undefined) continue;
    if (generations[dep] !== undefined && generations[dep] !== value) {
      markStale(dep);
      if (dep === 'catalog') catalogEpoch.value++;
    }
    generations[dep] = value;
  }
}

export function resetGenerations() {
  for (const key of Object.keys(generations)) delete generations[key as Dep];
}
