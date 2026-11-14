<script setup lang="ts">
const paths: Record<string, string> = {
  dashboard: 'M3 3h7v7H3z M14 3h7v7h-7z M14 14h7v7h-7z M3 14h7v7H3z',
  apps: 'M21 8l-9-5-9 5v8l9 5 9-5z M3 8l9 5 9-5 M12 13v8',
  profiles: 'M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2 M9 11a4 4 0 1 0 0-8 4 4 0 0 0 0 8z M22 21v-2a4 4 0 0 0-3-3.9 M16 3.1a4 4 0 0 1 0 7.8',
  rules: 'M4 21v-7 M4 10V3 M12 21v-9 M12 8V3 M20 21v-5 M20 12V3 M1 14h6 M9 8h6 M17 16h6',
  sessions: 'M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20z M12 6v6l4 2',
  history: 'M8 6h13 M8 12h13 M8 18h13 M3 6h.01 M3 12h.01 M3 18h.01',
  security: 'M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z',
  shield: 'M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z M9 12l2 2 4-4',
  settings: 'M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z M12 2v3 M12 19v3 M2 12h3 M19 12h3 M4.9 4.9L7 7 M17 17l2.1 2.1 M4.9 19.1L7 17 M17 7l2.1-2.1',
  diagnostics: 'M22 12h-4l-3 9L9 3l-3 9H2',
  about: 'M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20z M12 16v-4 M12 8h.01',
  lock: 'M5 11h14v10H5z M8 11V7a4 4 0 0 1 8 0v4',
  plus: 'M12 5v14 M5 12h14',
  trash: 'M3 6h18 M8 6V4h8v2 M6 6l1 14h10l1-14',
  search: 'M11 19a8 8 0 1 0 0-16 8 8 0 0 0 0 16z M21 21l-4.3-4.3',
  check: 'M20 6L9 17l-5-5',
  x: 'M18 6L6 18 M6 6l12 12',
  alert: 'M12 3l10 18H2z M12 10v5 M12 18h.01',
  play: 'M6 4l14 8-14 8z',
  stop: 'M6 6h12v12H6z',
  refresh: 'M21 12a9 9 0 1 1-3-6.7 M21 3v6h-6',
  download: 'M12 3v12 M7 10l5 5 5-5 M4 21h16',
  upload: 'M12 15V3 M7 8l5-5 5 5 M4 21h16',
  copy: 'M9 9h11v11H9z M5 15V4h11',
  edit: 'M4 20h4L19 9l-4-4L4 16z',
  eye: 'M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z',
  external: 'M14 3h7v7 M21 3l-9 9 M19 14v6H4V5h6',
};

defineProps<{ name: string }>();
</script>

<template>
  <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
    <path :d="paths[name] ?? ''" />
  </svg>
</template>
