#include "TestCheck.hpp"
#include "ManualSettingsWakeup.hpp"

#include <core/SettingsStore.hpp>
#include <core/SettingsCodec.hpp>
#include <stdexcept>
#include <core/SettingsLimits.hpp>
#include <util/Logger.hpp>
#include <util/RuntimeApartment.hpp>

#include <wil/resource.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <format>
#include <future>
#include <iterator>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {

class ControlledStorage final : public SettingsStoreStorage {
public:
    ~ControlledStorage() override {
        if (Destroyed) Destroyed->set_value();
    }

    std::shared_ptr<std::promise<void>> Destroyed;
    std::optional<std::string> Read(std::filesystem::path const&) override {
        bool waitedForReadRelease = false;
        const auto finish = wil::scope_exit([&] {
            std::scoped_lock lock(m_operationMutex);
            if (waitedForReadRelease) --m_blockedReads;
            --m_activeStorageOperations;
        });
        {
            std::unique_lock lock(m_operationMutex);
            ++m_reads;
            ++m_activeStorageOperations;
            m_maxConcurrentStorageOperations = std::max(m_maxConcurrentStorageOperations, m_activeStorageOperations);
            m_readStarted.notify_all();
            waitedForReadRelease = m_blockReads;
            if (waitedForReadRelease) ++m_blockedReads;
            m_allowRead.wait(lock, [&] { return !m_blockReads; });
        }
        std::scoped_lock lock(m_dataMutex);
        return m_input;
    }

    bool WriteAtomically(std::filesystem::path const&, std::string_view bytes) override {
        const auto finish = wil::scope_exit([&] {
            std::scoped_lock lock(m_operationMutex);
            ++m_completedWrites;
            --m_activeWriters;
            --m_activeStorageOperations;
            m_writeCompleted.notify_all();
        });
        {
            std::unique_lock lock(m_operationMutex);
            ++m_activeWriters;
            ++m_activeStorageOperations;
            m_maxActiveWriters = std::max(m_maxActiveWriters, m_activeWriters);
            m_maxConcurrentStorageOperations = std::max(m_maxConcurrentStorageOperations, m_activeStorageOperations);
            if (m_blockedReads != 0) m_writeEnteredWhileReadBlocked = true;
            ++m_writes;
            m_writeStarted.notify_all();
            m_allowWrite.wait(lock, [&] { return !m_blockWrites; });
            if (m_failWrites != 0) {
                --m_failWrites;
                return false;
            }
        }
        std::scoped_lock lock(m_dataMutex);
        m_output.assign(bytes);
        m_outputs.emplace_back(bytes);
        return true;
    }

    bool PreserveCorrupt(std::filesystem::path const&) noexcept override {
        ++m_corruptPreservations;
        return PreservationSucceeds;
    }

    bool PreservationSucceeds = true;

    void SetInput(std::optional<std::string> input) {
        std::scoped_lock lock(m_dataMutex);
        m_input = std::move(input);
    }

    void BlockWrites() {
        std::scoped_lock lock(m_operationMutex);
        m_blockWrites = true;
    }

    void BlockReads() {
        std::scoped_lock lock(m_operationMutex);
        m_blockReads = true;
    }

    void ReleaseWrites() {
        {
            std::scoped_lock lock(m_operationMutex);
            m_blockWrites = false;
        }
        m_allowWrite.notify_all();
    }

    void ReleaseReads() {
        {
            std::scoped_lock lock(m_operationMutex);
            m_blockReads = false;
        }
        m_allowRead.notify_all();
    }

    void WaitForWrite() {
        std::unique_lock lock(m_operationMutex);
        m_writeStarted.wait(lock, [&] { return m_writes != 0; });
    }

    void WaitForCompletedWrites(unsigned int count) {
        std::unique_lock lock(m_operationMutex);
        m_writeCompleted.wait(lock, [&] { return m_completedWrites >= count; });
    }

    void WaitForRead() {
        std::unique_lock lock(m_operationMutex);
        m_readStarted.wait(lock, [&] { return m_reads != 0; });
    }

    [[nodiscard]] std::string Output() const {
        std::scoped_lock lock(m_dataMutex);
        return m_output;
    }

    [[nodiscard]] std::vector<std::string> Outputs() const {
        std::scoped_lock lock(m_dataMutex);
        return m_outputs;
    }

    unsigned int m_failWrites = 0;
    std::atomic_uint32_t m_corruptPreservations = 0;
    [[nodiscard]] unsigned int MaxActiveWriters() const {
        std::scoped_lock lock(m_operationMutex);
        return m_maxActiveWriters;
    }

    [[nodiscard]] unsigned int ReadCount() const {
        std::scoped_lock lock(m_operationMutex);
        return m_reads;
    }

    [[nodiscard]] unsigned int MaxConcurrentStorageOperations() const {
        std::scoped_lock lock(m_operationMutex);
        return m_maxConcurrentStorageOperations;
    }

    [[nodiscard]] bool DidWriteEnterWhileReadBlocked() const {
        std::scoped_lock lock(m_operationMutex);
        return m_writeEnteredWhileReadBlocked;
    }

private:
    mutable std::mutex m_operationMutex;
    mutable std::mutex m_dataMutex;
    std::condition_variable m_writeStarted;
    std::condition_variable m_writeCompleted;
    std::condition_variable m_allowWrite;
    std::condition_variable m_readStarted;
    std::condition_variable m_allowRead;
    std::optional<std::string> m_input;
    std::string m_output;
    std::vector<std::string> m_outputs;
    unsigned int m_writes = 0;
    unsigned int m_completedWrites = 0;
    unsigned int m_reads = 0;
    unsigned int m_activeWriters = 0;
    unsigned int m_activeStorageOperations = 0;
    unsigned int m_blockedReads = 0;
    unsigned int m_maxActiveWriters = 0;
    unsigned int m_maxConcurrentStorageOperations = 0;
    bool m_blockWrites = false;
    bool m_blockReads = false;
    bool m_writeEnteredWhileReadBlocked = false;
};

class CallbackGate final {
public:
    void EnterAndWait() {
        std::unique_lock lock(m_mutex);
        m_entered = true;
        m_enteredChanged.notify_all();
        m_releaseChanged.wait(lock, [&] { return m_released; });
    }

    void WaitForEntry() {
        std::unique_lock lock(m_mutex);
        m_enteredChanged.wait(lock, [&] { return m_entered; });
    }

