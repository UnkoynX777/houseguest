# Privacy

Houseguest runs entirely on your computer.

- **No telemetry, accounts or analytics.** Nothing is sent anywhere automatically.
- **One optional network request.** On **About → Check for updates** the interface asks the public GitHub API for the latest release. No identifier is sent beyond what any web request carries.
- **Local data.** Settings, profiles and the audit log live in `%ProgramData%\Houseguest`, readable only by administrators. The audit log records blocked launches (program path, publisher, account identifier), session changes and security events. Delete the folder to remove them.
- **Interface data.** Window position, theme and notification preferences are stored for your Windows user in `%LOCALAPPDATA%\Houseguest\ui.json`, and WebView2 keeps its browser profile in `%LOCALAPPDATA%\Houseguest\WebView2`.
- **No secrets in logs.** The owner PIN is never stored or logged. Only a salted PBKDF2 verifier is kept.
- **Start with Windows** is optional and only adds a per-user entry that starts the interface hidden in the notification area. Protection does not depend on it.

The website is static pages without cookies or analytics scripts.
