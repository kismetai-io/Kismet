# Enforce release gates

## Goal
Maintainers can merge only a fully tested Windows/Mac matrix and publish the exact
candidate they smoke-tested. Useless if skipped builds satisfy the merge gate,
a later merge replaces tested packages, or newer publishing code runs instead.

## Context
- Main requires PRs but has no required status checks.
- Full-matrix PR coverage is restricted by paths; ordinary PRs test only UE 5.8.
- Draft refresh replaces assets, and publishing checks out moving main.

## Design
Every same-repository PR runs eight engine/platform jobs plus static checks.
A required aggregate fails unless the entire matrix succeeds, including when
builds are skipped. Fork changes must be moved to a repository branch for merge.
Remove the redundant UE 5.8 workflow. Require static and aggregate checks on an
up-to-date branch through a separate main ruleset.

A version's first draft is frozen to its source commit. Later merges skip it;
new packages require a version bump. A failed draft upload can resume from the
same commit and original artifacts, verifying existing bytes and adding only
missing assets. Never patch the draft or overwrite existing assets.
Publishing checks out the release event's exact SHA and validates it against
the receipt and tag. Evals and backend deployment remain disabled.

TDD: yes, for draft immutability and publisher source validation.

## Verification
Test draft rejection/resume and mismatched publisher commits. Run the complete
PR matrix, inspect the effective main rules, merge only when all required checks
pass, and verify the main workflow's frozen/published-version guard.