    void Release() {
        {
            std::scoped_lock lock(m_mutex);
            m_released = true;
        }
        m_releaseChanged.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_enteredChanged;
    std::condition_variable m_releaseChanged;
    bool m_entered = false;
    bool m_released = false;
};

class ScopedTestDirectory final {
public:
    ScopedTestDirectory() : m_path(CreatePath()) { std::filesystem::create_directories(m_path); }

    ~ScopedTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]] std::filesystem::path const& Path() const noexcept { return m_path; }
    [[nodiscard]] std::filesystem::path SettingsPath() const { return m_path / L"AudioPlaybackConnector2.json"; }

private:
    static std::filesystem::path CreatePath() {
        static std::atomic_uint64_t nextIdentifier = 0;
        return std::filesystem::temp_directory_path() /
               (L"AudioPlaybackConnector2.SettingsStoreTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
                std::to_wstring(nextIdentifier++));
    }

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

void TestOnlyCurrentFormatIsAccepted() {
    const std::vector<std::string> invalid{
        "{",
        "",
        "[]",
        R"({"language":"de","privacyModeEnabled":true})",
        R"({"schemaVersion":1,"language":"de"})",
        R"({"schemaVersion":3,"language":"de"})",
        R"({"schemaVersion":2,"language":"de","unknown":true})",
        R"({"schemaVersion":2,"showNotifications":1})",
        R"({"schemaVersion":2,"language":false})",
        R"({"schemaVersion":2,"language":"unknown"})",
        R"({"schemaVersion":2,"language":"en","language":"de"})",
        R"({"schemaVersion":2,"devices":[{"id":"a"},{"id":"a"}]})",
        R"({"schemaVersion":2,"devices":[7]})",
        R"({"schemaVersion":2,"lastConnectedIds":["a","a"]})",
        R"({"schemaVersion":2,"defaultDeviceMode":"specificDevice"})",
        R"({"schemaVersion":2,"settingsWindowBounds":{"width":320,"height":240,"dpi":0}})",
        R"({"schemaVersion":2,"settingsWindowBounds":{"width":320,"height":240,"x":2147483648}})",
        R"({"schemaVersion":2,"settingsWindowBounds":{"width":320,"height":240,"dpi":144.5}})"};
    for (auto const& bytes : invalid) {
        auto storage = std::make_shared<ControlledStorage>();
        storage->SetInput(bytes);
        SettingsStore store({}, storage);
        store.Load();
        Check(store.Snapshot().Data == SettingsData{} && store.Snapshot().Revision == 0 &&
                  storage->m_corruptPreservations == 1,
              "unsupported or invalid input must be preserved with no partial settings adoption");
        Check(store.SetLanguage(L"ja").IsApplied() && store.FlushNow(1),
              "after successful preservation, new preferences must save using current defaults");
        Check(storage->Output().find("\"schemaVersion\":2") != std::string::npos &&
                  apc::settings::Decode(storage->Output()) == store.Snapshot().Data,
              "every replacement must use the current format and round-trip its complete state");
        static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
    }
    Check(apc::settings::Decode(R"({"schemaVersion":2})") == SettingsData{},
          "missing optional current-format fields must use current defaults");
    SettingsData unicode;
    unicode.Devices.push_back({L"device", L"Kopfh\u00f6rer \u97f3", L"Desk", true, false});
    Check(apc::settings::Decode(apc::settings::Encode(unicode)) == unicode,
          "UTF-8 persistence must round-trip Unicode device names");

    SettingsData prompted;
    prompted.RatingPrompt.FirstLaunchDate = L"2026-09-08";
    prompted.RatingPrompt.UsageDays = 4;
    prompted.RatingPrompt.LastUsageDate = L"2026-09-20";
    prompted.RatingPrompt.State = RatingPromptState::Deferred;
    prompted.RatingPrompt.DeferUntil = L"2026-10-06";
    prompted.RatingPrompt.DeferCount = 1;
    Check(apc::settings::Decode(apc::settings::Encode(prompted)) == prompted,
          "the rating prompt state must round-trip through the current format");
    const auto expectRejected = [](std::string_view bytes, std::string_view message) {
        try {
            static_cast<void>(apc::settings::Decode(bytes));
            Check(false, message);
        } catch (std::invalid_argument const&) {
            Check(true, message);
        }
    };
    expectRejected(R"({"schemaVersion":2,"ratingPrompt":{"firstLaunchDate":"2026-99-99"}})",
                   "malformed rating prompt dates must be rejected");
    expectRejected(R"({"schemaVersion":2,"ratingPrompt":{"state":"suspicious"}})",
                   "unknown rating prompt states must be rejected");
    expectRejected(R"({"schemaVersion":2,"ratingPrompt":{"usageDays":-3}})",
                   "negative rating prompt counters must be rejected");
    expectRejected(R"({"schemaVersion":2,"ratingPrompt":{"unexpected":1}})",
                   "unknown rating prompt keys must be rejected");
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
        R"({"schemaVersion":2,"globalConnectOnStartup":true,"globalReconnectOnConnectionLoss":true,"allowIncomingConnections":true,"startWithWindows":true,"showNotifications":false,"useSystemBackdropEffects":false,"language":"de","privacyModeEnabled":true,"defaultDeviceMode":"specificDevice","defaultDeviceId":"a","settingsWindowBounds":{"x":1,"y":2,"width":3,"height":4,"dpi":144},"devices":[{"id":"a","name":"A","alias":"Desk","connectOnStartup":true,"reconnectOnConnectionLoss":true}],"lastConnectedIds":["a"]})");
    SettingsStore current({}, input);
    current.Load();
    const auto data = current.Snapshot().Data;
    Check(data.GlobalConnectOnStartup && data.GlobalReconnectOnConnectionLoss && data.AllowIncomingConnections &&
              data.StartWithWindows && !data.ShowNotifications && !data.UseSystemBackdropEffects &&
              data.Language == L"de" && data.PrivacyModeEnabled &&
              data.DefaultDevice == DefaultDeviceMode::SpecificDevice && data.DefaultDeviceId == L"a" &&
              data.SettingsWindowBounds == PersistedWindowBounds{1, 2, 3, 4, 144} && data.Devices.size() == 1 &&
              data.Devices.front() == DeviceSettings{L"a", L"A", L"Desk", true, true} &&
              data.LastConnectedIds == std::vector<std::wstring>{L"a"},
          "current-schema input must retain every persisted field");
    static_cast<void>(current.SetLanguage(L"ja"));
    input->SetInput(R"({"schemaVersion":2,"language":"fr"})");
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

