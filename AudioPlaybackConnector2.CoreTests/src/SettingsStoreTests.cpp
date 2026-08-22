#include <core/SettingsStore.hpp>
#include <core/SettingsLimits.hpp>
#include <util/RuntimeApartment.hpp>

#include <wil/resource.h>

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <format>
#include <iterator>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

int g_failures = 0;

void Check(bool condition, char const* message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

class ControlledStorage final : public SettingsStoreStorage {
public:
    std::optional<std::string> Read(std::filesystem::path const&) override {
        std::scoped_lock lock(m_mutex);
        return m_input;
    }

    bool WriteAtomically(std::filesystem::path const&, std::string_view bytes) override {
        std::unique_lock lock(m_mutex);
        ++m_activeWriters;
        m_maxActiveWriters = std::max(m_maxActiveWriters, m_activeWriters);
        const auto finish = wil::scope_exit([&] { --m_activeWriters; });
        ++m_writes;
        m_writeStarted.notify_all();
        m_allowWrite.wait(lock, [&] { return !m_blockWrites; });
        if (m_failWrites != 0) {
            --m_failWrites;
            return false;
        }
        m_output.assign(bytes);
        return true;
    }

    void PreserveCorrupt(std::filesystem::path const&) noexcept override { ++m_corruptPreservations; }

    void SetInput(std::optional<std::string> input) {
        std::scoped_lock lock(m_mutex);
        m_input = std::move(input);
    }

    void BlockWrites() {
        std::scoped_lock lock(m_mutex);
        m_blockWrites = true;
    }

    void ReleaseWrites() {
        {
            std::scoped_lock lock(m_mutex);
            m_blockWrites = false;
        }
        m_allowWrite.notify_all();
    }

    void WaitForWrite() {
        std::unique_lock lock(m_mutex);
        m_writeStarted.wait(lock, [&] { return m_writes != 0; });
    }

    [[nodiscard]] std::string Output() const {
        std::scoped_lock lock(m_mutex);
        return m_output;
    }

    unsigned int m_failWrites = 0;
    std::atomic_uint32_t m_corruptPreservations = 0;
    [[nodiscard]] unsigned int MaxActiveWriters() const {
        std::scoped_lock lock(m_mutex);
        return m_maxActiveWriters;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_writeStarted;
    std::condition_variable m_allowWrite;
    std::optional<std::string> m_input;
    std::string m_output;
    unsigned int m_writes = 0;
    unsigned int m_activeWriters = 0;
    unsigned int m_maxActiveWriters = 0;
    bool m_blockWrites = false;
};

class ScopedTestDirectory final {
public:
    ScopedTestDirectory() {
        static std::atomic_uint64_t nextIdentifier = 0;
        m_path = std::filesystem::temp_directory_path() /
                 (L"AudioPlaybackConnector2.SettingsStoreTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
                  std::to_wstring(nextIdentifier++));
        std::filesystem::create_directories(m_path);
    }

    ~ScopedTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]] std::filesystem::path const& Path() const noexcept { return m_path; }
    [[nodiscard]] std::filesystem::path SettingsPath() const { return m_path / L"AudioPlaybackConnector2.json"; }

private:
    std::filesystem::path m_path;
};

