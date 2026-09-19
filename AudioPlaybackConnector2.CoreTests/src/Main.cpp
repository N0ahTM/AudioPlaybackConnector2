#include <util/RuntimeApartment.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <string_view>
#include <system_error>

int RunProtocolBoundaryTests();
int RunAdaptiveResourceDiagnosticsTests();
int RunAdaptiveResourcePolicyTests();
int RunAppControllerTests();
int RunAppModelsTests();
int RunControlCommandAdapterTests();
int RunAppWorkCoordinatorTests();
int RunAutoReconnectPlannerTests();
int RunCommandClientTests();
int RunCommandLineControlServerTests();
int RunControlUiActionGateTests();
int RunControlTargetMatcherTests();
int RunDeviceServiceTests();
int RunDeviceWatcherTests();
int RunDiagnosticsLogCollectorTests();
int RunEventTests();
int RunSettingsDiagnosticsReportBuilderTests();
int RunSettingsStoreTests();
int RunDevicePickerSnapshotTests();
int RunLatestStartupTaskRequestStateTests();
int RunLegacyAppUseCaseBridgeTests();
int RunReconnectPolicyTests();
int RunResourcePressureMonitorTests();
int RunRuntimeApartmentTests();
int RunSettingsLimitsTests();
int RunSingleInstanceGuardTests();
int RunStartupTaskCoordinatorTests();
int RunTrayTooltipBuilderTests();

namespace {
struct Suite {
    std::string_view Name;
    int (*Run)();
};
} // namespace

int main(int argc, char** argv) {
    std::uint32_t seed = 0;
    std::string_view selectedSuite;
    bool shuffle = false;
    bool list = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--list") {
            list = true;
        } else if (option == "--suite" && index + 1 < argc) {
            selectedSuite = argv[++index];
        } else if (option == "--seed" && index + 1 < argc) {
            const std::string_view value = argv[++index];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seed);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                std::cerr << "Seed must be an unsigned 32-bit integer.\n";
                return 2;
            }
            shuffle = true;
        } else {
            std::cerr << "Usage: CoreTests [--list] [--suite NAME] [--seed UINT32]\n";
            return 2;
        }
    }
    std::array suites{
        Suite{"ProtocolBoundary", RunProtocolBoundaryTests},
        Suite{"AdaptiveResourceDiagnostics", RunAdaptiveResourceDiagnosticsTests},
        Suite{"AdaptiveResourcePolicy", RunAdaptiveResourcePolicyTests},
        Suite{"AppController", RunAppControllerTests},
        Suite{"AppModels", RunAppModelsTests},
        Suite{"ControlCommandAdapter", RunControlCommandAdapterTests},
        Suite{"AppWorkCoordinator", RunAppWorkCoordinatorTests},
        Suite{"AutoReconnectPlanner", RunAutoReconnectPlannerTests},
        Suite{"CommandClient", RunCommandClientTests},
        Suite{"CommandLineControlServer", RunCommandLineControlServerTests},
        Suite{"ControlUiActionGate", RunControlUiActionGateTests},
        Suite{"ControlTargetMatcher", RunControlTargetMatcherTests},
        Suite{"DeviceService", RunDeviceServiceTests},
        Suite{"DeviceWatcher", RunDeviceWatcherTests},
        Suite{"DiagnosticsLogCollector", RunDiagnosticsLogCollectorTests},
        Suite{"Event", RunEventTests},
        Suite{"SettingsDiagnosticsReportBuilder", RunSettingsDiagnosticsReportBuilderTests},
        Suite{"SettingsStore", RunSettingsStoreTests},
        Suite{"DevicePickerSnapshot", RunDevicePickerSnapshotTests},
        Suite{"LatestStartupTaskRequestState", RunLatestStartupTaskRequestStateTests},
        Suite{"LegacyAppUseCaseBridge", RunLegacyAppUseCaseBridgeTests},
        Suite{"ReconnectPolicy", RunReconnectPolicyTests},
        Suite{"ResourcePressureMonitor", RunResourcePressureMonitorTests},
        Suite{"RuntimeApartment", RunRuntimeApartmentTests},
        Suite{"SettingsLimits", RunSettingsLimitsTests},
        Suite{"SingleInstanceGuard", RunSingleInstanceGuardTests},
        Suite{"StartupTaskCoordinator", RunStartupTaskCoordinatorTests},
        Suite{"TrayTooltipBuilder", RunTrayTooltipBuilderTests},
    };
    if (!selectedSuite.empty() && std::ranges::find(suites, selectedSuite, &Suite::Name) == suites.end()) {
        std::cerr << "Unknown suite: " << selectedSuite << '\n';
        return 2;
    }
    if (list) {
        for (auto const& suite : suites)
            std::cout << suite.Name << '\n';
        return 0;
    }
    // Several suites activate WinRT classes. Keep the process apartment alive across suite boundaries
    // so temporary per-operation apartments cannot unload factories still cached by C++/WinRT.
    util::RuntimeApartment apartment;
    if (!apartment.Ready()) {
        std::cerr << "Unable to initialize the test process Windows Runtime apartment.\n";
        return 1;
    }
    if (shuffle) {
        std::mt19937 random(seed);
        std::shuffle(suites.begin(), suites.end(), random);
    }
    std::cout << "CoreTests seed=" << seed << " shuffled=" << shuffle << std::endl;
    int failures = 0;
    for (auto const& suite : suites) {
        if (!selectedSuite.empty() && selectedSuite != suite.Name) continue;
        std::cout << "[RUN] " << suite.Name << std::endl;
        try {
            failures += suite.Run();
        } catch (std::exception const& error) {
            ++failures;
            std::cerr << suite.Name << ": uncaught exception: " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << suite.Name << ": uncaught platform exception\n";
        }
    }
    if (failures != 0) {
        std::cerr << failures << " core test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
