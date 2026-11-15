import { createApp } from 'vue';

import App from './App.vue';
import { describe, notify } from './store';
import './style.css';

const app = createApp(App);
app.config.errorHandler = (error, _instance, info) => console.error(`Interface error (${info})`, error);
window.addEventListener('unhandledrejection', (event) => {
  console.error('Unhandled rejection', event.reason);
  notify('bad', describe(event.reason));
});
app.mount('#app');