void WriteBytes(std::filesystem::path const& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::string ReadBytes(std::filesystem::path const& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void TestCurrentLegacyPartialAndMalformed() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->SetInput(
        R"({"globalAutoReconnect":true,"devices":[7,{"id":"a","autoReconnect":true}],"lastConnectedIds":[7,"a","a"]})");
    SettingsStore store({}, storage);
    store.Load();
    const auto snapshot = store.Snapshot();
    Check(snapshot.Data.GlobalConnectOnStartup && snapshot.Data.GlobalReconnectOnConnectionLoss,
          "legacy global reconnect fields must populate both current fields");
    Check(snapshot.Data.Devices.size() == 1 && snapshot.Data.Devices.front().ConnectOnStartup,
          "wrong typed and duplicate device entries must be skipped independently");
    Check(snapshot.Data.LastConnectedIds == std::vector<std::wstring>{L"a"},
          "wrong typed and duplicate recent ids must be skipped independently");

    storage->SetInput("{");
    SettingsStore corrupt({}, storage);
    corrupt.Load();
    Check(storage->m_corruptPreservations == 1, "malformed JSON must be preserved through storage boundary");
}

void TestMissingEmptyAndCurrentRoundTrip() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore missing({}, storage);
    missing.Load();
    Check(missing.Snapshot().Data == SettingsData{}, "missing input must retain all defaults");
    storage->SetInput(std::string{});
    SettingsStore empty({}, storage);
    empty.Load();
    Check(empty.Snapshot().Data == SettingsData{}, "empty input must retain all defaults");

    auto input = std::make_shared<ControlledStorage>();
    input->SetInput(
        R"({"globalConnectOnStartup":true,"globalReconnectOnConnectionLoss":true,"allowIncomingConnections":true,"startWithWindows":true,"showNotifications":false,"useSystemBackdropEffects":false,"language":"de","lastUpdateCheckUnixSeconds":5,"lastNotifiedUpdateVersion":"2.0","privacyModeEnabled":true,"defaultDeviceMode":"specificDevice","defaultDeviceId":"a","settingsWindowBounds":{"x":1,"y":2,"width":3,"height":4,"dpi":144},"devices":[{"id":"a","name":"A","alias":"Desk","connectOnStartup":true,"reconnectOnConnectionLoss":true}],"lastConnectedIds":["a"]})");
    SettingsStore current({}, input);
    current.Load();
    const auto data = current.Snapshot().Data;
    Check(data.GlobalConnectOnStartup && data.GlobalReconnectOnConnectionLoss && data.AllowIncomingConnections &&
              data.StartWithWindows && !data.ShowNotifications && !data.UseSystemBackdropEffects &&
              data.Language == L"de" && data.LastUpdateCheckUnixSeconds == 5 &&
              data.LastNotifiedUpdateVersion == L"2.0" && data.PrivacyModeEnabled &&
              data.DefaultDevice == DefaultDeviceMode::SpecificDevice && data.DefaultDeviceId == L"a" &&
              data.SettingsWindowBounds == PersistedWindowBounds{1, 2, 3, 4, 144} && data.Devices.size() == 1 &&
              data.Devices.front() == DeviceSettings{L"a", L"A", L"Desk", true, true} &&
              data.LastConnectedIds == std::vector<std::wstring>{L"a"},
          "current-schema input must retain every persisted field");
    static_cast<void>(current.SetLanguage(L"ja"));
    input->SetInput(R"({"language":"fr"})");
    current.Load();
    Check(current.Snapshot().Data.Language == L"ja", "a second Load must not overwrite a committed runtime mutation");
    static_cast<void>(current.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestValidationAndLoadNormalizationMatrix() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    Check(store.SetSettingsWindowBounds(PersistedWindowBounds{0, 0, 1, 1, 1}).Status ==
              SettingsMutationStatus::Rejected,
          "out-of-range DPI must be rejected");
    Check(store.SetDeviceAlias(L"a", std::wstring(129, L'x')).Mutation.Status == SettingsMutationStatus::Rejected,
          "overlength alias must be rejected");
    Check(store.SetDeviceAlias(L"a", std::wstring(1, static_cast<wchar_t>(0xD800))).Mutation.Status ==
              SettingsMutationStatus::Rejected,
          "invalid UTF-16 must be rejected");
    const auto unknownEmpty = store.SetDeviceAlias(L"unknown", L"", L"Name");
    Check(!unknownEmpty.DeviceExists && unknownEmpty.Mutation.Status == SettingsMutationStatus::Unchanged,
          "an unknown device with an empty alias must not be reported as existing");
    const auto created = store.SetDeviceAlias(L"created", L"Desk", L"Name");
    Check(created.DeviceExists && created.Mutation.Status == SettingsMutationStatus::Applied,
          "a non-empty alias must create a previously unknown device");
    const auto unchanged = store.SetDeviceAlias(L"created", L"Desk", L"Name");
    Check(unchanged.DeviceExists && unchanged.Mutation.Status == SettingsMutationStatus::Unchanged,
          "an unchanged known device must remain distinguishable from an unknown device");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestProductionCorruptPreservationAndAtomicWrite() {
    ScopedTestDirectory directory;
    constexpr std::string_view malformed = "{";
    WriteBytes(directory.SettingsPath(), malformed);
    for (unsigned int suffix = 0; suffix != 102; ++suffix) {
        const auto suffixText = suffix == 0 ? L".corrupt.bak" : std::format(L".corrupt.{}.bak", suffix);
        WriteBytes(directory.SettingsPath().wstring() + suffixText, "prior-backup");
    }
    SettingsStore corrupt(directory.Path());
    corrupt.Load();
    const auto preserved = directory.SettingsPath().wstring() + L".corrupt.102.bak";
    Check(std::filesystem::exists(preserved), "corrupt backup saturation must select a new unused path");
    Check(ReadBytes(directory.SettingsPath().wstring() + L".corrupt.bak") == "prior-backup" &&
              ReadBytes(directory.SettingsPath().wstring() + L".corrupt.101.bak") == "prior-backup",
          "corrupt preservation must never overwrite an existing backup");
    Check(!std::filesystem::exists(directory.SettingsPath()), "corrupt input must leave the active path");
    static_cast<void>(corrupt.Shutdown(SettingsShutdownMode::DiscardStartupFailure));

    SettingsStore store(directory.Path());
    static_cast<void>(store.SetLanguage(L"ko"));
    Check(store.FlushNow(1), "production store must atomically persist a valid snapshot");
    Check(std::filesystem::exists(directory.SettingsPath()), "atomic replacement must create active settings file");
    Check(!std::filesystem::exists(directory.SettingsPath().wstring() + L".tmp"),
          "successful atomic replacement must leave no temporary file");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestReplacementFailurePreservesOldBytesAndCleansTemporaryFile() {
    ScopedTestDirectory directory;
    constexpr std::string_view original = R"({"language":"fr"})";
    WriteBytes(directory.SettingsPath(), original);
    SettingsStore store(directory.Path());
    static_cast<void>(store.SetLanguage(L"de"));
    wil::unique_hfile lockedFile(CreateFileW(directory.SettingsPath().c_str(),
                                             GENERIC_READ,
                                             FILE_SHARE_READ,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL,
                                             nullptr));
    Check(static_cast<bool>(lockedFile), "test must lock the prior settings file");
    Check(!store.FlushNow(1), "locked destination must report atomic replacement failure");
    Check(ReadBytes(directory.SettingsPath()) == original, "failed replacement must preserve prior bytes");
    Check(!std::filesystem::exists(directory.SettingsPath().wstring() + L".tmp"),
          "failed replacement must clean the temporary file");
    lockedFile.reset();
    Check(store.FlushNow(1), "the dirty revision must save after transient replacement failure clears");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestOversizedInputIsPreserved() {
    ScopedTestDirectory directory;
    WriteBytes(directory.SettingsPath(), std::string(apc::limits::c_maxSettingsFileBytes + 1, 'x'));
    SettingsStore store(directory.Path());
    store.Load();
    Check(!std::filesystem::exists(directory.SettingsPath()), "oversized input must leave the active path promptly");
    Check(std::filesystem::exists(directory.SettingsPath().wstring() + L".corrupt.bak"),
          "oversized bytes must be preserved as corrupt input");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestNoOpAndTypedMutationResults() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    const auto unchanged = store.SetPrivacyModeEnabled(false);
    Check(unchanged.Status == SettingsMutationStatus::Unchanged, "no-op must be distinguishable from rejection");
    const auto rejected = store.SetLanguage(L"not-a-language");
    Check(rejected.Status == SettingsMutationStatus::Rejected, "invalid mutation must be rejected without a revision");
    const auto applied = store.SetPrivacyModeEnabled(true);
    Check(applied.IsApplied() && applied.Revision != 0, "accepted mutation must report its revision");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestMutationDuringBlockedWriteAndFinalFlush() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->BlockWrites();
    SettingsStore store({}, storage);
    Check(store.SetLanguage(L"de").IsApplied(), "initial mutation must apply");
    std::jthread flushing([&] { static_cast<void>(store.FlushNow(2)); });
    storage->WaitForWrite();
    Check(store.SetPrivacyModeEnabled(true).IsApplied(), "mutation during write must apply as a newer revision");
    storage->ReleaseWrites();
    flushing.join();
    Check(store.FlushNow(2), "final flush must write the newest revision");
    Check(storage->MaxActiveWriters() == 1, "blocked write and final flush must never overlap writers");
    Check(storage->Output().find("privacyModeEnabled\":true") != std::string::npos,
          "newer mutation must not be acknowledged by the stale write");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestSubscriptionsAreOrderedAndReentrant() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    std::vector<std::uint64_t> revisions;
    auto subscription = store.Subscribe([&](SettingsSnapshot const& snapshot) {
        revisions.push_back(snapshot.Revision);
        if (snapshot.Revision == 1) static_cast<void>(store.SetShowNotifications(false));
    });
    static_cast<void>(store.SetPrivacyModeEnabled(true));
    Check(revisions == std::vector<std::uint64_t>{1, 2},
          "subscriptions must publish committed revisions in order and allow reentrancy");
    subscription.Reset();
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestRetryAndDiscardShutdown() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->m_failWrites = 1;
    SettingsStore store({}, storage);
    static_cast<void>(store.SetLanguage(L"fr"));
    Check(store.FlushNow(2), "bounded synchronous retry must retain and retry the same dirty revision");
    static_cast<void>(store.SetPrivacyModeEnabled(true));
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
    Check(storage->Output().find("privacyModeEnabled\":true") == std::string::npos,
          "startup-failure discard must not persist a later revision");
}

void TestNormalShutdownFlushAndNoLateCallback() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    std::atomic_uint32_t callbackCount = 0;
    auto subscription = store.Subscribe([&](SettingsSnapshot const&) { ++callbackCount; });
    static_cast<void>(store.SetLanguage(L"ja"));
    subscription.Reset();
    static_cast<void>(store.SetPrivacyModeEnabled(true));
    Check(callbackCount == 1, "subscription reset must fence already queued callbacks");
    Check(store.Shutdown(SettingsShutdownMode::Flush, 2), "normal shutdown must report its final flush result");
    Check(storage->Output().find("privacyModeEnabled\":true") != std::string::npos,
          "normal shutdown must synchronously flush the newest revision");
    const auto callbacksBeforeLateMutation = callbackCount.load();
    Check(store.SetLanguage(L"fr").Status == SettingsMutationStatus::Rejected,
          "shutdown must close future mutation admission");
    Check(callbackCount == callbacksBeforeLateMutation, "shutdown must suppress callbacks after it returns");
}

} // namespace

int RunSettingsStoreTests() {
    util::RuntimeApartment apartment;
    Check(apartment.Ready(), "SettingsStore tests require a usable Windows Runtime apartment");
    if (!apartment.Ready()) return g_failures;
    TestCurrentLegacyPartialAndMalformed();
    TestMissingEmptyAndCurrentRoundTrip();
    TestValidationAndLoadNormalizationMatrix();
    TestProductionCorruptPreservationAndAtomicWrite();
    TestReplacementFailurePreservesOldBytesAndCleansTemporaryFile();
    TestOversizedInputIsPreserved();
    TestNoOpAndTypedMutationResults();
    TestMutationDuringBlockedWriteAndFinalFlush();
    TestSubscriptionsAreOrderedAndReentrant();
    TestRetryAndDiscardShutdown();
    TestNormalShutdownFlushAndNoLateCallback();
    return g_failures;
}
