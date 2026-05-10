#pragma once

#include <targetver.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <versionhelpers.h>
#include <mmdeviceapi.h>
#include <appmodel.h>

#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>
#include <functional>
#include <filesystem>
#include <mutex>
#include <thread>
#include <algorithm>
#include <format>
#include <limits>
#include <tuple>

#ifndef _DEBUG
#define RESULT_DIAGNOSTICS_LEVEL 1
#endif

#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>
#include <wil/resource.h>
#include <wil/safecast.h>

#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Popups.h>

#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Navigation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>

#include <microsoft.ui.xaml.window.h>

#include "winrt/Windows.UI.Xaml.h"
#include "winrt/Windows.UI.Xaml.Interop.h"
