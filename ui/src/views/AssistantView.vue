<script setup lang="ts">
import { computed, nextTick, onActivated, onBeforeUnmount, onMounted, ref } from 'vue';

import { call } from '../api';
import Icon from '../components/Icon.vue';
import PageHeader from '../components/PageHeader.vue';
import Toggle from '../components/Toggle.vue';
import { t } from '../i18n';
import { renderMarkdown } from '../markdown';
import { app, attempt, describe, notify } from '../store';

interface Settings {
  enabled: boolean;
  has_key: boolean;
  model: string;
  share_context: boolean;
  limits: { per_minute: number; per_day: number; used_today: number };
  privacy: string;
}
interface Proposal {
  id: string;
  chat_id: string;
  kind: string;
  summary: string;
  status: 'pending' | 'applied' | 'discarded' | 'rolled_back' | 'expired';
  preview: { programs?: string[]; findings?: { message?: string; severity?: string }[]; affects_active_session?: boolean; profile?: string };
}
interface Message {
  role: 'user' | 'assistant';
  text: string;
  error?: boolean;
  proposals?: string[];
  live?: boolean;
  tools?: string[];
}

const settings = ref<Settings | null>(null);
const models = ref<string[]>([]);
const chats = ref<{ id: string; title: string; updated: number }[]>([]);
const chatId = ref('');
const messages = ref<Message[]>([]);
const proposals = ref<Proposal[]>([]);
const draft = ref('');
const key = ref('');
const sending = ref(false);
const job = ref('');
const showSettings = ref(false);
const log = ref<HTMLElement>();
let cancelled = false;

const ready = computed(() => !!settings.value?.enabled && settings.value.has_key);
const suggestions = computed(() => [
  t('Which programs are installed on drive D:?'),
  t('Create a profile for a friend who only needs Steam and Discord.'),
  t('Show what was blocked today.'),
  t('Why is a program blocked in my active profile?'),
]);

async function load() {
  settings.value = await call<Settings>('ai.status').catch(() => null);
  if (!settings.value) return;
  showSettings.value = !(settings.value.enabled && settings.value.has_key);
  chats.value = await call('ai.chat.list').catch(() => []);
  proposals.value = await call('ai.proposals').catch(() => []);
  if (settings.value.has_key && models.value.length === 0) models.value = await call<{ models: string[] }>('ai.models').then((r) => r.models, () => []);
}
onMounted(load);
onActivated(load);
onBeforeUnmount(() => (cancelled = true));

const scroll = async () => {
  await nextTick();
  if (log.value) log.value.scrollTop = log.value.scrollHeight;
};

async function saveKey() {
  const reply = await attempt(() => call<{ models: string[] }>('ai.key.set', { api_key: key.value.trim() }), t('The key works and is stored protected.'));
  if (!reply) return;
  key.value = '';
  models.value = reply.models;
  await load();
}

async function removeKey() {
  await attempt(() => call('ai.key.clear'), t('The key was removed and the assistant is off.'));
  models.value = [];
  await load();
}

async function change(changes: object) {
  await attempt(() => call('ai.settings.set', changes));
  await load();
}

async function open(id: string) {
  chatId.value = id;
  const chat = await attempt(() => call<{ messages: Message[] }>('ai.chat.get', { id }));
  if (chat) messages.value = chat.messages;
  await scroll();
}

function fresh() {
  chatId.value = '';
  messages.value = [];
}

async function remove() {
  if (!chatId.value) return;
  await attempt(() => call('ai.chat.delete', { id: chatId.value }));
  fresh();
  await load();
}

async function send(text = draft.value) {
  text = text.trim();
  if (!text || sending.value || !ready.value) return;
  draft.value = '';
  sending.value = true;
  cancelled = false;
  messages.value.push({ role: 'user', text });
  const answer: Message = { role: 'assistant', text: '', live: true, tools: [], proposals: [] };
  messages.value.push(answer);
  const context = settings.value?.share_context ? { view: app.route, ...app.context } : {};
  try {
    const started = await call<{ job: string }>('ai.chat.send', { chat_id: chatId.value, text, context });
    job.value = started.job;
    let since = 0;
    for (;;) {
      const reply = await call<{ events: any[]; next: number; done: boolean; chat_id: string }>('ai.job', { id: started.job, since });
      since = reply.next;
      for (const event of reply.events) {
        if (event.type === 'text') answer.text += event.delta;
        else if (event.type === 'tool') answer.tools!.push(event.name);
        else if (event.type === 'proposal') answer.proposals!.push(event.id);
        else if (event.type === 'error') {
          answer.error = true;
          answer.text = event.message;
        } else if (event.type === 'chat') chatId.value = event.chat_id;
      }
      await scroll();
      if (reply.done || cancelled) break;
      await new Promise((r) => setTimeout(r, 250));
    }
  } catch (error) {
    answer.error = true;
    answer.text = describe(error);
  } finally {
    answer.live = false;
    sending.value = false;
    job.value = '';
    await load();
    await scroll();
  }
}

