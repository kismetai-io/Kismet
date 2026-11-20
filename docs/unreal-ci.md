# Unreal pull-request CI

Every pull request from a branch inside `kismetai-io/Kismet` builds and tests
Unreal Engine 5.6–5.9 on Windows and Apple Silicon Mac: eight parallel jobs.
New commits cancel superseded PR runs. Main requires `Static checks` and
`Required CI` on an up-to-date branch. The latter succeeds only when every
matrix job succeeds; failed, cancelled and skipped builds cannot satisfy it.
Forks receive static checks but must be moved to a repository branch before
merging because engine-manifest secrets are unavailable to forks.

Each job packages Editor, Development and Shipping configurations using
`KismetAI/package.ps1`, then runs `KismetAI` and `KismetLayoutCore` automation
from the source package. Failed, unrun or incomplete suites fail the job.
Packages, checksums and test reports are retained for seven days.

The [draft release gate](release-gate.md) uses the same matrix after merging an
unpublished plugin version. Evals are not enabled by either workflow.

## Runner settings

Unity compilation, all logical cores and Unreal's default local UBA executor
remain enabled. Downloads use eight workers and a 2 GiB shared buffer. Every
selected engine file is hash-checked. No engine cache is uploaded to GitHub.

The download excludes other platforms, symbols, samples, templates and extras.
Runtime content stays because automation needs it. Mac runs `UnrealEditor-Cmd`
with `-skipminspeccheck -buildmachine` to avoid the 8 GiB advisory dialog on
7 GiB runners. Jobs have a 90-minute limit; automation has a 10-minute limit.

## Engine manifest maintenance

Eight encrypted manifests live under `.github/engine-manifests/releases/`, with
repository secret `UE_RELEASE_MANIFEST_KEY`. See [release maintenance](release-gate.md#credentials-and-maintenance)
for scope and runner versions. The older UE 5.8-only manifests and key are
retained for older workflow revisions; current PR CI uses the full matrix.

To replace a manifest, obtain its installation metadata through an authorized
Epic account, install `legendary-gl==0.21.1` and `pycryptodomex==3.24.2`, and use
`.github/scripts/engine_manifest.py --prepare` with the existing release key.
Never commit plaintext manifests, keys, account credentials or engine binaries.
Update all encrypted manifests together when rotating the release key.
