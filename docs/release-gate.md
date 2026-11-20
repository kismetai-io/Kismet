# Draft release gate

A push to main prepares one draft per plugin version after all eight build/test
jobs pass: UE 5.6, 5.7, 5.8 and 5.9 on Windows and Apple Silicon Mac. The same
version's later successful build replaces the unpublished draft. Published
versions are never replaced: bump `KismetAI/KismetAI.uplugin` VersionName first.
The main-branch release workflow is separate from PR CI. Changes to release
infrastructure also run the full matrix on same-repository PRs.

Each draft has 16 archives (binary and source variants for every combination)
and `release-receipt.json` with the source commit, byte sizes and SHA-256 hashes.
Smoke-test the draft packages, check that its source commit has not changed,
and publish it as a stable release. Publication verifies the tag and all hashes,
uploads the exact archives to R2, verifies the uploaded objects, then inserts
all eight Supabase release rows, including the published release notes, in one
transaction. It does not rebuild.

A publish during draft replacement fails validation if the packages and receipt
are inconsistent. Wait for draft preparation to finish before publishing. A
promotion failure leaves GitHub published but does not advertise a partial
matrix in the release catalogue. Retry the failed Actions run after fixing its
cause. Identical catalogue rows are accepted on retry; different existing rows
fail. R2 objects use version/hash/name keys and are never overwritten.

Evals, database migrations, Edge Function deployments, Discord announcements
and MCP Registry publication are not run by this gate. MCP Registry retains
its separate manual workflow.

## Credentials and maintenance

Builds use the repository secret `UE_RELEASE_MANIFEST_KEY` for the eight trimmed
encrypted installation manifests under `.github/engine-manifests/releases/`.
The existing UE 5.8 PR CI has its own unchanged manifest key. No Epic credentials
or engine binaries are committed. Anyone able to execute repository-branch
workflows can access repository secrets; forks cannot run these builds.

Configure these in the `release-production` GitHub environment:

- Secret `RELEASE_R2_ACCESS_KEY_ID` and `RELEASE_R2_SECRET_ACCESS_KEY`: an R2 S3
  credential limited to read/write objects in `kismetai-releases`.
- Variable `RELEASE_R2_ENDPOINT_URL`: the account's HTTPS S3-compatible endpoint.
- Secret `RELEASE_DATABASE_URL`: a production Postgres connection limited to
  SELECT and INSERT on `public.releases` and usage of its ID sequence. No schema
  or other application-table write access is needed. The connection requires TLS.

Restrict this environment to version tags (`v*`) using deployment branch/tag
rules, so branch and PR jobs cannot use publishing credentials.

The download domain is `https://releases.kismetai.io`. The workflow has read-only
GitHub permissions except for the draft creation job. Publishing the draft is
the human approval step; no production update happens when a draft is created.

UE 5.6 uses macOS 15 and Xcode 16.4 because its supported maximum is Xcode 16.x.
Other Mac builds use macOS 26. All Windows builds use windows-2022. Unity builds,
all logical cores and Unreal's default memory scheduling remain enabled.
Release downloads use a 2 GiB shared buffer; UE 5.7/5.9 Mac manifests exceed the
1 GiB buffer used by the existing UE 5.8 PR workflow.