async function stop() {
  if (job.value) await call('ai.cancel', { id: job.value }).catch(() => undefined);
}

const proposal = (id: string) => proposals.value.find((p) => p.id === id);

async function apply(p: Proposal) {
  if (await attempt(() => call('ai.proposal.apply', { id: p.id }), t('The change was applied.'))) {
    notify('info', t('You can undo it from this card.'));
  }
  await load();
}
async function discard(p: Proposal) {
  await attempt(() => call('ai.proposal.discard', { id: p.id }));
  await load();
}
async function undo(p: Proposal) {
  await attempt(() => call('ai.proposal.rollback', { id: p.id }), t('The change was undone.'));
  await load();
}
const statusText = (s: Proposal['status']) => ({ pending: t('Waiting for you'), applied: t('Applied'), discarded: t('Discarded'), rolled_back: t('Undone'), expired: t('Expired') })[s];
</script>

<template>
  <div class="page assistant">
    <PageHeader :title="t('Assistant')" :subtitle="t('Ask in plain words. It looks at the real state and proposes changes; you confirm them.')">
      <button class="btn sm ghost" @click="showSettings = !showSettings"><Icon name="settings" />{{ t('Setup') }}</button>
    </PageHeader>

    <section v-if="settings && showSettings" class="card stack">
      <div><h2>{{ t('Gemini') }}</h2>
        <p class="muted">{{ settings.privacy }}</p></div>
      <div class="row">
        <input v-model="key" class="input" type="password" autocomplete="off" spellcheck="false" :placeholder="settings.has_key ? t('A key is stored. Paste another to replace it.') : t('Gemini API key')" :aria-label="t('Gemini API key')" />
        <button class="btn" :disabled="key.trim().length < 20" @click="saveKey">{{ t('Check and save') }}</button>
        <button v-if="settings.has_key" class="btn danger" @click="removeKey">{{ t('Remove key') }}</button>
      </div>
      <p class="faint">{{ t('Create a key in Google AI Studio (aistudio.google.com). Free-tier keys may be used by Google to improve its products.') }}</p>
      <template v-if="settings.has_key">
        <div class="row"><div class="grow"><h2>{{ t('Model') }}</h2></div>
          <select class="select" style="width: 280px" :value="settings.model" :aria-label="t('Model')" @change="change({ model: ($event.target as HTMLSelectElement).value })">
            <option v-if="!models.includes(settings.model)" :value="settings.model">{{ settings.model }}</option>
            <option v-for="m in models" :key="m" :value="m">{{ m }}</option>
          </select></div>
        <Toggle :model-value="settings.enabled" :label="t('Turn the assistant on')" :hint="t('While it is off, nothing is sent to Google.')" @update:model-value="change({ enabled: $event })" />
        <Toggle :model-value="settings.share_context" :label="t('Share what I am looking at')" :hint="t('Sends the page name and the selected program or profile with each question.')" @update:model-value="change({ share_context: $event })" />
        <p class="faint">{{ t('Limits here: {a} questions per minute, {b} per day. Today: {c}.', { a: settings.limits.per_minute, b: settings.limits.per_day, c: settings.limits.used_today }) }}</p>
      </template>
    </section>

    <section v-if="ready" class="card chat">
      <div class="row chat-top">
        <select class="select" style="width: 240px" :value="chatId" :aria-label="t('Conversation')" @change="($event.target as HTMLSelectElement).value ? open(($event.target as HTMLSelectElement).value) : fresh()">
          <option value="">{{ t('New conversation') }}</option>
          <option v-if="chatId && !chats.some((c) => c.id === chatId)" :value="chatId">{{ t('Conversation') }}</option>
          <option v-for="c in chats" :key="c.id" :value="c.id">{{ c.title || t('Conversation') }}</option>
        </select>
        <span class="grow" />
        <button v-if="chatId" class="btn sm ghost" @click="remove"><Icon name="trash" />{{ t('Delete') }}</button>
      </div>

      <div ref="log" class="log" role="log" aria-live="polite">
        <div v-if="messages.length === 0" class="starter">
          <Icon name="assistant" :size="28" />
          <strong>{{ t('What would you like to do?') }}</strong>
          <div class="chips"><button v-for="s in suggestions" :key="s" class="chip" @click="send(s)">{{ s }}</button></div>
        </div>
        <div v-for="(m, i) in messages" :key="i" class="msg" :class="[m.role, { error: m.error }]">
          <div v-if="m.role === 'user'" class="bubble">{{ m.text }}</div>
          <div v-else class="bubble">
            <div v-if="m.tools?.length" class="tools"><span v-for="(tool, k) in m.tools" :key="k" class="badge">{{ tool.replace(/_/g, ' ') }}</span></div>
            <span v-if="m.live && !m.text" class="typing"><span /><span /><span /></span>
            <div v-else class="md" v-html="renderMarkdown(m.text)" />
            <template v-for="id in m.proposals ?? []" :key="id">
              <div v-if="proposal(id)" class="proposal">
                <div class="row"><strong class="grow">{{ proposal(id)!.summary }}</strong><span class="badge" :class="{ ok: proposal(id)!.status === 'applied', warn: proposal(id)!.status === 'pending' }">{{ statusText(proposal(id)!.status) }}</span></div>
                <ul v-if="proposal(id)!.preview.programs?.length" class="programs"><li v-for="p in proposal(id)!.preview.programs" :key="p">{{ p }}</li></ul>
                <p v-if="proposal(id)!.preview.affects_active_session" class="badge warn" style="height: auto; padding: 4px 9px; white-space: normal">{{ t('This profile is in use by the active session. The change takes effect immediately.') }}</p>
                <p v-for="(f, n) in proposal(id)!.preview.findings ?? []" :key="n" class="faint">{{ f.message }}</p>
                <div class="row">
                  <template v-if="proposal(id)!.status === 'pending'">
                    <button class="btn primary sm" @click="apply(proposal(id)!)"><Icon name="check" />{{ t('Apply') }}</button>
                    <button class="btn sm" @click="discard(proposal(id)!)">{{ t('Discard') }}</button>
                  </template>
                  <button v-else-if="proposal(id)!.status === 'applied'" class="btn sm" @click="undo(proposal(id)!)"><Icon name="undo" />{{ t('Undo') }}</button>
                </div>
              </div>
            </template>
          </div>
        </div>
      </div>

      <form class="composer" @submit.prevent="send()">
        <textarea v-model="draft" class="input" rows="1" maxlength="4000" :placeholder="t('Ask about apps, profiles or blocks…')" :aria-label="t('Message')" @keydown.enter.exact.prevent="send()" />
        <button v-if="sending" type="button" class="btn" @click="stop"><Icon name="stop" />{{ t('Stop') }}</button>
        <button v-else class="btn primary" type="submit" :disabled="!draft.trim()"><Icon name="send" />{{ t('Send') }}</button>
      </form>
    </section>
  </div>
