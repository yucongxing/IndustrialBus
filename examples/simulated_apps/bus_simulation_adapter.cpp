#include "simulation_app.h"

#include "simulation_topics.h"

#include "industrial_bus/bus_client.h"
#include "industrial_bus/core/idempotency_journal.h"
#include "industrial_bus/ids.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace simulated_apps {
namespace {

using namespace std::chrono_literals;

class BusLogSink final : public LogSink {
public:
    BusLogSink(std::weak_ptr<industrial_bus::BusClient> bus,
               std::string service,
               std::shared_ptr<const industrial_bus::examples::ChannelTopics>
                   topics)
        : bus_(std::move(bus)),
          service_(std::move(service)),
          topics_(std::move(topics)),
          worker_([this] { run(); }) {}

    ~BusLogSink() override { stop(); }

    void write(const LogRecord& record) noexcept override {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            if (records_.size() == capacity_) {
                records_.pop_front();
            }
            records_.push_back(record);
            ready_.notify_one();
        } catch (...) {
            // Bus logging is best effort and must never recurse into Logger.
        }
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
    }

private:
    static industrial_bus::logging::v1::LogLevel toProtocolLevel(
        LogLevel level) noexcept {
        switch (level) {
            case LogLevel::Info:
                return industrial_bus::logging::v1::INFO;
            case LogLevel::Warning:
                return industrial_bus::logging::v1::WARN;
            case LogLevel::Error:
                return industrial_bus::logging::v1::LOG_ERROR;
        }
        return industrial_bus::logging::v1::LOG_LEVEL_UNSPECIFIED;
    }

    void run() noexcept {
        for (;;) {
            LogRecord record;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock,
                            [&] { return stopping_ || !records_.empty(); });
                if (records_.empty()) {
                    if (stopping_) {
                        return;
                    }
                    continue;
                }
                record = std::move(records_.front());
                records_.pop_front();
            }

            const auto bus = bus_.lock();
            if (!bus) {
                continue;
            }
            industrial_bus::logging::v1::LogEvent event;
            event.set_timestamp_unix_ns(record.timestamp_unix_ns);
            event.set_service(service_);
            event.set_module(std::move(record.component));
            event.set_level(toProtocolLevel(record.level));
            event.set_category("application");
            event.set_message(std::move(record.message));
            event.set_thread_id(record.thread_id);
            event.set_sequence(++sequence_);
            industrial_bus::PublishOptions options;
            options.message_class =
                industrial_bus::examples::logMessageClass(event.level());
            (void)bus->publish(topics_->applicationLog(), event, options);
        }
    }

    static constexpr std::size_t capacity_{256};
    std::weak_ptr<industrial_bus::BusClient> bus_;
    std::string service_;
    std::shared_ptr<const industrial_bus::examples::ChannelTopics> topics_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<LogRecord> records_;
    bool stopping_{false};
    std::uint64_t sequence_{0};
    std::thread worker_;
};

class BusLayoutViewClient final : public LayoutViewClient {
public:
    BusLayoutViewClient(LayoutViewOptions options,
                        std::shared_ptr<Logger> logger)
        : options_(std::move(options)), logger_(std::move(logger)) {}

    ~BusLayoutViewClient() override { stop(); }

    bool start(std::string& error) override {
        if (!options_.channel_id.empty() &&
            !industrial_bus::examples::isValidChannelId(
                options_.channel_id)) {
            error = "LayoutView channel must be a UUID";
            return false;
        }
        industrial_bus::ClientOptions options;
        options.service_name = "layout-view";
        options.instance_id = industrial_bus::generateUuidV4();
        options.instance_mode = industrial_bus::InstanceMode::MultiInstance;
        options.hub_executable = options_.hub_executable;
        auto connected = industrial_bus::BusClient::connect(options);
        if (!connected) {
            error = connected.error().message;
            return false;
        }
        bus_ = connected.value();
        if (!options_.channel_id.empty()) {
            topics_ = std::make_shared<industrial_bus::examples::ChannelTopics>(
                options_.channel_id);
            bus_log_sink_ =
                std::make_shared<BusLogSink>(bus_, "layout-view", topics_);
            bus_log_sink_id_ = logger_->addSink(bus_log_sink_);
        }
        logger_->info("connected as layout-view");
        return true;
    }

