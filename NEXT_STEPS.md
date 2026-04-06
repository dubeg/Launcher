# Next Steps

## Current Priorities

- Keep font scope narrow: use `Segoe UI` only for now.
- Treat packaged/UWP app support as optional nice-to-have work, not core scope.
- Focus the next implementation pass on text editing behavior, selection, and file-search wildcard support.

## Text Editing

- Replace the current append/backspace-only query handling with a proper text input model.
- Track caret position independently from string length.
- Track anchor/caret pairs so selection exists as a first-class concept.
- Support replacing the current selection when typing.
- Support deleting the current selection.
- Support `Ctrl+A` select all.
- Support left/right caret movement by character.
- Support `Home` / `End`.
- Support word movement with `Ctrl+Left` / `Ctrl+Right`.
- Support word deletion with `Ctrl+Backspace` / `Ctrl+Delete`.
- Support shift-extended selection with arrows, `Home`, `End`, and word movement.
- Consider adding clipboard commands such as copy, cut, and paste for parity with common editors.

## Text Rendering

- Render a visible caret in the input field.
- Add caret blink timing.
- Render selection highlight behind selected text.
- Make selection rendering work with the shaped text pipeline.
- Add horizontal scrolling for long queries so the caret remains visible.
- Keep the implementation centered on `Segoe UI` only.

## Search

- Add wildcard support in file mode using Everything query syntax.
- Decide wildcard behavior:
  - allow raw Everything wildcards directly when `*` or `?` appears
  - preserve existing non-wildcard fuzzy behavior for ordinary queries
- Improve ranking when wildcard searches return many results.
- Move Everything querying toward an async path so file mode stays responsive under heavier searches.

## Catalog And Ranking Polish

- Improve `System32` alias coverage in `data/system_aliases.json`.
- Improve deduplication between Start Menu shortcuts and executable hits.
- Improve normalization and ranking heuristics for app names and file paths.
- Make `config/locations.json` parsing more robust.

## UI Polish

- Improve row layout, spacing, and visual hierarchy.
- Improve truncation for long subtitle/path lines.
- Refine selected-row styling.
- Remove or reduce temporary debug scaffolding once the next pass stabilizes.

## Nice To Have

- Packaged/UWP app discovery and launch support.
- Mouse hit-testing for caret placement and selection.
- Icon rendering for results.
