# iOS 2.6.0: Controller Navigation and Frontend Expansion

## Base

This delivery is rebased cleanly on the current `origin/master`:

- `247fa6f09499f69192c52c0542305ed16119518b` — latest `origin/master` at packaging time

The delivery archive contains a Git bundle and numbered patches for the complete
incremental commit series on top of that master revision. It preserves repository-
relative paths and deliberately excludes build products, IPAs, and unrelated local
files.

## Problem

Controller focus was implemented by several independent SwiftUI paths. Each
path could derive its own spatial ordering from a transient layout snapshot,
while scrolling and lazy stacks could remove an off-screen target before the
focus transfer completed. That produced skipped rows, jumps to unrelated
elements, focus that appeared to move without the corresponding scroll, and
input conflicts after changing tabs or rotating.

The problem was most visible in Settings, per-game settings, the Quick Menu,
and the landscape Game Library. It also affected the BIOS toolbar and bottom
tab bar, where independently rendered visual effects could leave duplicate or
misplaced navigation orbs.

## Root cause

- Controller-owned views competed for the same directional and right-stick
  input after their tab or menu was no longer frontmost.
- Focus candidates were selected from an unstable mixture of visual position,
  view registration order, and lazy-list availability. A no-longer-visible
  candidate could fall back to a different item in a distant section.
- Scroll changes and focus changes were not one transaction. A directional
  event could select a row before `ScrollView` had materialized it.
- Some layouts recalculated sizing and effects while controller focus moved,
  especially in landscape.

## Implementation

### One focus engine

- Added `ControllerAccessibilityNavigation`, a shared, deterministic focus
  graph for controller-navigable SwiftUI content.
- Every registered target has a stable semantic identity and a monotonic
  registration order. Directional selection is rebuilt for each discrete input
  from the current visible viewport rather than from stale geometry.
- Explicit directional links take precedence where a UI has a real semantic
  order, such as the BIOS toolbar and the Quick Menu's two-column controls.
- Scroll boundaries are first-class graph nodes. At the top of Quick Menu,
  focus remains on Resume/Continue; crossing a valid boundary only occurs when
  the destination is semantically available.
- Focus transfers retain an anchor and retry briefly after an animated scroll
  allows a lazy row to enter the hierarchy. The engine never substitutes an
  arbitrary nearest item if that intended candidate has not materialized.

### Input ownership and scrolling

- Added `MenuControllerInputRouter` to route directional controls and the
  right stick to the frontmost navigation session only.
- Analog right-stick scrolling is bound to the owning session, preventing a
  hidden tab or a previous rotation from consuming input.
- Repeated same-direction edges are debounced, so a held D-pad/analog movement
  produces one predictable focus transition instead of duplicate transfers.
- Settings roots, per-game settings, Quick Menu, BIOS, the Game Library,
  settings subpages, and controller context menus expose their own scrolling
  boundaries and active owner state.

### Screen integration and visuals

- Completed controller navigation registration for Settings and its subpages,
  per-game settings and nested editors, BIOS controls, Quick Menu and its
  per-game settings entry, context menu actions, Game Library cards, top
  toolbar buttons, and the bottom tab bar.
- Restored intentional horizontal links in BIOS: Boot BIOS → Import BIOS →
  Refresh, without skipping the plus/import action.
- Added a consistent cyan-blue neon focus outline and focused-text treatment
  to Game Library, settings, menus, and controls.
- Consolidated the controller-orb visual so it belongs to the active focused
  surface; the tab bar no longer receives a duplicate library orb when focus
  moves to it.
- Kept focused-card visual transformations render-only where possible so the
  card's flow layout does not resize during navigation. Game-card content,
  landscape placement, title layering, glass treatment, and toolbar/tab-bar
  controller state were adjusted around this model.
- Improved contextual actions, haptic/rumble sensation mapping, controller
  activation and cancellation behavior, alert focus, and game/context menu
  paths requested for controller use.

## Lifecycle and resource ownership

The navigation engine owns only controller focus state, target metadata, and a
short-lived deferred move task. Individual views own their models, scroll
proxies, and activation closures. A surface registers while it is visible and
identifies itself as the active router owner; it releases that ownership when
it leaves the frontmost tab, sheet, or menu. This prevents retained SwiftUI
views from intercepting directional or right-stick input and keeps no gameplay,
emulator, Metal, audio, or external-controller resources under UI ownership.

## Before and after