    std::vector<std::string> channels() const override {
        std::vector<std::string> result;
        if (!bus_) {
            return result;
        }
        for (const auto& service : bus_->services()) {
            if (service.service_name == "cmp-suite" &&
                industrial_bus::examples::isValidChannelId(
                    service.instance_id)) {
                result.push_back(service.instance_id);
            }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    bool runSimulation(const RunSimulationOptions& options,
                       std::string& result_uri,
                       std::string& error) override {
        if (!bus_) {
            error = "LayoutView client is not started";
            return false;
        }
        if (!topics_) {
            error = "LayoutView requires a channel to run a simulation";
            return false;
        }
        const auto operation_id = industrial_bus::generateUuidV4();
        struct Outcome {
            std::mutex mutex;
            std::condition_variable finished_cv;
            bool finished{false};
            bool succeeded{false};
            std::string result_uri;
            std::string error;
        };
        auto outcome = std::make_shared<Outcome>();
        auto logger = logger_;

        auto progress = bus_->subscribe(
            topics_->simulationProgress(),
            [logger, operation_id](
                const industrial_bus::cmp::v1::SimulationProgress& event) {
                if (event.operation_id() != operation_id) {
                    return;
                }
                logger->info(
                    "progress " + event.stage() + " " +
                    std::to_string(
                        static_cast<int>(event.overall_progress() * 100.0)) +
                    "% - " + event.message());
            });
        auto completed = bus_->subscribe(
            topics_->simulationCompleted(),
            [outcome, operation_id](
                const industrial_bus::cmp::v1::SimulationCompleted& event) {
                if (event.operation_id() != operation_id) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(outcome->mutex);
                    outcome->result_uri = event.result().uri();
                    outcome->succeeded = true;
                    outcome->finished = true;
                }
                outcome->finished_cv.notify_all();
            });
        auto failed = bus_->subscribe(
            topics_->simulationFailed(),
            [outcome, operation_id](
                const industrial_bus::cmp::v1::SimulationFailed& event) {
                if (event.operation_id() != operation_id) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(outcome->mutex);
                    outcome->error = event.error().message();
                    outcome->finished = true;
                }
                outcome->finished_cv.notify_all();
            });
        if (!progress || !completed || !failed) {
            error = "failed to install CMP event subscriptions";
            return false;
        }

        industrial_bus::cmp::v1::RunSimulation command;
        command.set_operation_id(operation_id);
        auto* input = command.mutable_input_layout();
        input->set_id("layout-" + operation_id);
        input->set_scope(industrial_bus::data::v1::LOCAL_USER);
        input->set_type(industrial_bus::data::v1::FILE);
        input->set_owner_node("local");
        input->set_uri(options.input_uri);
        input->set_format("OASIS");
        input->set_format_version(1);
        input->set_access_mode(
            industrial_bus::data::v1::IMMUTABLE_READ_ONLY);
        (*command.mutable_parameters())["steps"] =
            std::to_string(options.steps);

        industrial_bus::CommandOptions command_options;
        command_options.operation_id = operation_id;
        command_options.idempotency_key = "layout-view-" + operation_id;
        command_options.accept_timeout = 5s;
        logger_->info("requesting CMP operation " + operation_id);
        auto accepted = bus_->command(
            industrial_bus::Destination{"cmp-suite", options_.channel_id},
            topics_->runSimulation(), command,
            command_options);
        if (accepted.wait_for(6s) != std::future_status::ready) {
            error = "timed out waiting for CommandAck";
            return false;
        }
        auto ack = accepted.get();
        if (!ack) {
            error = ack.error().message;
            return false;
        }
        logger_->info("accepted by " + ack.value().owner_service() + "/" +
                      ack.value().owner_instance());

        {
            std::unique_lock<std::mutex> lock(outcome->mutex);
            if (!outcome->finished_cv.wait_for(
                    lock, options_.timeout,
                    [&] { return outcome->finished; })) {
                error = "timed out waiting for the final CMP event";
                return false;
            }
            result_uri = outcome->result_uri;
            error = outcome->error;
        }
        if (outcome->succeeded) {
            logger_->info("completed, result=" + result_uri);
        }
        return outcome->succeeded;
    }

