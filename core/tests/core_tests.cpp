#include "industrial_bus/core/bounded_priority_queue.h"
#include "industrial_bus/ids.h"
#include "industrial_bus/operation_state.h"
#include "industrial_bus/result.h"

#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {

using industrial_bus::BoundedPriorityQueue;
using industrial_bus::EnqueueDisposition;
using industrial_bus::MessageClass;
using industrial_bus::OperationState;
using industrial_bus::OperationStateMachine;
using industrial_bus::QueueMetadata;
using industrial_bus::QosPriority;

int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "FAILED line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void testResult() {
    industrial_bus::Result<int> good(42);
    CHECK(good);
    CHECK(good.value() == 42);

    industrial_bus::Result<int> bad(
        industrial_bus::Error::invalidArgument("bad argument", 7));
    CHECK(!bad);
    CHECK(bad.error().category == industrial_bus::ErrorCategory::InvalidArgument);
    CHECK(bad.error().code == 7);

    bool threw = false;
    try {
        (void)bad.value();
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);

    industrial_bus::Result<void> void_result;
    CHECK(void_result);
}

void testIdentifiers() {
    std::set<std::string> generated;
    for (int i = 0; i < 1000; ++i) {
        auto id = industrial_bus::generateUuidV4();
        CHECK(industrial_bus::isValidUuid(id));
        generated.insert(std::move(id));
    }
    CHECK(generated.size() == 1000);
    CHECK(!industrial_bus::isValidUuid("not-a-uuid"));
    CHECK(industrial_bus::isValidProtocolId("layout.boolean:instance-1"));
    CHECK(!industrial_bus::isValidProtocolId("has a space"));
    CHECK(!industrial_bus::isValidProtocolId(""));
}

void testOperationStateMachine() {
    OperationStateMachine operation;
    CHECK(operation.state() == OperationState::Created);
    CHECK(operation.transitionTo(OperationState::Running));
    CHECK(operation.requestCancellation());
    CHECK(operation.state() == OperationState::Cancelling);
    CHECK(operation.requestCancellation());
    CHECK(operation.transitionTo(OperationState::Completed));
    CHECK(industrial_bus::isTerminal(operation.state()));
    CHECK(!operation.transitionTo(OperationState::Running));

    OperationStateMachine queued;
    CHECK(queued.requestCancellation());
    CHECK(queued.state() == OperationState::Cancelled);
    CHECK(queued.requestCancellation());

    CHECK(!industrial_bus::isValidTransition(OperationState::Created,
                                             OperationState::Completed));
    CHECK(industrial_bus::isValidTransition(OperationState::Cancelling,
                                            OperationState::Failed));
}

QueueMetadata metadata(MessageClass message_class, std::string key = {}) {
    return QueueMetadata{industrial_bus::defaultPriority(message_class), message_class,
                         std::move(key)};
}

void testPriorityAndCapacity() {
    BoundedPriorityQueue<std::string> queue(3);
    CHECK(queue.tryEnqueue("event", metadata(MessageClass::Event)) ==
          EnqueueDisposition::Enqueued);
    CHECK(queue.tryEnqueue("request", metadata(MessageClass::Request)) ==
          EnqueueDisposition::Enqueued);
    CHECK(queue.tryEnqueue("reply", metadata(MessageClass::Reply)) ==
          EnqueueDisposition::Enqueued);

    CHECK(queue.tryDequeue().value() == "reply");
    CHECK(queue.tryDequeue().value() == "request");
    CHECK(queue.tryDequeue().value() == "event");
    CHECK(!queue.tryDequeue().has_value());

    BoundedPriorityQueue<int> high_only(1);
    CHECK(high_only.tryEnqueue(1, metadata(MessageClass::Command)) ==
          EnqueueDisposition::Enqueued);
    CHECK(high_only.tryEnqueue(2, metadata(MessageClass::Request)) ==
          EnqueueDisposition::Rejected);
    CHECK(high_only.tryDequeue().value() == 1);
}

void testProgressCoalescingAndEviction() {
    BoundedPriorityQueue<std::string> queue(2);
    CHECK(queue.tryEnqueue("p10", metadata(MessageClass::Progress, "operation-a")) ==
          EnqueueDisposition::Enqueued);
    CHECK(queue.tryEnqueue("p20", metadata(MessageClass::Progress, "operation-a")) ==
          EnqueueDisposition::Coalesced);
    CHECK(queue.size() == 1);
    CHECK(queue.tryDequeue().value() == "p20");

    CHECK(queue.tryEnqueue("debug", metadata(MessageClass::DebugLog)) ==
          EnqueueDisposition::Enqueued);
    CHECK(queue.tryEnqueue("info", metadata(MessageClass::InfoLog)) ==
          EnqueueDisposition::Enqueued);
    CHECK(queue.tryEnqueue("command", metadata(MessageClass::Command)) ==
          EnqueueDisposition::Enqueued);
    CHECK(queue.size() == 2);
    CHECK(queue.tryDequeue().value() == "command");
    CHECK(queue.tryDequeue().value() == "info");

    const auto stats = queue.stats();
    CHECK(stats.coalesced == 1);
    CHECK(stats.dropped == 1);
    CHECK(stats.high_watermark == 2);
}

void testCriticalOverflow() {
    BoundedPriorityQueue<int> queue(1);
    CHECK(queue.tryEnqueue(1, metadata(MessageClass::FinalState)) ==
          EnqueueDisposition::Enqueued);
    CHECK(queue.tryEnqueue(2, metadata(MessageClass::Reply)) ==
          EnqueueDisposition::CriticalOverflow);
    CHECK(queue.stats().critical_overflow == 1);

    BoundedPriorityQueue<int> zero(0);
    CHECK(zero.tryEnqueue(1, metadata(MessageClass::DebugLog)) ==
          EnqueueDisposition::Rejected);
}

}  // namespace

int main() {
    testResult();
    testIdentifiers();
    testOperationStateMachine();
    testPriorityAndCapacity();
    testProgressCoalescingAndEviction();
    testCriticalOverflow();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "industrial_bus_core_tests passed\n";
    return EXIT_SUCCESS;
}
