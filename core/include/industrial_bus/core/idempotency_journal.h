#pragma once

#include "industrial_bus/operation_state.h"
#include "industrial_bus/payload_codec.h"
#include "industrial_bus/result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace industrial_bus::core {

struct IdempotencyJournalOptions {
    std::string path;
    std::chrono::milliseconds retention{std::chrono::hours(24)};
    std::size_t max_live_records{100000};
};

struct CommandIdentity {
    std::string source_service;
    std::string command_topic;
    std::string idempotency_key;
};

struct CommandRegistration {
    CommandIdentity identity;
    ByteBuffer payload;
    std::string operation_id;
    std::string owner_service;
    std::string owner_instance;
    OperationState initial_state{OperationState::Created};
};

struct IdempotencyRecord {
    CommandIdentity identity;
    std::string payload_sha256;
    std::string operation_id;
    std::string owner_service;
    std::string owner_instance;
    OperationState state{OperationState::Created};
    std::int64_t created_at_unix_ns{0};
    std::int64_t expires_at_unix_ns{0};
};

struct RegistrationDecision {
    IdempotencyRecord record;
    bool duplicate{false};
};

// Linux append-only command acceptance journal. A successful new registration
// means its record has been written and fdatasync() has completed.
class IdempotencyJournal {
public:
    static Result<std::unique_ptr<IdempotencyJournal>> open(
        IdempotencyJournalOptions options);

    ~IdempotencyJournal();
    IdempotencyJournal(const IdempotencyJournal&) = delete;
    IdempotencyJournal& operator=(const IdempotencyJournal&) = delete;

    Result<RegistrationDecision> registerCommand(CommandRegistration command);
    Result<IdempotencyRecord> find(const CommandIdentity& identity) const;
    std::size_t liveRecordCount() const;

private:
    class Impl;
    explicit IdempotencyJournal(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace industrial_bus::core
