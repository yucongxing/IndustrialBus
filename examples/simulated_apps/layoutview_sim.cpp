#include "simulation_app.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    bool list_channels{false};
    std::chrono::seconds timeout{15};
    std::string hub_executable{simulated_apps::defaultCommHubExecutable()};
    std::string channel_id;
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--list-channels") {
            options.list_channels = true;
        } else if (argument == "--channel" && index + 1 < argc) {
            options.channel_id = argv[++index];
        } else if (argument == "--timeout" && index + 1 < argc) {
            const auto seconds = std::strtol(argv[++index], nullptr, 10);
            if (seconds <= 0) {
                throw std::invalid_argument("--timeout must be positive");
            }
            options.timeout = std::chrono::seconds(seconds);
        } else if (argument == "--hub" && index + 1 < argc) {
            options.hub_executable = argv[++index];
        } else {
            throw std::invalid_argument(
                "usage: layoutview_sim (--list-channels | --channel UUID) "
                "[--timeout SECONDS] [--hub PATH]");
        }
    }
    if (options.list_channels && !options.channel_id.empty()) {
        throw std::invalid_argument(
            "--list-channels and --channel are mutually exclusive");
    }
    if (!options.list_channels && options.channel_id.empty()) {
        throw std::invalid_argument(
            "--channel UUID is required when running a simulation");
    }
    if (!options.channel_id.empty() &&
        !simulated_apps::isValidChannelId(options.channel_id)) {
        throw std::invalid_argument("--channel must be a UUID");
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        auto logger = simulated_apps::makeConsoleLogger("LayoutView");
        simulated_apps::LayoutViewOptions client_options;
        client_options.hub_executable = options.hub_executable;
        client_options.channel_id = options.channel_id;
        client_options.timeout = options.timeout;
        auto client = simulated_apps::makeLayoutViewClient(client_options, logger);
        std::string error;
        if (!client->start(error)) {
            logger->error("connect failed: " + error);
            return 1;
        }

        if (options.list_channels) {
            const auto channels = client->channels();
            if (channels.empty()) {
                std::cout << "No online CMPSuite channels\n";
            } else {
                for (const auto& channel : channels) {
                    std::cout << channel << '\n';
                }
            }
            client->stop();
            return 0;
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
