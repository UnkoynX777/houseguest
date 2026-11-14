export type Action = 'allow' | 'deny';
export type MatchType = 'path' | 'hash' | 'publisher' | 'package';

export interface Rule {
  id: string;
  action: Action;
  priority: number;
  match: { type: MatchType; value: string };
  expires_at?: number;
}

export interface Profile {
  id: string;
  name: string;
  default_action: Action;
  rules: Rule[];
}

export interface Status {
  version: string;
  configured: boolean;
  armed: boolean;
  now: number;
  enforcement: 'supervised' | 'unavailable';
  store_tampered: boolean;
  language: string;
  ends_at?: number;
}

export interface AuditEntry {
  seq: number;
  ts: number;
  actor: string;
  event: string;
  detail: Record<string, unknown>;
}

export interface Dashboard extends Status {
  blocked: number;
  profiles: number;
  allowed_rules?: number;
  recent: AuditEntry[];
  session?: {
    profile_id: string;
    profile_name: string;
    started_at: number;
    ends_at: number;
    audit_only: boolean;
    guest_sids: string[];
  };
  exceptions?: Rule[];
}

export interface AppEntry {
  name: string;
  path: string;
  company: string;
}

export interface LocalUser {
  name: string;
  sid: string;
  owner: boolean;
}

export interface Finding {
  severity: 'info' | 'warning' | 'error';
  code: string;
  rules: string[];
  message: string;
}

export interface Preview {
  findings: Finding[];
  errors: boolean;
  warnings: boolean;
  allowed: number;
  denied: number;
  apps: { name: string; path: string; action: Action; rule: string; source: string }[];
}

export interface Diagnostics {
  version: string;
  capabilities: { edition: string; build: string; applocker_enforced: boolean; supervised: boolean; webview2: boolean };
  supervisor_running: boolean;
  supervisor_error: string;
  audit_chain_valid: boolean;
  state: 'loaded' | 'fresh' | 'recovered' | 'tampered';
  data_directory: string;
  install_directory: string;
  backups: number;
  uptime_seconds: number;
}
