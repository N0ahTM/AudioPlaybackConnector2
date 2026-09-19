#include "TestCheck.hpp"
#include "AppTestFixture.hpp"

#include <app/AppController.hpp>
#include <ui/TrayPrimaryActivation.hpp>

#include <chrono>
#include <future>
#include <semaphore>
#include <thread>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using apc::app::AppCommandContext;
using apc::app::AppCommandKind;
using apc::app::AppController;
using apc::app::AppDispatchPhase;
using apc::app::AppEvent;
using apc::app::AppResult;
using apc::app::AppResultCode;
using apc::app::AppSnapshot;
using apc::app::DeviceConnectedEvent;
using apc::app::DevicePickerOpenMode;
using apc::app::DeviceSelector;

void TestConcreteOwnersAndExplicitUseCases() {
    apc::tests::AppFixture fixture;
    auto& controller = fixture.Controller;
    (void)fixture.Settings->RememberDevice(L"device-a", L"Headphones");
    auto const target = *DeviceSelector::ById(L"device-a");
    auto const connected = controller.Connect(target, AppCommandContext::Detached());
    Check(connected.Succeeded() && fixture.Devices->ConnectionAccess->Connections.size() == 1,
          "connect must directly admit an operation on the concrete device owner");
    fixture.Devices->ConnectionAccess->LastConnection->CompleteStart(apc::device::DeviceConnectionResult::Success);
    fixture.Devices->ConnectionAccess->LastConnection->CompleteOpen(apc::device::DeviceConnectionResult::Success);
    Check(controller.SetAlias(L"device-a", L"Desk").Succeeded() && controller.SetDefault(L"device-a").Succeeded(),
          "device settings must commit through the concrete settings owner");
    auto snapshot = controller.Snapshot();
    Check(snapshot.Devices.size() == 1 && snapshot.Devices.front().IsConnected &&
              snapshot.Devices.front().DisplayName == L"Desk" && snapshot.DefaultDevice &&
              snapshot.DefaultDevice->IsConnected,
          "application snapshots must merge authoritative sessions and committed settings");
    snapshot.Devices.front().DisplayName = L"Copy";
    Check(controller.Snapshot().Devices.front().DisplayName == L"Desk", "snapshots must own independent value data");
    Check(controller.ClearAlias(L"device-a").Succeeded() && controller.ClearDefault().Succeeded(),
          "explicit clear actions must use the same persistence owner");
    Check(controller.Disconnect(target, {}).Succeeded() && !controller.Snapshot().Devices.front().IsConnected,
          "disconnect results must reflect the device owner's close transition");
    apc::tests::device::CompleteCloseAndCooldown(*fixture.Devices, fixture.Devices->ConnectionAccess->LastConnection);
}

void TestPreflightAndShutdownCloseAdmission() {
    apc::tests::AppFixture fixture;
    auto& controller = fixture.Controller;
    std::stop_source stop;
    stop.request_stop();
    AppCommandContext cancelled{stop.get_token(), AppCommandContext::TimePoint::max()};
    AppCommandContext expired;
    expired.Deadline = AppCommandContext::TimePoint::min();
    auto target = *DeviceSelector::ById(L"device-a");
    Check(controller.SetDefault(std::wstring_view{}).Code == AppResultCode::InvalidInput &&
              controller.Connect(target, cancelled).Code == AppResultCode::Cancelled &&
              controller.Connect(target, expired).Code == AppResultCode::TimedOut &&
              fixture.Devices->ConnectionAccess->Connections.empty(),
          "invalid, cancelled and expired actions must not enter device mutation");
    Check(controller.SetAlias(L"device-a", L"bad\nalias").Code == AppResultCode::InvalidInput,
          "invalid aliases must fail before settings mutation");
    controller.Shutdown();
    Check(controller.Connect(target, {}).Code == AppResultCode::Unavailable &&
              controller.ShowSettings().Code == AppResultCode::Unavailable &&
              fixture.Presentation->SettingsCalls == 0 && !controller.Snapshot().IsRunning,
          "shutdown must close command and UI admission and publish an unavailable snapshot");
}

