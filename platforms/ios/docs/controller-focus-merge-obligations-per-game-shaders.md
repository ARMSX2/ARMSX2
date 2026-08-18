# Controller focus obligations owed by the per-game shader section

A Wave 8 addendum to `controller-focus-merge-obligations.md`. The two are read together at the
merge; this one is separate only because that table's eight rows are all authored by commits that
landed, while this work is newer than the last of them. Folding these rows into that table is
available at any time now that `98daadde72` settled its obligation 7 — a merge-time tidy, not a
blocker.

Verified against `feat/controller-navigation` on 2026-08-18 with `git show`, no checkout.

## The obligations

| # | Owed by | What | Fails |
|---|---|---|---|
| A1 | `PerGameShaderSection` tri-state picker | A `.steps($chain, through: [-1, 0, 1])`. `PerGameTab.swift` declares that helper for exactly this: activate steps a picker on and wraps, so no row on a per-game tab opens a list the ring cannot enter. `GraphicsTab` carries 20 of them and every picker on it has one | **Silent** — no guard counts pickers |
| A2 | The Preset row | A focus item of its own, or not. `GraphicsTab` applies `.focusableControls()` at its own `:108`, which registers ordinary controls automatically, but a row that opens a sheet is the case `steps` was written for. Decide which of the two that branch expects for a sheet-opening row before writing either — a second registration stops the D-pad twice on one row, and none stops it never | **Silent** — same reason |
| A3 | `PerGameShaderSection`, the type | It is new, in a new file, and on no roster. `EXTRA_SCREENS` names all nine per-game tabs and `PerGameTab` itself, but nothing inside a tab. Decide at merge whether it is a screen (a roster entry, plus a declared focus order, plus an exit hint — two guards, not one) or a component like `BackgroundAssetRow` (a `COMPONENTS` entry, which exempts it from both). It is embedded in a tab that already declares both, so component is the likely answer, but somebody has to make that call: a type in neither dict is in no plan | **Loud** if added to `EXTRA_SCREENS` and left bare; **Silent** if simply forgotten |
| A4 | All of it | The pinned counters in `test_focus_authoring_surface_is_unchanged`. The branch pins `{".focusItem(": 70, ".focusableControls(": 37, ".controllerPad(": 15}` with `declared == 8`, but obligation 8 of the sibling document already claims 71 and 39 for plans 11 and 12, so state this plan's arithmetic on top of those. `.focusItem(` +1 if A2 is answered with a focus item on the Preset row, +1 again if the Clear Preset row takes one, otherwise +0. `.focusableControls(` +1 only if A3 is answered "screen", otherwise +0. `.controllerPad(` untouched. `.steps(` is not one of the three tokens counted, so A1 moves nothing. Reconcile the whole Wave 8 set in one edit — a merge carrying all of these moves the same counter three times | **Loud** |
| A5 | `ShaderPresetBrowserView` | It gains a second construction site: the per-game panel's sheet builds it directly, where before it was constructed in exactly one place, `ShaderChainSection.swift:48`, and reached two hosts only because two views mount that section. Its absence from `EXTRA_SCREENS` is recorded under "Also owed" in the sibling document as pre-dating Phase 04.3, so this widens that gap rather than introducing it. Do not attribute it here | **Silent** |

## One thing the sibling document does not say, and it is not this plan's

`test_a_row_has_one_push_path` collects every `NavigationLink` under `Views/` and asserts the
result equals `LINKS_ALLOWED`, which is `{}`. No shader file exists on `feat/controller-navigation`
at all, so the whole shader tree arrives at that assertion at once —
`ShaderPresetBrowserView.swift` included, and it pushes a link per subfolder. Obligation 2 of the
sibling document states the rule while naming only the Shaders settings root row, so the browser's
own links are owed and unlisted.

That debt landed with the browser in plan 05, not here. `PerGameShaderSection` carries no
`NavigationLink` at all, by construction: the panel builds a `NavigationStack` in portrait only,
and landscape is a rail and a detail pane in a plain `HStack`, so a link on a per-game tab is a row
that renders and does nothing on a wide panel. The preset row is a Button and the browser is
presented as a sheet from the panel. Recorded here because the merge is where somebody counts.

## The warning, again

Three of the five above are silent and one is loud. They fire in the same merge. Bumping A4 until
the suite goes green is not evidence that A1, A2 or A5 were done, and nothing in either tree will
ever say so.

## Addendum, 2026-08-18: the shader catalogue browser

`04.3-15` added `ShaderCatalogBrowserView` and a row that opens it from `ShaderSettingsView`.

| # | Owed by | What | Fails |
|---|---|---|---|
| A1 | `ShaderChainSection` | The Download row is a second `NavigationLink` in that section, beside the Preset one it already carries. `test_a_row_has_one_push_path` asserts `LINKS_ALLOWED == {}`, so both convert together, and the in-game panel needs whatever replaces them to still push from inside its own `NavigationStack` | Loud |
| A2 | `ShaderCatalogBrowserView` | Add to `EXTRA_SCREENS`, then a `ControllerHint(… .back)` and a declared focus order | **Silent** for the hint and the order; loud for the roster name |
| A3 | `ShaderCatalogBrowserView` | The Get button and the search field are the only focusable controls, and the list is 867 rows over 27 sections. Whoever adopts this has to decide whether the pad moves by row or by section before writing either | **Silent** |
| A4 | Both | The pinned counters take another `.focusableControls(` for the browser, on top of the deltas the two sibling documents already claim | Loud |

A3 is the one worth reading twice. The other screens in this phase have between two and twenty
rows. This one has 867, and a D-pad that steps one row at a time crosses a category in about
thirty presses.
