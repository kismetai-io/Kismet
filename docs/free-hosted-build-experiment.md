# Free hosted Unreal build experiment

Standard GitHub-hosted runners built Kismet 0.10.0 against UE 5.8.4 Launcher binaries on Windows and Apple Silicon Mac. The engine is downloaded selectively from Epic; Unreal Engine itself is not compiled from source.

## Fastest measured configuration

Use `windows-2022` and `macos-26` with `bUseAdaptiveUnityBuild=false` and `bAllCores=true`. Normal unity compilation groups source files; adaptive mode excluded nearly every file from those groups in the packaged source copy. Windows used four logical CPUs and Mac used three M1 cores. Unreal's memory-based scheduling remains enabled.

- Windows baseline: about 20 minutes for compilation and packaging, with individual compilation and two processes.
- Windows unity, two processes: about 5 minutes 55 seconds; 1,215 KismetAI tests passed.
- Windows unity, four processes: 3 minutes 58 seconds for the packaging step, including 3 minutes 55 seconds in UAT. Engine provisioning took 9 minutes 10 seconds, tests took 1 minute 51 seconds, and the complete job took 15 minutes 44 seconds. All 1,228 discovered tests passed: 1,215 KismetAI, 10 KismetLayoutCore, and three additional tests matched by the broad Kismet filter. 134 passed with warnings; none failed or remained unrun. [Windows result](https://github.com/kismetai-io/Kismet/actions/runs/44321121791/job/134860021119).
- Intel Mac baseline: 43 minutes 6 seconds in UAT for the arm64 package. [Intel Mac result](https://github.com/kismetai-io/Kismet/actions/runs/44313475931/job/134833342932).
- Native Apple Silicon Mac with unity: 6 minutes 44 seconds in UAT. Editor, Development and Shipping targets all built and both ZIPs passed local integrity, architecture and BuildId checks. Both initial test launches stalled before logging. A process sample identified a modal startup hardware advisory: the M1 runner has 7 GiB RAM, below Unreal's 8 GiB recommendation. The startup advisory is suppressed by `-skipminspeccheck`. A second sample traced another modal RAM warning to `UEditorEngine::UpdateAutoLoadProject`, whose hardware dialogs are skipped in `-buildmachine` mode. With both flags, all 1,229 discovered tests passed: 1,094 without warnings and 135 with warnings, with no failures or unrun tests. The test step took 3 minutes 43 seconds, including startup; reported test execution was 99.6 seconds. This test-only retry used the identical package from the successful native build. [Passing Mac automation](https://github.com/kismetai-io/Kismet/actions/runs/44441636183/job/135284023135). [Native Mac build](https://github.com/kismetai-io/Kismet/actions/runs/44321548721/job/134861526730).

These are individual cold-run measurements, not statistical benchmarks. The Mac comparison changes both hardware and unity settings. The macOS 15 ARM job could not acquire a runner; macOS 26 subsequently did. Queue times are additional and variable.

## Automatic CI

The measured settings are used by [Unreal pull-request CI](unreal-ci.md).
The experiment's temporary draft release and URL secrets were deleted after
verification. Standard runner compute is free for this public repository;
artifact storage has a separate allowance. See [GitHub runner documentation](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
and [billing](https://docs.github.com/en/billing/concepts/product-billing/github-actions).
