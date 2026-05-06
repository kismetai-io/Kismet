# Kismet — Install

Binary release for Unreal Engine 5.6. Builds are provided for multiple platforms (Mac, Win64, Linux).

## Install

1. Close the Unreal Editor.
2. Extract the release archive and copy the `KismetAI/` folder into a `Plugins/` directory — either:
   - **Per-project:** `<YourProject>/Plugins/KismetAI/`
   - **Editor-wide:** `<EngineDir>/Engine/Plugins/KismetAI/`

   The resulting layout should look like:
   ```
   Plugins/KismetAI/KismetAI.uplugin
   Plugins/KismetAI/Binaries/<Platform>/...
   ```
3. Launch the editor. Kismet loads automatically at `PostEngineInit`.

## API key

The plugin expects an Anthropic API key from one of:

- Environment variable `ANTHROPIC_API_KEY`
- File `~/.kismet/credentials` (single line, the key). The legacy path `~/.kismetai/credentials` is still read as a fallback.
