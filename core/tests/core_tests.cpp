#include "industrial_bus/core/bounded_priority_queue.h"
#include "industrial_bus/core/idempotency_journal.h"
#include "industrial_bus/ids.h"
#include "industrial_bus/operation_state.h"
#include "industrial_bus/qos.h"
#include "industrial_bus/result.h"
#include "industrial_bus/topic.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using industrial_bus::BoundedPriorityQueue;
using industrial_bus::EnqueueDisposition;
using industrial_bus::MessageClass;
using industrial_bus::MessageType;
using industrial_bus::OperationState;
using industrial_bus::OperationStateMachine;
using industrial_bus::QosPriority;
using industrial_bus::QueueMetadata;
namespace bus_core = industrial_bus::core;

class JournalTempDirectory {
public:
    JournalTempDirectory() {
        char directory_template[] = "/tmp/industrial-bus-journal-XXXXXX";
        const char* directory = ::mkdtemp(directory_template);
        if (directory == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        directory_ = directory;
        journal_path_ = directory_ + "/commands.journal";
    }

    ~JournalTempDirectory() {
        ::unlink(journal_path_.c_str());
        ::rmdir(directory_.c_str());
    }

    const std::string& journalPath() const noexcept { return journal_path_; }

private:
    std::string directory_;
    std::string journal_path_;
};

QueueMetadata metadata(MessageClass message_class, std::string key = {}) {
    return QueueMetadata{industrial_bus::defaultPriority(message_class), message_class,
                         std::move(key)};
}

TEST(MessageSemanticsTest, MapsEveryClassToCanonicalTypeAndPriority) {
    struct ExpectedSemantics {
        MessageClass message_class;
        MessageType message_type;
        QosPriority priority;
    };
    constexpr std::array<ExpectedSemantics, 14> expected{{
        {MessageClass::Reply, MessageType::Reply, QosPriority::Critical},
        {MessageClass::Accepted, MessageType::Reply, QosPriority::Critical},
        {MessageClass::FinalState, MessageType::Event, QosPriority::Critical},
        {MessageClass::Error, MessageType::Error, QosPriority::Critical},
        {MessageClass::Command, MessageType::Command, QosPriority::High},
        {MessageClass::Request, MessageType::Request, QosPriority::High},
        {MessageClass::Heartbeat, MessageType::Heartbeat, QosPriority::High},
        {MessageClass::ServiceState, MessageType::System, QosPriority::High},
        {MessageClass::Event, MessageType::Event, QosPriority::Normal},
        {MessageClass::Progress, MessageType::Progress, QosPriority::Normal},
        {MessageClass::ErrorLog, MessageType::Log, QosPriority::Normal},
        {MessageClass::WarnLog, MessageType::Log, QosPriority::Normal},
        {MessageClass::InfoLog, MessageType::Log, QosPriority::BestEffort},
        {MessageClass::DebugLog, MessageType::Log, QosPriority::BestEffort},
    }};

    for (const auto& item : expected) {
        EXPECT_EQ(industrial_bus::messageTypeFor(item.message_class),
                  item.message_type);
        EXPECT_EQ(industrial_bus::defaultPriority(item.message_class), item.priority);
    }
}

TEST(MessageSemanticsTest, SeparatesProtocolErrorFromErrorLog) {
    EXPECT_EQ(industrial_bus::messageTypeFor(MessageClass::Error),
              MessageType::Error);
    EXPECT_EQ(industrial_bus::messageTypeFor(MessageClass::ErrorLog),
              MessageType::Log);
    EXPECT_NE(MessageClass::Error, MessageClass::ErrorLog);
}

TEST(TopicTest, TwoFieldEventTopicDefaultsToOrdinaryEvent) {
    constexpr industrial_bus::EventTopic<int> topic{"legacy.event", "test.Int"};
    EXPECT_EQ(topic.name, "legacy.event");
    EXPECT_EQ(topic.payload_type, "test.Int");
    EXPECT_EQ(topic.message_class, MessageClass::Event);
}

TEST(ResultTest, CarriesValuesAndErrors) {
    industrial_bus::Result<int> good(42);
    ASSERT_TRUE(good);
    EXPECT_EQ(good.value(), 42);

    industrial_bus::Result<int> bad(
        industrial_bus::Error::invalidArgument("bad argument", 7));
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().category,
              industrial_bus::ErrorCategory::InvalidArgument);
    EXPECT_EQ(bad.error().code, 7);
    EXPECT_THROW((void)bad.value(), std::logic_error);