    void stop() noexcept override {
        logger_->removeSink(bus_log_sink_id_);
        bus_log_sink_id_ = 0;
        if (bus_log_sink_) {
            bus_log_sink_->stop();
            bus_log_sink_.reset();
        }
        auto bus = std::move(bus_);
        if (bus) {
            (void)bus->shutdown(200ms);
        }
    }

private:
    LayoutViewOptions options_;
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<industrial_bus::BusClient> bus_;
    std::shared_ptr<const industrial_bus::examples::ChannelTopics> topics_;
    std::shared_ptr<BusLogSink> bus_log_sink_;
    Logger::SinkId bus_log_sink_id_{0};
};

class BusSimulationJob final : public SimulationJob {
public:
    BusSimulationJob(
        std::shared_ptr<industrial_bus::BusClient> bus,
        std::shared_ptr<industrial_bus::core::IdempotencyJournal> journal,
        industrial_bus::core::CommandIdentity identity,
        std::string operation_id,
        std::string input_uri,
        std::size_t steps,
        std::shared_ptr<Logger> logger,
        std::shared_ptr<const industrial_bus::examples::ChannelTopics> topics)
        : bus_(std::move(bus)),
          journal_(std::move(journal)),
          identity_(std::move(identity)),
          operation_id_(std::move(operation_id)),
          input_uri_(std::move(input_uri)),
          steps_(steps),
          logger_(std::move(logger)),
          topics_(std::move(topics)) {}

    const std::string& operationId() const noexcept override {
        return operation_id_;
    }

    const std::string& inputUri() const noexcept override { return input_uri_; }

    std::size_t requestedSteps() const noexcept override { return steps_; }

    bool reportProgress(std::size_t current,
                        std::size_t total,
                        std::string stage,
                        std::string message) override {
        if (terminal_.load(std::memory_order_acquire) || total == 0) {
            return false;
        }
        industrial_bus::cmp::v1::SimulationProgress progress;
        progress.set_operation_id(operation_id_);
        progress.set_stage(std::move(stage));
        progress.set_overall_progress(
            static_cast<double>(current) / static_cast<double>(total));
        progress.set_message(std::move(message));
        industrial_bus::PublishOptions options;
        options.operation_id = operation_id_;
        const auto published = bus_->publish(
            topics_->simulationProgress(), progress, options);
        if (!published) {
            logger_->error("progress publish failed: " +
                           published.error().message);
        }
        return static_cast<bool>(published);
    }

    bool complete(std::string result_uri) override {
        bool expected = false;
        if (!terminal_.compare_exchange_strong(expected, true)) {
            return false;
        }
        auto persisted = journal_->updateState(
            identity_, operation_id_, industrial_bus::OperationState::Completed);
        if (!persisted) {
            publishFailure("final state persistence failed: " +
                           persisted.error().message);
            return false;
        }
        industrial_bus::cmp::v1::SimulationCompleted completed;
        completed.set_operation_id(operation_id_);
        auto* result = completed.mutable_result();
        result->set_id("cmp-result-" + operation_id_);
        result->set_scope(industrial_bus::data::v1::LOCAL_USER);
        result->set_type(industrial_bus::data::v1::FILE);
        result->set_owner_node("local");
        result->set_uri(std::move(result_uri));
        result->set_format("OASIS");
        result->set_format_version(1);
        result->set_access_mode(
            industrial_bus::data::v1::IMMUTABLE_READ_ONLY);
        industrial_bus::PublishOptions options;
        options.operation_id = operation_id_;
        const auto published = bus_->publish(
            topics_->simulationCompleted(), completed, options);
        if (!published) {
            logger_->error("completion publish failed: " +
                           published.error().message);
        }
        return static_cast<bool>(published);
    }