void TestFailedPreservationBlocksMutationAndFlush() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->SetInput("{");
    storage->PreservationSucceeds = false;
    SettingsStore store({}, storage);
    store.Load();
    Check(storage->m_corruptPreservations == 1 && store.SetLanguage(L"de").Status == SettingsMutationStatus::Rejected &&
              store.Snapshot().Revision == 0 && !store.FlushNow(1) && storage->Outputs().empty(),
          "failed preservation must reject mutations and flush without replacing the original");
    Check(!store.Shutdown(SettingsShutdownMode::Flush),
          "shutdown must report blocked persistence instead of claiming a successful flush");
}

void TestUnreadableFileIsNotTreatedAsMissing() {
    ScopedTestDirectory directory;
    constexpr std::string_view original = R"({"schemaVersion":2,"language":"fr"})";
    WriteBytes(directory.SettingsPath(), original);
    wil::unique_hfile blocker(CreateFileW(
        directory.SettingsPath().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(static_cast<bool>(blocker), "the production read-failure fixture must hold an exclusive file handle");
    SettingsStore store(directory.Path());
    store.Load();
    blocker.reset();
    Check(store.SetLanguage(L"de").Status == SettingsMutationStatus::Rejected && !store.FlushNow(1) &&
              ReadBytes(directory.SettingsPath()) == original,
          "a read and backup sharing violation must preserve bytes even after the external lock disappears");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestEmptyFileIsPreservedBeforeDefaultsCanBeSaved() {
    ScopedTestDirectory directory;
    WriteBytes(directory.SettingsPath(), "");
    SettingsStore store(directory.Path());
    store.Load();
    auto const backup = directory.SettingsPath().wstring() + L".corrupt.bak";
    Check(std::filesystem::exists(backup) && ReadBytes(backup).empty(),
          "an empty existing file is invalid input and must be preserved, not treated as absent");
    Check(store.SetLanguage(L"de").IsApplied() && store.FlushNow(1),
          "successful preservation must allow normal default-based persistence");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::Flush));
}

void TestCorruptFileDiagnosticsUseInjectedLogger() {
    ScopedTestDirectory directory;
    auto const logPath = directory.Path() / L"settings.log";
    util::Logger logger(logPath);
    WriteBytes(directory.SettingsPath(), "{invalid");
    SettingsStore store(directory.Path(), {}, {}, logger.Sink());
    store.Load();
    Check(store.Shutdown(SettingsShutdownMode::Flush), "settings owner drains before logging owner");
    Check(logger.Shutdown(std::chrono::milliseconds(5000)), "injected settings diagnostics drain");
    Check(ReadBytes(logPath).find("[SettingsStore] load failed") != std::string::npos,
          "settings persistence diagnostics use their explicitly injected logger");
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
    constexpr std::string_view original = R"({"schemaVersion":2,"language":"fr"})";
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

void TestDeviceIdValidationRejectsWithoutRevision() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    const auto initialRevision = store.Snapshot().Revision;
    const std::vector<std::wstring> invalidIds{L"",
                                               std::wstring(apc::limits::c_maxDeviceIdCharacters + 1, L'x'),
                                               std::wstring(1, static_cast<wchar_t>(0xD800))};
    for (auto const& id : invalidIds) {
        const auto remember = store.RememberDevice(id, L"Device");
        Check(remember.Status == SettingsMutationStatus::Rejected && remember.Revision == initialRevision,
              "invalid saved-device IDs must be rejected without a revision");
        const auto connect = store.SetDeviceConnectOnStartup(id, true);
        Check(connect.Status == SettingsMutationStatus::Rejected && connect.Revision == initialRevision,
              "invalid connect-on-startup IDs must be rejected without a revision");
        const auto reconnect = store.SetDeviceReconnectOnConnectionLoss(id, true);
        Check(reconnect.Status == SettingsMutationStatus::Rejected && reconnect.Revision == initialRevision,
              "invalid reconnect IDs must be rejected without a revision");
        const auto forget = store.ForgetDevice(id);
        Check(forget.Status == SettingsMutationStatus::Rejected && forget.Revision == initialRevision,
              "invalid forget IDs must be rejected without a revision");
    }
    Check(store.Snapshot().Revision == initialRevision, "rejected device IDs must not advance the store revision");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestDeviceSettingsBeforeFirstConnection() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    const auto initial = store.Snapshot();
    Check(store.RememberDevice(L"new", L"Phone").IsApplied(),
          "a discovered device must be configurable before its first connection");
    Check(store.SetDeviceConnectOnStartup(L"new", true).IsApplied(),
          "a never-connected device must accept a startup preference");
    Check(store.SetDeviceReconnectOnConnectionLoss(L"new", true).IsApplied(),
          "a never-connected device must accept a reconnect preference");
    Check(store.SetDeviceAlias(L"new", L"Desk").Mutation.IsApplied(), "an unconnected device must accept an alias");
    const auto configured = store.Snapshot();
    Check(configured.Data.LastConnectedIds == initial.Data.LastConnectedIds &&
              configured.Data.DefaultDevice == initial.Data.DefaultDevice &&
              configured.Data.DefaultDeviceId == initial.Data.DefaultDeviceId,
          "configuring a device must not fabricate connection history or change the default target");
    Check(store.RememberDevice(L"new", L"Changed discovery name").Status == SettingsMutationStatus::Unchanged &&
              store.Snapshot().Revision == configured.Revision,
          "remembering an existing device must preserve its settings and revision");
    const auto invalidName =
        store.RememberDevice(L"invalid-name", std::wstring(apc::limits::c_maxDeviceNameCharacters + 1, L'x'));
    Check(invalidName.Status == SettingsMutationStatus::Rejected && invalidName.Revision == configured.Revision,
          "an oversized saved-device name must be rejected without changing settings");
    Check(store.FlushNow(2), "preferences for an unconnected device must persist");
    storage->SetInput(storage->Output());
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));

    SettingsStore reader({}, storage);
    reader.Load();
    auto restored = reader.Snapshot().Data;
    Check(restored.Devices.size() == 1 && restored.LastConnectedIds.empty(),
          "reloading a configured device must not turn it into a previously connected device");
    if (!restored.Devices.empty()) {
        auto const& device = restored.Devices.front();
        Check(device.Id == L"new" && device.Name == L"Phone" && device.Alias == L"Desk" && device.ConnectOnStartup &&
                  device.ReconnectOnConnectionLoss,
              "all pre-connection preferences must survive persistence");
    }
    const auto connection = reader.RecordConnectedDevice(L"new", L"Phone renamed by OS");
    Check(connection.IsApplied() && reader.Snapshot().Data.Devices.front().ReconnectOnConnectionLoss,
          "the first real connection must reuse configured preferences");
    restored = reader.Snapshot().Data;
    Check(restored.Devices.size() == 1 && restored.Devices.front().Alias == L"Desk" &&
              restored.Devices.front().ConnectOnStartup &&
              restored.LastConnectedIds == std::vector<std::wstring>{L"new"},
          "the first connection must preserve settings and record real connection history exactly once");
    static_cast<void>(reader.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestRecordedPreferencesAndNames() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    const auto first = store.RecordConnectedDevice(L"known", L"Known");
    Check(first.IsApplied() && !store.Snapshot().Data.Devices.front().ReconnectOnConnectionLoss,
          "new devices must start with the global reconnect policy");
    Check(store.SetDeviceReconnectOnConnectionLoss(L"known", true).IsApplied(),
          "an existing per-device reconnect policy must apply");
    const auto known = store.RecordConnectedDevice(L"known", L"Known");
    Check(known.Status == SettingsMutationStatus::Unchanged &&
              store.Snapshot().Data.Devices.front().ReconnectOnConnectionLoss,
          "recording a known device must preserve its configured policy");
    Check(store.SetDeviceAlias(L"known", L"Desk").Mutation.IsApplied(),
          "an alias must be persisted before testing hidden name updates");
    const auto aliasedName = store.RecordConnectedDevice(L"known", L"Renamed");
    Check(aliasedName.IsApplied() && store.Snapshot().Data.Devices.front().Alias == L"Desk",
          "a renamed aliased device must persist its name without changing its visible presentation");
    Check(store.Snapshot().Data.Devices.front().Name == L"Renamed",
          "a renamed aliased device must update its stored name");
    Check(store.SetPrivacyModeEnabled(true).IsApplied(), "privacy mode must apply before the second hidden rename");
    const auto privateName = store.RecordConnectedDevice(L"known", L"PrivateName");
    Check(privateName.IsApplied() && store.Snapshot().Data.PrivacyModeEnabled,
          "a renamed private device must persist its name without changing its visible presentation");
    Check(store.Snapshot().Data.Devices.front().Name == L"PrivateName",
          "a renamed private device must update its stored name");

    for (std::size_t index = 1; index < apc::limits::c_maxPersistedDeviceCount; ++index) {
        const auto id = L"device-" + std::to_wstring(index);
        const auto added = store.RecordConnectedDevice(id, L"Device");
        Check(added.Status == SettingsMutationStatus::Applied,
              "the bounded device table must accept each unique device up to its limit");
    }
    Check(store.Snapshot().Data.Devices.size() == apc::limits::c_maxPersistedDeviceCount,
          "the device table must reach its configured bound");
    Check(store.RememberDevice(L"settings-overflow", L"Overflow").Status == SettingsMutationStatus::Unchanged &&
              store.Snapshot().Data.Devices.size() == apc::limits::c_maxPersistedDeviceCount,
          "configuring an additional device must respect the persisted-device limit");
    Check(store.SetGlobalReconnectOnConnectionLoss(true).IsApplied(),
          "the global reconnect policy must apply before the overflow record");
    const auto overflow = store.RecordConnectedDevice(L"overflow", L"Overflow");
    Check(overflow.IsApplied() && store.Snapshot().Data.Devices.size() == apc::limits::c_maxPersistedDeviceCount &&
              store.Snapshot().Data.LastConnectedIds.front() == L"overflow",
          "connection history remains bounded and current even when the saved-device table is full");
    Check(store.FlushNow(2), "hidden device-name mutations must flush successfully");
    storage->SetInput(storage->Output());
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));

    SettingsStore reader({}, storage);
    reader.Load();
    const auto& persisted = reader.Snapshot().Data;
    const auto persistedKnown = std::ranges::find(persisted.Devices, L"known", &DeviceSettings::Id);
    Check(persistedKnown != persisted.Devices.end() && persistedKnown->Name == L"PrivateName" &&
              persistedKnown->Alias == L"Desk",
          "hidden device-name mutations must round-trip with alias and privacy state");
    static_cast<void>(reader.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
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

void TestDebouncedWorkerWaitsForSynchronousWriter() {
    auto storage = std::make_shared<ControlledStorage>();
    auto wakeup = std::make_shared<ManualSettingsWakeup>();
    storage->BlockWrites();
    SettingsStore store({}, storage, wakeup);
    Check(store.SetLanguage(L"de").IsApplied(), "the first revision must schedule persistence");
    bool flushResult = false;
    std::jthread flushing([&] { flushResult = store.FlushNow(2); });
    storage->WaitForWrite();
    Check(store.SetPrivacyModeEnabled(true).IsApplied(),
          "a newer mutation must rearm the debounce timer during a synchronous write");

    const auto version = wakeup->Advance(std::chrono::seconds{1});
    Check(wakeup->WaitUntilParked(version, std::nullopt),
          "after the debounce deadline a busy worker must wait for a signal without a polling deadline");
    Check(storage->MaxActiveWriters() == 1, "the debounced worker must not overlap the synchronous writer");
    storage->ReleaseWrites();
    flushing.join();

    Check(flushResult, "the synchronous flush must complete after the blocked writer is released");
    Check(storage->Output().find("privacyModeEnabled\":true") != std::string::npos,
          "the synchronous flush must persist the newest revision after the debounce deadline");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestManualDebounceAndIdleWait() {
    auto storage = std::make_shared<ControlledStorage>();
    auto wakeup = std::make_shared<ManualSettingsWakeup>();
    SettingsStore store({}, storage, wakeup);
    Check(store.SetLanguage(L"de").IsApplied(), "debounce must begin with a committed mutation");
    auto deadline = SettingsStoreWakeup::Clock::time_point{} + std::chrono::milliseconds{300};
    Check(wakeup->WaitUntilParked(wakeup->Version(), deadline), "worker must schedule the debounce deadline");
    auto version = wakeup->Advance(std::chrono::milliseconds{299});
    Check(wakeup->WaitUntilParked(version, deadline) && storage->MaxActiveWriters() == 0,
          "a clock advance before the deadline must not start persistence");
    version = wakeup->Advance(std::chrono::milliseconds{1});
    storage->WaitForCompletedWrites(1);
    Check(wakeup->WaitUntilParked(version, std::nullopt) && !store.Snapshot().IsDirty,
          "after writing the due revision the clean worker must park indefinitely");
    version = wakeup->Advance(std::chrono::hours{1});
    Check(wakeup->WaitUntilParked(version, std::nullopt) && storage->Outputs().size() == 1,
          "clock changes alone must not cause a clean store to poll or write");
    Check(store.Shutdown(SettingsShutdownMode::Flush), "shutdown must wake an indefinitely parked worker");
}

void TestNotificationBeforeWaitEntryIsRetained() {
    auto storage = std::make_shared<ControlledStorage>();
    auto wakeup = std::make_shared<ManualSettingsWakeup>();
    auto gate = std::make_shared<ManualSettingsWakeup::WaitGate>();
    wakeup->BlockNextWait(gate);
    SettingsStore store({}, storage, wakeup);
    Check(gate->Entered.try_acquire_for(std::chrono::seconds{2}), "worker must reach the wait-entry barrier");
    Check(store.SetLanguage(L"fr").IsApplied(), "mutation must notify while wait entry is delayed");
    wakeup->Advance(std::chrono::seconds{1});
    gate->Release.release();
    storage->WaitForCompletedWrites(1);
    Check(store.Shutdown(SettingsShutdownMode::Flush), "notification before wait registration must not be lost");
    Check(storage->Output().find("\"language\":\"fr\"") != std::string::npos,
          "the raced wakeup must persist the committed revision");
}

void TestFrozenPersistenceClockCannotExtendShutdownBudget() {
    auto destroyed = std::make_shared<std::promise<void>>();
    auto released = destroyed->get_future();
    auto storage = std::make_shared<ControlledStorage>();
    storage->Destroyed = destroyed;
    std::weak_ptr<ControlledStorage> lifetime = storage;
    auto wakeup = std::make_shared<ManualSettingsWakeup>();
    auto gate = std::make_shared<ManualSettingsWakeup::WaitGate>();
    wakeup->BlockNextWait(gate);
    {
        SettingsStore store({}, storage, wakeup);
        Check(gate->Entered.try_acquire_for(std::chrono::seconds{2}), "worker must enter the delayed platform wait");
        auto started = std::chrono::steady_clock::now();
        Check(!store.Shutdown(SettingsShutdownMode::DiscardStartupFailure, 1, std::chrono::milliseconds{50}),
              "an undrained platform wait must report incomplete shutdown");
        Check(std::chrono::steady_clock::now() - started < std::chrono::seconds{1},
              "a frozen persistence clock must not freeze the real shutdown deadline");
    }
    storage.reset();
    Check(!lifetime.expired(), "the delayed worker must retain storage after facade destruction");
    gate->Release.release();
    Check(released.wait_for(std::chrono::seconds{2}) == std::future_status::ready,
          "releasing the delayed wait must let the cancelled worker release its retained state");
}

void TestWorkerPersistsDistinctCommittedRevisions() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->BlockWrites();
    SettingsStore store({}, storage);
    Check(store.SetLanguage(L"fr").IsApplied(), "the worker must receive its first committed revision");
    storage->WaitForWrite();
    auto retained = store.Snapshot();
    Check(store.SetLanguage(L"de").IsApplied(), "mutation must publish while an older revision is being written");
    storage->ReleaseWrites();
    storage->WaitForCompletedWrites(2);
    Check(store.Shutdown(SettingsShutdownMode::Flush), "both revisions must finish persistence");
    auto outputs = storage->Outputs();
    Check(outputs.size() == 2 && outputs.front().find("\"language\":\"fr\"") != std::string::npos &&
              outputs.back().find("\"language\":\"de\"") != std::string::npos,
          "the worker must write separate old and new committed values without mixing revisions");
    Check(retained.Data.Language == L"fr" && retained.Revision < store.Snapshot().Revision,
          "a previously returned snapshot must remain independent of subsequent changes");
}

void TestStorageFailureRetriesLatestRevision() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->m_failWrites = 1;
    storage->BlockWrites();
    SettingsStore store({}, storage);
    Check(store.SetLanguage(L"fr").IsApplied(), "a revision must be pending before the failed write");
    storage->WaitForWrite();
    Check(store.SetLanguage(L"de").IsApplied(), "a newer revision must be accepted during the failed write");
    storage->ReleaseWrites();
    storage->WaitForCompletedWrites(2);
    Check(store.Shutdown(SettingsShutdownMode::Flush), "a storage retry must persist the latest revision");
    auto outputs = storage->Outputs();
    Check(outputs.size() == 1 && outputs.front().find("\"language\":\"de\"") != std::string::npos,
          "retry must capture the current committed data instead of replaying the failed old revision");
}