    industrial_bus::Result<void> void_result;
    EXPECT_TRUE(void_result);
}

TEST(IdentifiersTest, GeneratesAndValidatesProtocolIdentifiers) {
    std::set<std::string> generated;
    for (int i = 0; i < 1000; ++i) {
        auto id = industrial_bus::generateUuidV4();
        EXPECT_TRUE(industrial_bus::isValidUuid(id));
        generated.insert(std::move(id));
    }
    EXPECT_EQ(generated.size(), 1000U);
    EXPECT_FALSE(industrial_bus::isValidUuid("not-a-uuid"));
    EXPECT_TRUE(
        industrial_bus::isValidProtocolId("layout.boolean:instance-1"));
    EXPECT_FALSE(industrial_bus::isValidProtocolId("has a space"));
    EXPECT_FALSE(industrial_bus::isValidProtocolId(""));
}

TEST(OperationStateMachineTest, EnforcesTransitionsAndCancellation) {
    OperationStateMachine operation;
    EXPECT_EQ(operation.state(), OperationState::Created);
    ASSERT_TRUE(operation.transitionTo(OperationState::Running));
    ASSERT_TRUE(operation.requestCancellation());
    EXPECT_EQ(operation.state(), OperationState::Cancelling);
    EXPECT_TRUE(operation.requestCancellation());
    ASSERT_TRUE(operation.transitionTo(OperationState::Completed));
    EXPECT_TRUE(industrial_bus::isTerminal(operation.state()));
    EXPECT_FALSE(operation.transitionTo(OperationState::Running));

    OperationStateMachine queued;
    ASSERT_TRUE(queued.requestCancellation());
    EXPECT_EQ(queued.state(), OperationState::Cancelled);
    EXPECT_TRUE(queued.requestCancellation());

    EXPECT_FALSE(industrial_bus::isValidTransition(OperationState::Created,
                                                   OperationState::Completed));
    EXPECT_TRUE(industrial_bus::isValidTransition(OperationState::Cancelling,
                                                  OperationState::Failed));
}

TEST(BoundedPriorityQueueTest, HonorsPriorityAndCapacity) {
    BoundedPriorityQueue<std::string> queue(3);
    EXPECT_EQ(queue.tryEnqueue("event", metadata(MessageClass::Event)),
              EnqueueDisposition::Enqueued);
    EXPECT_EQ(queue.tryEnqueue("request", metadata(MessageClass::Request)),
              EnqueueDisposition::Enqueued);
    EXPECT_EQ(queue.tryEnqueue("reply", metadata(MessageClass::Reply)),
              EnqueueDisposition::Enqueued);

    auto item = queue.tryDequeue();
    ASSERT_TRUE(item);
    EXPECT_EQ(*item, "reply");
    item = queue.tryDequeue();
    ASSERT_TRUE(item);
    EXPECT_EQ(*item, "request");
    item = queue.tryDequeue();
    ASSERT_TRUE(item);
    EXPECT_EQ(*item, "event");
    EXPECT_FALSE(queue.tryDequeue());
    const auto ordered_stats = queue.stats();
    EXPECT_EQ(ordered_stats.enqueued, 3U);
    EXPECT_EQ(ordered_stats.dequeued, 3U);
    EXPECT_EQ(ordered_stats.high_watermark, 3U);

    BoundedPriorityQueue<int> high_only(1);
    EXPECT_EQ(high_only.tryEnqueue(1, metadata(MessageClass::Command)),
              EnqueueDisposition::Enqueued);
    EXPECT_EQ(high_only.tryEnqueue(2, metadata(MessageClass::Request)),
              EnqueueDisposition::Rejected);
    const auto high_item = high_only.tryDequeue();
    ASSERT_TRUE(high_item);
    EXPECT_EQ(*high_item, 1);
    EXPECT_EQ(high_only.stats().rejected, 1U);
}

