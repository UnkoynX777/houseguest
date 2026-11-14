export type Action = 'allow' | 'deny';
export type MatchType = 'path' | 'hash' | 'publisher' | 'package';

export interface Rule {
  id: string;
  action: Action;
  priority: number;
  match: { type: MatchType; value: string };
  expires_at?: number;
}

export type Role = 'main' | 'launcher' | 'helper' | 'runtime' | 'updater' | 'service' | 'component';
export type Scope = 'descendant' | 'independent';
export type DependencyMode = 'selected_only' | 'approved' | 'related';

export interface Component {
  id: string;
  label: string;
  role: Role;
  scope: Scope;
  path: string;
  sha256: string;
  publisher: string;
  package_family: string;
  follow_updates: boolean;
  expires_at?: number;
}

export interface Application {
  id: string;
  name: string;
  mode: DependencyMode;
  unknown: 'deny' | 'review';
  priority: number;
  components: Component[];
}

export interface Profile {
  id: string;
  name: string;
  default_action: Action;
  rules: Rule[];
  apps?: Application[];
}

export interface Status {
  version: string;
  configured: boolean;
  armed: boolean;
  now: number;
  enforcement: 'supervised' | 'polling' | 'unavailable';
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
  version?: string;
  description?: string;
  source?: string;
  package_family?: string;
}

export interface LibraryApp extends Required<AppEntry> {
  status: 'allowed' | 'blocked';
  via: string;
  reason: string;
  rule_id: string;
  app_id: string;
  blocks: number;
  listed?: string;
  dependencies?: number;
  pending?: number;
  conflicts?: number;
}

export interface DecisionInfo {
  action: Action;
  source: string;
  reason: string;
  rule_id: string;
  app_id: string;
  component_id: string;
  note: string;
  review_app: string;
}

export type ComponentState = 'approved' | 'updated' | 'changed' | 'missing' | 'unpinned' | 'expired' | 'pattern' | 'unknown';

export interface ComponentView extends Component {
  state: ComponentState;
  on_disk?: { sha256: string; publisher: string };
  effective: DecisionInfo | null;
}

export interface BlockEntry {
  event: string;
  ts: number;
  path: string;
  process: string;
  sha256: string;
  publisher: string;
  profile: string;
  rule: string;
  source: string;
  reason: string;
  note: string;
  related_app: string;
  parent: string;
  recommendation: string;
}

export interface PendingReview {
  id: string;
  app_id: string;
  kind: 'unknown_dependency' | 'component_changed';
  path: string;
  sha256: string;
  publisher: string;
  parent: string;
  profile_id: string;
  count: number;
  first: number;
  last: number;
}

export interface Explanation {
  path: string;
  sha256: string;
  publisher: string;
  decision: DecisionInfo;
  application?: { id: string; name: string; mode: DependencyMode; unknown: 'deny' | 'review'; priority: number };
  components: ComponentView[];
  summary: { approved: number; changed: number; missing: number; unpinned: number; pending: number };
  pending: PendingReview[];
  history: BlockEntry[];
  findings: Finding[];
}

export interface ObservedItem {
  path: string;
  name: string;
  sha256: string;
  publisher: string;
  package_family: string;
  parent: string;
  runs: number;
  first_seen_ms: number;
  descendant: boolean;
  role: Role | 'tool';
  verdict: 'main' | 'recommended' | 'review' | 'avoid' | 'windows';
  risks: string[];
}

export interface DiscoveryReport {
  running: boolean;
  label: string;
  path: string;
  ends_at: number;
  elapsed_ms: number;
  dropped: number;
  items: ObservedItem[];
}

export interface ContinuityReport {
  install_directory: string;
  install_trusted: boolean;
  pinned: boolean;
  missing: string[];
  webview_runtimes: string[];
  webview_trusted: boolean;
  cli: string;
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
  apps: { name: string; path: string; action: Action; rule: string; source: string; app_id?: string; reason?: string }[];
  applications?: number;
  continuity?: Omit<ContinuityReport, 'cli'>;
}

export interface CapabilityCheck {
  id: string;
  health: 'ok' | 'degraded' | 'missing';
  critical: boolean;
  detail: string;
}

export interface Diagnostics {
  version: string;
  capabilities: {
    product_name: string;
    edition: string;
    display_version: string;
    build: string;
    supported_build: boolean;
    ready: boolean;
    webview2: boolean;
    checks: CapabilityCheck[];
  };
  supervisor_running: boolean;
  supervisor: {
    mode: 'supervised' | 'polling' | 'unavailable';
    degraded_reason: string;
    inspected: number;
    terminated: number;
    termination_failed: number;
    pid_reuse_skipped: number;
    identity_cache_entries: number;
    protection_spared?: number;
    lineage_records?: number;
    lineage_saturated?: number;
    assessment_us?: { samples: number; average: number; p95: number; max: number };
    latency_ms: { samples: number; average: number; p95: number; max: number };
  };
  audit_chain_valid: boolean;
  state: 'loaded' | 'fresh' | 'recovered' | 'tampered';
  data_directory: string;
  install_directory: string;
  backups: number;
  uptime_seconds: number;
}
