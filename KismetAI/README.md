# KismetAI

KismetAI lets an AI agent inspect, create and edit Unreal Engine Blueprint graphs.
It runs in the editor and checks changes by compiling and inspecting the result.

The plugin source is Apache-2.0 licensed. Configure your own supported provider to
run locally without a Kismet account. Model requests use your own provider
credentials and connection. An optional account adds bug reports from the editor and diagnostics sharing.
Kismet does not provide hosted inference or model quota. Provider charges are
handled by your provider; there is no Kismet checkout.

## Start

1. Install Unreal Engine and its platform compiler.
2. Put this `KismetAI` folder at `<YourProject>/Plugins/KismetAI`.
3. Build the project's Editor target, then open **Tools → Kismet**.
4. Configure a provider in the panel and make a small request on a test Blueprint.

See [INSTALL.md](INSTALL.md) for build commands.
Keep your project in source control and review generated edits.

## Development

The editor adapter lives in `Source/KismetAI`, the provider-neutral runtime in
`Source/KismetAIRuntime`, and graph layout in `Source/KismetLayoutCore`.
Configuration and embedded panel UI are in `Config`.

See CONTRIBUTING.md for tests and module boundaries and SECURITY.md for reports
(both at the root of <https://github.com/kismetai-io/Kismet>, and beside this
file in release archives), and [NOTICE.md](NOTICE.md) for third-party
terms. Unreal Engine is a separate dependency and is not covered by our license.

## Source archive

Install Python 3 and gitleaks, then run:

```sh
python3 Scripts/package_source.py /tmp/KismetAI-source.zip
```

The exporter includes source, configuration, public documentation and notices.
It excludes Git history, development projects, local data and engine binaries,
and scans the exact archive input for secrets. Review Epic's distribution terms
before publishing editor-tool source or compiled packages.
