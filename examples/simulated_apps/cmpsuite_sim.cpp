#include "simulation_app.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

std::atomic<bool> stop_requested{false};

extern "C" void requestStop(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

struct Options {
    bool once{false};
    std::string hub_executable{simulated_apps::defaultCommHubExecutable()};
    std::string journal_path;
    std::string channel_id;
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--once") {
            options.once = true;
        } else if (argument == "--channel" && index + 1 < argc) {
            options.channel_id = argv[++index];
        } else if (argument == "--hub" && index + 1 < argc) {
            options.hub_executable = argv[++index];
        } else if (argument == "--journal" && index + 1 < argc) {
            options.journal_path = argv[++index];
        } else {
            throw std::invalid_argument(
                "usage: cmpsuite_sim [--once] [--channel UUID] [--hub PATH] "
                "[--journal PATH]");
        }
    }
    if (options.channel_id.empty()) {
        options.channel_id = simulated_apps::generateChannelId();
    } else if (!simulated_apps::isValidChannelId(options.channel_id)) {
        throw std::invalid_argument("--channel must be a UUID");
    }
    if (options.journal_path.empty()) {
        options.journal_path =
            "/tmp/industrial-bus-cmpsuite-" +
            std::to_string(static_cast<long>(::getuid())) + "-" +
            options.channel_id + ".journal";
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        auto logger = simulated_apps::makeConsoleLogger("CMPSuite");
        logger->info("channel=" + options.channel_id);
        simulated_apps::CmpSuiteOptions service_options;
        service_options.hub_executable = options.hub_executable;
        service_options.journal_path = options.journal_path;
        service_options.channel_id = options.channel_id;
        auto service =
            simulated_apps::makeCmpSuiteService(service_options, logger);
        std::atomic<bool> completed_once{false};
        std::string error;
        if (!service->start(
                [logger, &completed_once](
                    const std::shared_ptr<simulated_apps::SimulationJob>& job) {
                    logger->info("running operation " + job->operationId() +
                                 ", input=" + job->inputUri());
                    const auto steps = job->requestedSteps();
                    for (std::size_t step = 1; step <= steps; ++step) {
                        std::this_thread::sleep_for(150ms);
                        (void)job->reportProgress(
                            step, steps,
                            step < steps ? "polishing" : "finishing",
                            "simulated CMP step " + std::to_string(step) + "/" +
                                std::to_string(steps));
                    }
                    const auto completed = job->complete(
                        "file:///tmp/cmp-result-" + job->operationId() +
                        ".oas");
                    if (!completed) {
                        logger->error("failed to complete operation " +
                                      job->operationId());
                    }
                    completed_once.store(true, std::memory_order_release);
                },
                error)) {
            logger->error("startup failed: " + error);
            return 1;
        }

        std::signal(SIGINT, requestStop);
        std::signal(SIGTERM, requestStop);
        while (!stop_requested.load(std::memory_order_relaxed) &&
               !(options.once &&
                 completed_once.load(std::memory_order_acquire))) {
            std::this_thread::sleep_for(50ms);
        }
        service->stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CMPSuite: " << error.what() << '\n';
        return 64;
    }
}
