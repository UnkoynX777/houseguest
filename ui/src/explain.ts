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
      registry: t('Installed programs'), installed: t('Installed programs'), app_paths: t('App paths'), start_menu: t('Start menu'), store: t('Microsoft Store'),
      path: t('PATH folders'), folder: t('Program folders'), portable: t('Portable folder'), manual: t('Added by hand'), winget: t('winget'), scoop: t('Scoop'), chocolatey: t('Chocolatey'),
    } as Record<string, string>
  )[source] ?? source;
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