void TestPresentationBoundaryRetainsPickerIntent() {
    apc::tests::AppFixture fixture;
    auto controller = std::make_shared<AppController>(fixture.Settings, fixture.Service, fixture.Presentation);
    auto tray = apc::ui::MakeTrayPrimaryActivationCallback(controller);
    tray();
    Check(controller->ShowDevicePicker(DevicePickerOpenMode::EnsureOpen).Succeeded() &&
              controller->ShowSettings().Succeeded(),
          "explicit UI actions must use the presentation boundary");
    Check(fixture.Presentation->Modes == std::vector<DevicePickerOpenMode>{DevicePickerOpenMode::ToggleIfOpen,
                                                                           DevicePickerOpenMode::EnsureOpen} &&
              fixture.Presentation->Contexts.front().Completion == AppCommandContext::CompletionMode::Detached,
          "tray toggling and control ensure-open must preserve their distinct intent and completion contracts");
    controller.reset();
    auto const before = fixture.Presentation->Modes.size();
    tray();
    Check(fixture.Presentation->Modes.size() == before, "tray callback must not retain its controller");
}

void TestShutdownClosesEventAdmission() {
    apc::tests::AppFixture fixture;
    int delivered = 0;
    auto subscription = fixture.Controller.Subscribe([&](auto const&) { ++delivered; });
    auto const target = *DeviceSelector::ById(L"device-a");
    (void)fixture.Controller.Connect(target, AppCommandContext::Detached());
    Check(delivered != 0, "admitted device commands must publish their transition");
    fixture.Controller.Shutdown();
    auto const before = delivered;
    auto* connection = fixture.Devices->ConnectionAccess->LastConnection;
    connection->CompleteStart(apc::device::DeviceConnectionResult::Success);
    connection->CompleteOpen(apc::device::DeviceConnectionResult::Success);
    auto lateSubscription = fixture.Controller.Subscribe([&](auto const&) { ++delivered; });
    Check(delivered == before && !lateSubscription && !fixture.Controller.Snapshot().IsRunning,
          "shutdown must reject later device delivery and new observers without reviving the snapshot");
}

void TestEventOrderingAndReentrantUnsubscribe() {
    apc::tests::AppFixture fixture;
    auto& controller = fixture.Controller;
    auto id = apc::core::DeviceId::TryCreate(L"device-a");
    Check(id.has_value(), "event fixture must have a valid device ID");
    if (!id) return;

    std::vector<int> order;
    std::optional<AppController::Subscription> selfSubscription;
    selfSubscription.emplace(controller.Subscribe([&](AppController::EventNotification const&) {
        order.push_back(1);
        selfSubscription->Reset();
    }));
    auto throwingSubscription = controller.Subscribe([&](AppController::EventNotification const&) {
        order.push_back(2);
        throw std::runtime_error("observer failure");
    });
    auto remainingSubscription =
        controller.Subscribe([&](AppController::EventNotification const&) { order.push_back(3); });

    AppEvent event = DeviceConnectedEvent{*id};
    controller.Publish(event);
    controller.Publish(event);

    Check(order == std::vector<int>{1, 2, 3, 2, 3},
          "events must preserve registration order and safely continue after reentrant unsubscribe/throws");
    Check(selfSubscription && !*selfSubscription && throwingSubscription && remainingSubscription,
          "subscription tokens must expose active ownership until reset or scope exit");
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Subscription Concurrency //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void TestConcurrentAndReentrantPublicationsHaveOneOrder() {
    apc::tests::AppFixture fixture;
    auto& controller = fixture.Controller;
    AppEvent const event = apc::app::DeviceActivityChangedEvent{};
    std::binary_semaphore entered(0);
    std::binary_semaphore release(0);
    std::vector<std::pair<int, std::uint64_t>> order;
    auto first = controller.Subscribe([&](AppController::EventNotification const& notification) {
        order.emplace_back(1, notification.Revision);
        Check(std::holds_alternative<apc::app::DeviceActivityChangedEvent>(notification.Event),
              "ordered notification must retain the original event payload");
        if (notification.Revision == 1) {
            entered.release();
            release.acquire();
            controller.Publish(event);
        }
    });
    auto second = controller.Subscribe(
        [&](AppController::EventNotification const& notification) { order.emplace_back(2, notification.Revision); });
    std::jthread publisher([&] { controller.Publish(event); });
    entered.acquire();
    controller.Publish(event);
    release.release();
    publisher.join();
    Check(order == std::vector<std::pair<int, std::uint64_t>>{{1, 1}, {2, 1}, {1, 2}, {2, 2}, {1, 3}, {2, 3}},
          "concurrent and reentrant publications must preserve one revision and recipient order without overlap");
}

