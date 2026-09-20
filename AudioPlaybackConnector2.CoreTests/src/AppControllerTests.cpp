#include "TestCheck.hpp"
#include "AppTestFixture.hpp"

#include <app/AppController.hpp>

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

void TestStartupConnectionsUseSavedPolicyAndRecentOrder() {
    using Status = AppController::StartupConnectionStatus;
    {
        apc::tests::AppFixture fixture;
        Check(fixture.Controller.RestoreStartupConnections() == Status::NoTargets,
              "empty settings must not start connections");
        (void)fixture.Settings->RememberDevice(L"a", L"A");
        (void)fixture.Settings->RememberDevice(L"b", L"B");
        (void)fixture.Settings->RememberDevice(L"c", L"C");
        (void)fixture.Settings->RecordConnectedDevice(L"a", L"A");
        (void)fixture.Settings->RecordConnectedDevice(L"c", L"C");
        (void)fixture.Controller.SetGlobalConnectOnStartup(true);
        Check(fixture.Controller.RestoreStartupConnections() == Status::Submitted &&
                  fixture.Devices->ConnectionAccess->CreatedIds == std::vector<std::wstring>{L"c", L"a", L"b"},
              "global startup policy must submit every saved device once, with recent devices first");
    }
    {
        apc::tests::AppFixture fixture;
        (void)fixture.Settings->RememberDevice(L"disabled", L"Disabled");
        (void)fixture.Settings->RememberDevice(L"enabled", L"Enabled");
        Check(fixture.Controller.RestoreStartupConnections() == Status::NoTargets,
              "saved devices with disabled startup policies must produce no work");
        (void)fixture.Controller.SetDeviceConnectOnStartup(L"enabled", true);
        Check(fixture.Controller.RestoreStartupConnections() == Status::Submitted &&
                  fixture.Devices->ConnectionAccess->CreatedIds == std::vector<std::wstring>{L"enabled"},
              "per-device startup policy must work without connection history");
        fixture.Controller.RequestStop();
        auto const before = fixture.Devices->ConnectionAccess->CreatedIds;
        Check(fixture.Controller.RestoreStartupConnections() == Status::Unavailable &&
                  fixture.Devices->ConnectionAccess->CreatedIds == before,
              "a stopped controller must not submit startup connections");
    }
}

void TestConnectionFactsRecordPreferencesBeforeNotification() {
    apc::tests::AppFixture fixture;
    (void)fixture.Controller.SetGlobalConnectOnStartup(true);
    (void)fixture.Service->Start();
    fixture.Devices->WatcherAccess->LastWatcher->Add(L"device-a", L"Speaker");
    bool persistedBeforeNotification = false;
    auto observation = fixture.Controller.SnapshotAndSubscribe([&](auto const& event) {
        if (!std::holds_alternative<DeviceConnectedEvent>(event.Event)) return;
        auto const saved = fixture.Settings->Snapshot();
        persistedBeforeNotification = saved.Data.LastConnectedIds == std::vector<std::wstring>{L"device-a"} &&
                                      saved.Data.Devices.size() == 1 && saved.Data.Devices.front().Name == L"Speaker";
    });
    apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"device-a");
    auto const connected = fixture.Controller.Snapshot();
    Check(persistedBeforeNotification && connected.Settings.Devices.front().ConnectOnStartup,
          "a real connection must record its name, defaults and history before notifying application observers");
    auto const revision = fixture.Settings->Snapshot().Revision;
    (void)fixture.Controller.SetDeviceReconnectOnConnectionLoss(L"device-a", true);
    Check(fixture.Settings->Snapshot().Revision == revision + 1 &&
              fixture.Settings->Snapshot().Data.LastConnectedIds == std::vector<std::wstring>{L"device-a"},
          "a policy-induced repeated connected fact must not create another settings commit");
}

