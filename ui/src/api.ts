import { reactive } from 'vue';

interface Bridge {
  postMessage(message: string): void;
  addEventListener(type: 'message', handler: (event: { data: unknown }) => void): void;
}

export class ApiError extends Error {
  constructor(
    public code: string,
    message: string,
    public details: Record<string, any> = {},
  ) {
    super(message);
  }
}

export const auth = reactive({ token: '' });

const bridge = (window as unknown as { chrome?: { webview?: Bridge } }).chrome?.webview;
const pending = new Map<number, { resolve: (value: any) => void; reject: (error: ApiError) => void }>();
let sequence = 0;

type HostEvent = { event: string; [key: string]: unknown };
const listeners = new Set<(event: HostEvent) => void>();

export function onHostEvent(listener: (event: HostEvent) => void) {
  listeners.add(listener);
}

bridge?.addEventListener('message', (event) => {
  const reply = typeof event.data === 'string' ? JSON.parse(event.data) : event.data;
  if (typeof reply.event === 'string') {
    listeners.forEach((listener) => listener(reply));
    return;
  }
  const waiter = pending.get(reply.id);
  if (!waiter) return;
  pending.delete(reply.id);
  if (reply.ok) {
    waiter.resolve(reply.data);
    return;
  }
  if (reply.error.code === 'unauthorized') auth.token = '';
  waiter.reject(new ApiError(reply.error.code, reply.error.message, reply.error.details));
});

function send<T>(cmd: string, args: object, token?: string): Promise<T> {
  return new Promise((resolve, reject) => {
    if (!bridge) {
      reject(new ApiError('no_host', 'This interface must run inside the Houseguest application.'));
      return;
    }
    const id = ++sequence;
    const timer = setTimeout(() => {
      if (pending.delete(id)) reject(new ApiError('timeout', 'The service did not answer in time.'));
    }, 30000);
    pending.set(id, {
      resolve: (value) => (clearTimeout(timer), resolve(value)),
      reject: (error) => (clearTimeout(timer), reject(error)),
    });
    bridge.postMessage(JSON.stringify({ id, cmd, args, ...(token ? { token } : {}) }));
  });
}

export const call = <T = any>(cmd: string, args: object = {}) => send<T>(cmd, args, auth.token || undefined);
export const host = <T = any>(cmd: string, args: object = {}) => send<T>(`host.${cmd}`, args);

export interface IconSource {
  kind: string;
  path: string;
  index: number;
}

const icons = new Map<string, Promise<string>>();
const hints = new Map<string, IconSource[]>();
let epoch = -1;

// Pixels to ask for: the size on screen times the display scale, in the steps the host caches.
export function iconPixels(size: number): number {
  const wanted = Math.ceil(size * (window.devicePixelRatio || 1));
  return [32, 48, 64, 96, 128, 192, 256].find((step) => step >= wanted) ?? 256;
}

// The library knows where each application's pictures are declared; every other page shows the same
// program from a path alone, so what the library learned is shared with them.
export function rememberIconSources(apps: { path?: string; icons?: IconSource[] }[]) {
  for (const entry of apps) {
    if (entry.path && entry.icons?.length) hints.set(entry.path.toLowerCase(), entry.icons);
  }
}

export const iconSourcesFor = (path: string): IconSource[] => hints.get(path.toLowerCase()) ?? [];

export function icon(path: string, size: number, sources: IconSource[], generation: number): Promise<string> {
  if (generation !== epoch) {
    icons.clear();
    epoch = generation;
  }
  const key = `${size}|${path.toLowerCase()}|${sources.map((s) => `${s.kind}:${s.path}:${s.index}`).join('|')}`;
  let known = icons.get(key);
  if (!known) {
    if (icons.size > 1500) icons.clear();
    known = host<{ png: string }>('icon', { path, size, sources }).then((reply) => reply.png, () => '');
    icons.set(key, known);
  }
  return known;
}
