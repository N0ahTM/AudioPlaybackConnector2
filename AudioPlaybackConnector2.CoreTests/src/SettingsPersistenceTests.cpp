#include <core/Settings.hpp>
#include <util/RuntimeApartment.hpp>

#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

class ScopedTestDirectory final {
public:
    ScopedTestDirectory() {
        static std::atomic_uint64_t nextId = 0;
        m_path = std::filesystem::temp_directory_path() /
                 (L"AudioPlaybackConnector2.SettingsPersistenceTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
                  std::to_wstring(nextId.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(m_path);
    }

    ~ScopedTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    ScopedTestDirectory(ScopedTestDirectory const&) = delete;
    ScopedTestDirectory& operator=(ScopedTestDirectory const&) = delete;

    [[nodiscard]] std::filesystem::path const& Path() const noexcept { return m_path; }
    [[nodiscard]] std::filesystem::path SettingsPath() const { return m_path / L"AudioPlaybackConnector2.json"; }
    [[nodiscard]] std::filesystem::path TemporaryPath() const { return SettingsPath().wstring() + L".tmp"; }

private:
    std::filesystem::path m_path;
};

void WriteUtf8File(std::filesystem::path const& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] std::string ReadUtf8File(std::filesystem::path const& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

[[nodiscard]] SettingsData ReadData(Settings const& settings) {
    auto data = settings.LockSharedData();
    return *data;
}

void PopulateValidSettings(Settings& settings) {
    auto data = settings.LockExclusiveData();
    auto& mutableData = data.Mutate();
    mutableData.GlobalConnectOnStartup = true;
    mutableData.GlobalReconnectOnConnectionLoss = true;
    mutableData.AllowIncomingConnections = true;
    mutableData.StartWithWindows = true;
    mutableData.ShowNotifications = false;
    mutableData.UseSystemBackdropEffects = false;
    mutableData.Language = L"de";
    mutableData.LastUpdateCheckUnixSeconds = 1'726'000'000;
    mutableData.LastNotifiedUpdateVersion = L"2.7.0";
    mutableData.SettingsWindowBounds = PersistedWindowBounds{25, 35, 900, 650, 144};
    mutableData.PrivacyModeEnabled = true;
    mutableData.DefaultDevice = DefaultDeviceMode::SpecificDevice;
    mutableData.DefaultDeviceId = L"device-primary";
    mutableData.Devices = {{L"device-primary", L"Headphones", L"Desk", true, true},
                           {L"device-secondary", L"Speaker", L"", false, true}};
    mutableData.LastConnectedIds = {L"device-primary", L"device-secondary"};
}

void TestMissingCurrentAndRoundTrip() {
    ScopedTestDirectory directory;
    Settings settings(directory.Path());
    settings.Load(nullptr);
    Check(ReadData(settings) == SettingsData{}, "a missing settings file must retain defaults");
    Check(!settings.HasUnsavedChanges(), "loading a missing settings file must leave settings clean");

    PopulateValidSettings(settings);
    const auto expected = ReadData(settings);
    Check(settings.HasUnsavedChanges(), "a mutable settings guard must record a revision");
    Check(settings.Save(nullptr), "valid settings must save to the configured directory");
    Check(std::filesystem::exists(directory.SettingsPath()), "save must create the current JSON file");
    Check(!std::filesystem::exists(directory.TemporaryPath()), "successful atomic replacement must leave no temp file");
    Check(!settings.HasUnsavedChanges(), "a completed save must mark its snapshot clean");

    Settings reloaded(directory.Path());
    reloaded.Load(nullptr);
    Check(ReadData(reloaded) == expected, "current JSON must round-trip every persisted settings field");
    Check(!reloaded.HasUnsavedChanges(), "a loaded current JSON file must start clean");
}

void TestLegacyAndPartialInputNormalization() {
    ScopedTestDirectory directory;
    WriteUtf8File(
        directory.SettingsPath(),
        "{\"globalAutoReconnect\":true,\"language\":\"not-supported\",\"lastNotifiedUpdateVersion\":\"version\","
        "\"defaultDeviceMode\":\"specificDevice\",\"defaultDeviceId\":\"device-a\",\"settingsWindowBounds\":{\"width\":"
        "0},"
        "\"devices\":[{\"id\":\"device-a\",\"name\":\"legacy\",\"alias\":\"alias\",\"autoReconnect\":true},"
        "{\"id\":\"device-a\"},{\"id\":\"\",\"name\":\"ignored\"},{\"id\":4}],"
        "\"lastConnectedIds\":[\"device-a\",\"device-a\",4,\"device-b\"]}");

    Settings settings(directory.Path());
    settings.Load(nullptr);
    const auto data = ReadData(settings);
    Check(data.GlobalConnectOnStartup && data.GlobalReconnectOnConnectionLoss,
          "legacy globalAutoReconnect must populate both current reconnect flags");
    Check(data.Language == L"system", "unsupported language input must normalize to system");
    Check(data.DefaultDevice == DefaultDeviceMode::SpecificDevice && data.DefaultDeviceId == L"device-a",
          "a valid specific default device must load from partial JSON");
    Check(!data.SettingsWindowBounds, "invalid partial window bounds must be ignored");
    Check(data.Devices.size() == 1 && data.Devices.front().ConnectOnStartup &&
              data.Devices.front().ReconnectOnConnectionLoss,
          "legacy device autoReconnect and invalid device entries must normalize deterministically");
    Check(data.LastConnectedIds == std::vector<std::wstring>{L"device-a", L"device-b"},
          "recent ids must omit duplicates and values with invalid JSON types");
}

void TestMalformedAndCorruptInputPreservation() {
    ScopedTestDirectory directory;
    constexpr std::string_view malformed = R"({"devices":[)";
    WriteUtf8File(directory.SettingsPath(), malformed);

    Settings settings(directory.Path());
    settings.Load(nullptr);

    const auto backup = directory.SettingsPath().wstring() + L".corrupt.bak";
    Check(!std::filesystem::exists(directory.SettingsPath()),
          "malformed settings input must be moved out of the active path");
    Check(std::filesystem::exists(backup), "malformed settings input must be preserved as a corrupt backup");
    Check(ReadUtf8File(backup) == malformed, "the corrupt backup must preserve the original bytes");
    Check(ReadData(settings) == SettingsData{}, "malformed input must not partially mutate in-memory defaults");
}

void TestValidationFailureLeavesRevisionDirty() {
    ScopedTestDirectory directory;
    Settings settings(directory.Path());
    {
        auto data = settings.LockExclusiveData();
        data.Mutate().Language = L"invalid-language";
    }
    Check(!settings.Save(nullptr), "invalid in-memory settings must not be persisted");
    Check(settings.HasUnsavedChanges(), "a rejected save must retain the unsaved revision for a later retry");
    Check(!std::filesystem::exists(directory.SettingsPath()) && !std::filesystem::exists(directory.TemporaryPath()),
          "validation failure must not create current or temporary files");

    {
        auto data = settings.LockExclusiveData();
        data.Mutate().Language = L"en";
    }
    Check(settings.Save(nullptr), "a corrected revision must be saveable after validation failure");
    Check(!settings.HasUnsavedChanges(), "a successful retry must flush the corrected revision");
}

void TestFailedReplacePreservesExistingFileAndCanRetry() {
    ScopedTestDirectory directory;
    constexpr std::string_view original = R"({"language":"fr"})";
    WriteUtf8File(directory.SettingsPath(), original);

    Settings settings(directory.Path());
    {
        auto data = settings.LockExclusiveData();
        data.Mutate().Language = L"de";
    }

    wil::unique_hfile lockedFile(CreateFileW(directory.SettingsPath().c_str(),
                                             GENERIC_READ,
                                             FILE_SHARE_READ,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL,
                                             nullptr));
    Check(static_cast<bool>(lockedFile),
          "the existing settings file must be lockable for the replacement failure test");
    Check(!settings.Save(nullptr), "a failed atomic replacement must report failure");
    Check(settings.HasUnsavedChanges(), "a failed replacement must retain the unsaved revision");
    Check(ReadUtf8File(directory.SettingsPath()) == original,
          "failed replacement must preserve the prior current file");
    Check(!std::filesystem::exists(directory.TemporaryPath()), "failed replacement must clean the temporary file");

    lockedFile.reset();
    Check(settings.Save(nullptr), "the same dirty revision must save after the transient replacement failure clears");
    Check(!settings.HasUnsavedChanges(), "successful retry must clear the saved revision");
    Check(!std::filesystem::exists(directory.TemporaryPath()), "retry success must not leave a temporary file");

    Settings reloaded(directory.Path());
    reloaded.Load(nullptr);
    Check(ReadData(reloaded).Language == L"de", "retry must atomically replace the old file with the saved snapshot");
}

void TestFinalSaveCapturesLaterMutation() {
    ScopedTestDirectory directory;
    Settings settings(directory.Path());
    {
        auto data = settings.LockExclusiveData();
        data.Mutate().Language = L"en";
    }
    Check(settings.Save(nullptr), "an initial save must succeed");
    {
        auto data = settings.LockExclusiveData();
        data.Mutate().PrivacyModeEnabled = true;
    }
    Check(settings.HasUnsavedChanges(), "a mutation after a completed save must remain dirty for final flush");
    Check(settings.Save(nullptr), "a final synchronous save must flush the later mutation");

    Settings reloaded(directory.Path());
    reloaded.Load(nullptr);
    const auto data = ReadData(reloaded);
    Check(data.Language == L"en" && data.PrivacyModeEnabled,
          "final save must retain both the prior and later mutation without lost updates");
}

} // namespace

int RunSettingsPersistenceTests() {
    util::RuntimeApartment apartment;
    Check(apartment.Ready(), "settings persistence tests require a usable Windows Runtime apartment");
    if (!apartment.Ready()) return g_failures;

    TestMissingCurrentAndRoundTrip();
    TestLegacyAndPartialInputNormalization();
    TestMalformedAndCorruptInputPreservation();
    TestValidationFailureLeavesRevisionDirty();
    TestFailedReplacePreservesExistingFileAndCanRetry();
    TestFinalSaveCapturesLaterMutation();
    return g_failures;
}
