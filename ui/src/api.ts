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
    pending.set(id, { resolve, reject });
    bridge.postMessage(JSON.stringify({ id, cmd, args, ...(token ? { token } : {}) }));
  });
}

export const call = <T = any>(cmd: string, args: object = {}) => send<T>(cmd, args, auth.token || undefined);
export const host = <T = any>(cmd: string, args: object = {}) => send<T>(`host.${cmd}`, args);

const icons = new Map<string, Promise<string>>();

export function icon(path: string, size = 48): Promise<string> {
  const key = `${size}|${path.toLowerCase()}`;
  let known = icons.get(key);
  if (!known) {
    if (icons.size > 1500) icons.clear();
    known = host<{ png: string }>('icon', { path, size }).then((reply) => reply.png, () => '');
    icons.set(key, known);
  }
  return known;
}
