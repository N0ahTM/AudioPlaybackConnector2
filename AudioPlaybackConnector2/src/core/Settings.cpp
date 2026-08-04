#include <pch.h>
#include <core/Settings.hpp>
#include <util/Util.hpp>

#include <unordered_set>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Helpers ///////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {
constexpr uint64_t c_maxSettingsFileBytes = 4 * 1024 * 1024;

bool GetOptionalBoolean(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key, bool fallback) {
    if (!json.HasKey(key)) return fallback;
    auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean ? value.GetBoolean() : fallback;
}

winrt::hstring GetOptionalString(winrt::Windows::Data::Json::JsonObject const& json,
                                 winrt::hstring const& key,
                                 winrt::hstring const& fallback) {
    if (!json.HasKey(key)) return fallback;
    auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String ? value.GetString() : fallback;
}

int64_t
GetOptionalInt64(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key, int64_t fallback) {
    if (!json.HasKey(key)) return fallback;
    auto value = json.Lookup(key);
    if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number) return fallback;

    constexpr double c_int64Min = -9223372036854775808.0;
    constexpr double c_int64ExclusiveMax = 9223372036854775808.0;
    const auto number = value.GetNumber();
    if (!std::isfinite(number) || std::trunc(number) != number || number < c_int64Min ||
        number >= c_int64ExclusiveMax) {
        return fallback;
    }
    return static_cast<int64_t>(number);
}

int32_t
GetOptionalInt32(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key, int32_t fallback) {
    if (!json.HasKey(key)) return fallback;
    auto value = json.Lookup(key);
    if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number) return fallback;

    auto number = value.GetNumber();
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        number > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return fallback;
    }
    return static_cast<int32_t>(number);
}

winrt::Windows::Data::Json::JsonObject GetOptionalObject(winrt::Windows::Data::Json::JsonObject const& json,
                                                         winrt::hstring const& key) {
    if (!json.HasKey(key)) return nullptr;
    auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object ? value.GetObject() : nullptr;
}

winrt::Windows::Data::Json::JsonArray GetOptionalArray(winrt::Windows::Data::Json::JsonObject const& json,
                                                       winrt::hstring const& key) {
    if (!json.HasKey(key)) return nullptr;
    auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Array ? value.GetArray() : nullptr;
}

DefaultDeviceMode ParseDefaultDeviceMode(std::wstring_view value) noexcept {
    return value == L"specificDevice" ? DefaultDeviceMode::SpecificDevice : DefaultDeviceMode::LastConnected;
}

std::wstring_view SerializeDefaultDeviceMode(DefaultDeviceMode mode) noexcept {
    return mode == DefaultDeviceMode::SpecificDevice ? L"specificDevice" : L"lastConnected";
}

