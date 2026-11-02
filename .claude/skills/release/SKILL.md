---
name: release
description: Cut a ygol-access release. Writes the player-facing changes into CHANGELOG.md's Unreleased section, bumps the version, tags with those notes, and pushes so the Release workflow builds the zip and publishes the GitHub release. Then watches that build and fixes it if it fails.
argument-hint: "[patch|minor|major|x.y.z]"
disable-model-invocation: true
---

# Release

`scripts/release.ps1` does the work. Run it; don't redo its steps by hand.

0. Update `CHANGELOG.md` so its `## Unreleased` section lists every player-facing change since the last tag (all of them, for the first release). The script refuses to release without it, and the section becomes the GitHub release notes.
   - Read each commit in `git log <last tag>..HEAD` by its body and diff (`git show --stat <hash>`, then the diff where the stat leaves doubt), not just its subject. A commit is player-facing when it changes what the player hears from their screen reader, which hotkeys they press, or how they install the mod. Keep the changelog to those; refactors, tests, scripts, docs, CI, logging and agent notes stay in the git history.
   - Write one bullet per feature or fix: a bold summary, then in plain words what changed for the player, with an example of what is spoken where it helps ("Elemental HERO Knospe, attack, ATK 700 (original 600)"). New features and changed behavior go under `### New`, fixes under `### Fixed`. One change spread over several commits is one bullet, and that includes a later fix to something no release has shipped yet. A fix to something an earlier release shipped gets its own `### Fixed` bullet.
   - Keep any bullets already under `## Unreleased` and add the missing ones. The section sits above the newest version.
   - When no commit is player-facing, tell the user and ask whether to release anyway, since the script needs at least one bullet.
   - Show the draft, with the commits each bullet covers and the commits left out, and wait for the user's OK. Then commit it as `Update the changelog for X.Y.Z` before step 1.
1. Preview with `powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Version "$ARGUMENTS" -DryRun`. The script rejects a leading `v`, so pass `v0.0.1` as `0.0.1`. With no argument the script bumps the patch version, or releases the current `CMakeLists.txt` version when no tag exists yet.
2. If the preview throws (not on `main`, uncommitted changes, behind `origin/main`, nothing to release, tag already exists), report the message and stop. Don't commit, stash, or pull to get past it.
3. Run the same command without `-DryRun`. It renames `## Unreleased` to `## X.Y.Z (date)`, commits that with the version bump in `CMakeLists.txt` as `Release vX.Y.Z`, tags with that section as the notes, and pushes `main` and the tag together.
4. Get the workflow run with `gh run list --workflow release.yml --branch vX.Y.Z --limit 1 --json databaseId,url,status`. It can take a few seconds to appear. Tell the user the tag and run URL now.
5. Watch the build until it finishes: `gh run watch <id> --exit-status --interval 30`. A build takes a few minutes, so run it in the background and wait for it to exit rather than polling.
6. On success, confirm with `gh release view vX.Y.Z --json url,assets` that `ygol-access-X.Y.Z.zip` is attached, then reply with the tag, the notes, the run URL and the release URL.

## If the build fails

Read `gh run view <id> --log-failed` and find the cause before changing anything.

- **CI-only problem** (runner outage, network timeout, rate limit, the MinHook FetchContent clone failing): `gh run rerun <id> --failed`, then watch again. The publish step uploads with `--clobber`, so a rerun is safe.
- **Problem in the repo** (compile error, a failing test, `scripts/package_release.ps1`, the workflow, a missing file in `third_party/prism/`): the tag already points at the broken commit, and moving a pushed tag needs the user's say-so. So fix it forward:
  1. Fix the root cause on `main`. Reproduce it locally first where you can: `powershell -ExecutionPolicy Bypass -File scripts\package_release.ps1` builds, tests and packages. CI builds with the runner's Visual Studio, which can be older than the local one, so a compile error may only show in CI; check the runner's compiler version in the log.
  2. Show the user the failing log lines, the diff, and the patch version you would release, and ask before committing. Don't commit or release until they agree.
  3. Once they agree, commit the fix with a normal imperative subject, cut the patch release with step 1 onward (no argument bumps the patch), and watch that build too.
- **Tag and `CMakeLists.txt` disagree**: `release.ps1` should prevent this, so find out why before fixing; then cut a new patch release as above.

Stop and report after three failed builds, or sooner when the fix needs something only the user can do (repo secrets, permissions, GitHub settings, a decision to delete a tag). The report says which run failed, the log lines that show why, what you tried, and the release that is still broken on GitHub, if any.
