# Contributing

Build and test a change in a separate Unreal project before submitting it.
The plugin lives in the [`KismetAI/`](KismetAI) folder of this repository; link or
copy that folder into your test project's `Plugins/` directory.
Contributions to KismetAI-owned code use Apache License 2.0. Retain third-party
notices and identify code copied or adapted from elsewhere.

- Keep UObject access on the game thread.
- Keep the runtime and layout modules independent of editor modules.
- Use Unreal naming conventions, tabs, and `#pragma once` in headers.
- Add a regression test for behavior changes and verify compile errors are
  handled by the agent loop.
- Test provider execution while signed out and skill publishing while signed in
  when changing auth, provider routing or telemetry. Account credentials must
  never be used as provider credentials.
- Never commit keys, account tokens, private project data or generated outputs.

## Branches and pull requests

- `main` is the trunk. Fork the repository, branch from `main`, and open a pull
  request against `main`.
- Keep a pull request to one change. It is squashed to a single commit when
  merged, so the title should describe the change.
- Rebase on `main` if it has moved; do not merge `main` into your branch.
- Each pull request runs static checks (`python3 .github/scripts/static_checks.py`
  runs most of them locally). They do not compile the plugin or run its tests:
  build and run the automation tests yourself, as described below.
- Releases are tags on `main` (`v<version>`). Do not target a `release/...`
  branch unless a maintainer asks for a fix to be backported.

## Building and testing

Build the host project's Editor target as in [KismetAI/INSTALL.md](KismetAI/INSTALL.md), then run:

```sh
/path/to/UnrealEditor /absolute/path/YourProject.uproject \
  -ExecCmds="Automation RunTests KismetAI" -TestExit="Automation Test Queue Empty" \
  -unattended -nopause -NullRHI -nosplash -nosound
```

Windows and Linux support claims require real builds on those platforms.
The source archive is self-contained; the commands above do not require the
private service or evaluation repositories.

For skills, test both permitted and denied tool calls. Skill descriptions are
untrusted content; permissions must be enforced in dispatch rather than only
included in prompts.

If generated build outputs become inconsistent, close the Editor and ask
UnrealBuildTool to rebuild the host project. On macOS or Linux:

```sh
KismetAI/Scripts/rebuild.sh /absolute/path/to/UnrealEngine \
  /absolute/path/YourProject.uproject YourProjectEditor
```

The helper passes `-Rebuild -NoUBA -NoUBALocal`; Unreal manages its build outputs.
Do not manually delete project `Saved`, `Intermediate`, or plugin `Binaries`
directories. On Windows, pass the same flags to the engine's `Build.bat`.
Use `KismetAI/Scripts/set-embed.sh on` or `off` to change optional Config embedding;
Unreal tracks that marker for incremental builds. After changing the
`KISMETAI_DEV_LOGGING` environment override, use the rebuild command above.