void TestLoadAdmissionFencesMutationAndFlush() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->SetInput(R"({"schemaVersion":2,"language":"fr","privacyModeEnabled":false})");
    storage->BlockReads();
    SettingsStore store({}, storage);
    std::jthread loading([&] { store.Load(); });
    storage->WaitForRead();

    SettingsMutationResult mutation;
    bool flushed = false;
    std::jthread mutating([&] { mutation = store.SetPrivacyModeEnabled(true); });
    std::jthread flushing([&] { flushed = store.FlushNow(2); });
    storage->ReleaseReads();
    loading.join();
    mutating.join();
    flushing.join();

    const auto snapshot = store.Snapshot();
    Check(mutation.IsApplied() && snapshot.Data.Language == L"fr" && snapshot.Data.PrivacyModeEnabled,
          "an admitted load must normalize before a concurrent mutation is committed");
    Check(flushed && storage->MaxConcurrentStorageOperations() == 1,
          "load and synchronous flush must serialize storage access");
    Check(!storage->DidWriteEnterWhileReadBlocked(),
          "a synchronous write must not enter the storage boundary while the admitted read is blocked");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestShutdownWaitsForAdmittedLoad() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->SetInput(R"({"schemaVersion":2,"language":"ko"})");
    storage->BlockReads();
    SettingsStore store({}, storage);
    std::jthread loading([&] { store.Load(); });
    storage->WaitForRead();

    bool shutdownResult = false;
    std::jthread shuttingDown([&] { shutdownResult = store.Shutdown(SettingsShutdownMode::Flush, 2); });
    storage->ReleaseReads();
    loading.join();
    shuttingDown.join();

    Check(shutdownResult && store.Snapshot().Data.Language == L"ko",
          "shutdown must retain an admitted load before completing its final flush boundary");
    Check(storage->MaxConcurrentStorageOperations() == 1,
          "shutdown must not overlap its persistence boundary with an admitted read");
}