void TestResetDrainsAdmittedCallbackAndSkipsQueuedDelivery() {
    apc::tests::AppFixture fixture;
    auto& controller = fixture.Controller;
    AppEvent const event = apc::app::DeviceActivityChangedEvent{};
    std::binary_semaphore entered(0);
    std::binary_semaphore release(0);
    std::binary_semaphore resetting(0);
    int calls = 0;
    auto subscription = controller.Subscribe([&](AppController::EventNotification const&) {
        if (++calls == 1) {
            entered.release();
            release.acquire();
        }
    });
    std::jthread publisher([&] { controller.Publish(event); });
    entered.acquire();
    controller.Publish(event);
    auto reset = std::async(std::launch::async, [&] {
        resetting.release();
        subscription.Reset();
    });
    resetting.acquire();
    auto const drained = reset.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    release.release();
    reset.get();
    publisher.join();
    controller.Publish(event);
    Check(drained && calls == 1 && !subscription,
          "reset must drain an admitted callback and prevent both queued and later callbacks");
}

void TestSubscriptionsCaptureAdmissionAtPublication() {
    apc::tests::AppFixture fixture;
    auto& controller = fixture.Controller;
    AppEvent const event = apc::app::DeviceActivityChangedEvent{};
    std::optional<AppController::Subscription> late;
    std::vector<std::uint64_t> received;
    auto original = controller.Subscribe([&](AppController::EventNotification const& notification) {
        if (notification.Revision != 1) return;
        controller.Publish(event);
        late.emplace(controller.Subscribe(
            [&](AppController::EventNotification const& next) { received.push_back(next.Revision); }));
        controller.Publish(event);
    });
    controller.Publish(event);
    Check(received == std::vector<std::uint64_t>{3},
          "a new subscription must receive publications admitted after registration, not older queued events");
}

void TestReentrantControllerDestructionCancelsRemainingDelivery() {
    apc::tests::AppFixture fixture;
    auto controller = std::make_unique<AppController>(fixture.Settings, fixture.Service);
    AppEvent const event = apc::app::DeviceActivityChangedEvent{};
    int calls = 0;
    auto first = controller->Subscribe([&](AppController::EventNotification const&) {
        ++calls;
        controller->Publish(event);
        controller.reset();
    });
    auto second = controller->Subscribe([&](AppController::EventNotification const&) { ++calls; });
    controller->Publish(event);
    first.Reset();
    second.Reset();
    Check(!controller && calls == 1,
          "destruction inside a callback must invalidate queued and remaining recipients without self-deadlock");
}

void TestControllerDestructionDrainsForeignCallback() {
    apc::tests::AppFixture fixture;
    auto controller = std::make_unique<AppController>(fixture.Settings, fixture.Service);
    AppEvent const event = apc::app::DeviceActivityChangedEvent{};
    std::binary_semaphore entered(0);
    std::binary_semaphore release(0);
    std::binary_semaphore closing(0);
    int calls = 0;
    auto subscription = controller->Subscribe([&](AppController::EventNotification const&) {
        if (++calls == 1) {
            entered.release();
            release.acquire();
        }
    });
    auto* const publisherFacade = controller.get();
    std::jthread publisher([&] { publisherFacade->Publish(event); });
    entered.acquire();
    controller->Publish(event);
    auto closed = std::async(std::launch::async, [&] {
        closing.release();
        controller.reset();
    });
    closing.acquire();
    auto const drained = closed.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    release.release();
    closed.get();
    publisher.join();
    subscription.Reset();
    Check(drained && calls == 1,
          "controller destruction must drain an admitted foreign callback and discard pending delivery");
}

