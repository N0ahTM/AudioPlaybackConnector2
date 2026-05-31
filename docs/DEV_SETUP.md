# Developer Setup and Build

## Prerequisites

- Visual Studio 2022 (17.14+ recommended)
- Desktop development with C++ workload
- Windows application development workload (WinUI / Windows App SDK)
- Windows SDK 10.0.26100.0 or newer
- Windows App SDK Runtime 2.0.1 or newer
- Microsoft Windows App Runtime Singleton

## Open the solution

Open:

```txt
AudioPlaybackConnector2.slnx
```

Recommended target:

```txt
Configuration: Release
Platform: x64
```

## Build commands

Restore packages:

```powershell
msbuild AudioPlaybackConnector2.slnx -t:restore -p:RestorePackagesConfig=true
```

Debug build:

```powershell
msbuild AudioPlaybackConnector2.slnx /p:Configuration=Debug /p:Platform=x64
```

Project-only debug build (no packaging):

```powershell
msbuild AudioPlaybackConnector2/AudioPlaybackConnector2.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:PreferredToolArchitecture=x64
```

Release rebuild:

```powershell
msbuild AudioPlaybackConnector2.slnx /p:Configuration=Release /p:Platform=x64 /t:Rebuild
```

Run from debug output:

```powershell
.\x64\Debug\AudioPlaybackConnector2\AudioPlaybackConnector2.exe
```

## CI-equivalent local checks

Formatting:

```powershell
clang-format --dry-run --Werror AudioPlaybackConnector2/src/**/*.cpp AudioPlaybackConnector2/include/**/*.hpp
```

Static analysis:

```powershell
cppcheck --enable=warning,performance,portability --std=c++20 --platform=win64 AudioPlaybackConnector2/src/
```

## Visual Studio notes

- If C++23 features fail to compile, set language standard to `/std:c++latest`.
- If packaging fails due to certificate thumbprint mismatch, create/select a local test certificate in `Package.appxmanifest` and update `PackageCertificateThumbprint` for `AudioPlaybackConnector2 (Package)`.