void TestShutdownClosesAdmissionBeforeAdmittedLoadCompletes() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->SetInput(R"({"schemaVersion":2,"language":"fr"})");
    storage->BlockReads();
    SettingsStore store({}, storage);
    std::jthread loading([&] { store.Load(); });
    storage->WaitForRead();

    bool shutdownResult = false;
    SettingsMutationResult postShutdownMutation;
    std::mutex mutationMutex;
    std::condition_variable mutationChanged;
    bool mutationStarted = false;
    bool mutationFinished = false;
    // The mutation begins while Load owns admission. It cannot pass that fence before shutdown closes the
    // store, regardless of which waiter receives the state mutex first after the read completes.
    std::jthread mutating([&] {
        {
            std::scoped_lock lock(mutationMutex);
            mutationStarted = true;
        }
        mutationChanged.notify_all();
        postShutdownMutation = store.SetPrivacyModeEnabled(false);
        {
            std::scoped_lock lock(mutationMutex);
            mutationFinished = true;
        }
        mutationChanged.notify_all();
    });
    {
        std::unique_lock lock(mutationMutex);
        mutationChanged.wait(lock, [&] { return mutationStarted; });
    }
    std::jthread shuttingDown([&] { shutdownResult = store.Shutdown(SettingsShutdownMode::Flush, 2); });
    bool mutationRejectedBeforeReadRelease = false;
    {
        std::unique_lock lock(mutationMutex);
        mutationRejectedBeforeReadRelease =
            mutationChanged.wait_for(lock, std::chrono::seconds(5), [&] { return mutationFinished; });
    }
    storage->ReleaseReads();
    loading.join();
    mutating.join();
    shuttingDown.join();

    std::atomic_uint32_t lateCallbackCount = 0;
    auto lateSubscription = store.Subscribe([&](SettingsSnapshot const&) { ++lateCallbackCount; });
    static_cast<void>(store.SetLanguage(L"de"));
    lateSubscription.Reset();
    Check(mutationRejectedBeforeReadRelease && shutdownResult &&
              postShutdownMutation.Status == SettingsMutationStatus::Rejected,
          "shutdown must promptly reject a mutation waiting behind an admitted blocked Load");
    Check(store.Snapshot().Data.Language == L"fr" && lateCallbackCount == 0,
          "an admitted load must publish state without callbacks after shutdown closes admission");
    Check(!storage->DidWriteEnterWhileReadBlocked(),
          "shutdown's final flush must not enter storage while the admitted read remains blocked");
}

