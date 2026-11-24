# Scanner proofs

Prove the five brace/quote scanners in `src/expand.c` with Frama-C WP.

## Run

    make verify

## Expected

    [wp] Proved goals:  108 / 108

## Tooling

Frama-C 33.0 (Arsenic) and Alt-Ergo 2.0.0; `frama-c` must be on `PATH`.

## Covered

`flat_skip_marker`, `flat_skip_single`, `flat_skip_double`, `flat_balanced`,
`flat_skip`.

Contracts assert:

- `\old(i) < \result <= len`
- `flat_balanced`: `\result == len || s[\result - 1] == close`

Loops carry `i <= len`, a decreasing variant, and (for `flat_balanced`)
`depth >= 1` and `depth <= i`.

## Not covered

Glob (`fnmatch`, `glob`, `opendir`, `stat`), `flat_is_range`,
`brace_find_group_flat`, `flat_split_alts`, and the arithmetic evaluator: they
call libc, allocate, or use `strtoll`.

## Caveats

- WP uses the `typed` model; `s[0 .. len-1]` must be readable.
- A backslash skip is capped at `len` (`i = len` when `i + 1 == len`). This
  keeps `i <= len` and does not change any return value.