</template>

<style scoped>
.assistant { height: 100%; }
.chat { display: flex; flex-direction: column; gap: 10px; flex: 1; min-height: 360px; padding: 10px 12px; }
.chat-top { flex: none; }
.log { flex: 1; min-height: 0; overflow-y: auto; display: flex; flex-direction: column; gap: 10px; padding: 4px 2px; }
.starter { margin: auto; text-align: center; display: grid; gap: 10px; justify-items: center; color: var(--text-2); max-width: 520px; }
.chips { display: flex; flex-wrap: wrap; gap: 6px; justify-content: center; }
.chip { border: 1px solid var(--border-strong); background: var(--surface-2); color: var(--text); border-radius: 999px; padding: 5px 12px; font: inherit; font-size: 12.5px; cursor: pointer; }
.chip:hover { background: var(--accent-soft); }
.msg { display: flex; }
.msg.user { justify-content: flex-end; }
.bubble { max-width: 88%; padding: 8px 12px; border-radius: 12px; background: var(--surface-2); overflow-wrap: anywhere; display: flex; flex-direction: column; gap: 6px; }
.msg.user .bubble { background: var(--accent-soft); white-space: pre-wrap; }
.msg.error .bubble { background: var(--danger-soft); color: var(--danger); }
.md :deep(p) { margin: 0 0 6px; }
.md :deep(p:last-child) { margin-bottom: 0; }
.md :deep(ul), .md :deep(ol) { margin: 0 0 6px; padding-left: 20px; }
.md :deep(pre) { margin: 4px 0; padding: 8px 10px; background: var(--sunken); border-radius: var(--radius-s); overflow-x: auto; }
.md :deep(code) { font-family: var(--mono); font-size: 12px; }
.tools { display: flex; flex-wrap: wrap; gap: 4px; }
.proposal { border: 1px solid var(--border-strong); background: var(--surface); border-radius: var(--radius-s); padding: 8px 10px; display: grid; gap: 6px; }
.programs { margin: 0; padding-left: 18px; color: var(--text-2); }
.composer { flex: none; display: flex; gap: 8px; align-items: flex-end; }
.composer textarea { height: auto; min-height: 34px; max-height: 96px; padding: 7px 12px; resize: none; line-height: 1.4; }
.typing { display: inline-flex; gap: 4px; }
.typing span { width: 6px; height: 6px; border-radius: 50%; background: var(--text-3); animation: pulse-dot 1s infinite ease-in-out; }
.typing span:nth-child(2) { animation-delay: 0.15s; }
.typing span:nth-child(3) { animation-delay: 0.3s; }
@keyframes pulse-dot { 0%, 80%, 100% { opacity: 0.3; } 40% { opacity: 1; } }
@media (prefers-reduced-motion: reduce) { .typing span { animation: none; opacity: 0.6; } }
</style>