void TestMutationBeforeLoadSkipsStorageRead() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->SetInput(R"({"schemaVersion":2,"language":"fr"})");
    SettingsStore store({}, storage);
    Check(store.SetLanguage(L"de").IsApplied(), "runtime mutation must commit before a first load attempt");
    store.Load();
    Check(storage->ReadCount() == 0 && store.Snapshot().Data.Language == L"de",
          "a mutation before Load must make the later load a no-op without storage I/O");
    Check(store.FlushNow(2), "the runtime-owned revision must remain persistable after a skipped load");
    Check(storage->MaxConcurrentStorageOperations() == 1, "a skipped load must not race the writer with a file read");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestAutomaticDebounceAndRetry() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->m_failWrites = 1;
    SettingsStore store({}, storage);
    Check(store.SetLanguage(L"fr").IsApplied(), "automatic persistence must begin from a committed mutation");
    storage->WaitForCompletedWrites(2);
    Check(storage->Output().find("\"language\":\"fr\"") != std::string::npos,
          "the worker must retry the failed debounced write without a caller-driven flush");
    Check(storage->MaxActiveWriters() == 1, "automatic retry must retain one writer at a time");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestPersistedMutationRoundTrip() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore writer({}, storage);
    Check(writer.SetGlobalConnectOnStartup(true).IsApplied(), "global startup policy must persist");
    Check(writer.SetGlobalReconnectOnConnectionLoss(true).IsApplied(), "global reconnect policy must persist");
    Check(writer.SetAllowIncomingConnections(true).IsApplied(), "incoming policy must persist");
    Check(writer.SetStartWithWindows(true).IsApplied(), "startup registration preference must persist");
    Check(writer.SetShowNotifications(false).IsApplied(), "notification preference must persist");
    Check(writer.SetUseSystemBackdropEffects(false).IsApplied(), "backdrop preference must persist");
    Check(writer.SetLanguage(L"de").IsApplied(), "language must persist");
    Check(writer.SetPrivacyModeEnabled(true).IsApplied(), "privacy preference must persist");
    Check(writer.SetSettingsWindowBounds(PersistedWindowBounds{1, 2, 320, 240, 144}).IsApplied(),
          "validated window bounds must persist");
    Check(writer.RecordConnectedDevice(L"primary", L"Primary").IsApplied(), "recorded device data must persist");
    Check(writer.SetDeviceConnectOnStartup(L"primary", false).IsApplied(),
          "per-device startup policy mutation must persist");
    Check(writer.SetDeviceReconnectOnConnectionLoss(L"primary", false).IsApplied(),
          "per-device reconnect policy mutation must persist");
    Check(writer.SetDeviceAlias(L"primary", L"Desk").Mutation.IsApplied(), "device alias mutation must persist");
    Check(writer.RecordConnectedDevice(L"removed", L"Removed").IsApplied(),
          "a second device must exercise forget-device persistence");
    Check(writer.ForgetDevice(L"removed").IsApplied(), "forget-device mutation must persist its removal");
    Check(writer.SetDefaultDevice(L"primary").IsApplied(), "default-device mutation must persist");
    Check(writer.ClearDefaultDevice().IsApplied(), "clear-default mutation must persist");
    Check(writer.SetDefaultDevice(L"primary").IsApplied(), "default-device reset must persist");
    Check(writer.FlushNow(2), "all persisted mutation fields must serialize in one snapshot");
    Check(storage->Output().find("\"schemaVersion\":2") != std::string::npos,
          "persisted preferences must declare the sole supported format");
    storage->SetInput(storage->Output());
    static_cast<void>(writer.Shutdown(SettingsShutdownMode::DiscardStartupFailure));

    SettingsStore reader({}, storage);
    reader.Load();
    const auto& data = reader.Snapshot().Data;
    Check(data.GlobalConnectOnStartup && data.GlobalReconnectOnConnectionLoss && data.AllowIncomingConnections &&
              data.StartWithWindows && !data.ShowNotifications && !data.UseSystemBackdropEffects &&
              data.Language == L"de" && data.PrivacyModeEnabled &&
              data.SettingsWindowBounds == PersistedWindowBounds{1, 2, 320, 240, 144} &&
              data.DefaultDevice == DefaultDeviceMode::SpecificDevice && data.DefaultDeviceId == L"primary" &&
              data.Devices ==
                  std::vector<DeviceSettings>{DeviceSettings{L"primary", L"Primary", L"Desk", false, false}} &&
              data.LastConnectedIds == std::vector<std::wstring>{L"primary"},
          "every serialized settings field must round-trip through a fresh store");
    static_cast<void>(reader.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
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

void TestResetFencesBlockedCallback() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    CallbackGate callbackGate;
    std::atomic_bool callbackCompleted = false;
    std::atomic_uint32_t callbackCount = 0;
    auto subscription = store.Subscribe([&](SettingsSnapshot const&) {
        callbackGate.EnterAndWait();
        callbackCompleted = true;
        ++callbackCount;
    });

    std::jthread mutating([&] { static_cast<void>(store.SetPrivacyModeEnabled(true)); });
    callbackGate.WaitForEntry();
    std::atomic_bool resetReturnedAfterCallback = false;
    std::jthread resetting([&] {
        subscription.Reset();
        resetReturnedAfterCallback = callbackCompleted.load();
    });
    callbackGate.Release();
    mutating.join();
    resetting.join();

    Check(resetReturnedAfterCallback,
          "Reset must wait for a callback admitted before its deactivation fence before returning");
    static_cast<void>(store.SetLanguage(L"fr"));
    Check(callbackCount == 1, "a reset subscription must remain inactive for later queued publications");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestSelfResetSuppressesQueuedCallback() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    std::vector<std::uint64_t> revisions;
    std::optional<SettingsStore::Subscription> subscription;
    subscription.emplace(store.Subscribe([&](SettingsSnapshot const& snapshot) {
        revisions.push_back(snapshot.Revision);
        if (snapshot.Revision != 1) return;
        Check(store.SetLanguage(L"fr").IsApplied(), "reentrant mutation must enqueue a later publication");
        subscription->Reset();
    }));
    Check(store.SetPrivacyModeEnabled(true).IsApplied(), "initial callback-driving mutation must apply");
    Check(revisions == std::vector<std::uint64_t>{1},
          "self-reset must not deadlock and must suppress the callback already queued by reentrancy");
    static_cast<void>(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure));
}

