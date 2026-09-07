# Third-Party Notices

AudioPlaybackConnector2 includes or depends on the following third-party components.
Their licenses are reproduced below or linked to their canonical sources.

---

## Microsoft Windows App SDK

**Version:** 2.0.1

**License:** Microsoft Software License Terms

**Source:** [Microsoft Windows App SDK](https://github.com/microsoft/WindowsAppSDK)
**License and notices:** [Microsoft.WindowsAppSDK 2.0.1 package](https://www.nuget.org/packages/Microsoft.WindowsAppSDK/2.0.1/License)

The Windows App SDK package is not licensed as a single MIT component. Its NuGet package contains version-specific `license.txt` and `NOTICE.txt` files. The following Windows App SDK packages are restored and distributed with release packages when required:

| Package | Version |
|---------|---------|
| Microsoft.WindowsAppSDK.Base | 2.0.3 |
| Microsoft.WindowsAppSDK.Foundation | 2.0.20 |
| Microsoft.WindowsAppSDK.Runtime | 2.0.1 |
| Microsoft.WindowsAppSDK.WinUI | 2.0.12 |
| Microsoft.WindowsAppSDK.InteractiveExperiences | 2.0.12 |
| Microsoft.WindowsAppSDK.DWrite | 2.0.26041403 |

---

## C++/WinRT (microsoft/cppwinrt)

**Version:** 2.0.250303.1  
**License:** MIT  
**Source:** https://github.com/microsoft/cppwinrt  

> Copyright (c) Microsoft Corporation. All rights reserved.
>
> Licensed under the MIT License reproduced in [MIT License Text](#mit-license-text).

---

## Windows Implementation Libraries (WIL)

**Version:** 1.0.260126.7  
**License:** MIT  
**Source:** https://github.com/microsoft/wil  

> Copyright (c) Microsoft Corporation. All rights reserved.
>
> Licensed under the MIT License reproduced in [MIT License Text](#mit-license-text). The WIL NuGet package also contains version-specific `ThirdPartyNotices.txt`.

---

## Microsoft Fluent UI System Icons

**Usage:** Derived toast status icon assets under `AudioPlaybackConnector2 (Package)/Images/Toast*.scale-*.png`  
**License:** MIT  
**Source:** https://github.com/microsoft/fluentui-system-icons  

> Copyright (c) Microsoft Corporation.
>
> Licensed under the MIT License reproduced in [MIT License Text](#mit-license-text).

### MIT License Text

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

---

## Microsoft Edge WebView2

**Version:** 1.0.3719.77  
**Source and package notices:** [Microsoft.Web.WebView2 1.0.3719.77](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.3719.77)

> Copyright (C) Microsoft Corporation. All rights reserved.
>
> Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
>
> - Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
> - Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
> - The name of Microsoft Corporation, or the names of its contributors may not be used to endorse or promote products derived from this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The NuGet package also contains a version-specific `NOTICE.txt`.

---

## Build-only dependencies

The following packages are used to build or package the application and are not runtime payloads.

### Windows SDK Build Tools

**Package:** Microsoft.Windows.SDK.BuildTools 10.0.28000.1839  
**License:** Microsoft Software License  
**Source:** https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/

---

### Windows SDK Build Tools (MSIX)

**Package:** Microsoft.Windows.SDK.BuildTools.MSIX 1.7.251221100  
**License:** Microsoft Software License  
**Source:** https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/

---

## Windows System Libraries

The following Windows import libraries are used at link time. Their corresponding Windows DLLs are operating-system components and are not redistributed by this repository.

- `shell32.lib` — Windows Shell API
- `gdiplus.lib` — GDI+ graphics
- `gdi32.lib` — GDI API
- `comctl32.lib` — Common Controls
- `shlwapi.lib` — Shell Lightweight Utility API
- `dwmapi.lib` — Desktop Window Manager API
