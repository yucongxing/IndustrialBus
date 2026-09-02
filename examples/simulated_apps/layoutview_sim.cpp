#include "simulation_app.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::chrono::seconds timeout{15};
    std::string hub_executable{simulated_apps::defaultCommHubExecutable()};
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
        auto logger = simulated_apps::makeConsoleLogger("LayoutView");
        auto client = simulated_apps::makeLayoutViewClient(
            {options.hub_executable, options.timeout}, logger);
        std::string error;
        if (!client->start(error)) {
            logger->error("connect failed: " + error);
            return 1;
        }

        std::string result_uri;
        if (!client->runSimulation({}, result_uri, error)) {
            logger->error("simulation failed: " + error);
            client->stop();
            return 2;
        }
        client->stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LayoutView: " << error.what() << '\n';
        return 64;
    }
}
