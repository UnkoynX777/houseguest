# Assistant (Gemini)

An optional assistant that answers questions about Houseguest and prepares changes, in plain language. It uses the official Google Gemini API with **your own API key**. It is off by default and nothing is sent to Google while it is off or has no key.

## Privacy

What may be sent to Google when you ask a question:

- your message and the earlier messages of that conversation;
- the results of the lookups the assistant makes to answer: installed application names, paths and publishers, profile names and rules, recent blocked launches, history lines and diagnostics;
- if you turn on **Share what I am looking at**: the page name and the selected program or profile.

It never sends your PIN, API tokens, the remote-access keys, or the contents of files. **On Gemini's free tier, Google may use submitted content to improve its products** (per its current pricing page); a paid plan does not. Choose accordingly, and avoid asking about things you do not want to share.

The key is stored with Windows DPAPI (machine scope) in the service data folder, never in the page's storage. Removing the key turns the assistant off. Conversations are kept locally in the service data folder (up to 30, each trimmed) and can be deleted from the page.

## What it can and cannot do

It can look: status, profiles, installed applications (also by drive), recent blocks, history, diagnostics, why a program is allowed or blocked, and what a profile would allow.

It can **propose** three changes: create a profile that allows a list of installed programs and blocks everything else, allow one program in a profile, remove one program from a profile. A proposal is validated and simulated, shown with its effect, and does nothing until you press **Apply**. Applied changes can be undone from the same card. Proposals expire after 30 minutes.

It cannot: change settings, the PIN, accounts, backups or remote access; turn protection off; choose "allow by default"; write rules; run commands, PowerShell or scripts; read files; or call the service directly. Those tools do not exist. Every request still goes through the same checks as the interface, and the service decides what is valid.

Application names, paths and log lines are treated as untrusted data: tool results are wrapped as data on every turn, control characters are stripped, and the instructions tell the model to ignore commands found in them. Prompt injection cannot be fully excluded for any language model, so the structure matters more than the instructions: whatever the model asks for is limited to the tools above, and nothing it proposes happens without your confirmation.

Limits: 10 questions per minute and 400 per day from this computer, up to 6 tool steps per question.

## Models and cost

The model list comes from your key (only models that can generate content). The default is `gemini-3.1-flash-lite`, which Google lists with a free tier at the time of writing; limits and prices change, so check the current Gemini API pricing and rate-limit pages.

## Not verified yet

The Gemini client, the tool-calling loop, the proposals and the interface are covered by tests with simulated Gemini responses. They have **not been run against the live Gemini API**, because that needs your key. If Google changes the response format, the assistant reports an error rather than guessing.
