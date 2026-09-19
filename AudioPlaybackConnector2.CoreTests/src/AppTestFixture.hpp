#pragma once

#include "DeviceTestFixture.hpp"
#include <app/AppController.hpp>
#include <core/SettingsStore.hpp>

namespace apc::tests {

class MemorySettingsStorage final : public SettingsStoreStorage {
public:
    std::optional<std::string> Read(std::filesystem::path const&) override { return std::nullopt; }
    bool WriteAtomically(std::filesystem::path const&, std::string_view) override { return true; }
    void PreserveCorrupt(std::filesystem::path const&) noexcept override {}
};

inline std::shared_ptr<SettingsStore> MakeTestSettings() {
    return std::make_shared<SettingsStore>(std::filesystem::path{L"."}, std::make_shared<MemorySettingsStorage>());
}

class TestPresentation final : public apc::app::AppPresentation {
public:
    apc::app::AppUiActionResult PresentDevicePicker(apc::app::DevicePickerOpenMode mode,
                                                    apc::app::AppCommandContext const& context) override {
        Modes.push_back(mode);
        Contexts.push_back(context);
        return {apc::app::AppActionStatus::Succeeded, ++OpenedGeneration};
    }
    apc::app::AppUiActionResult PresentSettings(apc::app::AppCommandContext const& context) override {
        ++SettingsCalls;
        if (SettingsAction) return SettingsAction(context);
        Contexts.push_back(context);
        return {apc::app::AppActionStatus::Succeeded, std::nullopt};
    }
    apc::app::AppSnapshot::ResourceStatusSnapshot ResourceStatus() const override {
        if (BeforeResourceRead) BeforeResourceRead();
        return Resources;
    }
    std::uint64_t PickerOpenedGeneration() const override { return OpenedGeneration; }

    std::vector<apc::app::DevicePickerOpenMode> Modes;
    std::vector<apc::app::AppCommandContext> Contexts;
    std::atomic_int SettingsCalls = 0;
    std::uint64_t OpenedGeneration = 0;
    apc::app::AppSnapshot::ResourceStatusSnapshot Resources;
    std::function<apc::app::AppUiActionResult(apc::app::AppCommandContext const&)> SettingsAction;
    std::function<void()> BeforeResourceRead;
};

struct AppFixture {
    std::shared_ptr<SettingsStore> Settings = MakeTestSettings();
    std::shared_ptr<device::Fixture> Devices = std::make_shared<device::Fixture>();
    std::shared_ptr<TestPresentation> Presentation = std::make_shared<TestPresentation>();
    std::shared_ptr<apc::device::DeviceService> Service{Devices, &Devices->Service};
    apc::app::AppController Controller{Settings, Service, Presentation};

    ~AppFixture() {
        Controller.Shutdown();
        (void)Settings->Shutdown(SettingsShutdownMode::DiscardStartupFailure);
    }
};

} // namespace apc::tests
