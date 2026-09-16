# Security Policy

## Supported versions

Security fixes target the latest Microsoft Store and GitHub release. Older releases should be updated before troubleshooting unless the report concerns the update mechanism itself.

## Reporting a vulnerability

Do not open a public issue for a vulnerability that could put users, signing material, credentials, package delivery, local files, or command-channel trust at risk. Use GitHub's **Report a vulnerability** option in the repository Security tab to create a private report. If that option is unavailable, contact the maintainer privately through the contact method on the GitHub profile and include only enough information to establish a secure follow-up channel.

Include the affected version, installation source, Windows version, impact, prerequisites, reproducible steps, and any proposed mitigation. Remove access tokens, certificates with private keys, personal device identifiers, user paths, and unrelated memory from diagnostics. Minidumps may contain sensitive memory and should not be uploaded publicly.

The maintainer will acknowledge a complete report, validate it, coordinate a fix and release, and credit the reporter when requested and safe. Public disclosure should wait until users can obtain the fixed release.

Ordinary crashes, Bluetooth compatibility problems, and non-sensitive defects belong in the public [bug tracker](https://github.com/N0ahTM/AudioPlaybackConnector2/issues).
