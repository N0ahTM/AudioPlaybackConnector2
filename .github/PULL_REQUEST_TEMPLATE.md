## Summary

<!-- What does this PR change and why? -->

## Type of Change

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor / code cleanup
- [ ] Documentation update
- [ ] Build / CI change

## Checklist

- [ ] Release builds succeed for x64 and ARM64 (commands in `CONTRIBUTING.md`)
- [ ] Formatting and CppCheck pass with the commands in `CONTRIBUTING.md`
- [ ] Native regression tests pass (`.\x64\Release\tests\AudioPlaybackConnector2.CoreTests.exe`)
- [ ] No new compiler warnings (`/W4 /WX` must remain clean)
- [ ] New user-visible strings added to all 8 locale files in `res/strings/`
- [ ] Thread-safety considered for any `DeviceManager` or `Settings` changes
- [ ] Installer and release packaging validated when those files are affected
- [ ] `CHANGELOG.md` updated under `[Unreleased]`

## Related Issues

<!-- Closes #xxx -->
