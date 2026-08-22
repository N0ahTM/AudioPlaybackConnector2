#include <app/AppController.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using apc::app::AppCommand;
using apc::app::AppCommandContext;
using apc::app::AppCommandKind;
using apc::app::AppController;
using apc::app::AppDispatchPhase;
using apc::app::AppEvent;
using apc::app::AppResult;
using apc::app::AppResultCode;
using apc::app::AppSnapshot;
using apc::app::DeviceConnectedEvent;
using apc::app::DeviceSelector;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

AppCommand ConnectCommand() {
    auto target = DeviceSelector::ById(L"device-a");
    if (!target) throw std::logic_error("test command target must be valid");
    return {AppCommandKind::Connect, std::move(*target), {}};
}

void TestUiAndCliEquivalentCommandsUseOneExecutor() {
    std::vector<AppCommand> executed;
    AppSnapshot snapshot;
    auto const command = ConnectCommand();
    AppController controller(
        [&](AppCommand const& received, AppCommandContext const&) {
            executed.push_back(received);
            return AppResult{AppResultCode::Success, received.Kind};
        },
        [&]() { return snapshot; });

    auto const uiResult = controller.Execute(command);
    auto const cliResult = controller.Execute(command);

    Check(uiResult == cliResult, "UI and CLI adapters must receive equivalent normalized results");
    Check(uiResult.Code == AppResultCode::Success && uiResult.Command == AppCommandKind::Connect,
          "a valid command must delegate and retain its command kind");
    Check(uiResult.DispatchPhase == AppDispatchPhase::Started && cliResult.DispatchPhase == AppDispatchPhase::Started,
          "delegated results must record that the application executor was entered");
    Check(executed.size() == 2 && executed[0] == command && executed[1] == command,
          "equivalent UI and CLI intents must use the same typed executor");
}

void TestMalformedCancelledAndExpiredCommandsShortCircuit() {
    std::size_t calls = 0;
    AppController controller(
        [&](AppCommand const&, AppCommandContext const&) {
            ++calls;
            return AppResult{AppResultCode::Success, AppCommandKind::Status};
        },
        [] { return AppSnapshot{}; });

    auto malformed = ConnectCommand();
    malformed.Target.reset();
    auto const malformedResult = controller.Execute(malformed);

    std::stop_source stopSource;
    stopSource.request_stop();
    auto cancelledContext = AppCommandContext{stopSource.get_token(), AppCommandContext::TimePoint::max()};
    auto const cancelledResult = controller.Execute(ConnectCommand(), cancelledContext);

    auto expiredContext = AppCommandContext{};
    expiredContext.Deadline = AppCommandContext::TimePoint::min();
    auto const expiredResult = controller.Execute(ConnectCommand(), expiredContext);

    Check(malformedResult.Code == AppResultCode::InvalidInput,
          "malformed commands must normalize to invalid input before delegation");
    Check(cancelledResult.Code == AppResultCode::Cancelled,
          "cancelled commands must normalize to cancellation before delegation");
    Check(expiredResult.Code == AppResultCode::TimedOut,
          "expired commands must normalize to timeout before delegation");
    Check(cancelledResult.DispatchPhase == AppDispatchPhase::NotStarted &&
              expiredResult.DispatchPhase == AppDispatchPhase::NotStarted,
          "pre-dispatch cancellation and deadline results must record that the executor was not entered");
    Check(calls == 0, "malformed, cancelled, and expired commands must not invoke the executor");
}

void TestExecutorExceptionsBecomeInternalErrors() {
    AppController controller(
        [](AppCommand const&, AppCommandContext const&) -> AppResult { throw std::runtime_error("backend failure"); },
        [] { return AppSnapshot{}; });

    auto const result = controller.Execute(ConnectCommand());

    Check(result.Code == AppResultCode::InternalError,
          "an executor exception must be contained as an internal error result");
    Check(result.Command == AppCommandKind::Connect, "an exception result must retain the command that was attempted");
    Check(result.DispatchPhase == AppDispatchPhase::Started,
          "an executor exception must still record that dispatch began");
}

void TestSnapshotIsReturnedByValue() {
    auto id = apc::core::DeviceId::TryCreate(L"device-a");
    Check(id.has_value(), "snapshot fixture must have a valid device ID");
    if (!id) return;

    AppSnapshot source;
    source.Generation = 8;
    source.Devices.push_back({*id, L"Headphones", {}, L"Headphones", {}, true, true, false});
    AppController controller([](AppCommand const& command,
                                AppCommandContext const&) { return AppResult{AppResultCode::Success, command.Kind}; },
                             [&]() { return source; });

    auto first = controller.Snapshot();
    first.Generation = 99;
    first.Devices.front().DisplayName = L"Changed copy";
    auto const second = controller.Snapshot();

    Check(source.Generation == 8 && source.Devices.front().DisplayName == L"Headphones",
          "mutating a snapshot result must not mutate the provider-owned snapshot");
    Check(second.Generation == 8 && second.Devices.front().DisplayName == L"Headphones",
          "each snapshot call must return an independent immutable-value copy");
}

void TestEventOrderingAndReentrantUnsubscribe() {
    AppController controller([](AppCommand const& command,
                                AppCommandContext const&) { return AppResult{AppResultCode::Success, command.Kind}; },
                             [] { return AppSnapshot{}; });
    auto id = apc::core::DeviceId::TryCreate(L"device-a");
    Check(id.has_value(), "event fixture must have a valid device ID");
    if (!id) return;

    std::vector<int> order;
    std::optional<AppController::Subscription> selfSubscription;
    selfSubscription.emplace(controller.Subscribe([&](AppEvent const&) {
        order.push_back(1);
        selfSubscription->Reset();
    }));
    auto throwingSubscription = controller.Subscribe([&](AppEvent const&) {
        order.push_back(2);
        throw std::runtime_error("observer failure");
    });
    auto remainingSubscription = controller.Subscribe([&](AppEvent const&) { order.push_back(3); });

    AppEvent event = DeviceConnectedEvent{*id};
    controller.Publish(event);
    controller.Publish(event);

    Check(order == std::vector<int>{1, 2, 3, 2, 3},
          "events must preserve registration order and safely continue after reentrant unsubscribe/throws");
    Check(selfSubscription && !*selfSubscription && throwingSubscription && remainingSubscription,
          "subscription tokens must expose active ownership until reset or scope exit");
}

} // namespace

int RunAppControllerTests() {
    TestUiAndCliEquivalentCommandsUseOneExecutor();
    TestMalformedCancelledAndExpiredCommandsShortCircuit();
    TestExecutorExceptionsBecomeInternalErrors();
    TestSnapshotIsReturnedByValue();
    TestEventOrderingAndReentrantUnsubscribe();
    return g_failures;
}