void TestResetDestroysHandlerCapturesOutsideTheOwnerLock() {
    apc::tests::AppFixture fixture;
    auto& controller = fixture.Controller;
    struct Capture {
        AppController& Controller;
        bool& Released;
        ~Capture() {
            auto subscription = Controller.Subscribe([](AppController::EventNotification const&) {});
            Released = static_cast<bool>(subscription);
        }
    };
    bool released = false;
    auto capture = std::make_shared<Capture>(controller, released);
    auto subscription = controller.Subscribe([capture](AppController::EventNotification const&) {});
    capture.reset();
    subscription.Reset();
    Check(released, "handler capture destruction must be able to reenter registration and reset without an owner lock");
}

void TestSnapshotRegistrationReconcilesChangesDuringCapture() {
    apc::tests::AppFixture fixture;
    (void)fixture.Settings->RememberDevice(L"device-a", L"Headphones");
    bool changeDuringCapture = true;
    fixture.Presentation->BeforeResourceRead = [&] {
        if (!std::exchange(changeDuringCapture, false)) return;
        (void)fixture.Settings->SetDeviceAlias(L"device-a", L"New alias");
        apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"device-a");
    };
    std::vector<AppController::EventNotification> updates;
    auto observation = fixture.Controller.SnapshotAndSubscribe([&](auto const& event) { updates.push_back(event); });
    Check(observation.Updates && observation.Snapshot.IsRunning && observation.Snapshot.Devices.size() == 1 &&
              observation.Snapshot.Devices.front().DisplayName == L"New alias" &&
              observation.Snapshot.Devices.front().IsConnected && observation.Revision != 0 && updates.empty(),
          "settings and device changes during capture must be reconciled into the initial value before registration");
    (void)fixture.Settings->SetDeviceAlias(L"device-a", L"Later alias");
    Check(updates.size() == 1 && updates.front().Revision > observation.Revision &&
              std::get<apc::app::SettingsChangedEvent>(updates.front().Event).SettingsRevision ==
                  fixture.Settings->Snapshot().Revision,
          "a later settings commit must cross the same ordered subscription with its owner revision");
    (void)fixture.Service->Disconnect(L"device-a");
    Check(updates.size() > 1 && updates.back().Revision > updates.front().Revision,
          "device changes after registration must follow the initial event watermark");
}

void TestSnapshotRegistrationDoesNotReceiveOlderQueuedDelivery() {
    apc::tests::AppFixture fixture;
    std::binary_semaphore entered(0), release(0);
    auto blocker = fixture.Controller.Subscribe([&](auto const& event) {
        if (event.Revision != 1) return;
        entered.release();
        release.acquire();
    });
    std::jthread publisher([&] { fixture.Controller.Publish(apc::app::DeviceActivityChangedEvent{}); });
    entered.acquire();
    std::vector<std::uint64_t> revisions;
    auto observation =
        fixture.Controller.SnapshotAndSubscribe([&](auto const& event) { revisions.push_back(event.Revision); });
    Check(observation.Updates && observation.Revision == 1 && revisions.empty(),
          "registration must not wait for an older observer or join an already admitted delivery");
    fixture.Controller.Publish(apc::app::DeviceActivityChangedEvent{});
    release.release();
    publisher.join();
    Check(revisions == std::vector<std::uint64_t>{2},
          "only publications following the captured watermark may target the new observer");
}

