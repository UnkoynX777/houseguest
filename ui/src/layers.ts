import { onBeforeUnmount, onDeactivated, onMounted, type Ref } from 'vue';

// Every drawer, dialog and menu that sits above the page registers here, so that one place decides which of them
// answers the keyboard, which parts of the document are reachable and where the focus goes back to afterwards.
interface Layer {
  root: Ref<HTMLElement | undefined>;
  close: () => void;
  modal: boolean;
  opener: Element | null;
}

const stack: Layer[] = [];
const focusable = 'a[href], button:not([disabled]), input:not([disabled]):not([type="hidden"]), select:not([disabled]), textarea:not([disabled]), summary, [tabindex]:not([tabindex="-1"])';

function topModal(): Layer | undefined {
  return [...stack].reverse().find((layer) => layer.modal);
}

function reachable() {
  const top = topModal();
  for (const layer of stack) if (layer.modal && layer.root.value) layer.root.value.inert = layer !== top;
  const page = document.getElementById('app');
  if (page) page.inert = !!top;
}

function onKey(event: KeyboardEvent) {
  const top = stack.at(-1);
  if (!top || event.defaultPrevented) return;
  if (event.key === 'Escape' && !event.isComposing) {
    event.preventDefault();
    event.stopPropagation();
    top.close();
    return;
  }
  if (event.key !== 'Tab' || !top.modal || !top.root.value) return;
  const items = [...top.root.value.querySelectorAll<HTMLElement>(focusable)].filter((item) => item.offsetParent !== null || item === document.activeElement);
  if (!items.length) {
    event.preventDefault();
    top.root.value.focus();
    return;
  }
  const first = items[0];
  const last = items[items.length - 1];
  const inside = top.root.value.contains(document.activeElement);
  if (event.shiftKey && (document.activeElement === first || !inside)) {
    event.preventDefault();
    last.focus();
  } else if (!event.shiftKey && (document.activeElement === last || !inside)) {
    event.preventDefault();
    first.focus();
  }
}

function add(layer: Layer) {
  if (!stack.length) window.addEventListener('keydown', onKey, true);
  stack.push(layer);
  reachable();
}

function remove(layer: Layer) {
  const at = stack.indexOf(layer);
  if (at < 0) return;
  stack.splice(at, 1);
  if (!stack.length) window.removeEventListener('keydown', onKey, true);
  reachable();
  if (layer.modal && layer.opener instanceof HTMLElement && layer.opener.isConnected && !topModal()) layer.opener.focus({ preventScroll: true });
}

// Puts a layer on top of the others until the returned function is called. A modal layer makes everything below it
// unreachable and gets the focus; a modeless one (a menu) only answers Escape first.
export function enterLayer(root: Ref<HTMLElement | undefined>, close: () => void, options: { modal?: boolean; focus?: string } = {}): () => void {
  const layer: Layer = { root, close, modal: options.modal ?? true, opener: document.activeElement };
  add(layer);
  if (layer.modal && root.value) {
    const target = options.focus ? root.value.querySelector<HTMLElement>(options.focus) : null;
    (target ?? root.value).focus({ preventScroll: true });
  }
  return () => remove(layer);
}

export function useLayer(root: Ref<HTMLElement | undefined>, close: () => void, options: { modal?: boolean; focus?: string } = {}) {
  let leave: (() => void) | undefined;
  onMounted(() => (leave = enterLayer(root, close, options)));
  onBeforeUnmount(() => leave?.());
  // A page kept alive in the background must not leave its dialogs over the page that replaced it.
  onDeactivated(() => close());
}