TEST(BoundedPriorityQueueTest, CoalescesProgressAndEvictsLowPriorityItems) {
    BoundedPriorityQueue<std::string> queue(2);
    EXPECT_EQ(queue.tryEnqueue(
                  "p10", metadata(MessageClass::Progress, "operation-a")),
              EnqueueDisposition::Enqueued);
    EXPECT_EQ(queue.tryEnqueue(
                  "p20", metadata(MessageClass::Progress, "operation-a")),
              EnqueueDisposition::Coalesced);
    EXPECT_EQ(queue.size(), 1U);
    auto item = queue.tryDequeue();
    ASSERT_TRUE(item);
    EXPECT_EQ(*item, "p20");

    EXPECT_EQ(queue.tryEnqueue("debug", metadata(MessageClass::DebugLog)),
              EnqueueDisposition::Enqueued);
    EXPECT_EQ(queue.tryEnqueue("info", metadata(MessageClass::InfoLog)),
              EnqueueDisposition::Enqueued);
    EXPECT_EQ(queue.tryEnqueue("command", metadata(MessageClass::Command)),
              EnqueueDisposition::Enqueued);
    EXPECT_EQ(queue.size(), 2U);
    item = queue.tryDequeue();
    ASSERT_TRUE(item);
    EXPECT_EQ(*item, "command");
    item = queue.tryDequeue();
    ASSERT_TRUE(item);
    EXPECT_EQ(*item, "info");

    const auto stats = queue.stats();
    EXPECT_EQ(stats.coalesced, 1U);
    EXPECT_EQ(stats.dropped, 1U);
    EXPECT_EQ(stats.high_watermark, 2U);
}

TEST(BoundedPriorityQueueTest, ReportsCriticalOverflow) {
    BoundedPriorityQueue<int> queue(1);
    EXPECT_EQ(queue.tryEnqueue(1, metadata(MessageClass::FinalState)),
              EnqueueDisposition::Enqueued);
    EXPECT_EQ(queue.tryEnqueue(2, metadata(MessageClass::Reply)),
              EnqueueDisposition::CriticalOverflow);
    EXPECT_EQ(queue.stats().critical_overflow, 1U);
    EXPECT_EQ(queue.stats().rejected, 1U);

    BoundedPriorityQueue<int> zero(0);
    EXPECT_EQ(zero.tryEnqueue(1, metadata(MessageClass::DebugLog)),
              EnqueueDisposition::Rejected);
    EXPECT_EQ(zero.stats().dropped, 1U);
    EXPECT_EQ(zero.stats().rejected, 1U);
}