    bool fail(std::string message) override {
        bool expected = false;
        if (!terminal_.compare_exchange_strong(expected, true)) {
            return false;
        }
        auto error = industrial_bus::Error::internal(message);
        error.operation_id = operation_id_;
        const auto persisted = journal_->updateState(
            identity_, operation_id_, industrial_bus::OperationState::Failed,
            error);
        if (!persisted) {
            logger_->error("failed state persistence failed: " +
                           persisted.error().message);
        }
        return publishFailure(std::move(message));
    }

private:
    bool publishFailure(std::string message) {
        industrial_bus::cmp::v1::SimulationFailed failed;
        failed.set_operation_id(operation_id_);
        failed.mutable_error()->set_category(
            industrial_bus::system::v1::INTERNAL_ERROR);
        failed.mutable_error()->set_message(std::move(message));
        failed.mutable_error()->set_operation_id(operation_id_);
        industrial_bus::PublishOptions options;
        options.operation_id = operation_id_;
        const auto published = bus_->publish(
            topics_->simulationFailed(), failed, options);
        if (!published) {
            logger_->error("failure event publish failed: " +
                           published.error().message);
        }
        return static_cast<bool>(published);
    }

    std::shared_ptr<industrial_bus::BusClient> bus_;
    std::shared_ptr<industrial_bus::core::IdempotencyJournal> journal_;
    industrial_bus::core::CommandIdentity identity_;
    std::string operation_id_;
    std::string input_uri_;
    std::size_t steps_;
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<const industrial_bus::examples::ChannelTopics> topics_;
    std::atomic<bool> terminal_{false};
};

class BusCmpSuiteService final : public CmpSuiteService {
public:
    BusCmpSuiteService(CmpSuiteOptions options,
                       std::shared_ptr<Logger> logger)
        : options_(std::move(options)), logger_(std::move(logger)) {}

    ~BusCmpSuiteService() override { stop(); }

