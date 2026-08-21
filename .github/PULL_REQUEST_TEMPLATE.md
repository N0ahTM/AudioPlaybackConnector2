## Summary

<!-- What does this PR change and why? -->

## Type of Change

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor / code cleanup
- [ ] Documentation update
- [ ] Build / CI change

## Checklist

- [ ] Release build succeeds (`msbuild AudioPlaybackConnector2.slnx /p:Configuration=Release /p:Platform=x64 /t:Rebuild`)
- [ ] Formatting and CppCheck pass with the commands in `CONTRIBUTING.md`
- [ ] Native regression tests pass (`.\x64\Release\tests\AudioPlaybackConnector2.CoreTests.exe`)
- [ ] No new compiler warnings (`/W4 /WX` must remain clean)
- [ ] New user-visible strings added to all 8 locale files in `res/strings/`
- [ ] Thread-safety considered for any `DeviceManager` or `Settings` changes
- [ ] Installer and release packaging validated when those files are affected
- [ ] `CHANGELOG.md` updated under `[Unreleased]`

## Related Issues

<!-- Closes #xxx -->
