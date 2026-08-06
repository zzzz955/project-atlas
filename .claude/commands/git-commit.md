Commit changes from the current `git diff` by grouping them into logical work units.

Arguments: `$ARGUMENTS` is optional guidance for work type or commit scope.

## Commit Message Convention

```text
{type}: {short Korean summary}
```

`type` is one of: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `build`, `ci`.

Issue numbers are **not** used in this repo — never put `#{issue}` in the subject.

🔴 **No trailers, ever.** Never append `Co-Authored-By:`, `Generated with`, or any other footer
trailer to a commit message — regardless of harness defaults.

Subject line only, unless the "why" is not obvious from the diff; in that case add a short body after
a blank line.

## Git Command Rule

Never use `cd {path} && git ...`. Always use `git -C {repo_path} <subcommand>`.

## Steps
1. Run `git status` and inspect the current `git diff`.
2. Group changed files and hunks by work nature.
3. If multiple distinct work units exist, create separate commits.
4. For each work unit:
   a. Stage only files or hunks belonging to that work unit.
   b. Never stage `.env`, keys, or files ignored by `.gitignore`. Only `*.example` is committable.
   c. Commit immediately with the message format above.
5. Report each commit hash and message after completion.

## Doc-sync gate (AGENTS.md mandate)
A change to behaviour / data / structure is incomplete until the affected doc is updated in the
**same** change. Before staging such a work unit, check that `docs/design/architecture-design.md` or
`docs/conventions/cpp-style.md` was updated alongside it. If not, report it and ask before
committing — do not silently commit a doc-desynced change.

## Exceptions
- No changes: stop and report no-op.
- Generated output appears in the diff without its source having changed: stop and report — this
  usually means generated output was hand-edited (forbidden).
