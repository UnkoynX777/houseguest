import { t } from '../i18n';
import type { AuditEntry } from '../types';

export type EventGroup = 'blocked' | 'sessions' | 'security' | 'changes';

const groups: Record<string, EventGroup> = {
  'app.blocked': 'blocked',
  'app.would_block': 'blocked',
  'app.block_failed': 'blocked',
  'protection.spared': 'blocked',
  'session.started': 'sessions',
  'session.ended': 'sessions',
  'session.extended': 'sessions',
  'session.resumed': 'sessions',
  'exception.added': 'sessions',
  unlock: 'security',
  'unlock.denied': 'security',
  'pin.failed': 'security',
  'pin.changed': 'security',
  recover: 'security',
  setup: 'security',
  'state.integrity_failure': 'security',
  'state.recovered': 'security',
};

export const groupOf = (entry: AuditEntry): EventGroup => groups[entry.event] ?? 'changes';

const fileName = (path: unknown) => String(path ?? '').split('\\').pop() ?? '';

export function eventLabel(entry: AuditEntry): string {
  const d = entry.detail;
  switch (entry.event) {
    case 'app.blocked': return t('Blocked {name}', { name: fileName(d.path) });
    case 'app.would_block': return t('Would block {name} (audit mode)', { name: fileName(d.path) });
    case 'session.started': return t('Session started for {n} minutes', { n: Number(d.minutes) });
    case 'session.ended': return t('Session ended ({reason})', { reason: String(d.reason) });
    case 'session.extended': return t('Session extended by {n} minutes', { n: Number(d.minutes) });
    case 'session.resumed': return t('Session resumed after a restart');
    case 'exception.added': return t('Temporary exception added');
    case 'unlock': return t('Owner unlocked Houseguest');
    case 'unlock.denied': return t('Unlock attempt from a non-owner account');
    case 'pin.failed': return t('Incorrect PIN (attempt {n})', { n: Number(d.attempts) });
    case 'pin.changed': return t('PIN changed');
    case 'setup': return t('Owner account created');
    case 'recover': return t('Administrator recovery used');
    case 'profile.saved': return t('Profile “{name}” saved', { name: String(d.name) });
    case 'profile.imported': return t('Profile “{name}” imported', { name: String(d.name) });
    case 'profile.deleted': return t('Profile deleted');
    case 'settings.changed': return t('Settings changed');
    case 'backup.created': return t('Backup created');
    case 'backup.restored': return t('Backup restored');
    case 'state.integrity_failure': return t('Settings failed their integrity check');
    case 'state.recovered': return t('Settings recovered from the previous snapshot');
    case 'supervisor.unavailable': return t('Process monitoring could not start');
    case 'supervisor.mode': return d.mode === 'supervised' ? t('Process monitoring uses process events') : d.mode === 'polling' ? t('Process monitoring switched to polling') : t('Process monitoring stopped');
    case 'protection.spared': return t('Left running: {name} (essential to Windows)', { name: String(d.process ?? '') });
    case 'app.saved': return t('Application settings changed');
    case 'app.removed': return t('Application removed from a profile');
    case 'app.dependency_approved': return t('Dependency approved');
    case 'app.component_reauthorized': return t('Changed file approved again');
    case 'discovery.started': return t('Observation of an application started');
    case 'discovery.stopped': return t('Observation of an application stopped');
    case 'library.saved': return t('Dependency set saved');
    case 'library.deleted': return t('Dependency set deleted');
    case 'app.block_failed': return t('Could not stop {name}', { name: String(d.path).split(/[\\/]/).pop() ?? '' });
    default: return entry.event;
  }
}
