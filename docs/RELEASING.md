# Releasing

Maintainer procedure, not authorization to publish. Ordinary commits do not release: a semantic-version tag creates a candidate; production promotion is explicit. [CONTRIBUTING](../CONTRIBUTING.md#dependency-and-supply-chain-checks) owns license, vulnerability, SBOM and attestation commands/policy.

## Version sources

| Source | Contract |
| --- | --- |
| `vX.Y.Z` Git tag | Authoritative human version decision |
| MSIX / file version | `X.Y.Z.0`, derived by ReleaseVersion.psm1/MSBuild |
| CHANGELOG.md | `## [X.Y.Z] - YYYY-MM-DD` provides GitHub notes |
| store/whats-new.json | Same version; eight localized Store notes |
| Local build | Nearest reachable tag or explicit `/p:ReleaseTag=vX.Y.Z` |
| Package.appxmanifest | `0.0.0.0` template; build generates versioned copy |

Fetch tags locally; source archives require ReleaseTag. If PackageVersion is also supplied it must agree. No independent version constants/parsers. Components are 0–65535; reject leading zeros, whitespace and prerelease/build suffixes. Check with `pwsh ./scripts/test-release-version.ps1`.

## Prepare

- [ ] Choose SemVer; ensure previous Store submission is no longer processing.
- [ ] Move completed Unreleased entries to the dated version; leave empty Unreleased. Update Store version and notes for en/de/fr/es/ja/ko/zh-Hans/zh-Hant (nonempty, at most 1,500 characters each).
- [ ] Update behavior-affected docs, screenshots, permissions/privacy only. Candidate builds use `/p:ReleaseTag=vX.Y.Z`; leave manifest template unchanged.
- [ ] Run `pwsh ./scripts/validate-localizations.ps1`, `pwsh ./scripts/validate-markdown-links.ps1`, and `pwsh ./scripts/release/validate-release-metadata.ps1 -Version X.Y.Z`.
- [ ] Push preparation to main; Build and CodeQL must pass.
- [ ] Run **Release Dry Run**, `semver=X.Y.Z`; inspect x64/ARM64 packages/summary and smoke-test changed behavior on x64 and architecture-sensitive behavior on ARM64.

## Create the immutable release candidate

From clean, current main:

```powershell
git switch main
git pull --ff-only
git tag -a vX.Y.Z -m "AudioPlaybackConnector2 X.Y.Z"
git push origin vX.Y.Z
```

Tag-triggered **Release** validates metadata, builds/verifies signed GitHub x64/ARM64 packages and unsigned Store upload, then creates a draft. It does not publish or submit to Store.

Review:

- [ ] Correct changelog body, tag commit, X.Y.Z.0 and both architectures.
- [ ] Bundle, appinstaller, certificate, exact framework dependencies and `AudioPlaybackConnector2_SBOM.zip` present.
- [ ] Each embedded app MSIX has LICENSE and THIRD_PARTY_NOTICES.md exactly once, SHA256-identical to checkout. Both channels enforce this (`pwsh ./scripts/test-package-notices.ps1`).
- [ ] Shared app/CLI EXE bytes across channels, separate identities; complete SBOM/hash and attestation gates pass. Offline fixtures alone do not verify signed attestations.

Never move/recreate a published tag; source/metadata corrections require a new version.

## Publish

Manually run **Release** with existing `tag=vX.Y.Z`, `store_only=false`. The workflow verifies immutable tag artifacts, publishes GitHub, verifies public assets, deploys App Installer and submits the Store-linked package with localized notes after replacing older Partner Center packages. Promotion must use verified bytes; the ordinary two-channel build reuses native outputs for packaging.

A public-asset/feed deployment failure returns the GitHub release to draft; correct it before retrying. Store certification is asynchronous: successful submission is not certification success.

## Verify after publication

- [ ] Public non-prerelease GitHub release and downloadable assets.
- [ ] Stable appinstaller resolves X.Y.Z.0; upgrade a previous GitHub installation.
- [ ] Store install/update after certification, listing and localized What's new.
- [ ] Diagnostics/About version; close fixed issues. A Discussion announcement is optional.

## Store-only retry

`store_only=true` requires an already-public GitHub release and the same tag. Rebuild Store-linked package from that tag with current verification logic; read store/whats-new.json from the tag. Do not republish GitHub/feed or change product code/content/notes; those require a patch release.

## Secret maintenance

Production secrets: SIGNING_CERTIFICATE_PFX, SIGNING_CERTIFICATE_PASSWORD, AZURE_AD_TENANT_ID, SELLER_ID, AZURE_AD_APPLICATION_CLIENT_ID, AZURE_AD_APPLICATION_SECRET. Keep credentials in GitHub environment/repository secrets, never source/logs/artifacts/committed .env/docs. After Store credential rotation run **Store Access Check**. Rotate compromised credentials immediately and review logs before release.
