(() => {
  const now = Math.floor(Date.now() / 1000);
  const listeners = [];
  const swatch = (name, color) => {
    const c = document.createElement('canvas');
    c.width = c.height = 64;
    const g = c.getContext('2d');
    g.fillStyle = color;
    g.beginPath();
    g.roundRect(4, 4, 56, 56, 14);
    g.fill();
    g.fillStyle = '#fff';
    g.font = 'bold 28px sans-serif';
    g.textAlign = 'center';
    g.fillText(name.slice(0, 1), 32, 43);
    return c.toDataURL();
  };
  const colors = ['#5865f2', '#2fa97a', '#e0744a', '#4f8fe8', '#8c6fe0'];
  const mk = (name, company, source, status, extra = {}) => ({
    name, path: `C:\\Program Files\\${name}\\${name.toLowerCase().replace(/ /g, '')}.exe`, company, version: '1.0.' + name.length,
    description: '', source, package_family: '', status, via: status === 'allowed' ? 'application' : 'default',
    reason: status === 'allowed' ? 'application.main' : 'default.deny', rule_id: '', app_id: '', blocks: 0,
    id: 'id-' + name.replace(/ /g, ''), platform: '', volume: 'C:', resolution: 'resolved', candidates: [], evidence: [{ kind: 'registry.uninstall', detail: 'x' }], offline: false, product_id: '', sources: [source, 'registry'], kind: source === 'store' ? 'store' : 'installed', role: 'application', scope: 'machine', install_dir: `C:\Program Files\${name}`, modified: 0, size: 1000, first_seen: 0, manual: false, ...extra,
  });
  const apps = [
    mk('Counter-Strike 2', 'Valve', 'steam', 'blocked', { platform: 'steam', volume: 'D:', resolution: 'candidates', status: 'unresolved', path: '', sources: ['steam', 'windows_registry'], install_dir: 'D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive', candidates: [{ path: 'D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe', score: 35, reasons: ['graphical program'] }, { path: 'D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\vconsole2.exe', score: 36, reasons: ['graphical program', 'running now'] }] }),
    mk('Wallpaper Engine', 'Skutta', 'steam', 'blocked', { platform: 'steam', volume: 'D:', offline: true }),
    mk('Portable Notes', 'Indie Dev', 'portable', 'blocked', { kind: 'portable', first_seen: Math.floor(Date.now() / 1000) - 3600, sources: ['portable'] }),
    mk('Hand Tool', '', 'manual', 'blocked', { kind: 'manual', manual: true, sources: ['manual'] }),
    mk('curl', 'curl', 'path', 'blocked', { role: 'tool', sources: ['path'] }),
    mk('Minecraft Launcher', 'Mojang AB', 'store', 'allowed', { listed: 'app-mc', dependencies: 4, pending: 1, blocks: 3, description: 'Minecraft' }),
    mk('Discord', 'Discord Inc.', 'installed', 'blocked', { blocks: 5 }),
    mk('Steam', 'Valve Corporation', 'start_menu', 'allowed', { listed: 'app-steam', dependencies: 2 }),
    mk('Google Chrome', 'Google LLC', 'start_menu', 'blocked'),
    mk('Mozilla Firefox', 'Mozilla', 'start_menu', 'blocked', { conflicts: 1, listed: 'x', dependencies: 0 }),
    mk('Visual Studio Code', 'Microsoft Corporation', 'installed', 'blocked'),
    mk('Spotify', 'Spotify AB', 'installed', 'blocked'),
    mk('VLC media player', 'VideoLAN', 'start_menu', 'allowed'),
    mk('Notepad', 'Microsoft Corporation', 'app_paths', 'allowed'),
    mk('Microsoft Word', 'Microsoft Corporation', 'start_menu', 'blocked'),
    mk('Zoom', 'Zoom Video Communications', 'installed', 'blocked'),
    mk('OBS Studio', 'OBS Project', 'start_menu', 'blocked'),
    mk('Epic Games Launcher', 'Epic Games, Inc.', 'start_menu', 'blocked', { blocks: 2 }),
    mk('Roblox Player', 'Roblox Corporation', 'installed', 'allowed'),
    mk('7-Zip', 'Igor Pavlov', 'start_menu', 'blocked'),
    mk('Calculator', 'Microsoft Corporation', 'store', 'allowed'),
    mk('Long Path Game', 'Riot Games, Inc.', 'installed', 'blocked', {
      id: 'long-path', path: 'D:\\Very Long Folder Name For A Game Library\\Riot Games\\VALORANT\\live\\ShooterGame\\Binaries\\Win64\\VALORANT-Win64-Shipping.exe',
      install_dir: 'D:\\Very Long Folder Name For A Game Library\\Riot Games\\VALORANT\\live',
      icons: [{ kind: 'registry', path: 'C:\\ProgramData\\Riot Games\\Metadata\\valorant.live\\valorant.live.ico', index: 0 }],
      origins: { name: 'windows_registry', company: 'windows_registry', version: '' },
      alternatives: [{ field: 'company', value: 'Riot Games', source: 'packages' }],
    }),
  ];
  const profile = (id, name, def, extra = {}) => ({ id, name, default_action: def, rules: [], apps: [], ...extra });
  const profiles = [
    profile('builtin-guest', 'Guest', 'deny', { apps: [{ id: 'app-mc', name: 'Minecraft Launcher', mode: 'approved', unknown: 'review', priority: 50, components: [] }] }), profile('builtin-gaming', 'Gaming', 'deny', {
      apps: [{ id: 'app-mc', name: 'Minecraft Launcher', mode: 'approved', unknown: 'review', priority: 50, components: [] }],
    }),
    profile('builtin-work', 'Work', 'allow'), profile('builtin-restricted', 'Restricted', 'deny'), profile('builtin-custom', 'Custom', 'deny'),
  ];
  const comp = (id, label, role, scope, extra = {}) => ({ id, label, role, scope, path: `C:\\Games\\Minecraft\\${label}`, sha256: 'a'.repeat(64), publisher: 'CN=Mojang AB', package_family: '', follow_updates: true, ...extra });
  const components = [
    { ...comp('c1', 'MinecraftLauncher.exe', 'main', 'independent'), state: 'approved', effective: { action: 'allow', source: 'application', reason: 'application.main', rule_id: '', app_id: 'app-mc', component_id: 'c1', note: '', review_app: '' } },
    { ...comp('c2', 'runtime\\java\\bin\\javaw.exe', 'runtime', 'descendant', { publisher: '' }), state: 'approved', effective: { action: 'allow', source: 'dependency', reason: 'application.dependency', rule_id: '', app_id: 'app-mc', component_id: 'c2', note: '', review_app: '' } },
    { ...comp('c3', 'updater.exe', 'updater', 'descendant'), state: 'changed', on_disk: { sha256: 'b'.repeat(64), publisher: '' }, effective: { action: 'deny', source: 'default', reason: 'default.deny', rule_id: '', app_id: '', component_id: '', note: 'component_changed:c3', review_app: 'app-mc' } },
    { ...comp('c4', 'crashreporter.exe', 'helper', 'descendant', { sha256: '', publisher: '' }), state: 'unpinned', effective: null },
    { ...comp('c5', 'EasyAntiCheat.exe', 'service', 'independent'), state: 'missing', effective: null },
  ];
  const now2 = now - 400;
  const explanation = (path) => ({
    path, sha256: 'a'.repeat(64), publisher: 'CN="Riot Games, Inc.", O="Riot Games, Inc.", L=Los Angeles, S=California, C=US, SERIALNUMBER=4558474, 2.5.4.15=Private Organization, 1.3.6.1.4.1.311.60.2.1.3=US',
    decision: path.includes('Minecraft') ? { action: 'allow', source: 'application', reason: 'application.main', rule_id: '', app_id: 'app-mc', component_id: 'c1', note: '', review_app: '' }
      : { action: 'deny', source: 'default', reason: 'default.deny', rule_id: '', app_id: '', component_id: '', note: '', review_app: '' },
    application: path.includes('Minecraft') ? { id: 'app-mc', name: 'Minecraft Launcher', mode: 'approved', unknown: 'review', priority: 50 } : undefined,
    components: path.includes('Minecraft') ? components : [],
    summary: path.includes('Minecraft') ? { approved: 2, changed: 1, missing: 1, unpinned: 1, pending: 1 } : { approved: 0, changed: 0, missing: 0, unpinned: 0, pending: 0 },
    pending: path.includes('Minecraft') ? [{ id: 'r1', app_id: 'app-mc', kind: 'unknown_dependency', path: 'C:\\Games\\Minecraft\\telemetry.exe', sha256: 'c'.repeat(64), publisher: '', parent: 'C:\\Games\\Minecraft\\MinecraftLauncher.exe', profile_id: 'builtin-gaming', count: 3, first: now2, last: now - 20 }] : [],
    history: [{ event: 'app.blocked', ts: now - 120, path: 'C:\\Games\\Minecraft\\telemetry.exe', process: 'telemetry.exe', sha256: '', publisher: '', profile: 'Gaming', rule: '', source: 'default', reason: 'default.deny', note: '', related_app: 'app-mc', parent: 'C:\\Games\\Minecraft\\MinecraftLauncher.exe', recommendation: 'approve_dependency' }],
    findings: [],
  });
  const reply = (cmd, args) => {
    switch (cmd) {
      case 'host.info': return { elevated: false };
      case 'host.get_prefs': case 'host.set_prefs': return { minimize_to_tray: true, start_with_windows: false, theme: window.__theme || 'light' };
      case 'host.icon': { (window.__iconCalls ||= []).push({ path: args.path, size: args.size, sources: args.sources });  const name = args.path.split('\\').slice(-2)[0] || 'A'; return { png: Math.abs([...name].reduce((s, c) => s + c.charCodeAt(0), 0)) % 3 === 0 ? '' : swatch(name, colors[name.length % colors.length]) }; }
      case 'status': return { version: '0.4.0', configured: true, armed: true, now, enforcement: 'supervised', store_tampered: false, language: 'en', ends_at: now + 3000, profile: 'Gaming', generations: { policy: window.__gp || 1, catalog: window.__gc || 1, history: window.__gh || 1 } };
      case 'audit.query': {
        if (window.__failHistory) throw Object.assign(new Error('pipe closed'), {});
        if (window.__emptyHistory) return { entries: [], more: false };
        const all = []; const names = ['steam.exe','steamwebhelper.exe','discord.exe','cmd.exe','javaw.exe','MinecraftLauncher.exe','powershell.exe'];
        for (let i = 160; i >= 1; i--) { const n = names[i % names.length]; const k = i % 9;
          all.push(k === 0 ? { seq: i, ts: now - i * 400, actor: 'owner', event: 'unlock', detail: null }
            : k === 1 ? { seq: i, ts: now - i * 400, actor: 'owner', event: 'session.started', detail: { minutes: 60 } }
            : { seq: i, ts: now - i * 400, actor: 'supervisor', event: i % 3 ? 'app.allowed' : 'app.blocked', detail: { path: 'C:/Games/' + n, process: n, profile: 'Gaming', source: i % 3 ? 'application' : 'default', reason: i % 3 ? 'application.dependency' : 'default.deny', rule: '', publisher: 'CN=Valve', sha256: 'a'.repeat(64), parent: 'C:/Games/steam.exe', note: '', related_app: '' } }); }
        let rows = all.filter((e) => (!args.before || e.seq < args.before) && (!args.events || args.events.includes(e.event)) && (!args.text || JSON.stringify(e).toLowerCase().includes(args.text.toLowerCase())));
        return { entries: rows.slice(0, args.limit || 50).map((e) => ({ ...e, detail: e.detail ?? {} })), more: rows.length > (args.limit || 50) };
      }
      case 'apps.sources': return { volumes: [{ root: 'C:\\', type: 'fixed', label: 'Windows', filesystem: 'NTFS', ready: true, included: true, reason: '' }, { root: 'D:\\', type: 'fixed', label: 'Data', filesystem: 'NTFS', ready: true, included: true, reason: '' }, { root: 'E:\\', type: 'removable', label: '', filesystem: '', ready: false, included: false, reason: 'removable drives are skipped unless enabled in the library settings' }], resolved: 10, candidates: 1, unresolved: 3, offline: 1, discarded: 40, include_removable: false, deep: false, sources: [{ name: 'windows_registry', note: 'Uninstall entries', available: true, found: 85, unique: 32, merged: 25, discarded: 30, milliseconds: 44, error: '', scanned: true }, { name: 'steam', note: 'Steam libraries', available: true, found: 8, unique: 1, merged: 7, discarded: 0, milliseconds: 4, error: '', scanned: true }, { name: 'epic', note: '', available: false, found: 0, unique: 0, merged: 0, discarded: 0, milliseconds: 0, error: 'the Epic Games Launcher manifest folder does not exist', scanned: true }], portable_roots: ['D:\\Portable'], manual: [{ path: 'C:\\Tools\\hand.exe', name: 'Hand Tool', missing: true }], scanning: false, updated_at: now, total: 14 };
      case 'apps.sources_old': return { sources: [{ name: 'registry', available: true, reason: '', found: 65, milliseconds: 180, scanned: true }, { name: 'store', available: false, reason: 'the package repository is not available on this Windows', found: 0, milliseconds: 0, scanned: true }, { name: 'path', available: true, reason: '', found: 166, milliseconds: 124, scanned: true }], portable_roots: ['D:\Portable'], manual: [{ path: 'C:\Tools\hand.exe', name: 'Hand Tool', missing: true }], scanning: false, updated_at: now, total: 12 };
      case 'apps.diagnose': return { verdict: 'not_detected', advice: 'Add it with Add application.', checks: [{ step: 'exists', ok: true, detail: 'The file exists.' }, { step: 'sources', ok: false, detail: 'No registry entry, App Paths entry, Start menu shortcut or package points to this file.' }] };
      case 'apps.refresh': return { started: true, scanning: true };
      case 'unlock': return { token: 't' };
      case 'profiles.list': return profiles;
      case 'apps.library': { const list = window.__bulk ? Array.from({ length: window.__bulk }, (_, i) => mk('Bulk App ' + i, 'Company ' + (i % 30), 'installed', i % 3 ? 'blocked' : 'allowed', { id: 'bulk-' + i, path: 'C:\Bulk\App' + i + '\app' + i + '.exe' })) : apps; return { apps: list, scanning: false, allowed: 0, blocked: 0 }; }
      case 'apps.library_old': return { apps, allowed: apps.filter((a) => a.status === 'allowed').length, blocked: apps.filter((a) => a.status === 'blocked').length };
      case 'apps.explain': return explanation(args.path);
      case 'apps.details': {
        window.__detailCalls = (window.__detailCalls || 0) + 1;
        if (window.__detailsFail) throw Object.assign(new Error('The file could not be read.'), {});
        const unsigned = /Hand Tool|curl/.test(args.path);
        const name = (cn) => ({ common_name: cn, organization: cn, country: 'US', fields: [{ label: 'Common name', oid: '2.5.4.3', value: cn }, { label: 'Organization', oid: '2.5.4.10', value: cn }, { label: 'Jurisdiction country', oid: '1.3.6.1.4.1.311.60.2.1.3', value: 'US' }, { label: 'Country', oid: '2.5.4.6', value: 'US' }] });
        return {
          path: args.path, exists: true, size: 240096528,
          declared: { company: 'Riot Games, Inc.', product: 'VALORANT', description: 'VALORANT', file_version: '', product_version: '', copyright: '', original_name: 'VALORANT-Win64-Shipping.exe', internal_name: '' },
          signature: unsigned ? { state: 'unsigned', signer: '', via_catalog: false, timestamped: false, certificate_expired: false, subject: { common_name: '', organization: '', country: '', fields: [] }, issuer: { common_name: '', organization: '', country: '', fields: [] }, thumbprint_sha1: '', thumbprint_sha256: '', serial: '', algorithm: '', not_before: 0, not_after: 0, status_code: '0x800B0100', revocation_checked: false }
            : { state: window.__sigState || 'valid', signer: 'Riot Games, Inc.', via_catalog: false, timestamped: true, certificate_expired: false, subject: name('Riot Games, Inc.'), issuer: name('DigiCert Trusted G4 Code Signing RSA4096 SHA384 2021 CA1'), thumbprint_sha1: 'A'.repeat(40), thumbprint_sha256: 'B'.repeat(64), serial: '0a1b2c3d4e5f60718293a4b5c6d7e8f9', algorithm: 'sha384RSA', not_before: now - 86400 * 200, not_after: now + 86400 * 160, status_code: '0x00000000', revocation_checked: false },
        };
      }
      case 'host.reveal': window.__revealed = args.path; return { opened: true };
      case 'reviews.list': return [{ id: 'r1', app_id: 'app-mc', kind: 'unknown_dependency', path: 'C:\\Games\\Minecraft\\telemetry.exe', sha256: '', publisher: '', parent: '', profile_id: 'builtin-gaming', count: 3, first: now2, last: now }];
      case 'continuity.report': return { install_directory: 'C:\\Program Files\\Houseguest', install_trusted: true, pinned: true, missing: [], webview_runtimes: ['x'], webview_trusted: true, cli: '' };
      case 'dashboard': return { version: '0.4.0', configured: true, armed: true, now, enforcement: 'supervised', store_tampered: false, language: 'en', ends_at: now + 3000, blocked: 7, profiles: 5, allowed_rules: 3, recent: [], session: { profile_id: 'builtin-gaming', profile_name: 'Gaming', started_at: now - 600, ends_at: now + 3000, audit_only: false, guest_sids: ['S-1'] } };
      case 'discovery.start': case 'discovery.status': case 'discovery.stop': return { running: true, label: 'x', path: '', ends_at: now + 300, elapsed_ms: 4000, dropped: 0, items: [
        { path: 'C:\\Games\\Minecraft\\MinecraftLauncher.exe', name: 'MinecraftLauncher.exe', sha256: 'a'.repeat(64), publisher: 'CN=Mojang AB', package_family: '', parent: '', runs: 1, first_seen_ms: 0, descendant: true, role: 'main', verdict: 'main', risks: [] },
        { path: 'C:\\Games\\Minecraft\\runtime\\java\\bin\\javaw.exe', name: 'javaw.exe', sha256: 'd'.repeat(64), publisher: '', package_family: '', parent: '', runs: 2, first_seen_ms: 900, descendant: true, role: 'runtime', verdict: 'recommended', risks: ['unsigned'] },
        { path: 'C:\\Users\\kid\\AppData\\Local\\Temp\\setup.exe', name: 'setup.exe', sha256: 'e'.repeat(64), publisher: '', package_family: '', parent: '', runs: 1, first_seen_ms: 1500, descendant: true, role: 'tool', verdict: 'avoid', risks: ['unsigned', 'sensitive_tool', 'user_writable_location'] },
        { path: 'C:\\Windows\\System32\\conhost.exe', name: 'conhost.exe', sha256: 'f'.repeat(64), publisher: 'CN=Microsoft Windows', package_family: '', parent: '', runs: 1, first_seen_ms: 500, descendant: true, role: 'helper', verdict: 'windows', risks: [] },
      ] };
      default: return {};
    }
  };
  window.chrome = { webview: {
    addEventListener: (_t, h) => listeners.push(h),
    postMessage: (text) => {
      const { id, cmd, args } = JSON.parse(text);
      setTimeout(() => { let r; try { r = { id, ok: true, data: reply(cmd, args || {}) }; } catch (e) { r = { id, ok: false, error: { code: 'service_unavailable', message: e.message, details: {} } }; } listeners.forEach((h) => h({ data: JSON.stringify(r) })); }, 30);
    },
  } };
})();