TEST(IdempotencyJournalTest, PersistsDeduplicatesAndRecoversCommands) {
    JournalTempDirectory temp;
    bus_core::IdempotencyJournalOptions options;
    options.path = temp.journalPath();
    options.max_live_records = 2;

    auto opened = bus_core::IdempotencyJournal::open(options);
    ASSERT_TRUE(opened);
    auto journal = std::move(opened.value());

    bus_core::CommandRegistration first;
    first.identity = {"caller", "layout.boolean.execute", "intent-1"};
    first.payload = {'a', 'b', 'c'};
    first.operation_id = "operation-1";
    first.owner_service = "layout-boolean";
    first.owner_instance = "worker-1";
    auto inserted = journal->registerCommand(first);
    ASSERT_TRUE(inserted);
    EXPECT_FALSE(inserted.value().duplicate);
    EXPECT_EQ(inserted.value().record.payload_sha256,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    auto duplicate_command = first;
    duplicate_command.operation_id = "must-not-replace-operation";
    auto duplicate = journal->registerCommand(std::move(duplicate_command));
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate.value().duplicate);
    EXPECT_EQ(duplicate.value().record.operation_id, "operation-1");

    auto conflicting_command = first;
    conflicting_command.payload = {'x'};
    auto conflict = journal->registerCommand(std::move(conflicting_command));
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().category,
              industrial_bus::ErrorCategory::IdempotencyConflict);
    EXPECT_EQ(conflict.error().operation_id, "operation-1");

    auto running = journal->updateState(first.identity, "operation-1",
                                        OperationState::Running);
    ASSERT_TRUE(running);
    EXPECT_EQ(running.value().state, OperationState::Running);
    auto terminal_error = industrial_bus::Error::internal("algorithm failed", 73);
    terminal_error.operation_id = "operation-1";
    terminal_error.context["stage"] = "prepare";
    auto failed = journal->updateState(first.identity, "operation-1",
                                       OperationState::Failed, terminal_error);
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed.value().state, OperationState::Failed);
    EXPECT_EQ(failed.value().error, terminal_error);
    auto invalid_transition = journal->updateState(
        first.identity, "operation-1", OperationState::Running);
    ASSERT_FALSE(invalid_transition);
    EXPECT_EQ(invalid_transition.error().category,
              industrial_bus::ErrorCategory::InvalidArgument);
    auto wrong_owner = journal->updateState(
        first.identity, "another-operation", OperationState::Running);
    ASSERT_FALSE(wrong_owner);
    EXPECT_EQ(wrong_owner.error().category,
              industrial_bus::ErrorCategory::IdempotencyConflict);

    auto locked = bus_core::IdempotencyJournal::open(options);
    ASSERT_FALSE(locked);
    EXPECT_EQ(locked.error().category, industrial_bus::ErrorCategory::FileIo);

    journal.reset();
    auto recovered = bus_core::IdempotencyJournal::open(options);
    ASSERT_TRUE(recovered);
    journal = std::move(recovered.value());
    EXPECT_EQ(journal->liveRecordCount(), 1U);
    auto found = journal->find(first.identity);
    ASSERT_TRUE(found);
    EXPECT_EQ(found.value().operation_id, "operation-1");
    EXPECT_EQ(found.value().state, OperationState::Failed);
    EXPECT_EQ(found.value().error, terminal_error);

    bus_core::CommandRegistration second;
    second.identity = {"caller", "layout.boolean.execute", "intent-2"};
    second.payload = {'d'};
    second.operation_id = "operation-2";
    second.owner_service = "layout-boolean";
    second.owner_instance = "worker-1";
    ASSERT_TRUE(journal->registerCommand(second));

    std::atomic<int> execution_claims{0};
    std::atomic<int> claim_errors{0};
    std::vector<std::thread> claimers;
    for (int i = 0; i < 8; ++i) {
        claimers.emplace_back([&] {
            auto claimed = journal->claimForExecution(second.identity,
                                                      second.operation_id);
            if (!claimed) {
                ++claim_errors;
            } else if (claimed.value()) {
                ++execution_claims;
            }
        });
    }
    for (auto& claimer : claimers) {
        claimer.join();
    }
    EXPECT_EQ(claim_errors.load(), 0);
    EXPECT_EQ(execution_claims.load(), 1);

    auto third = second;
    third.identity.idempotency_key = "intent-3";
    third.operation_id = "operation-3";
    auto full = journal->registerCommand(std::move(third));
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().category,
              industrial_bus::ErrorCategory::ResourceExhausted);

    journal.reset();
    struct stat complete_info {};
    ASSERT_EQ(::stat(options.path.c_str(), &complete_info), 0);
    const int append_fd =
        ::open(options.path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    ASSERT_GE(append_fd, 0);
    const unsigned char partial[] = {0x49, 0x42, 0x4a};
    EXPECT_EQ(::write(append_fd, partial, sizeof(partial)),
              static_cast<ssize_t>(sizeof(partial)));
    EXPECT_EQ(::close(append_fd), 0);

    auto tail_recovered = bus_core::IdempotencyJournal::open(options);
    ASSERT_TRUE(tail_recovered);
    journal = std::move(tail_recovered.value());
    EXPECT_EQ(journal->liveRecordCount(), 2U);
    struct stat recovered_info {};
    ASSERT_EQ(::stat(options.path.c_str(), &recovered_info), 0);
    EXPECT_EQ(recovered_info.st_size, complete_info.st_size);
}

