# Maintaining KismetAI

For people who review changes, cut releases, or keep a fork current. Read
[CONTRIBUTING.md](CONTRIBUTING.md) first for coding rules and how to build and
run the tests.

## Repository layout

- `KismetAI/` is the plugin: the folder that goes into a project's or an
  engine's `Plugins/` directory, and the one release archives are built from.
- `LICENSE`, `CONTRIBUTING.md`, `SECURITY.md` and this file sit at the root.
  The plugin folder carries its own `LICENSE`, `NOTICE.md` and `INSTALL.md`,
  because those ship inside every archive.
- Tags `v0.9.1` and earlier mark releases made before the plugin was open
  source. They carry binaries only, under the license included in each
  archive; the source in this repository starts at 0.10.0.

## What is supported

- Unreal Engine 5.6, 5.7, 5.8 and 5.9.
- macOS on Apple Silicon and Windows (Win64).
- Linux (x86_64) compiles from source but is not a released platform yet: no
  Linux archive is published and no Linux build has been tested end to end.
- One plugin version at a time. Fixes go into the next release; older releases
  are not patched. Releases before 0.10.0 are archived, keep their original
  license and receive no changes.

A platform or engine version is supported only while a release build for it
compiles and its automation tests pass. If one stops building, either fix it
before the release or remove it from this list, the README and the website in
the same change.

## Before merging a change

1. The project's Editor target builds with the change on at least one supported
   engine version.
2. `Automation RunTests KismetAI` passes (command in CONTRIBUTING.md).
3. Behaviour changes come with a regression test. A change to a tool also
   updates its manifest in `KismetAI/Config/tools_*.json`, its registry definition and
   its dispatch entry; the `KismetAI.ToolRegistry` and `KismetAI.ToolManifest`
   tests fail when these disagree.
4. `KismetAIRuntime` and `KismetLayoutCore` gain no editor dependencies. Both
   are compiled into game targets, where editor modules do not exist.
5. Code copied or adapted from elsewhere keeps its notice, and NOTICE.md and
   `ThirdPartyNotices/` are updated to match.
6. No keys, tokens, private project data or build output are committed.

Two kinds of breakage do not show up on a single machine, so check them before
a release rather than per change:

- **Linux.** macOS file systems ignore case and macOS exports every symbol.
  A wrong-case `#include`, a missing `*_API` export, or a header that only
  arrives indirectly on macOS fails on Linux alone. Build for Linux before
  claiming Linux support, and keep it compiling in the meantime.
- **Other engine versions.** An API introduced in a newer engine breaks older
  ones. Build against the oldest supported engine as well as the newest.
  Differences between engine versions belong in
  `KismetAI/Source/KismetAI/Private/KismetAIEngineCompat.h`; use its helpers instead of
  engine members that only exist in newer versions.

## Versions and branches

- `main` is the trunk and the only long-lived branch. It must build on every
  supported engine version at every commit.
- Changes reach `main` through pull requests from short-lived branches
  (`fix/...`, `feat/...`) or from forks, squashed to one commit each. Delete the
  branch after merging.
- Never force-push `main` or move a release tag.
- The version is `VersionName` in `KismetAI/KismetAI.uplugin`, in `major.minor.patch`
  form. Bump the patch number for fixes, the minor number for new behaviour or
  anything that changes stored data, tool contracts or configuration files.
- A release is the commit tagged `v<version>`. Every published archive for that
  version is built from that tag, on every platform.
- Releases are normally tagged on `main`. When an older release needs a fix
  and `main` has moved on, create `release/<major>.<minor>` from that release's
  tag, cherry-pick the fix from `main`, and tag the patch release there. Fix
  `main` first, so a fix is never only on a release branch. Do not create
  release branches in advance.

## Cutting a release

1. Set `VersionName`, write the release notes, commit, and tag `v<version>`.
2. Build each engine version and platform with `KismetAI/package.ps1`:

   ```sh
   pwsh ./KismetAI/package.ps1 -EnginePath "<engine>" -LauncherEnginePath "<engine>" \
     -TargetPlatforms Mac -Variant release
   ```

   Use `Win64` on Windows. Linux, when it is released, is compiled with
   `RunUAT BuildPlugin` inside Epic's Linux image and then packaged with
   `-PrebuiltPackageDir`; see the parameter help at the top of `package.ps1`.
3. Each build produces two archives:
   - `...-release.zip`: binaries, configuration and notices. `Source/` holds
     generated `Build.cs` stubs. This is the default download and what the
     installers and the in-editor updater fetch.
   - `...-release-src.zip`: the same build plus the complete source.
4. Check an archive before publishing: it contains `LICENSE`, `NOTICE.md` and
   `ThirdPartyNotices/`; the plugin loads in a clean project on that engine;
   the toolbar icon appears; a first agent run works with a provider key.
5. `-LauncherEnginePath` stamps the engine's build id into the binaries so they
   load on Epic's builds of that engine. Never edit a build id by hand to make
   a binary load on a different engine.
6. Publish the archives with their SHA-256 values. The installers refuse an
   archive without one.

`KismetAI/Scripts/package_source.py` writes a source-only archive and scans it for
secrets with gitleaks. Run it before publishing source outside the repository.

The Kismet team's upload, release-manifest and website steps are not part of
this repository. A fork can publish its own archives anywhere; the in-editor
updater only follows the release service it is configured for.

## Withdrawing a release

Mark the release as deprecated in the release service. Installed copies fall
back to the newest release that is not deprecated on their next update check.
Do not delete or replace a published archive under the same name: its SHA-256
is already recorded and an installer would reject the replacement.

## Models and providers

Model and provider data live in `KismetAI/Config/models.json` and
`KismetAI/Config/providers.json`. Adding a model is one entry in `models.json`; the
`KismetAI.ModelCatalog` tests validate the file. A new entry is configuration,
not proof that the provider works: run a real request before announcing it.

## Issues and security reports

- Reproduce a bug on a supported engine version before fixing it, and ask for
  the engine version, platform, plugin version and the `LogKismetAI` lines.
- Never ask a reporter for credentials or private project files.
- Security reports arrive privately; see [SECURITY.md](SECURITY.md). Fix and
  release before discussing the details in public.

## Known limits to keep stated honestly

- The updater does not verify a package signature (see INSTALL.md).
- Rollback restores in-memory state for supported edit batches. It is not a
  disk-level undo.
- Provider keys are stored in encrypted files, not an operating-system
  credential vault.

Keep these in the README, the website and release notes until they change in
the code.