void TestShutdownDrainsDeviceFactSettingsCommit() {
    apc::tests::AppFixture fixture;
    (void)fixture.Service->Connect(L"device-a");
    auto* connection = fixture.Devices->ConnectionAccess->LastConnection;
    connection->CompleteStart(apc::device::DeviceConnectionResult::Success);
    std::binary_semaphore entered(0), release(0), joining(0);
    auto blocker = fixture.Settings->Subscribe([&](auto const& snapshot) {
        if (snapshot.Data.LastConnectedIds.empty()) return;
        entered.release();
        release.acquire();
    });
    auto completion =
        std::async(std::launch::async, [&] { connection->CompleteOpen(apc::device::DeviceConnectionResult::Success); });
    entered.acquire();
    fixture.Controller.RequestStop();
    auto shutdown = std::async(std::launch::async, [&] {
        joining.release();
        fixture.Controller.Shutdown();
    });
    joining.acquire();
    auto const waited = shutdown.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    release.release();
    completion.get();
    shutdown.get();
    Check(waited && fixture.Settings->Snapshot().Data.LastConnectedIds == std::vector<std::wstring>{L"device-a"},
          "shutdown must drain an admitted device-fact commit even when no controller command is active");
}

void TestConnectionObserverCanRequestStopAndLateFactsCannotPersist() {
    apc::tests::AppFixture fixture;
    auto observation = fixture.Controller.SnapshotAndSubscribe([&](auto const& event) {
        if (std::holds_alternative<DeviceConnectedEvent>(event.Event)) fixture.Controller.RequestStop();
    });
    apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"device-a");
    fixture.Controller.Shutdown();
    auto const stopped = fixture.Settings->Snapshot();
    apc::tests::device::ConnectSuccessfully(*fixture.Devices, L"late-device");
    Check(!fixture.Controller.Snapshot().IsRunning &&
              stopped.Data.LastConnectedIds == std::vector<std::wstring>{L"device-a"} &&
              fixture.Settings->Snapshot() == stopped,
          "callback stop must not wait on itself, and later native connections must not mutate settings");
}

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
    (void)controller->ShowDevicePicker(DevicePickerOpenMode::ToggleIfOpen, AppCommandContext::Detached());
    Check(controller->ShowDevicePicker(DevicePickerOpenMode::EnsureOpen).Succeeded() &&
              controller->ShowSettings().Succeeded(),
          "explicit UI actions must use the presentation boundary");
    Check(fixture.Presentation->Modes == std::vector<DevicePickerOpenMode>{DevicePickerOpenMode::ToggleIfOpen,
                                                                           DevicePickerOpenMode::EnsureOpen} &&
              fixture.Presentation->Contexts.front().Completion == AppCommandContext::CompletionMode::Detached,
          "tray toggling and control ensure-open must preserve their distinct intent and completion contracts");
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

void TestStopRequestInsideAdmittedMutation() {
    apc::tests::AppFixture fixture;
    int delivered = 0;
    auto first = fixture.Controller.Subscribe([&](auto const&) {
        ++delivered;
        fixture.Controller.RequestStop();
        fixture.Controller.RequestStop();
    });
    auto remaining = fixture.Controller.Subscribe([&](auto const&) { ++delivered; });
    auto const mutation = fixture.Controller.SetPrivacyMode(true);
    Check(mutation.IsApplied() && fixture.Settings->Snapshot().Data.PrivacyModeEnabled && delivered == 1,
          "a stop request in a committed mutation's observer must return and skip remaining recipients");
    Check(fixture.Controller.SetPrivacyMode(false).Status == SettingsMutationStatus::Rejected &&
              !fixture.Controller.Snapshot().IsRunning,
          "a callback stop request must reject subsequent commands and snapshot admission");
    fixture.Controller.Shutdown();
}