| Area | Before | After |
| --- | --- | --- |
| Long settings lists | Directional moves could skip rows or jump to the bottom. | A visible, deterministic next target is focused and revealed before activation. |
| Lazy/off-screen controls | Focus could be lost or fall back to another section. | The requested target is retained through scroll materialization; no arbitrary fallback occurs. |
| Right stick | Hidden/previous surfaces could consume scrolling. | Only the frontmost session receives analog scroll input. |
| Quick Menu | Column movement and top boundary could drift or self-navigate. | Semantic column links and explicit boundaries keep Resume/Continue and Stop predictable. |
| BIOS toolbar | Right from Boot BIOS skipped Import BIOS. | The intended Boot BIOS → Import BIOS → Refresh order is explicit. |
| Orbs/focus effects | Effects could be duplicated or misplaced at the tab bar. | The active focus surface owns one aligned effect. |

## Performance, battery, and thermal impact

The controller path avoids per-card timers, display links, and focus-driven
layout resizing. It builds a small current-surface graph only for discrete
controller transitions, filters candidates to the scroll viewport, and keeps
the delayed materialization retry bounded. The right stick is routed to one
owner instead of broadcasting work to retained views. This reduces avoidable
SwiftUI invalidation and layout work during landscape navigation, which can
lower transient CPU/GPU wakeups and therefore the associated battery and
thermal pressure. No fixed performance, battery, or temperature percentage is
claimed; device profiling remains necessary for a quantitative result.

## Compatibility

- Changes are isolated to the iOS SwiftUI/controller presentation layer.
- The normal non-controller UI remains the master-style UI when no physical
  controller is connected.
- The iOS device build remains unsigned and uses the existing packaging flow.
- No emulator timing, GS rendering semantics, game data, BIOS data, or Android
  code paths are changed by the controller focus engine.

## Risks and mitigations

- **Dynamic SwiftUI content:** targets can appear/disappear as toggles, alerts,
  and navigation paths change. Rebuilding the graph for each command and
  retaining semantic IDs avoids stale-target jumps.
- **Lazy stacks:** materialization is asynchronous. The bounded retry only
  follows the intended target; it cannot silently activate a different row.
- **Multiple presentation layers:** a sheet, menu, or tab can overlap another
  registered surface. Frontmost ownership limits input to the active surface.
- **Visual styling:** focused effects are presentation-only, so they do not
  alter control values or application state.

## Validation performed

- Fetched and rebased local `master` onto `origin/master`; `HEAD` and
  `origin/master` both resolve to `e509b17e7a8f741f89c1e2a36539c32122467d34`.
- Restored the dirty local worktree after the clean rebase without conflicts.
- Ran `git diff --check` successfully.
- Built the unsigned ARM64 Release iOS IPA after the rebase with:

  ```sh
  platforms/ios/scripts/build-ios-ipa.sh
  ```

  The build completed successfully. The packaging script's normal compiler
  warnings remain non-fatal and are unrelated to this SwiftUI change set.

## Files changed

The archive includes the relevant iOS application files, grouped as follows:

- `Models/ControllerAccessibilityNavigation.swift` and
  `Models/MenuControllerInputRouter.swift` — shared focus graph and input
  ownership.
- `Views/GameListView.swift`, `RootView.swift`, `BIOSListView.swift`,
  `QuickMenuView.swift`, `PerGameSettingsPanel.swift`, and
  `ControllerGameContextMenu.swift` — library, root navigation, BIOS, menu,
  and per-game integration.
- `Views/Settings/` and `Views/Settings/PerGame/` — settings roots, subpages,
  rows, toggles, number controls, and per-game editors.
- `Views/ControllerNavigationOrbField.swift`, overlay design, dynamic
  background presentation, and game-screen views — controller visual feedback
  and presentation adjustments.
- Supporting state, Info, cheats/patches, and related iOS UI files needed by
  those interactions.

## Usage examples

- Use the D-pad or left stick to move one semantic focus target at a time.
- Use the right stick to scroll the currently frontmost library, menu, or
  settings surface.
- In BIOS, navigate right from Boot BIOS to Import BIOS, then Refresh.
- In Quick Menu, move left from Resume/Continue to Stop and remain on
  Resume/Continue when moving up at the top boundary.
- Use Select + Start to open Quick Menu; controller focus then follows the active menu
  or its per-game settings path.

## Intentionally not changed

- No emulator-core, renderer, timing, BIOS-content, game-library data, or
  Android behavior changes.
- No private third-party, generated, build, derived-data, or IPA files are in
  the source archive.
- No remote branch or pull request is created automatically; the local feature
  branch and archive are ready for review and publication.
