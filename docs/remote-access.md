# Remote access

Remote access lets you look at Houseguest from your phone: the protection state, the active profile, what was blocked, the history, pending decisions, and (with a key that may change things) extending or ending a session. It is **off until you set it up and turn it on**, and nothing in local protection depends on it.

## How it works

```
phone browser  --HTTPS-->  Cloudflare (Access sign-in)  --tunnel-->  cloudflared  -->  127.0.0.1:<port>  Houseguest API  --> service
```

- **Local API.** The service opens an HTTP API on `127.0.0.1` only. The port is chosen by Windows (bind to port 0, read back with `getsockname`) and held with `SO_EXCLUSIVEADDRUSE`, so no other program can take it and there is no window between choosing and using the port. It changes at every start; the interface learns it from the service (`api.info`), never from a file.
- **Tunnel.** `cloudflared` is the official Cloudflare client. Houseguest installs it only when you ask (download from the official GitHub release, accepted only if the file has a valid Authenticode signature issued to Cloudflare), runs it hidden in a job object, never puts secrets on its command line, and restarts it with growing delays (1 s up to 5 min, stopping after 6 restarts in 10 minutes instead of looping).
- **Two sign-ins.** Cloudflare Access checks who you are (only the e-mail addresses you list). Houseguest then verifies the signed Access token itself: RS256 signature against the team's keys, issuer, audience, expiry and not-before. Anything that carries proxy headers is treated as remote and must pass; if remote access is off, it is refused. A header alone is never believed.
- **Device keys.** Each phone is paired by you, on the computer, and gets its own 256-bit key, shown once. Only a hash is stored. Keys expire after 30 days, after 12 hours without use, or when you remove them. A key is either **read only** or **can change**.
- **Changes.** Anything that loosens protection or edits a profile (extending a session, saving a profile, approving a program, starting a session) also needs the owner PIN, typed on the phone, valid for 5 minutes. Requests that change things carry a timestamp and a one-time nonce (replay protection). Wrong PINs lock the remote confirmation for growing periods, and that counter is separate from the local PIN: a stolen phone key cannot lock you out of your own computer.
- **What is not reachable.** There is no route for PIN changes, settings, accounts, backups, recovery, the assistant, remote-access setup, files, or running anything. The route table is explicit and the service repeats the check with its own allow-list.

## Setting it up

You need a Cloudflare account with a domain, Zero Trust turned on (the free plan is enough), and an API token. In **Remote access**:

1. Create a token at <https://dash.cloudflare.com/profile/api-tokens> with: Account · Cloudflare Tunnel · Edit; Account · Access: Apps and Policies · Edit; Account · Access: Organizations · Read; Zone · DNS · Edit; Zone · Zone · Read.
2. Paste it, choose the account and domain, name the host (for example `houseguest.example.com`) and list the e-mail addresses allowed to sign in. The token is used once and is not saved.
3. Houseguest creates the tunnel, the DNS record and the Access application, and keeps only what it needs to run them (the tunnel secret, protected with Windows DPAPI).
4. Install `cloudflared` if it is missing, then **Turn on**. Use **Check that it is protected**: it requests the public address without signing in and reports a failure if the API answers.
5. Add a phone, copy its key, open the address on the phone and paste the key.

**Remove remote access** stops the tunnel and forgets it; with a token it also deletes the tunnel, DNS record and policy from Cloudflare.

## What it protects, and what it does not

- Traffic is encrypted between the phone and Cloudflare and between Cloudflare and the computer. **Cloudflare terminates TLS and can technically read requests while forwarding them.** Houseguest does not add a second encryption layer: it would not hide anything from the party that holds the tunnel keys, and an end-to-end layer with key pairing would be a separate, larger design. If that matters to you, keep remote access off or read-only.
- Certificate errors are never ignored by Houseguest's own HTTPS client (TLS 1.2 or newer, system trust store). A phone browser's certificate checks are the browser's.
- A stolen phone key can read until you remove it; with the PIN it can change what the key allows. Remove lost phones from **Remote access** at once, or turn remote access off.
- Anyone with administrator control of the computer can still read the device file's contents, stop the service or remove Houseguest. This is not a defence against that.
- If the internet, Cloudflare or `cloudflared` fail, local protection is unaffected. Only the phone loses access.

## Not verified yet

The Cloudflare setup, the tunnel and the Access check are covered by tests with simulated Cloudflare responses and by a real local API driven from a browser. They have **not been run against a live Cloudflare account**, because that needs your credentials. Run **Check that it is protected** after your first setup and report anything unexpected.