void TestStopRequestDoesNotWaitForForeignCallback() {
    apc::tests::AppFixture fixture;
    std::binary_semaphore entered(0), release(0);
    int calls = 0;
    auto first = fixture.Controller.Subscribe([&](auto const&) {
        ++calls;
        entered.release();
        release.acquire();
    });
    auto remaining = fixture.Controller.Subscribe([&](auto const&) { ++calls; });
    auto mutation = std::async(std::launch::async, [&] { return fixture.Controller.SetPrivacyMode(true); });
    entered.acquire();
    fixture.Controller.RequestStop();
    auto joined = std::async(std::launch::async, [&] { fixture.Controller.Shutdown(); });
    Check(joined.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
          "the lifecycle join must still drain a callback after the nonwaiting stop request");
    release.release();
    Check(mutation.get().IsApplied(), "the admitted commit must retain its result during stop");
    joined.get();
    Check(calls == 1, "stopping during delivery must invalidate remaining recipients");
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

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Controller Device Events //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

namespace {
namespace device_events {
using namespace apc::tests::device;

void TestControllerOwnsDeviceFactNormalization() {
    auto fixture = std::make_shared<Fixture>();
    std::vector<apc::app::AppController::EventNotification> events;
    auto settings = apc::tests::MakeTestSettings();
    (void)settings->SetGlobalReconnectOnConnectionLoss(true);
    apc::app::AppController controller(settings, std::shared_ptr<DeviceService>(fixture, &fixture->Service));
    auto subscription = controller.Subscribe([&](auto const& event) { events.push_back(event); });
    const auto longId = std::wstring(513, L'd');
    ConnectSuccessfully(*fixture, longId);
    auto const connected = std::ranges::find_if(
        events, [](auto const& event) { return std::holds_alternative<apc::app::DeviceConnectedEvent>(event.Event); });
    Check(connected != events.end(), "the controller must normalize the concrete owner's connected fact");
    if (connected == events.end()) return;
    auto const retainedConnected = *connected;
    Check(std::get<apc::app::DeviceConnectedEvent>(retainedConnected.Event).Id.View() == longId &&
              controller.IsCurrent(retainedConnected),
          "controller events must preserve opaque external identities and current delivery tokens");
    (void)fixture->Service.Disconnect(longId);
    Check(!controller.IsCurrent(retainedConnected),
          "a queued connected UI event must become stale when the session disconnects");
    auto const manual = std::ranges::find_if(events, [](auto const& event) {
        return std::holds_alternative<apc::app::DeviceDisconnectedEvent>(event.Event);
    });
    Check(manual != events.end() && !std::get<apc::app::DeviceDisconnectedEvent>(manual->Event).NotifyUser,
          "manual disconnect events must not request a loss notification");
    CompleteCloseAndCooldown(*fixture, fixture->ConnectionAccess->LastConnection);
    ConnectSuccessfully(*fixture, longId);
    fixture->ConnectionAccess->LastConnection->Signal(DeviceConnectionState::Closed);
    Check(std::ranges::any_of(events,
                              [](auto const& event) {
                                  auto const* disconnected =
                                      std::get_if<apc::app::DeviceDisconnectedEvent>(&event.Event);
                                  return disconnected && disconnected->NotifyUser;
                              }),
          "unexpected loss must carry the notification policy in the typed application event");
    Check(std::ranges::any_of(events,
                              [](auto const& event) {
                                  return std::holds_alternative<apc::app::AutoReconnectTriggeredEvent>(event.Event);
                              }),
          "entering reconnect wait must produce the typed reconnect event");
    for (std::size_t i = 1; i < events.size(); ++i)
        Check(events[i - 1].Revision < events[i].Revision,
              "device publications must share the controller's total revision order");
}

void TestControllerStatusTokensFollowTheDeviceOwner() {
    auto fixture = std::make_shared<Fixture>();
    std::vector<apc::app::AppController::EventNotification> events;
    apc::app::AppController controller(apc::tests::MakeTestSettings(),
                                       std::shared_ptr<DeviceService>(fixture, &fixture->Service));
    auto subscription = controller.Subscribe([&](auto const& event) { events.push_back(event); });
    auto lastStatus = [&]() -> std::optional<apc::app::AppController::EventNotification> {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (std::holds_alternative<apc::app::DeviceStatusChangedEvent>(it->Event)) return *it;
        return std::nullopt;
    };
    (void)fixture->Service.Start();
    fixture->WatcherAccess->LastWatcher->Add(L"status-token", L"Status token");
    Check(std::ranges::any_of(events,
                              [](auto const& event) {
                                  return std::holds_alternative<apc::app::DeviceInventoryChangedEvent>(event.Event);
                              }) &&
              std::ranges::any_of(events,
                                  [](auto const& event) {
                                      return std::holds_alternative<apc::app::DeviceActivityChangedEvent>(event.Event);
                                  }),
          "inventory and activity must use the same typed controller subscription");

    (void)fixture->Service.Connect(L"status-token");
    auto const connecting = lastStatus();
    (void)controller.SetGlobalReconnectOnConnectionLoss(true);
    auto const duplicate = lastStatus();
    Check(connecting && duplicate && connecting->Revision < duplicate->Revision && controller.IsCurrent(*connecting) &&
              controller.IsCurrent(*duplicate),
          "equal source statuses must retain current tokens while preserving distinct publication revisions");
    auto* const connection = fixture->ConnectionAccess->LastConnection;
    connection->CompleteStart(DeviceConnectionResult::Denied);
    CompleteCloseAndCooldown(*fixture, connection);
    auto const failed = lastStatus();
    Check(connecting && duplicate && failed && !controller.IsCurrent(*connecting) &&
              !controller.IsCurrent(*duplicate) && controller.IsCurrent(*failed) &&
              std::get<apc::app::DeviceStatusChangedEvent>(failed->Event).State ==
                  apc::app::DeviceConnectionState::Failed,
          "terminal failure must invalidate queued connecting statuses before UI delivery");

    (void)fixture->Service.Connect(L"status-token");
    fixture->ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture->ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
    Check(failed && !controller.IsCurrent(*failed), "a replacement connection must invalidate a queued failure event");
    (void)controller.SetGlobalReconnectOnConnectionLoss(false);
    Check(std::ranges::count_if(events,
                                [](auto const& event) {
                                    return std::holds_alternative<apc::app::DeviceConnectedEvent>(event.Event);
                                }) == 1,
          "repeated connected status must not emit a second connected transition");
    Check(StateFor(fixture->Service, L"status-token") == DeviceLifecycleState::Connected,
          "retained old notifications must not change the authoritative session state");
}

void TestControllerDeviceFailureAndLifetime() {
    auto fixture = std::make_shared<Fixture>();
    std::vector<apc::app::AppController::EventNotification> events;
    auto controller = std::make_unique<apc::app::AppController>(
        apc::tests::MakeTestSettings(), std::shared_ptr<DeviceService>(fixture, &fixture->Service));
    auto subscription = controller->Subscribe([&](auto const& event) { events.push_back(event); });
    (void)fixture->Service.Connect(L"denied");
    fixture->ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Denied);
    CompleteCloseAndCooldown(*fixture, fixture->ConnectionAccess->LastConnection);
    Check(std::ranges::any_of(events,
                              [](auto const& event) {
                                  auto const* error = std::get_if<apc::app::DeviceConnectionErrorEvent>(&event.Event);
                                  return error &&
                                         error->FailureReason == apc::app::DeviceConnectionErrorEvent::Reason::Denied;
                              }),
          "the application must carry a typed failure reason without loading localized UI resources");
    controller.reset();
    auto const count = events.size();
    (void)fixture->Service.Connect(L"after-controller");
    fixture->ConnectionAccess->LastConnection->CompleteStart(DeviceConnectionResult::Success);
    fixture->ConnectionAccess->LastConnection->CompleteOpen(DeviceConnectionResult::Success);
    Check(events.size() == count,
          "controller destruction must detach the source and make retained callbacks ineffective");
    subscription.Reset();
}

} // namespace device_events
} // namespace