void BackupUnreadableSettingsFile(std::filesystem::path const& path) noexcept {
    if (path.empty()) return;

    try {
        if (!std::filesystem::exists(path)) return;

        auto backup = path;
        backup += L".corrupt.bak";
        for (int suffix = 1; std::filesystem::exists(backup) && suffix < 100; ++suffix) {
            backup = path;
            backup += std::format(L".corrupt.{}.bak", suffix);
        }

        if (MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DebugTrace(L"[Settings] Corrupt settings file moved to: {0}", backup.wstring());
        } else {
            DebugTrace(L"[Settings] ERROR: failed to move corrupt settings file: {0}", path.wstring());
        }
    } catch (std::exception const& ex) {
        DebugTrace(L"[Settings] ERROR: failed to backup corrupt settings file: {0}", util::Utf8ToUtf16(ex.what()));
    } catch (...) {
        DebugTrace(L"[Settings] ERROR: failed to backup corrupt settings file");
    }
}
} // namespace

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void Settings::Load(HINSTANCE hInst) {
    auto persistenceGuard = std::scoped_lock(m_persistenceMutex);
    m_revision.store(0, std::memory_order_release);
    m_savedRevision.store(0, std::memory_order_release);
    std::filesystem::path path;
    try {
        path = GetPath(hInst);
        if (!std::filesystem::exists(path)) return;

        wil::unique_hfile hFile(CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!hFile) return;

        LARGE_INTEGER fileSize{};
        THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(hFile.get(), &fileSize));
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE),
                    fileSize.QuadPart < 0 || static_cast<uint64_t>(fileSize.QuadPart) > c_maxSettingsFileBytes);

        std::string buf;
        buf.reserve(static_cast<size_t>(fileSize.QuadPart));
        DWORD read = 0;
        char tmp[4096];
        while (true) {
            THROW_IF_WIN32_BOOL_FALSE(ReadFile(hFile.get(), tmp, sizeof(tmp), &read, nullptr));
            if (read == 0) break;
            buf.append(tmp, read);
        }

        if (buf.empty()) return;

        // Parse all fields into locals before acquiring the lock so I/O and
        // JSON parsing don't block concurrent readers.
        auto json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(buf));

        bool globalConnectOnStartup = false;
        bool globalReconnectOnConnectionLoss = false;
        bool allowIncomingConnections = false;
        bool startWithWindows = false;
        bool showNotifications = true;
        std::wstring language = L"system";
        int64_t lastUpdateCheckUnixSeconds = 0;
        std::wstring lastNotifiedUpdateVersion;
        std::optional<PersistedWindowBounds> settingsWindowBounds;
        bool privacyModeEnabled = false;
        DefaultDeviceMode defaultDevice = DefaultDeviceMode::LastConnected;
        std::wstring defaultDeviceId;
        std::vector<DeviceSettings> devices;
        std::vector<std::wstring> lastConnectedIds;
        std::unordered_set<std::wstring> deviceIds;
        std::unordered_set<std::wstring> connectedIds;

        const bool legacyGlobalAutoReconnect = GetOptionalBoolean(json, L"globalAutoReconnect", false);
        globalConnectOnStartup = GetOptionalBoolean(json, L"globalConnectOnStartup", legacyGlobalAutoReconnect);
        globalReconnectOnConnectionLoss =
            GetOptionalBoolean(json, L"globalReconnectOnConnectionLoss", legacyGlobalAutoReconnect);
        allowIncomingConnections = GetOptionalBoolean(json, L"allowIncomingConnections", false);
        startWithWindows = GetOptionalBoolean(json, L"startWithWindows", startWithWindows);
        showNotifications = GetOptionalBoolean(json, L"showNotifications", showNotifications);
        privacyModeEnabled = GetOptionalBoolean(json, L"privacyModeEnabled", privacyModeEnabled);
        lastUpdateCheckUnixSeconds = GetOptionalInt64(json, L"lastUpdateCheckUnixSeconds", lastUpdateCheckUnixSeconds);
        lastNotifiedUpdateVersion = GetOptionalString(json, L"lastNotifiedUpdateVersion", L"");
        defaultDevice = ParseDefaultDeviceMode(std::wstring(GetOptionalString(json, L"defaultDeviceMode", L"")));
        defaultDeviceId = GetOptionalString(json, L"defaultDeviceId", L"");
        if (defaultDeviceId.empty()) {
            defaultDevice = DefaultDeviceMode::LastConnected;
        }

        {
            auto lang = GetOptionalString(json, L"language", L"system");
            language = lang.empty() ? L"system" : std::wstring(lang);
            if (language == L"en" && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_ENGLISH) language = L"system";
        }

        if (auto boundsJson = GetOptionalObject(json, L"settingsWindowBounds")) {
            PersistedWindowBounds bounds;
            bounds.X = GetOptionalInt32(boundsJson, L"x", 0);
            bounds.Y = GetOptionalInt32(boundsJson, L"y", 0);
            bounds.Width = GetOptionalInt32(boundsJson, L"width", 0);
            bounds.Height = GetOptionalInt32(boundsJson, L"height", 0);
            const auto dpi = GetOptionalInt32(boundsJson, L"dpi", USER_DEFAULT_SCREEN_DPI);
            bounds.Dpi = dpi > 0 ? static_cast<uint32_t>(dpi) : USER_DEFAULT_SCREEN_DPI;
            if (bounds.Width > 0 && bounds.Height > 0) {
                settingsWindowBounds = bounds;
            } else {
                DebugTrace(L"[Settings] Load WARNING: ignoring invalid settings window bounds");
            }
        }

        if (auto array = GetOptionalArray(json, L"devices")) {
            for (auto val : array) {
                try {
                    if (val.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
                    auto obj = val.GetObject();
                    DeviceSettings ds;
                    ds.Id = GetOptionalString(obj, L"id", L"");
                    if (ds.Id.empty() || !deviceIds.insert(ds.Id).second) continue;
                    ds.Name = GetOptionalString(obj, L"name", L"");
                    ds.Alias = GetOptionalString(obj, L"alias", L"");
                    const bool legacyAutoReconnect = GetOptionalBoolean(obj, L"autoReconnect", false);
                    ds.ConnectOnStartup = GetOptionalBoolean(obj, L"connectOnStartup", legacyAutoReconnect);
                    ds.ReconnectOnConnectionLoss =
                        GetOptionalBoolean(obj, L"reconnectOnConnectionLoss", legacyAutoReconnect);
                    devices.push_back(std::move(ds));
                } catch (...) {
                    DebugTrace(L"[Settings] Load WARNING: skipping invalid device entry");
                }
            }
        }

        if (auto array = GetOptionalArray(json, L"lastConnectedIds")) {
            for (auto val : array) {
                if (val.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) continue;
                auto id = std::wstring(val.GetString());
                if (!id.empty() && connectedIds.insert(id).second) {
                    lastConnectedIds.push_back(std::move(id));
                }
            }
        }

        auto guard = m_lock.lock_exclusive();
        m_data.GlobalConnectOnStartup = globalConnectOnStartup;
        m_data.GlobalReconnectOnConnectionLoss = globalReconnectOnConnectionLoss;
        m_data.AllowIncomingConnections = allowIncomingConnections;
        m_data.StartWithWindows = startWithWindows;
        m_data.ShowNotifications = showNotifications;
        m_data.Language = std::move(language);
        m_data.LastUpdateCheckUnixSeconds = lastUpdateCheckUnixSeconds;
        m_data.LastNotifiedUpdateVersion = std::move(lastNotifiedUpdateVersion);
        m_data.SettingsWindowBounds = settingsWindowBounds;
        m_data.PrivacyModeEnabled = privacyModeEnabled;
        m_data.DefaultDevice = defaultDevice;
        m_data.DefaultDeviceId = std::move(defaultDeviceId);
        m_data.Devices = std::move(devices);
        m_data.LastConnectedIds = std::move(lastConnectedIds);
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[Settings] Load ERROR (hresult): {0}", ex.message());
        BackupUnreadableSettingsFile(path);
    } catch (std::exception const& ex) {
        DebugTrace(L"[Settings] Load ERROR (std): {0}", util::Utf8ToUtf16(ex.what()));
        BackupUnreadableSettingsFile(path);
    } catch (...) {
        DebugTrace(L"[Settings] Load ERROR: Unknown exception");
        BackupUnreadableSettingsFile(path);
    }
}