void TestCallbackCanDestroyStoreDuringPublicationDrain() {
    auto storage = std::make_shared<ControlledStorage>();
    auto store = std::make_unique<SettingsStore>(std::filesystem::path{}, storage);
    std::optional<SettingsStore::Subscription> subscription;
    bool callbackReturned = false;
    subscription.emplace(store->Subscribe([&](SettingsSnapshot const&) {
        subscription->Reset();
        store.reset();
        callbackReturned = true;
    }));
    auto* rawStore = store.get();
    Check(rawStore->SetPrivacyModeEnabled(true).IsApplied(),
          "callback-driven destruction must leave commit completion valid");
    Check(!store && callbackReturned,
          "publication draining must retain the implementation until callback-driven destruction returns");
    subscription.reset();
}

void TestSubscriberInitiatedShutdownDoesNotDeadlock() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    bool callbackReturned = false;
    auto subscription = store.Subscribe([&](SettingsSnapshot const&) {
        Check(store.Shutdown(SettingsShutdownMode::DiscardStartupFailure),
              "subscriber-initiated shutdown must complete and report its core result");
        callbackReturned = true;
    });
    Check(store.SetPrivacyModeEnabled(true).IsApplied(), "the mutation driving subscriber shutdown must apply");
    Check(callbackReturned, "subscriber-initiated shutdown must return control to the publisher");
    Check(store.Shutdown(SettingsShutdownMode::Flush),
          "shutdown after a subscriber-initiated shutdown must return the stored result");
    subscription.Reset();
}

void TestConcurrentShutdownCallersShareCoreResult() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->BlockWrites();
    storage->m_failWrites = 2;
    SettingsStore store({}, storage);
    static_cast<void>(store.SetLanguage(L"de"));

    bool firstResult = true;
    bool secondResult = true;
    std::jthread first([&] { firstResult = store.Shutdown(SettingsShutdownMode::Flush, 1); });
    storage->WaitForWrite();
    std::jthread second([&] { secondResult = store.Shutdown(SettingsShutdownMode::DiscardStartupFailure, 1); });
    storage->ReleaseWrites();
    first.join();
    second.join();
    Check(!firstResult && !secondResult,
          "concurrent shutdown callers must wait for and return the executor's stored flush result");
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

void TestShutdownBudgetFencesBlockedLoad() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->SetInput(R"({"schemaVersion":2,"language":"ko"})");
    storage->BlockReads();
    SettingsStore store({}, storage);
    std::jthread loading([&] { store.Load(); });
    storage->WaitForRead();
    const auto before = store.Snapshot();
    std::promise<bool> completed;
    auto result = completed.get_future();
    std::jthread shuttingDown(
        [&] { completed.set_value(store.Shutdown(SettingsShutdownMode::Flush, 3, std::chrono::milliseconds(30))); });
    const auto returned = result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    Check(returned, "shutdown must return while an admitted read remains blocked");
    storage->ReleaseReads();
    loading.join();
    shuttingDown.join();
    Check(!result.get(), "blocked load must report incomplete shutdown");
    Check(store.Snapshot() == before, "load completion after timeout must not change the public snapshot");
    Check(store.SetLanguage(L"de").Status == SettingsMutationStatus::Rejected,
          "load timeout must permanently close mutation admission");
}

