# Controller focus obligations owed by `feat/retroarch-shaders`

Phase 04.3 added three screens on `feat/retroarch-shaders` while Phase 1.1's controller
navigation sits on `feat/controller-navigation`. Neither branch can see the other's guards, so
these obligations only come due when the two merge. Written down here, in the tracked tree,
because the phase records that carry them live under `.planning/`, and `git clean -xdf` removes
that whole directory.

Verified against `feat/controller-navigation` on 2026-08-17 with `git show`, no checkout.

## Two of these fail silently. Read that part first.

`test_focus_coverage` asserts only that a *screen* declares a focus order. It does not check
which items are in it. `QuickMenuView` already declares one, so a row added without its focus ID
renders, the suite stays green, and the D-pad simply never stops on it.

The trap is that a loud obligation fires in the same merge. Bumping the pinned counters until the
suite passes is **not** confirmation that the quiet ones were done. Nothing will ever say so.

## The obligations

| # | Owed by | What | Fails |
|---|---|---|---|
| 1 | Shaders settings page | The `settingsDetail(for:)` arm must stay the two-line no-argument shape `PANE_DESTINATION` matches. The pane enrols itself from the enum regardless, so a mis-shaped arm leaves the roster unable to resolve it | Loud — `self_check()` names the pane |
| 2 | Shaders settings page | Root row converts to `paneRow(...)` with its eighteen neighbours. `test_a_row_has_one_push_path` asserts `LINKS_ALLOWED == {}`, so no file under `Views/` may contain `NavigationLink` | Loud |
| 3 | Shaders settings page | Add a `PUSH_ROW_SITES` entry. `GraphicsSettingsView` already carries `.focusableControls()` | Loud — equality assertion |
| 4 | `ShaderSettingsView` | A `ControllerHint(… .back)` in the body, and a declared focus order. Two guards, not one: `test_focus_coverage` reads the order, `test_declares_exit` reads the hint, and satisfying either leaves the other red | Loud — `PENDING_ADOPTION` is asserted empty |
| 5 | `ShaderControlPanel` | Add to `EXTRA_SCREENS`. In-game panels are named by hand and cannot enrol themselves the way a `SettingsPane` case does | Loud — unresolvable roster name |
| 6 | `ShaderControlPanel` | A `ControllerHint(… .back)` in the body, and a declared focus order | **Silent** |
| 7 | Quick Menu shaders row | A focus ID in `gameToolIDs`, guarded on `shaderChainAvailable` — not `vmMenuAvailable` — positioned after the `cheats` entry and before `resetROM`. That list carries the whole right column, so the end of the Game Tools card is the middle of it, not the end. `focusIDs` pairs the columns by index, so a wrong position walks the ring diagonally | **Silent** |
| 8 | Both | Bump the pinned counters in `test_focus_authoring_surface_is_unchanged`. Expect `.focusItem(` 70 → 71 and `.focusableControls(` 37 → 39, `.controllerPad(` untouched. Reconcile the whole set in one edit — a merge carrying both plans moves the same counter twice | Loud |

A `FocusID` appended to a list is not a modifier call site. Do not bump a counter for obligation 7
or the total overshoots. Obligations 4 and 6 are the two that move `.focusableControls(`, one each,
which is where 37 → 39 comes from.

## Also owed, unrelated to focus

`ShaderPresetBrowserView` and `ShaderPresetSaveSheet` are absent from `EXTRA_SCREENS`. That gap
pre-dates Phase 04.3 and is recorded here only so it is not mistaken for something this phase
introduced.