bool Settings::Save(HINSTANCE hInst) {
    auto persistenceGuard = std::scoped_lock(m_persistenceMutex);
    try {
        SettingsData snapshot;
        std::uint64_t revisionAtSnapshot = 0;
        {
            auto guard = m_lock.lock_shared();
            revisionAtSnapshot = m_revision.load(std::memory_order_acquire);
            if (revisionAtSnapshot == m_savedRevision.load(std::memory_order_acquire)) return true;
            snapshot = m_data;
        }

        winrt::Windows::Data::Json::JsonObject json;
        json.Insert(L"globalConnectOnStartup",
                    winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.GlobalConnectOnStartup));
        json.Insert(
            L"globalReconnectOnConnectionLoss",
            winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.GlobalReconnectOnConnectionLoss));
        json.Insert(L"allowIncomingConnections",
                    winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.AllowIncomingConnections));
        json.Insert(L"startWithWindows",
                    winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.StartWithWindows));
        json.Insert(L"showNotifications",
                    winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.ShowNotifications));
        json.Insert(L"privacyModeEnabled",
                    winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.PrivacyModeEnabled));
        json.Insert(L"language", winrt::Windows::Data::Json::JsonValue::CreateStringValue(snapshot.Language));
        json.Insert(L"lastUpdateCheckUnixSeconds",
                    winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
                        static_cast<double>(snapshot.LastUpdateCheckUnixSeconds)));
        json.Insert(L"lastNotifiedUpdateVersion",
                    winrt::Windows::Data::Json::JsonValue::CreateStringValue(snapshot.LastNotifiedUpdateVersion));
        json.Insert(L"defaultDeviceMode",
                    winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                        winrt::hstring(SerializeDefaultDeviceMode(snapshot.DefaultDevice))));
        json.Insert(L"defaultDeviceId",
                    winrt::Windows::Data::Json::JsonValue::CreateStringValue(snapshot.DefaultDeviceId));

        if (snapshot.SettingsWindowBounds) {
            winrt::Windows::Data::Json::JsonObject boundsJson;
            boundsJson.Insert(
                L"x", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->X));
            boundsJson.Insert(
                L"y", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->Y));
            boundsJson.Insert(
                L"width",
                winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->Width));
            boundsJson.Insert(
                L"height",
                winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->Height));
            boundsJson.Insert(
                L"dpi", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(snapshot.SettingsWindowBounds->Dpi));
            json.Insert(L"settingsWindowBounds", boundsJson);
        }

        winrt::Windows::Data::Json::JsonArray devArr;
        for (const auto& d : snapshot.Devices) {
            winrt::Windows::Data::Json::JsonObject obj;
            obj.Insert(L"id", winrt::Windows::Data::Json::JsonValue::CreateStringValue(d.Id));
            obj.Insert(L"name", winrt::Windows::Data::Json::JsonValue::CreateStringValue(d.Name));
            obj.Insert(L"alias", winrt::Windows::Data::Json::JsonValue::CreateStringValue(d.Alias));
            obj.Insert(L"connectOnStartup",
                       winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(d.ConnectOnStartup));
            obj.Insert(L"reconnectOnConnectionLoss",
                       winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(d.ReconnectOnConnectionLoss));
            devArr.Append(obj);
        }
        json.Insert(L"devices", devArr);

        winrt::Windows::Data::Json::JsonArray lastArr;
        for (const auto& id : snapshot.LastConnectedIds)
            lastArr.Append(winrt::Windows::Data::Json::JsonValue::CreateStringValue(id));
        json.Insert(L"lastConnectedIds", lastArr);

        auto utf8 = util::Utf16ToUtf8(json.Stringify());
        auto path = GetPath(hInst);
        auto tmp = path;
        tmp += L".tmp";

        // Delete the temp file if any step below throws before the atomic rename.
        auto cleanupTmp = wil::scope_exit([&tmp]() { DeleteFileW(tmp.c_str()); });

        wil::unique_hfile hFile(
            CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!hFile);
        DWORD written = 0;
        THROW_IF_WIN32_BOOL_FALSE(
            WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
        THROW_HR_IF(E_FAIL, written != utf8.size());
        THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(hFile.get()));
        hFile.reset();

        THROW_IF_WIN32_BOOL_FALSE(
            MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
        cleanupTmp.release(); // rename succeeded — nothing to clean up
        m_savedRevision.store(revisionAtSnapshot, std::memory_order_release);
        return true;
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[Settings] Save ERROR (hresult): {0}", ex.message());
    } catch (std::exception const& ex) {
        DebugTrace(L"[Settings] Save ERROR (std): {0}", util::Utf8ToUtf16(ex.what()));
    } catch (...) {
        DebugTrace(L"[Settings] Save ERROR: Unknown exception");
    }
    return false;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::filesystem::path Settings::GetPath(HINSTANCE hInst) const {
    try {
        auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
        return std::filesystem::path(std::wstring(localFolder.Path())) / c_fileName;
    } catch (...) {
        wchar_t* localAppData = nullptr;
        auto freeEnv = wil::scope_exit([&]() {
            if (localAppData) free(localAppData);
        });
        if (_wdupenv_s(&localAppData, nullptr, L"LOCALAPPDATA") == 0 && localAppData && *localAppData) {
            auto dir = std::filesystem::path(localAppData) / L"AudioPlaybackConnector2";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (!ec) return dir / c_fileName;
        }
    }
    return util::GetModuleFsPath(hInst).remove_filename() / c_fileName;
}
