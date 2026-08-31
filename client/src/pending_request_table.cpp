#include "industrial_bus/client/pending_request_table.h"

#include "industrial_bus/error.h"

#include <utility>
#include <vector>

namespace industrial_bus::client {

PendingRequestTable::PendingRequestTable() : timer_([this] { timerLoop(); }) {}

PendingRequestTable::~PendingRequestTable() {
    shutdown(Error::make(ErrorCategory::Cancelled, "request table stopped"));
}

bool PendingRequestTable::add(std::string message_id,
                              std::string expected_payload_type,
                              Clock::time_point deadline,
                              Completion completion) {
    if (message_id.empty() || !completion) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || entries_.find(message_id) != entries_.end()) {
            return false;
        }
        deadlines_.emplace(deadline, message_id);
        entries_.emplace(std::move(message_id),
                         Entry{std::move(expected_payload_type), deadline,
                               std::move(completion)});
    }
    cv_.notify_one();
    return true;
}

bool PendingRequestTable::complete(const std::string& message_id,
                                   const std::string& received_payload_type,
                                   Result<ByteBuffer> result) {
    Completion completion;
    std::string expected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(message_id);
        if (it == entries_.end()) {
            ++late_replies_;
            return false;
        }
        expected = it->second.expected_payload_type;
        completion = std::move(it->second.completion);
        entries_.erase(it);
    }
    if (result && !expected.empty() && expected != received_payload_type) {
        completion(Error::protocol("reply payload type does not match request contract"));
    } else {
        completion(std::move(result));
    }
    return true;
}

void PendingRequestTable::failAll(const Error& error) {
    std::vector<Completion> completions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completions.reserve(entries_.size());
        for (auto& item : entries_) {
            completions.push_back(std::move(item.second.completion));
        }
        entries_.clear();
        deadlines_.clear();
    }
    for (auto& completion : completions) {
        completion(error);
    }
}

void PendingRequestTable::shutdown(const Error& error) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (timer_.joinable()) {
        timer_.join();
    }
    try {
        failAll(error);
    } catch (...) {
    }
}

std::size_t PendingRequestTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::uint64_t PendingRequestTable::lateReplyCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return late_replies_;
}

void PendingRequestTable::timerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (stopping_) {
            return;
        }
        if (deadlines_.empty()) {
            cv_.wait(lock, [this] { return stopping_ || !deadlines_.empty(); });
            continue;
        }

        const auto next = deadlines_.begin()->first;
        cv_.wait_until(lock, next);
        if (stopping_) {
            return;
        }

        const auto now = Clock::now();
        std::vector<Completion> expired;
        while (!deadlines_.empty() && deadlines_.begin()->first <= now) {
            auto deadline_it = deadlines_.begin();
            auto entry_it = entries_.find(deadline_it->second);
            if (entry_it != entries_.end() && entry_it->second.deadline <= now) {
                expired.push_back(std::move(entry_it->second.completion));
                entries_.erase(entry_it);
            }
            deadlines_.erase(deadline_it);
        }
        lock.unlock();
        for (auto& completion : expired) {
            completion(Error::timeout("request deadline exceeded"));
        }
        lock.lock();
    }
}

}  // namespace industrial_bus::client