void TestShutdownBudgetRetainsBlockedWriterLifetime() {
    auto storage = std::make_shared<ControlledStorage>();
    auto destroyed = std::make_shared<std::promise<void>>();
    auto released = destroyed->get_future();
    storage->Destroyed = destroyed;
    storage->BlockWrites();
    auto store = std::make_unique<SettingsStore>(std::filesystem::path{}, storage);
    static_cast<void>(store->SetLanguage(L"de"));
    std::promise<bool> completed;
    auto result = completed.get_future();
    std::jthread shuttingDown(
        [&] { completed.set_value(store->Shutdown(SettingsShutdownMode::Flush, 3, std::chrono::milliseconds(30))); });
    storage->WaitForWrite();
    const auto returned = result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    Check(returned, "shutdown must not join a persistence worker blocked in storage");
    if (!returned) storage->ReleaseWrites();
    shuttingDown.join();
    Check(!result.get(), "blocked final write must report incomplete shutdown");
    Check(store->Snapshot().IsDirty, "a timed-out write must not report persistence success");
    store.reset();
    Check(released.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
          "storage must remain alive after facade destruction while its write is blocked");
    storage->ReleaseWrites();
    storage.reset();
    Check(released.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "the final writer must release retained state without a worker ownership cycle");
}

void TestShutdownBudgetFencesLateWriteCompletion() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->BlockWrites();
    SettingsStore store({}, storage);
    static_cast<void>(store.SetLanguage(L"fr"));
    std::promise<bool> completed;
    auto result = completed.get_future();
    std::jthread shuttingDown(
        [&] { completed.set_value(store.Shutdown(SettingsShutdownMode::Flush, 1, std::chrono::milliseconds(30))); });
    storage->WaitForWrite();
    const auto returned = result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    Check(returned, "a blocked write must not consume an unlimited shutdown budget");
    const auto before = store.Snapshot();
    storage->ReleaseWrites();
    shuttingDown.join();
    storage->WaitForCompletedWrites(1);
    Check(!result.get(), "the write timeout must remain observable after its I/O completes");
    Check(store.Snapshot() == before, "late write completion must not update revision or dirty state");
}

void TestShutdownBudgetIncludesBlockedSubscriber() {
    auto storage = std::make_shared<ControlledStorage>();
    SettingsStore store({}, storage);
    CallbackGate gate;
    auto subscription = store.Subscribe([&](SettingsSnapshot const&) { gate.EnterAndWait(); });
    std::jthread mutating([&] { static_cast<void>(store.SetLanguage(L"ja")); });
    gate.WaitForEntry();
    std::promise<bool> completed;
    auto result = completed.get_future();
    std::jthread shuttingDown(
        [&] { completed.set_value(store.Shutdown(SettingsShutdownMode::Flush, 1, std::chrono::milliseconds(30))); });
    const auto returned = result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    Check(returned, "publication drain must share the total shutdown budget");
    gate.Release();
    mutating.join();
    shuttingDown.join();
    Check(!result.get(), "an executing subscriber must make shutdown report incomplete drain");
    Check(store.SetLanguage(L"de").Status == SettingsMutationStatus::Rejected,
          "publication timeout must not reopen mutation admission");
    subscription.Reset();
}

void TestConcurrentShutdownCallerHasItsOwnBudget() {
    auto storage = std::make_shared<ControlledStorage>();
    storage->BlockWrites();
    SettingsStore store({}, storage);
    static_cast<void>(store.SetLanguage(L"fr"));
    bool firstResult = false;
    std::jthread first([&] { firstResult = store.Shutdown(SettingsShutdownMode::Flush, 1, std::chrono::seconds(5)); });
    storage->WaitForWrite();
    std::promise<bool> completed;
    auto secondResult = completed.get_future();
    std::jthread second([&] {
        completed.set_value(
            store.Shutdown(SettingsShutdownMode::DiscardStartupFailure, 1, std::chrono::milliseconds(30)));
    });
    const auto returned = secondResult.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    Check(returned, "a second shutdown caller must not inherit the executor's longer deadline");
    storage->ReleaseWrites();
    first.join();
    second.join();
    Check(!secondResult.get() && firstResult,
          "a caller timeout must not change the first caller's flush policy or successful result");
}

} // namespace

int RunSettingsStoreTests() {
    TestOnlyCurrentFormatIsAccepted();
    util::RuntimeApartment apartment;
    Check(apartment.Ready(), "SettingsStore tests require a usable Windows Runtime apartment");
    if (!apartment.Ready()) return g_failures;
    TestMissingEmptyAndCurrentRoundTrip();
    TestValidationAndLoadNormalizationMatrix();
    TestProductionCorruptPreservationAndAtomicWrite();
    TestFailedPreservationBlocksMutationAndFlush();
    TestUnreadableFileIsNotTreatedAsMissing();
    TestEmptyFileIsPreservedBeforeDefaultsCanBeSaved();
    TestCorruptFileDiagnosticsUseInjectedLogger();
    TestReplacementFailurePreservesOldBytesAndCleansTemporaryFile();
    TestOversizedInputIsPreserved();
    TestNoOpAndTypedMutationResults();
    TestDeviceIdValidationRejectsWithoutRevision();
    TestDeviceSettingsBeforeFirstConnection();
    TestRecordedPreferencesAndNames();
    TestMutationDuringBlockedWriteAndFinalFlush();
    TestDebouncedWorkerWaitsForSynchronousWriter();
    TestManualDebounceAndIdleWait();
    TestNotificationBeforeWaitEntryIsRetained();
    TestFrozenPersistenceClockCannotExtendShutdownBudget();
    TestWorkerPersistsDistinctCommittedRevisions();
    TestStorageFailureRetriesLatestRevision();
    TestLoadAdmissionFencesMutationAndFlush();
    TestShutdownWaitsForAdmittedLoad();
    TestShutdownClosesAdmissionBeforeAdmittedLoadCompletes();
    TestMutationBeforeLoadSkipsStorageRead();
    TestAutomaticDebounceAndRetry();
    TestPersistedMutationRoundTrip();
    TestSubscriptionsAreOrderedAndReentrant();
    TestResetFencesBlockedCallback();
    TestSelfResetSuppressesQueuedCallback();
    TestCallbackCanDestroyStoreDuringPublicationDrain();
    TestSubscriberInitiatedShutdownDoesNotDeadlock();
    TestConcurrentShutdownCallersShareCoreResult();
    TestRetryAndDiscardShutdown();
    TestNormalShutdownFlushAndNoLateCallback();
    TestShutdownBudgetFencesBlockedLoad();
    TestShutdownBudgetRetainsBlockedWriterLifetime();
    TestShutdownBudgetFencesLateWriteCompletion();
    TestShutdownBudgetIncludesBlockedSubscriber();
    TestConcurrentShutdownCallerHasItsOwnBudget();
    return g_failures;
}