void TestUnstableOrStoppedObservationCannotInstallAnObserver() {
    apc::tests::AppFixture fixture;
    int delivered = 0;
    fixture.Presentation->BeforeResourceRead = [&] {
        fixture.Controller.Publish(apc::app::DeviceActivityChangedEvent{});
    };
    auto unstable = fixture.Controller.SnapshotAndSubscribe([&](auto const&) { ++delivered; });
    Check(!unstable.Updates && !unstable.Snapshot.IsRunning && delivered == 0,
          "an exhausted capture retry must return no partial subscription or apparently usable snapshot");
    fixture.Presentation->BeforeResourceRead = {};
    auto stable = fixture.Controller.SnapshotAndSubscribe([&](auto const&) { ++delivered; });
    Check(stable.Updates && stable.Snapshot.IsRunning, "a later stable capture must be able to register normally");
    fixture.Controller.Shutdown();
    auto stopped = fixture.Controller.SnapshotAndSubscribe([&](auto const&) { ++delivered; });
    (void)fixture.Settings->RememberDevice(L"late", L"Late");
    Check(!stopped.Updates && !stopped.Snapshot.IsRunning && delivered == 0,
          "shutdown must reject observation and invalidate subsequent settings delivery");
}

void TestInitialSnapshotContainsDiscoveredUnconnectedDevices() {
    apc::tests::AppFixture fixture;
    (void)fixture.Service->Start();
    fixture.Devices->WatcherAccess->LastWatcher->Add(L"discovered", L"Discovered");
    auto observation = fixture.Controller.SnapshotAndSubscribe([](auto const&) {});
    Check(observation.Updates && observation.Snapshot.Devices.size() == 1 &&
              observation.Snapshot.Devices.front().Id.View() == L"discovered" &&
              !observation.Snapshot.Devices.front().IsConnected,
          "the initial device projection must include inventory even without settings or a live session");
}

void TestShutdownDrainsAnObservationBeingCaptured() {
    apc::tests::AppFixture fixture;
    std::binary_semaphore entered(0), release(0);
    fixture.Presentation->BeforeResourceRead = [&] {
        entered.release();
        release.acquire();
    };
    auto capture =
        std::async(std::launch::async, [&] { return fixture.Controller.SnapshotAndSubscribe([](auto const&) {}); });
    entered.acquire();
    auto shutdown = std::async(std::launch::async, [&] { fixture.Controller.Shutdown(); });
    std::stop_source stop;
    stop.request_stop();
    AppCommandContext cancelled{stop.get_token(), AppCommandContext::TimePoint::max()};
    while (fixture.Controller.ShowSettings(cancelled).Code != AppResultCode::Unavailable)
        std::this_thread::yield();
    Check(shutdown.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
          "shutdown must wait for an admitted observation that is still reading presentation");
    release.release();
    shutdown.get();
    auto observation = capture.get();
    Check(!observation.Updates && !observation.Snapshot.IsRunning,
          "a snapshot capture overtaken by shutdown must not install a late observer");
}

} // namespace

int RunAppControllerTests() {
    TestSnapshotRegistrationReconcilesChangesDuringCapture();
    TestSnapshotRegistrationDoesNotReceiveOlderQueuedDelivery();
    TestUnstableOrStoppedObservationCannotInstallAnObserver();
    TestInitialSnapshotContainsDiscoveredUnconnectedDevices();
    TestShutdownDrainsAnObservationBeingCaptured();
    TestConcreteOwnersAndExplicitUseCases();
    TestPreflightAndShutdownCloseAdmission();
    TestPresentationBoundaryRetainsPickerIntent();
    TestShutdownClosesEventAdmission();
    TestEventOrderingAndReentrantUnsubscribe();
    TestConcurrentAndReentrantPublicationsHaveOneOrder();
    TestResetDrainsAdmittedCallbackAndSkipsQueuedDelivery();
    TestSubscriptionsCaptureAdmissionAtPublication();
    TestReentrantControllerDestructionCancelsRemainingDelivery();
    TestResetDestroysHandlerCapturesOutsideTheOwnerLock();
    TestControllerDestructionDrainsForeignCallback();
    return g_failures;
}
