# Upgrading to 5.0

Run the new `Houseguest-5.0.0-x64.msi` as administrator over the existing installation. The installer is a major upgrade: it replaces the service and the interface, keeps your profiles, library, settings and audit log in `%ProgramData%\Houseguest`, and keeps the owner PIN. If a guest session is active the installer refuses to continue (see the main guide), so end it first.

## What changes after the upgrade

- The window opens at one fixed size (1000 × 700 logical) with its own title bar. The window position is remembered; the size is not.
- Two new pages appear: **Remote access** and **Assistant**. Both are **off**. Nothing is sent anywhere and no extra process runs until you set them up.
- The service now opens a local API on `127.0.0.1` at a port Windows chooses. It answers only requests that carry a paired device key, and there are no devices until you pair one. It cannot be reached from the network.
- Windows Firewall does not prompt for loopback-only listeners. If a security product reports the new listener, it is the service, and it can be inspected in **Remote access**.

## First steps

1. Open Houseguest and unlock it as usual.
2. Optional: **Remote access** → follow the setup (see [remote access](remote-access.md)).
3. Optional: **Assistant** → add a Gemini key (see [the assistant](assistant.md)), after reading what is sent to Google.

## Going back

Uninstall 5.0.0 and install the earlier MSI. The new files `api-devices.json`, `remote.json`, `ai.json` and `ai-chats.json` in the data folder are ignored by earlier versions. If remote access was set up, remove it first (**Remove remote access**, with a Cloudflare token to also delete the tunnel, DNS record and Access policy), because the tunnel, DNS record and Access policy stay in your Cloudflare account until they are removed, and an earlier version cannot remove them.

## Checking what you installed

Compare the MSI with `SHA256SUMS.txt`, and verify the provenance attestation with `gh attestation verify <file> --repo UnkoynX777/houseguest`.
