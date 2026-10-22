# meeting-record 0.2.1

Release preparation for the `meeting-record` npm package and Rust crate.

## Changes since 0.2.0

- Windows process capture discovers audio helpers that start after recording begins and retries helpers whose first activation fails.
- Windows capture distinguishes reused process IDs from the original captured processes, keeping app-exit handling accurate.
- Device notifications trigger source recovery. On macOS, default-device recovery responds only to changes to the relevant default input or output.
- Startup track events reach Node.js listeners after capture resolves, so applications can observe the initial source status.
- Windows process loopback checks for a supported Windows version before starting capture.
- Expanded binding and native coverage for track status, app exit, late helpers, microphone removal, and device switching.
- Removed scheduling-dependent startup-mixing assertions that could fail on busy CI runners.
- Updated the Node.js and Rust guides to describe the same capture and recovery behavior.

## Compatibility

- macOS 14.2 or later; Windows 11 or Server 2022 or later.
- Native builds for arm64 and x86-64 on both platforms.
- Node.js 22.13 or later in the 22.x line, or Node.js 24 or later.

## Prepare artifacts

Run the Release workflow from the release commit with `package=all` and `publish=false`:

```sh
gh workflow run release.yml --ref <release-branch-or-commit> -f package=all -f publish=false
```

The workflow produces `npm-package` (the tarball with all four native prebuilds) and `rust-crate` (the crate with bundled native sources). Confirm Rust CI and Node CI pass on the same commit, and confirm the release workflow's artifact verification succeeds.

## Publish

After release approval, run the Release workflow from that same reviewed commit with `package=all` and `publish=true`. Both selected packages must pass artifact verification before either publish job starts.

```sh
gh workflow run release.yml --ref <reviewed-release-branch-or-commit> -f package=all -f publish=true
```

Verify both registries report version `0.2.1` after publishing. Use the changes above for the GitHub release notes.
