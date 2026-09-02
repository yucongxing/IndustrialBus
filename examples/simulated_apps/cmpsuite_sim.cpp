#include "simulation_topics.h"

#include "industrial_bus/bus_client.h"
#include "industrial_bus/core/idempotency_journal.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::atomic<bool> stop_requested{false};

extern "C" void requestStop(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

struct Options {
    bool once{false};
    std::string hub_executable{INDUSTRIAL_BUS_SIM_COMMHUB_PATH};
    std::string journal_path{
        "/tmp/industrial-bus-cmpsuite-" +
        std::to_string(static_cast<long>(::getuid())) + ".journal"};
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--once") {
            options.once = true;
        } else if (argument == "--hub" && index + 1 < argc) {
            options.hub_executable = argv[++index];
        } else if (argument == "--journal" && index + 1 < argc) {
            options.journal_path = argv[++index];
        } else {
            throw std::invalid_argument(
                "usage: cmpsuite_sim [--once] [--hub PATH] [--journal PATH]");
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        industrial_bus::core::IdempotencyJournalOptions journal_options;
        journal_options.path = options.journal_path;
        auto opened = industrial_bus::core::IdempotencyJournal::open(
            std::move(journal_options));
        if (!opened) {
            std::cerr << "CMPSuite: journal open failed: "
                      << opened.error().message << '\n';
            return 1;
        }
        auto journal = std::shared_ptr<industrial_bus::core::IdempotencyJournal>(
            std::move(opened.value()));

        industrial_bus::ClientOptions client_options;
        client_options.service_name = "cmp-suite";
        client_options.hub_executable = options.hub_executable;
        client_options.idempotency_journal = journal;
        auto connected = industrial_bus::BusClient::connect(client_options);
        if (!connected) {
            std::cerr << "CMPSuite: connect failed: "
                      << connected.error().message << '\n';
            return 2;
        }
        auto bus = connected.value();

        std::mutex workers_mutex;
        std::vector<std::thread> workers;
        bool stopping = false;
        std::atomic<bool> completed_once{false};
        auto command_handler = bus->handleCommand(
            industrial_bus::examples::run_simulation,
            [&, bus, journal](
                const industrial_bus::cmp::v1::RunSimulation& command,
                const industrial_bus::MessageContext& context)
                -> industrial_bus::Result<void> {
                const auto operation_id = context.operation_id.empty()
                                              ? command.operation_id()
                                              : context.operation_id;
                const industrial_bus::core::CommandIdentity identity{
                    context.source_service,
                    std::string(industrial_bus::examples::run_simulation.name),
                    context.idempotency_key};
                std::lock_guard<std::mutex> lock(workers_mutex);
                if (stopping) {
                    return industrial_bus::Error::cancelled(
                        "CMPSuite is shutting down");
                }
                workers.emplace_back(
                    [bus, journal, identity, operation_id, &completed_once] {
                        constexpr int steps = 5;
                        for (int step = 1; step <= steps; ++step) {
                            std::this_thread::sleep_for(150ms);
                            industrial_bus::cmp::v1::SimulationProgress progress;
                            progress.set_operation_id(operation_id);
                            progress.set_stage(
                                step < steps ? "polishing" : "finishing");
                            progress.set_overall_progress(
                                static_cast<double>(step) /
                                static_cast<double>(steps));
                            progress.set_message("simulated CMP step " +
                                                 std::to_string(step) + "/" +
                                                 std::to_string(steps));
                            industrial_bus::PublishOptions publish_options;
                            publish_options.operation_id = operation_id;
                            const auto published = bus->publish(
                                industrial_bus::examples::simulation_progress,
                                progress, publish_options);
                            if (!published) {
                                std::cerr
                                    << "CMPSuite: progress publish failed: "
                                    << published.error().message << '\n';
                            }
                        }

                        auto persisted = journal->updateState(
                            identity, operation_id,
                            industrial_bus::OperationState::Completed);
                        if (!persisted) {
                            std::cerr
                                << "CMPSuite: final state persistence failed: "
                                << persisted.error().message << '\n';
                            industrial_bus::cmp::v1::SimulationFailed failed;
                            failed.set_operation_id(operation_id);
                            failed.mutable_error()->set_category(
                                industrial_bus::system::v1::INTERNAL_ERROR);
                            failed.mutable_error()->set_message(
                                persisted.error().message);
                            industrial_bus::PublishOptions publish_options;
                            publish_options.operation_id = operation_id;
                            (void)bus->publish(
                                industrial_bus::examples::simulation_failed,
                                failed, publish_options);
                            completed_once.store(true,
                                                 std::memory_order_release);
                            return;
                        }
                        industrial_bus::cmp::v1::SimulationCompleted completed;
                        completed.set_operation_id(operation_id);
                        auto* result = completed.mutable_result();
                        result->set_id("cmp-result-" + operation_id);
                        result->set_scope(
                            industrial_bus::data::v1::LOCAL_USER);
                        result->set_type(industrial_bus::data::v1::FILE);
                        result->set_owner_node("local");
                        result->set_uri("file:///tmp/cmp-result-" +
                                        operation_id + ".oas");
                        result->set_format("OASIS");
                        result->set_format_version(1);
                        result->set_access_mode(
                            industrial_bus::data::v1::IMMUTABLE_READ_ONLY);
                        industrial_bus::PublishOptions publish_options;
                        publish_options.operation_id = operation_id;
                        const auto published = bus->publish(
                            industrial_bus::examples::simulation_completed,
                            completed, publish_options);
                        if (!published) {
                            std::cerr
                                << "CMPSuite: completion publish failed: "
                                << published.error().message << '\n';
                        }
                        completed_once.store(true,
                                             std::memory_order_release);
                    });
                std::cout << "CMPSuite: accepted operation " << operation_id
                          << " from " << context.source_service << '\n';
                return {};
            });
        if (!command_handler) {
            std::cerr << "CMPSuite: failed to register cmp.simulation.run\n";
            (void)bus->shutdown(100ms);
            return 3;
        }

        std::signal(SIGINT, requestStop);
        std::signal(SIGTERM, requestStop);
        std::cout << "CMPSuite: ready; journal=" << options.journal_path << '\n';
        while (!stop_requested.load(std::memory_order_relaxed) &&
               !(options.once &&
                 completed_once.load(std::memory_order_acquire))) {
            std::this_thread::sleep_for(50ms);
        }

        command_handler.cancel();
        std::vector<std::thread> stopping_workers;
        {
            std::lock_guard<std::mutex> lock(workers_mutex);
            stopping = true;
            stopping_workers.swap(workers);
        }
        for (auto& worker : stopping_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        (void)bus->shutdown(200ms);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CMPSuite: " << error.what() << '\n';
        return 64;
    }
}