int RunAppControllerTests() {
    device_events::TestControllerOwnsDeviceFactNormalization();
    device_events::TestControllerStatusTokensFollowTheDeviceOwner();
    device_events::TestControllerDeviceFailureAndLifetime();

    TestStartupConnectionsUseSavedPolicyAndRecentOrder();
    TestConnectionFactsRecordPreferencesBeforeNotification();
    TestShutdownDrainsDeviceFactSettingsCommit();
    TestConnectionObserverCanRequestStopAndLateFactsCannotPersist();
    TestSnapshotRegistrationReconcilesChangesDuringCapture();
    TestSnapshotRegistrationDoesNotReceiveOlderQueuedDelivery();
    TestUnstableOrStoppedObservationCannotInstallAnObserver();
    TestInitialSnapshotContainsDiscoveredUnconnectedDevices();
    TestShutdownDrainsAnObservationBeingCaptured();
    TestConcreteOwnersAndExplicitUseCases();
    TestPreflightAndShutdownCloseAdmission();
    TestPresentationBoundaryRetainsPickerIntent();
    TestShutdownClosesEventAdmission();
    TestStopRequestInsideAdmittedMutation();
    TestStopRequestDoesNotWaitForForeignCallback();
    TestEventOrderingAndReentrantUnsubscribe();
    TestConcurrentAndReentrantPublicationsHaveOneOrder();
    TestResetDrainsAdmittedCallbackAndSkipsQueuedDelivery();
    TestSubscriptionsCaptureAdmissionAtPublication();
    TestReentrantControllerDestructionCancelsRemainingDelivery();
    TestResetDestroysHandlerCapturesOutsideTheOwnerLock();
    TestControllerDestructionDrainsForeignCallback();
    return g_failures;
}
