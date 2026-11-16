# Install KismetAI

KismetAI supports Unreal Engine 5.6, 5.7, 5.8 and 5.9 on macOS (Apple Silicon)
and Windows. Linux archives are not published yet; the plugin can be built
from source on Linux, but that build is not part of the tested release set. Use a licensed Unreal Engine installation. Each packaged
archive is built for one engine version and one platform; an archive for a
different engine version will not load.

No Kismet account is needed. A Kismet account is optional and enables bug
reports from the editor and diagnostics sharing; it does not supply inference
credentials or quota.

## Which archive

Every release is published in two forms, from the same build:

- `KismetAI-<version>-UE<engine>-<platform>-release.zip` holds ready-to-run
  binaries. `Source/` contains only generated `Build.cs` stubs that link those
  binaries. Use it with an engine from the Epic Games Launcher.
- `KismetAI-<version>-UE<engine>-<platform>-release-src.zip` holds the same
  binaries plus the complete source and build scripts. Use it if you compiled
  the engine yourself, if you package a game with the plugin enabled, or if you
  want to modify the plugin.

## Install a packaged build

1. Close the editor.
2. Extract the archive. It contains one folder named `KismetAI`; do not rename it.
3. Move that folder into `Engine/Plugins/Marketplace/` of your engine, which
   makes it available to every project on that engine:
   - macOS: `/Users/Shared/Epic Games/UE_<engine>/Engine/Plugins/Marketplace/`
   - Windows: `C:\Program Files\Epic Games\UE_<engine>\Engine\Plugins\Marketplace\`

   To install for a single project instead, use `<YourProject>/Plugins/`.
4. Open the project, enable Kismet under Edit > Plugins if needed, and open its
   panel from the toolbar.
5. Choose your provider and configure your own credentials. Model requests go
   through that connection.

The installers at https://kismetai.io/download do steps 1 to 3 for every
matching engine they find.

## Build from source

Use this with the `-src` archive or a clone of the source repository. It is
required for an engine you compiled yourself: such an engine only loads plugins
built against it.

1. Close the editor.
2. Place the source in `<YourProject>/Plugins/KismetAI`, so `KismetAI.uplugin`
   is directly inside that directory. From a clone of the repository, that is
   its `KismetAI` folder: copy it, or link it with `ln -s` (`mklink /D` on
   Windows). If the folder came from a `-src` archive
   and your engine is self-built, delete its `Binaries` folder.
3. Build the project's Editor target with the engine's supported compiler:

```sh
# macOS
"/Users/Shared/Epic Games/UE_5.9/Engine/Build/BatchFiles/Mac/Build.sh" \
  YourProjectEditor Mac Development /absolute/path/YourProject.uproject

# Linux (untested; no release build is published for it yet)
<engine>/Engine/Build/BatchFiles/Linux/Build.sh \
  YourProjectEditor Linux Development /absolute/path/YourProject.uproject
```

```bat
:: Windows
"C:\Program Files\Epic Games\UE_5.9\Engine\Build\BatchFiles\Build.bat" ^
  YourProjectEditor Win64 Development C:\path\YourProject.uproject
```

   A Blueprint-only project has no Editor target of its own: open the project
   and accept the prompt to rebuild the missing modules.
4. Open the project, enable Kismet if needed, and configure your provider.

When building from source, keep `Source/`, `Config/` and `Scripts/` together
with the plugin. Embedding configuration is optional for development and
enabled for packaged binaries. Source builds can adapt configuration and
rebuild it.

## Linux notes

Linux is not a released platform yet. If you build from source on Linux:

- The plugin calls your model provider through `/usr/bin/curl`. Install your
  distribution's curl package if that file is missing.
- Provider keys are stored encrypted under `~/.kismet/`. To use a key from the
  environment, export it and start `UnrealEditor` from that same terminal.

## Upgrading from hosted inference

If upgrading from a build configured for Kismet-hosted inference, open the
panel's **More** menu (the three dots), review **API keys**, then choose
**Use my provider**. Requests remain blocked until you explicitly confirm the
change to your provider connection and its charges.

## Local server and logs

The local HTTP server listens on loopback port 8323. Requests still require its
local server token, and unexpected browser origins are rejected. Signing out of
Kismet does not disable these local protections.

Do not edit an engine BuildId to force an incompatible binary to load. Rebuild
against the target engine instead. Logs are in the project's normal log folder;
search for `LogKismetAI` and `LogKismetAIAgent`.

## Updating packaged installations

The updater downloads a package from the configured release service and replaces
the installed plugin. It does not verify a cryptographic package signature or
a trusted package digest. A successful download, replacement and restart does
not establish package authenticity. Authenticated release signing and verification
remain unfinished implementation work; they require a producer format and trust
provisioning shared by the release service and plugin.
