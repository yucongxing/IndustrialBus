#include "simulation_topics.h"

#include "industrial_bus/bus_client.h"
#include "industrial_bus/ids.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;

struct Options {
    std::chrono::seconds timeout{15};
    std::string hub_executable{INDUSTRIAL_BUS_SIM_COMMHUB_PATH};
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--timeout" && index + 1 < argc) {
            const auto seconds = std::strtol(argv[++index], nullptr, 10);
            if (seconds <= 0) {
                throw std::invalid_argument("--timeout must be positive");
            }
            options.timeout = std::chrono::seconds(seconds);
        } else if (argument == "--hub" && index + 1 < argc) {
            options.hub_executable = argv[++index];
        } else {
            throw std::invalid_argument(
                "usage: layoutview_sim [--timeout SECONDS] [--hub PATH]");
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        industrial_bus::ClientOptions client_options;
        client_options.service_name = "layout-view";
        client_options.hub_executable = options.hub_executable;
        auto connected = industrial_bus::BusClient::connect(client_options);
        if (!connected) {
            std::cerr << "LayoutView: connect failed: "
                      << connected.error().message << '\n';
            return 1;
        }
        auto bus = connected.value();

        const auto operation_id = industrial_bus::generateUuidV4();
        std::mutex mutex;
        std::condition_variable finished_cv;
        bool finished = false;
        bool succeeded = false;

        auto progress = bus->subscribe(
            industrial_bus::examples::simulation_progress,
            [&](const industrial_bus::cmp::v1::SimulationProgress& event) {
                if (event.operation_id() != operation_id) {
                    return;
                }
                std::cout << "LayoutView: progress " << event.stage() << " "
                          << static_cast<int>(event.overall_progress() * 100.0)
                          << "% - " << event.message() << '\n';
            });
        auto completed = bus->subscribe(
            industrial_bus::examples::simulation_completed,
            [&](const industrial_bus::cmp::v1::SimulationCompleted& event) {
                if (event.operation_id() != operation_id) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    succeeded = true;
                    finished = true;
                }
                std::cout << "LayoutView: completed, result="
                          << event.result().uri() << '\n';
                finished_cv.notify_all();
            });
        auto failed = bus->subscribe(
            industrial_bus::examples::simulation_failed,
            [&](const industrial_bus::cmp::v1::SimulationFailed& event) {
                if (event.operation_id() != operation_id) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    finished = true;
                }
                std::cerr << "LayoutView: simulation failed: "
                          << event.error().message() << '\n';
                finished_cv.notify_all();
            });
        if (!progress || !completed || !failed) {
            std::cerr << "LayoutView: failed to install event subscriptions\n";
            (void)bus->shutdown(100ms);
            return 2;
        }

        industrial_bus::cmp::v1::RunSimulation command;
        command.set_operation_id(operation_id);
        auto* input = command.mutable_input_layout();
        input->set_id("layout-" + operation_id);
        input->set_scope(industrial_bus::data::v1::LOCAL_USER);
        input->set_type(industrial_bus::data::v1::FILE);
        input->set_owner_node("local");
        input->set_uri("file:///tmp/layoutview-simulated-layout.oas");
        input->set_format("OASIS");
        input->set_format_version(1);
        input->set_access_mode(industrial_bus::data::v1::IMMUTABLE_READ_ONLY);
        (*command.mutable_parameters())["steps"] = "5";

        industrial_bus::CommandOptions command_options;
        command_options.operation_id = operation_id;
        command_options.idempotency_key = "layout-view-" + operation_id;
        command_options.accept_timeout = 5s;
        std::cout << "LayoutView: requesting CMP operation " << operation_id
                  << '\n';
        auto accepted = bus->command(
            industrial_bus::Destination{"cmp-suite", {}},
            industrial_bus::examples::run_simulation, command,
            command_options);
        if (accepted.wait_for(6s) != std::future_status::ready) {
            std::cerr << "LayoutView: timed out waiting for CommandAck\n";
            (void)bus->shutdown(100ms);
            return 3;
        }
        auto ack = accepted.get();
        if (!ack) {
            std::cerr << "LayoutView: command rejected: "
                      << ack.error().message << '\n';
            (void)bus->shutdown(100ms);
            return 4;
        }
        std::cout << "LayoutView: accepted by " << ack.value().owner_service()
                  << "/" << ack.value().owner_instance() << '\n';

        bool final_event_timed_out = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            final_event_timed_out = !finished_cv.wait_for(
                lock, options.timeout, [&] { return finished; });
        }
        if (final_event_timed_out) {
            std::cerr << "LayoutView: timed out waiting for final event\n";
            (void)bus->shutdown(100ms);
            return 5;
        }
        (void)bus->shutdown(200ms);
        return succeeded ? 0 : 6;
    } catch (const std::exception& error) {
        std::cerr << "LayoutView: " << error.what() << '\n';
        return 64;
    }
}
