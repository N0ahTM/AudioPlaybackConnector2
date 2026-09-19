# Releasing

This maintainer guide documents the existing GitHub, App Installer, and Microsoft Store pipeline. A release is not created by an ordinary commit to `main`: it requires a semantic-version tag followed by an explicit production promotion.

## Version sources

- Git tag: `vX.Y.Z`; authoritative for release automation
- MSIX/file version: `X.Y.Z.0`
- `CHANGELOG.md`: dated `## [X.Y.Z] - YYYY-MM-DD` section used for GitHub release notes
- `store/whats-new.json`: matching `version` plus eight localized Store notes
- `Directory.Build.props` and `Package.appxmanifest`: matching checked-in default `X.Y.Z.0` for local builds

Do not add another independent version constant. The workflows pass `PackageVersion` from the tag to release builds.

## Prepare

- [ ] Choose a version according to Semantic Versioning.
- [ ] Confirm the previous Microsoft Store submission is no longer being processed.
- [ ] Move completed entries from `[Unreleased]` to `## [X.Y.Z] - YYYY-MM-DD`, then leave an empty `[Unreleased]` section for future work.
- [ ] Update `store/whats-new.json` to `X.Y.Z` and write concise release notes for `en`, `de`, `fr`, `es`, `ja`, `ko`, `zh-Hans`, and `zh-Hant`.
- [ ] Update `Directory.Build.props` and `Package.appxmanifest` to `X.Y.Z.0`.
- [ ] Update README, usage, CLI, installation, troubleshooting, screenshots, permissions, and privacy statements only where behavior changed.
- [ ] Run `pwsh ./scripts/validate-localizations.ps1`.
- [ ] Run `pwsh ./scripts/validate-markdown-links.ps1`.
- [ ] Run `pwsh ./scripts/release/validate-release-metadata.ps1 -Version X.Y.Z`.
- [ ] Push the preparation commit to `main` and wait for Build and CodeQL to pass.
- [ ] Run the **Release Dry Run** workflow with `semver=X.Y.Z`; inspect its x64/ARM64 packages and summary.
- [ ] Smoke-test important changed behavior from the dry-run package on x64 and, when architecture-sensitive, ARM64 Windows.

The Store notes validator rejects missing text and notes longer than 1,500 characters. Keep Store notes user-focused; implementation details belong in the changelog.

## Create the immutable release candidate

From an up-to-date clean `main`:

```powershell
git switch main
git pull --ff-only
git tag -a vX.Y.Z -m "AudioPlaybackConnector2 X.Y.Z"
git push origin vX.Y.Z
```

The tag-triggered **Release** workflow validates metadata, builds and verifies signed x64/ARM64 GitHub packages and an unsigned Store upload, then creates a draft GitHub release. It does not publish the draft or submit to the Store.

Review the draft:

- [ ] Release body is the correct changelog section.
- [ ] `.msixbundle`, `.appinstaller`, `.cer`, and dependency assets are present.
- [ ] Artifact version is `X.Y.Z.0` and contains x64 and ARM64.
- [ ] Each application MSIX contains `LICENSE` and `THIRD_PARTY_NOTICES.md` at its root, matching the release source.
- [ ] The tag points to the intended `main` commit.

Never move or recreate a published release tag. Fix source or metadata with a newer version.

The bundle verifier checks `LICENSE` and `THIRD_PARTY_NOTICES.md` in every embedded application package.
Each must occur exactly once and its SHA256 must match the release checkout. Both GitHub and Store bundle
verification enforce this requirement. Run `pwsh ./scripts/test-package-notices.ps1` for its local regression tests.

## Publish

Run the **Release** workflow manually with:

- `tag`: the existing `vX.Y.Z` tag
- `store_only`: `false`

The workflow reuses and verifies the immutable tag artifact, publishes the GitHub release, verifies public assets, deploys the App Installer feed, builds the Store-linked upload, replaces older Partner Center packages, applies localized release notes, and commits the submission to certification.

If public asset or App Installer deployment fails after publication, the workflow returns the GitHub release to draft. Correct the failure before promoting again.

## Verify after publication

- [ ] GitHub release is public and not marked prerelease.
- [ ] Public assets download successfully.
- [ ] The stable `.appinstaller` resolves to `X.Y.Z.0`.
- [ ] Upgrade a previous GitHub App Installer installation.
- [ ] Install or update through Microsoft Store after certification completes.
- [ ] Check the Store listing and localized “What’s new” text.
- [ ] Confirm diagnostics and About show the expected version.
- [ ] Close fixed issues and optionally publish a GitHub Discussion announcement.

Microsoft certification is asynchronous. A successful workflow means the submission was committed, not that certification already passed.

## Store-only retry

Use `store_only=true` only when the GitHub release is already public and the Store submission alone must be retried. Use the same tag. The workflow rebuilds the Store-linked package from that exact tag with current Store verification logic, reads `store/whats-new.json` from that tag, and does not republish GitHub assets or the App Installer feed.

Do not use Store-only mode to change application code, package contents, or release notes that should have been part of the immutable tag. Create a new patch release instead.

## Secret maintenance

Production release uses `SIGNING_CERTIFICATE_PFX`, `SIGNING_CERTIFICATE_PASSWORD`, `AZURE_AD_TENANT_ID`, `SELLER_ID`, `AZURE_AD_APPLICATION_CLIENT_ID`, and `AZURE_AD_APPLICATION_SECRET`. Store credentials belong only in GitHub environment or repository secrets. Never put their values in source, logs, release artifacts, local `.env` files committed to Git, or documentation.

Use the **Store Access Check** workflow after rotating Store credentials. Rotate a compromised credential immediately and review workflow logs before another release.
