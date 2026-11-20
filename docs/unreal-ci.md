# Unreal pull-request CI

Pull requests from branches inside `kismetai-io/Kismet` compile, package and test
Kismet with Unreal Engine 5.8.4 on `windows-2022` (Win64) and `macos-26` (arm64).
Both jobs start in parallel. New commits cancel superseded runs for that PR.
This PR workflow has no push or scheduled triggers. Fork PRs run static checks
only. The separate [draft release gate](release-gate.md) builds the full engine
matrix on main and when release infrastructure changes.

Each job builds the Editor, Development and Shipping configurations through
`KismetAI/package.ps1`, then runs `KismetAI` and `KismetLayoutCore` automation
from the packaged source archive. A failed, unrun or incomplete test suite fails
the job. Packages, checksums and test reports are retained for three days.

## Runner settings

Unity compilation stays enabled for clean checkouts, all logical CPU cores are
available to UnrealBuildTool, and Unreal's default local UBA executor is retained.
These were the fastest successful settings in the hosted-runner experiment:
Windows packaging took 3m58s and Mac packaging took 6m44s, excluding engine setup.
Full jobs also download and verify about 26 GiB (Windows) or 38 GiB (Mac).
No engine cache is uploaded to GitHub.

The engine download excludes other platforms, symbols, samples, templates and
extras. Runtime content stays because automation needs it. Downloads use eight
workers and a 1 GiB shared buffer; every regular file is hash-checked before use.
Mac runs `UnrealEditor-Cmd` with `-skipminspeccheck -buildmachine` to avoid hardware
advisory dialogs on the 7 GiB runner. Each job has a 60-minute timeout; the test
process has a 10-minute timeout.

## Engine manifest maintenance

`.github/engine-manifests/ue-5.8.4/` contains AES-256-GCM encrypted, trimmed Epic
installation manifests, not engine binaries or account credentials. The 32-byte
hex key lives only in the repository Actions secret `UE_MANIFEST_KEY`. Authentication
is checked before a decrypted manifest is written to the runner's temporary disk.
The key is exposed only to the decryption step, and checkout does not retain GitHub
credentials. Anyone permitted to run code on a repository branch can access its
Actions secrets; do not move this job to `pull_request_target` for fork support.

To replace the engine manifests, obtain both installation manifests using an
Epic account with Unreal access, install `legendary-gl==0.21.1` and
`pycryptodomex==3.24.2`, and set `UE_MANIFEST_KEY` to a securely generated 32-byte
hex value without logging or committing it. Prepare each platform with:

```sh
python .github/scripts/engine_manifest.py /private/Windows.manifest \
  .github/engine-manifests/ue-5.8.4/Windows.manifest.enc --prepare
```

Repeat for Mac, update the repository secret and both encrypted files together,
and run a same-repository PR to verify both builds. Changing Unreal versions also
requires updating the workflow paths and the test project's engine association.
Never commit plaintext manifests or the encryption key. Rotate the key and
replace both encrypted files if access to it is compromised.
