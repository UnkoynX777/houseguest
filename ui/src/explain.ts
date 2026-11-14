import { t } from './i18n';
import type { BlockEntry, DecisionInfo, LibraryApp } from './types';

const protectedKinds: Record<string, string> = {
  kernel_critical: 'Windows marks this process as critical, so it is never ended.',
  protected_process: 'This is a protected Windows process, so it is never ended.',
  windows_core: 'Essential Windows component for sign-in, services or the desktop.',
  windows_shell: 'Windows shell component that the desktop needs.',
  input_and_accessibility: 'Input or accessibility component needed to use and recover the computer.',
  driver_package: 'Program from a signed driver package (graphics, audio, input hardware).',
  shell_app: 'Signed Windows system app.',
  system_infrastructure: 'Windows infrastructure signed by Windows itself (error reporting, search, printing, updates).',
  windows_com_server: 'Windows program registered as a system COM server and started by the system, not by the user.',
};

export function describeDecision(decision: DecisionInfo, applicationName = ''): string {
  const reason = decision.reason;
  if (decision.source === 'protected') return t(protectedKinds[reason] ?? 'Protected Windows component.');
  if (reason === 'houseguest') return t('Part of Houseguest itself, verified by its pinned file hash.');
  if (reason === 'houseguest.webview2') return t('WebView2 process started by Houseguest, verified by its Microsoft signature.');
  if (reason === 'rule') return t('Matched the profile rule {rule}.', { rule: decision.rule_id });
  if (reason === 'application.main') return t('Allowed because the application {name} is approved.', { name: applicationName });
  if (reason === 'application.dependency') return t('Approved dependency, started by its application.');
  if (reason === 'application.dependency.independent') return t('Approved component that Windows or a broker may start.');
  if (reason === 'application.related.signer') return t('Started by the application and signed by the same publisher.');
  if (reason === 'application.related.location') return t('Started by the application from its own folder.');
  if (reason === 'default.allow') return t('No rule matches, so the profile default allows it.');
  if (reason === 'default.deny') return t('No rule allows it, so the profile default blocks it.');
  if (reason === 'identity.unresolved') return t('The program could not be identified, so it is blocked.');
  return reason;
}

export function describeNote(note: string): string {
  if (note.startsWith('component_changed:')) return t('The file changed since it was approved. Review it to allow it again.');
  if (note.startsWith('component_updated:')) return t('The file was updated by its publisher and still carries the approved signature.');
  return '';
}

export function describeBlock(entry: BlockEntry): string {
  if (entry.related_app) return t('A program the allowed application needed is not approved yet.');
  if (entry.note.startsWith('component_changed:')) return t('An approved file changed and needs a new review.');
  if (entry.event === 'protection.spared') return t('Windows marks this process as essential, so Houseguest left it running.');
  return describeDecision({ reason: entry.reason, source: entry.source, rule_id: entry.rule } as DecisionInfo);
}

export function recommendation(entry: BlockEntry): string {
  switch (entry.recommendation) {
    case 'approve_dependency': return t('Approve it as a dependency if the application needs it.');
    case 'reauthorize_component': return t('Check the new file and re-approve it if you trust the update.');
    case 'review_rule': return t('A profile rule blocks it. Edit the rule to change this.');
    case 'allow_if_needed': return t('Allow the program in this profile if it should run.');
    default: return '';
  }
}

const categories: [string, RegExp][] = [
  ['games', /\b(game|games|steam|epic games|riot|minecraft|mojang|ubisoft|blizzard|battle\.net|valve|roblox|xbox|rockstar|geforce experience|2k|bethesda|paradox|gog galaxy|fivem|overwolf)\b/i],
  ['browsers', /\b(chrome|chromium|firefox|edge|opera|brave|vivaldi|browser|camoufox)\b/i],
  ['communication', /\b(discord|teams|zoom|slack|skype|whatsapp|telegram|signal|outlook|thunderbird|mail)\b/i],
  ['development', /\b(visual studio|vs code|code|cursor|git|github|node|nvm|python|java|jdk|jetbrains|intellij|pycharm|android studio|docker|powershell|terminal|dbeaver|rust|cargo|qt|cmake|postman|sdk|compiler)\b/i],
  ['media', /\b(vlc|spotify|obs|audacity|media player|photos?|video|music|netflix|itunes|gimp|paint|premiere|davinci|ffmpeg|handbrake|wallpaper)\b/i],
  ['office', /\b(word|excel|powerpoint|office|libreoffice|onenote|acrobat|reader|notepad|pdf|obsidian|notion)\b/i],
  ['system', /\b(microsoft windows|control panel|task manager|registry|defender|security|revo|uninstaller|cleaner|driver|nvidia control|device|settings)\b/i],
];