TEST(IdempotencyJournalTest, PersistentlyFailsInterruptedNonTerminalRecords) {
    JournalTempDirectory temp;
    bus_core::IdempotencyJournalOptions options;
    options.path = temp.journalPath();
    options.max_live_records = 8;

    auto opened = bus_core::IdempotencyJournal::open(options);
    ASSERT_TRUE(opened);
    auto journal = std::move(opened.value());

    const auto register_operation = [&](std::string key, std::string operation_id) {
        bus_core::CommandRegistration registration;
        registration.identity = {"caller", "cmp.simulation.run", std::move(key)};
        registration.payload = {'c', 'm', 'p'};
        registration.operation_id = std::move(operation_id);
        registration.owner_service = "cmp-suite";
        registration.owner_instance = "channel-a";
        return journal->registerCommand(std::move(registration));
    };

    auto created = register_operation("created", "operation-created");
    auto running = register_operation("running", "operation-running");
    auto cancelling = register_operation("cancelling", "operation-cancelling");
    auto completed = register_operation("completed", "operation-completed");
    auto failed = register_operation("failed", "operation-failed");
    ASSERT_TRUE(created);
    ASSERT_TRUE(running);
    ASSERT_TRUE(cancelling);
    ASSERT_TRUE(completed);
    ASSERT_TRUE(failed);

    ASSERT_TRUE(journal->claimForExecution(running.value().record.identity,
                                           "operation-running"));
    ASSERT_TRUE(journal->claimForExecution(cancelling.value().record.identity,
                                           "operation-cancelling"));
    ASSERT_TRUE(journal->updateState(cancelling.value().record.identity,
                                     "operation-cancelling",
                                     OperationState::Cancelling));
    ASSERT_TRUE(journal->claimForExecution(completed.value().record.identity,
                                           "operation-completed"));
    ASSERT_TRUE(journal->updateState(completed.value().record.identity,
                                     "operation-completed",
                                     OperationState::Completed));
    ASSERT_TRUE(journal->claimForExecution(failed.value().record.identity,
                                           "operation-failed"));
    auto existing_failure = industrial_bus::Error::internal("algorithm failed", 51);
    existing_failure.operation_id = "operation-failed";
    ASSERT_TRUE(journal->updateState(failed.value().record.identity,
                                     "operation-failed", OperationState::Failed,
                                     existing_failure));

    journal.reset();
    opened = bus_core::IdempotencyJournal::open(options);
    ASSERT_TRUE(opened);
    journal = std::move(opened.value());

    auto interruption =
        industrial_bus::Error::ownerUnavailable("CMPSuite process interrupted", 73);
    interruption.operation_id = "must-be-replaced";
    interruption.context["recovery"] = "startup";
    auto recovered = journal->failNonTerminal(interruption);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered.value(), 3U);

    for (const auto* decision : {&created.value(), &running.value(),
                                 &cancelling.value()}) {
        auto record = journal->find(decision->record.identity);
        ASSERT_TRUE(record);
        EXPECT_EQ(record.value().state, OperationState::Failed);
        ASSERT_TRUE(record.value().error);
        EXPECT_EQ(record.value().error->category,
                  industrial_bus::ErrorCategory::OwnerUnavailable);
        EXPECT_EQ(record.value().error->operation_id,
                  decision->record.operation_id);
        EXPECT_EQ(record.value().error->context.at("recovery"), "startup");
    }

    auto completed_record = journal->find(completed.value().record.identity);
    ASSERT_TRUE(completed_record);
    EXPECT_EQ(completed_record.value().state, OperationState::Completed);
    EXPECT_FALSE(completed_record.value().error);
    auto failed_record = journal->find(failed.value().record.identity);
    ASSERT_TRUE(failed_record);
    EXPECT_EQ(failed_record.value().state, OperationState::Failed);
    EXPECT_EQ(failed_record.value().error, existing_failure);

    auto idempotent = journal->failNonTerminal(interruption);
    ASSERT_TRUE(idempotent);
    EXPECT_EQ(idempotent.value(), 0U);

    journal.reset();
    opened = bus_core::IdempotencyJournal::open(options);
    ASSERT_TRUE(opened);
    journal = std::move(opened.value());
    for (const auto* decision : {&created.value(), &running.value(),
                                 &cancelling.value()}) {
        auto record = journal->find(decision->record.identity);
        ASSERT_TRUE(record);
        EXPECT_EQ(record.value().state, OperationState::Failed);
        ASSERT_TRUE(record.value().error);
        EXPECT_EQ(record.value().error->operation_id,
                  decision->record.operation_id);
    }
}

}  // namespace
