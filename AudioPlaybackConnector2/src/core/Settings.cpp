#include <pch.h>
#include <core/Settings.hpp>
#include <util/Util.hpp>

/* Helpers */
/*------------------------------------------------------------------------------------------------------------------*/

namespace {
bool GetOptionalBoolean(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key, bool fallback) {
    if (!json.HasKey(key)) return fallback;
    auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean
               ? value.GetBoolean()
               : fallback;
}

winrt::hstring GetOptionalString(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key, winrt::hstring const& fallback) {
    if (!json.HasKey(key)) return fallback;
    auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String
               ? value.GetString()
               : fallback;
}

winrt::Windows::Data::Json::JsonArray GetOptionalArray(winrt::Windows::Data::Json::JsonObject const& json, winrt::hstring const& key) {
    if (!json.HasKey(key)) return nullptr;
    auto value = json.Lookup(key);
    return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Array
               ? value.GetArray()
               : nullptr;
}
} // namespace

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Public Interface /////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

void Settings::Load(HINSTANCE hInst) {
    try {
        auto path = GetPath(hInst);
        if (!std::filesystem::exists(path)) return;

        wil::unique_hfile hFile(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!hFile) return;

        std::string buf;
        DWORD read = 0;
        char tmp[4096];
        while (ReadFile(hFile.get(), tmp, sizeof(tmp), &read, nullptr) && read > 0)
            buf.append(tmp, read);

        if (buf.empty()) return;

        // Parse all fields into locals before acquiring the lock so I/O and
        // JSON parsing don't block concurrent readers.
        auto json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(buf));

        bool globalAutoReconnect = false;
        bool startWithWindows = false;
        std::wstring language = L"system";
        std::vector<DeviceSettings> devices;
        std::vector<std::wstring> lastConnectedIds;

        globalAutoReconnect = GetOptionalBoolean(json, L"globalAutoReconnect", globalAutoReconnect);
        startWithWindows = GetOptionalBoolean(json, L"startWithWindows", startWithWindows);

        {
            auto lang = GetOptionalString(json, L"language", L"system");
            language = lang.empty() ? L"system" : std::wstring(lang);
            if (language == L"en" && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_ENGLISH)
                language = L"system";
        }

        if (auto array = GetOptionalArray(json, L"devices")) {
            for (auto val : array) {
                try {
                    if (val.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
                    auto obj = val.GetObject();
                    DeviceSettings ds;
                    ds.Id = GetOptionalString(obj, L"id", L"");
                    if (ds.Id.empty()) continue;
                    ds.Name = GetOptionalString(obj, L"name", L"");
                    ds.AutoReconnect = GetOptionalBoolean(obj, L"autoReconnect", false);
                    devices.push_back(std::move(ds));
                } catch (...) {
                    DebugTrace(L"[Settings] Load WARNING: skipping invalid device entry");
                }
            }
        }

        if (auto array = GetOptionalArray(json, L"lastConnectedIds")) {
            for (auto val : array) {
                if (val.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                    lastConnectedIds.push_back(std::wstring(val.GetString()));
            }
        }

        auto guard = m_lock.lock_exclusive();
        m_data.GlobalAutoReconnect = globalAutoReconnect;
        m_data.StartWithWindows = startWithWindows;
        m_data.Language = std::move(language);
        m_data.Devices = std::move(devices);
        m_data.LastConnectedIds = std::move(lastConnectedIds);
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[Settings] Load ERROR (hresult): {0}", ex.message());
    } catch (std::exception const& ex) {
        DebugTrace(L"[Settings] Load ERROR (std): {0}", util::Utf8ToUtf16(ex.what()));
    } catch (...) {
        DebugTrace(L"[Settings] Load ERROR: Unknown exception");
    }
}

void Settings::Save(HINSTANCE hInst) {
    try {
        SettingsData snapshot;
        {
            auto guard = m_lock.lock_shared();
            snapshot = m_data;
        }

        winrt::Windows::Data::Json::JsonObject json;
        json.Insert(L"globalAutoReconnect", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.GlobalAutoReconnect));
        json.Insert(L"startWithWindows", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(snapshot.StartWithWindows));
        json.Insert(L"language", winrt::Windows::Data::Json::JsonValue::CreateStringValue(snapshot.Language));

        winrt::Windows::Data::Json::JsonArray devArr;
        for (const auto& d : snapshot.Devices) {
            winrt::Windows::Data::Json::JsonObject obj;
            obj.Insert(L"id", winrt::Windows::Data::Json::JsonValue::CreateStringValue(d.Id));
            obj.Insert(L"name", winrt::Windows::Data::Json::JsonValue::CreateStringValue(d.Name));
            obj.Insert(L"autoReconnect", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(d.AutoReconnect));
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

        wil::unique_hfile hFile(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!hFile);
        DWORD written = 0;
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
        THROW_HR_IF(E_FAIL, written != utf8.size());
        hFile.reset();

        THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING));
        cleanupTmp.release(); // rename succeeded — nothing to clean up
    } catch (winrt::hresult_error const& ex) {
        DebugTrace(L"[Settings] Save ERROR (hresult): {0}", ex.message());
    } catch (std::exception const& ex) {
        DebugTrace(L"[Settings] Save ERROR (std): {0}", util::Utf8ToUtf16(ex.what()));
    } catch (...) {
        DebugTrace(L"[Settings] Save ERROR: Unknown exception");
    }
}

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Private Implementation ///////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

std::filesystem::path Settings::GetPath(HINSTANCE hInst) const {
    try {
        auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
        return std::filesystem::path(std::wstring(localFolder.Path())) / c_fileName;
    } catch (...) {
        wchar_t* localAppData = nullptr;
        auto freeEnv = wil::scope_exit([&]() { if (localAppData) free(localAppData); });
        if (_wdupenv_s(&localAppData, nullptr, L"LOCALAPPDATA") == 0 && localAppData && *localAppData) {
            auto dir = std::filesystem::path(localAppData) / L"AudioPlaybackConnector2";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (!ec) return dir / c_fileName;
        }
    }
    return util::GetModuleFsPath(hInst).remove_filename() / c_fileName;
}
