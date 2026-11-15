# Panels, dialogs and failures in the interface

## Layers

Everything that sits above the page is drawn through three components and one module, so that the rules exist once.

| Piece | Role |
| --- | --- |
| `components/Drawer.vue` | the side panel with its backdrop |
| `components/Modal.vue` | the dialog |
| `components/Menu.vue` | the dropdown menu |
| `layers.ts` | the stack that decides who answers Escape, what can be reached and where the focus returns |

`Drawer` and `Modal` are teleported to `body`, so no page container, `overflow` or transformed ancestor can clip or offset them. The toasts are teleported too. While a modal layer is open the page (`#app`) and every layer below the top one are `inert`: they take no clicks, no keys and no focus. Tab stays inside the top layer. When the last modal layer closes, the focus goes back to the element that opened it. Escape closes the top layer only, whether it is a menu, a dialog or a panel; a page kept alive in the background closes its layers when it is left.

Stacking uses the tokens `--z-raised`, `--z-popover`, `--z-drawer`, `--z-modal` and `--z-toast` in `style.css`; no other `z-index` is written.

## The detail panel

Which program is shown, whether the panel is open, which tab is selected, what has loaded and what failed are separate. Switching tabs changes only the tab; the panel, its header and its data are not rebuilt, each tab keeps its scroll position, and an answer that arrives after a newer request, or after the panel closed, is dropped. A reload keeps the data already shown; placeholders appear only while nothing has loaded, and a failed first load shows an error with a retry inside the panel.

`components/Boundary.vue` isolates a section: an error thrown while drawing it is replaced there by a message and a retry, and the rest of the panel, or of the page, keeps working. The detail panel wraps its tab content in one, and `App.vue` wraps the pages. Uncaught errors and rejected promises are logged and shown as a notice.

## Audit events have no fixed shape

The history the service returns mixes events written by different versions and of different kinds. A process the kernel protects, for instance, is recorded without a note, rule or source. `blockEntry` in `explain.ts` reads any such event into the full shape, and the text helpers tolerate absent fields. This was the cause of the Blocks tab emptying the panel: reading the missing `note` raised an error while the panel was drawn, and a component whose drawing fails is replaced by nothing.

## Checks

`scripts/ui-check/panel.mjs` (part of `scripts/ui-check/run.ps1`) covers opening, every tab, rapid switching, scroll, a failing section, closing by button, Escape and backdrop, repeated opening, stacked layers, menus, resizing, loading and failing loads, and leftover overlays, with the browser's own engine against the mock service. It does not run the WebView2 window.

## Component library: shadcn-vue and Reka UI were evaluated

The v5 brief asked to investigate shadcn-vue and Reka UI. They were not adopted, for these reasons:

- shadcn-vue is copied source built on Tailwind CSS and Reka UI. Adopting it means adding a Tailwind pipeline and a second styling system next to the existing token-based stylesheet, and restyling every page to match.
- The overlay behaviour (one layer stack, focus return, inert background, Escape order, error isolation per section) was rebuilt and verified in 0.4.6 with 40 browser checks. Replacing Drawer, Modal and Menu with another library's primitives would reopen exactly the area that regressed before.
- The interface is small (about 350 KB of script) and loads from disk inside WebView2; added runtime weight has a direct cost on every start.

What was done instead: the existing token system was kept and tightened (compact density, a 52 px icon rail, a title bar drawn in the page, consistent badges, segmented controls and cards), and the two new pages reuse the same Modal, Toggle, CopyText and layer primitives. If a later release needs a component the current set lacks (a combobox or a data table), Reka UI primitives can be added for that component alone, behind the same layer rules.