    bool start(SimulationHandler handler, std::string& error) override {
        if (!industrial_bus::examples::isValidChannelId(options_.channel_id)) {
            error = "CMPSuite channel must be a UUID";
            return false;
        }
        industrial_bus::core::IdempotencyJournalOptions journal_options;
        journal_options.path = options_.journal_path;
        auto opened = industrial_bus::core::IdempotencyJournal::open(
            std::move(journal_options));
        if (!opened) {
            error = opened.error().message;
            return false;
        }
        journal_ =
            std::shared_ptr<industrial_bus::core::IdempotencyJournal>(
                std::move(opened.value()));
        topics_ = std::make_shared<industrial_bus::examples::ChannelTopics>(
            options_.channel_id);

        industrial_bus::ClientOptions client_options;
        client_options.service_name = "cmp-suite";
        client_options.instance_id = options_.channel_id;
        client_options.instance_mode = industrial_bus::InstanceMode::MultiInstance;
        client_options.hub_executable = options_.hub_executable;
        client_options.idempotency_journal = journal_;
        auto connected = industrial_bus::BusClient::connect(client_options);
        if (!connected) {
            error = connected.error().message;
            journal_.reset();
            return false;
        }
        bus_ = connected.value();
        auto interruption = industrial_bus::Error::ownerUnavailable(
            "CMPSuite process was interrupted before completion");
        interruption.context["recovery"] = "startup";
        auto recovered = journal_->failNonTerminal(std::move(interruption));
        if (!recovered) {
            error = recovered.error().message;
            auto bus = std::move(bus_);
            (void)bus->shutdown(200ms);
            topics_.reset();
            journal_.reset();
            return false;
        }
        bus_log_sink_ =
            std::make_shared<BusLogSink>(bus_, "cmp-suite", topics_);
        bus_log_sink_id_ = logger_->addSink(bus_log_sink_);
        if (recovered.value() != 0) {
            logger_->warning(
                "marked " + std::to_string(recovered.value()) +
                " interrupted operation(s) as failed");
        }
        command_handler_ = bus_->handleCommand(
            topics_->runSimulation(),
            [this, handler = std::move(handler)](
                const industrial_bus::cmp::v1::RunSimulation& command,
                const industrial_bus::MessageContext& context)
                -> industrial_bus::Result<void> {
                const auto operation_id = context.operation_id.empty()
                                              ? command.operation_id()
                                              : context.operation_id;
                auto steps = std::size_t{5};
                const auto found = command.parameters().find("steps");
                if (found != command.parameters().end()) {
                    const auto parsed = std::strtoul(
                        found->second.c_str(), nullptr, 10);
                    if (parsed > 0) {
                        steps = static_cast<std::size_t>(parsed);
                    }
                }
                std::lock_guard<std::mutex> lock(workers_mutex_);
                if (stopping_) {
                    return industrial_bus::Error::cancelled(
                        "CMPSuite is shutting down");
                }
                auto job = std::make_shared<BusSimulationJob>(
                    bus_, journal_,
                    industrial_bus::core::CommandIdentity{
                        context.source_service,
                        std::string(topics_->runSimulation().name),
                        context.idempotency_key},
                    operation_id, command.input_layout().uri(), steps, logger_,
                    topics_);
                workers_.emplace_back([handler, job] {
                    try {
                        handler(job);
                    } catch (const std::exception& exception) {
                        (void)job->fail(exception.what());
                    } catch (...) {
                        (void)job->fail("simulation handler threw");
                    }
                });
                logger_->info("accepted operation " + operation_id + " from " +
                              context.source_service);
                return {};
            });
        if (!command_handler_) {
            error = "failed to register " +
                    std::string(topics_->runSimulation().name);
            stop();
            return false;
        }
        logger_->info("ready; channel=" + options_.channel_id +
                      "; journal=" + options_.journal_path);
        return true;
    }

    void stop() noexcept override {
        command_handler_.cancel();
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            stopping_ = true;
            workers.swap(workers_);
        }
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        logger_->removeSink(bus_log_sink_id_);
        bus_log_sink_id_ = 0;
        if (bus_log_sink_) {
            bus_log_sink_->stop();
            bus_log_sink_.reset();
        }
        auto bus = std::move(bus_);
        if (bus) {
            (void)bus->shutdown(200ms);
        }
        journal_.reset();
        topics_.reset();
    }

private:
    CmpSuiteOptions options_;
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<industrial_bus::core::IdempotencyJournal> journal_;
    std::shared_ptr<industrial_bus::BusClient> bus_;
    std::shared_ptr<const industrial_bus::examples::ChannelTopics> topics_;
    std::shared_ptr<BusLogSink> bus_log_sink_;
    Logger::SinkId bus_log_sink_id_{0};
    industrial_bus::Subscription command_handler_;
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};

}  // namespace

std::string defaultCommHubExecutable() {
    return INDUSTRIAL_BUS_SIM_COMMHUB_PATH;
}

std::string generateChannelId() {
    return industrial_bus::generateUuidV4();
}

bool isValidChannelId(std::string_view channel_id) noexcept {
    return industrial_bus::examples::isValidChannelId(channel_id);
}

std::unique_ptr<LayoutViewClient> makeLayoutViewClient(
    LayoutViewOptions options,
    std::shared_ptr<Logger> logger) {
    return std::make_unique<BusLayoutViewClient>(std::move(options),
                                                 std::move(logger));
}

std::unique_ptr<CmpSuiteService> makeCmpSuiteService(
    CmpSuiteOptions options,
    std::shared_ptr<Logger> logger) {
    return std::make_unique<BusCmpSuiteService>(std::move(options),
                                                std::move(logger));
}

}  // namespace simulated_apps