export function categoryOf(app: LibraryApp): string {
  const primary = `${app.name} ${app.company}`;
  const haystack = `${primary} ${app.description} ${app.path}`;
  return categories.find(([, pattern]) => pattern.test(primary))?.[0] ?? categories.find(([, pattern]) => pattern.test(haystack))?.[0] ?? 'other';
}

export function categoryLabel(key: string): string {
  return (
    {
      games: t('Games'), browsers: t('Browsers'), communication: t('Communication'), media: t('Media'),
      development: t('Development'), office: t('Office'), system: t('System'), other: t('Other'),
    } as Record<string, string>
  )[key] ?? key;
}

export function sourceLabel(source: string): string {
  return (
    {
      windows_registry: t('Installed programs'), registry: t('Installed programs'), installed: t('Installed programs'), app_paths: t('App paths'),
      shortcuts: t('Shortcuts'), start_menu: t('Start menu'), packages: t('Microsoft Store'), store: t('Microsoft Store'), xbox_games: t('Xbox games'),
      steam: t('Steam'), epic: t('Epic Games'), gog: t('GOG Galaxy'), ubisoft: t('Ubisoft Connect'), ea: t('EA app'), battlenet: t('Battle.net'),
      package_managers: t('Package managers'), path: t('PATH folders'), folder: t('Program folders'), portable: t('Portable folder'),
      portable_folders: t('Portable folders'), manual: t('Added by hand'), volume_survey: t('Deep scan'), resolver: t('Executable search'),
      winget: t('winget'), scoop: t('Scoop'), chocolatey: t('Chocolatey'),
    } as Record<string, string>
  )[source] ?? source;
}

export function platformLabel(platform: string): string {
  return (
    {
      steam: t('Steam'), epic: t('Epic Games'), gog: t('GOG Galaxy'), ubisoft: t('Ubisoft Connect'), ea: t('EA app'), xbox: t('Xbox'), store: t('Microsoft Store'),
      winget: t('winget'), scoop: t('Scoop'), chocolatey: t('Chocolatey'), '': t('Windows'),
    } as Record<string, string>
  )[platform] ?? platform;
}

export function evidenceLabel(kind: string): string {
  return (
    {
      'registry.uninstall': t('Windows installed-programs entry'), 'registry.app_paths': t('App Paths registration'), shortcut: t('Shortcut'),
      'package.manifest': t('Package manifest'), 'steam.manifest': t('Steam app manifest'), 'steam.launch': t('Steam launch configuration'),
      'epic.manifest': t('Epic manifest'), 'gog.registry': t('GOG registration'), 'ubisoft.registry': t('Ubisoft registration'), 'ea.registry': t('EA registration'),
      'xbox.config': t('Xbox game configuration'), manual: t('Added by hand'), volume_survey: t('Found by the deep scan'), folder: t('Registered folder'),
      path: t('PATH entry'), user: t('Chosen by you'), resolved: t('Chosen from file evidence'), pending: t('Waiting for a time budget'),
    } as Record<string, string>
  )[kind] ?? kind;
}

export function reasonLabel(reason: string): string {
  return (
    {
      'graphical program': t('Graphical program'), 'console program': t('Console program'), 'product name matches': t('Product name matches'),
      'same publisher': t('Same publisher'), 'running now': t('Running now'),
    } as Record<string, string>
  )[reason] ?? reason;
}

export function kindLabel(kind: string): string {
  return ({ installed: t('Installed'), portable: t('Portable'), store: t('Microsoft Store'), manual: t('Added by hand') } as Record<string, string>)[kind] ?? kind;
}

export function roleKindLabel(role: string): string {
  return ({ application: t('Application'), tool: t('Command-line or system tool'), helper: t('Helper component'), installer: t('Installer or uninstaller') } as Record<string, string>)[role] ?? role;
}

export function roleLabel(role: string): string {
  return (
    { main: t('Main program'), launcher: t('Launcher'), helper: t('Helper'), runtime: t('Runtime'), updater: t('Updater'), service: t('Service'), component: t('Component'), tool: t('Tool') } as Record<string, string>
  )[role] ?? role;
}

export function riskLabel(risk: string): string {
  return (
    {
      unsigned: t('Unsigned'), outside_app_folder: t('Outside the app folder'), user_writable_location: t('In a folder users can write to'),
      sensitive_tool: t('Can run other programs'), not_started_by_app: t('Not started by the app'), different_signer: t('Different signer'),
    } as Record<string, string>
  )[risk] ?? risk;
}
