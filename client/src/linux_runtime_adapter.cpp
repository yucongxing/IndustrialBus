#include "linux_runtime_adapter.h"

#include "industrial_bus/bootstrap/endpoint_record.h"
#include "industrial_bus/bootstrap/hub_launcher.h"
#include "industrial_bus/bootstrap/hub_lock.h"
#include "industrial_bus/bootstrap/runtime_paths.h"
#include "industrial_bus/client/runtime_adapter.h"
#include "industrial_bus/ids.h"
#include "industrial_bus/runtime/transport_runtime_adapter.h"
#include "zeromq_transport.h"

#include <algorithm>
#include <chrono>
#include <limits.h>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace industrial_bus::client {
namespace {

using Clock = std::chrono::steady_clock;

std::string adjacentCommHubExecutable() {
    std::string executable(PATH_MAX, '\0');
    const auto length =
        ::readlink("/proc/self/exe", executable.data(), executable.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= executable.size()) {
        throw std::runtime_error("cannot resolve the current executable path");
    }
    executable.resize(static_cast<std::size_t>(length));
    const auto separator = executable.find_last_of('/');
    if (separator == std::string::npos) {
        throw std::runtime_error("current executable path has no parent directory");
    }
    return executable.substr(0, separator + 1) + "commhub";
}

Error discoveryError(const std::exception& error) {
    return Error::serviceUnavailable(std::string("CommHub discovery failed: ") +
                                     error.what());
}

class LinuxRuntimeAdapter final : public IRuntimeAdapter {
public:
    explicit LinuxRuntimeAdapter(const ClientOptions& options)
        : options_(options), instance_id_(options.instance_id.empty()
                                              ? generateUuidV4()
                                              : options.instance_id) {}

    Result<void> start() override {
        if (options_.service_name.empty() || options_.node_id.empty() ||
            options_.service_version.empty() ||
            options_.startup_timeout <= Duration::zero()) {
            return Error::invalidArgument(
                "local runtime requires service identity and a positive startup timeout");
        }
        if (options_.startup_mode == HubStartupMode::EmbeddedForTest) {
            return Error::invalidArgument(
                "EmbeddedForTest requires ClientOptions.runtime_adapter");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != ConnectionState::Disconnected) {
                return Error::invalidArgument("local runtime adapter is already started");
            }
            state_ = ConnectionState::Connecting;
        }

        try {
            paths_ = std::make_unique<bootstrap::RuntimePaths>(
                bootstrap::RuntimePaths::fromEnvironment());
        } catch (const std::exception& error) {
            setDisconnected();
            return Error::invalidArgument(error.what());
        }

        const auto deadline = Clock::now() + options_.startup_timeout;
        Error last_error = Error::serviceUnavailable("CommHub is not ready");
        auto first = connectOnce(deadline, options_.startup_mode ==
                                               HubStartupMode::ConnectOnly);
        if (first) {
            install(std::move(first.value()));
            return {};
        }
        last_error = first.error();
        if (options_.startup_mode == HubStartupMode::ConnectOnly) {
            setDisconnected();
            return last_error;
        }

        std::optional<bootstrap::HubLock> startup_lock;
        try {
            startup_lock = bootstrap::HubLock::tryAcquire(*paths_);
        } catch (const std::exception& error) {
            setDisconnected();
            return discoveryError(error);
        }
        if (startup_lock) {
            auto double_check = connectOnce(deadline, false);
            if (double_check) {
                install(std::move(double_check.value()));
                return {};
            }
            last_error = double_check.error();
            try {
                const auto executable = options_.hub_executable.empty()
                                            ? adjacentCommHubExecutable()
                                            : options_.hub_executable;
                (void)bootstrap::launchCommHub(*paths_, std::move(*startup_lock),
                                               executable);
            } catch (const std::exception& error) {
                setDisconnected();
                return Error::serviceUnavailable(
                    std::string("failed to launch CommHub: ") + error.what());
            }
        }

        while (Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto connected = connectOnce(deadline, false);
            if (connected) {
                install(std::move(connected.value()));
                return {};
            }
            last_error = connected.error();
        }
        setDisconnected();
        auto timeout = Error::timeout("CommHub did not become ready before startup timeout");
        timeout.context["last_error"] = last_error.message;
        return timeout;
    }

    Result<void> send(OutboundMessage message) override {
        auto runtime = delegate();
        return runtime ? runtime->send(std::move(message))
                       : Result<void>{Error::connectionLost("local runtime is not connected")};
    }

    Result<void> subscribe(const std::string& topic) override {
        auto runtime = delegate();
        return runtime ? runtime->subscribe(topic)
                       : Result<void>{Error::connectionLost("local runtime is not connected")};
    }

    Result<void> unsubscribe(const std::string& topic) override {
        auto runtime = delegate();
        return runtime ? runtime->unsubscribe(topic)
                       : Result<void>{Error::connectionLost("local runtime is not connected")};
    }

    void setReceiveHandler(ReceiveHandler handler) override {
        std::shared_ptr<runtime::TransportRuntimeAdapter> runtime;
        ReceiveHandler installed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            receive_handler_ = std::move(handler);
            installed = receive_handler_;
            runtime = delegate_;
        }
        if (runtime) {
            runtime->setReceiveHandler(std::move(installed));
        }
    }

    void setConnectionHandler(ConnectionHandler handler) override {
        std::shared_ptr<runtime::TransportRuntimeAdapter> runtime;
        ConnectionHandler installed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connection_handler_ = std::move(handler);
            installed = connection_handler_;
            runtime = delegate_;
        }
        if (runtime) {
            runtime->setConnectionHandler(std::move(installed));
        }
    }

    ConnectionState state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    std::uint64_t generation() const override {
        auto runtime = delegate();
        return runtime ? runtime->generation() : 0;
    }

    ServiceSnapshot services() const override {
        auto runtime = delegate();
        return runtime ? runtime->services() : ServiceSnapshot{};
    }

    ServiceInfo localService() const override {
        return {options_.service_name, instance_id_, true};
    }

    bool flush(std::chrono::milliseconds timeout) override {
        auto runtime = delegate();
        return !runtime || runtime->flush(timeout);
    }

    void close() noexcept override {
        std::shared_ptr<runtime::TransportRuntimeAdapter> runtime;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            runtime = std::move(delegate_);
            state_ = ConnectionState::Disconnected;
        }
        if (runtime) {
            runtime->close();
        }
    }

private:
    Result<std::shared_ptr<runtime::TransportRuntimeAdapter>> connectOnce(
        Clock::time_point deadline,
        bool use_remaining_timeout) {
        bootstrap::EndpointRecord endpoint;
        std::string token;
        try {
            endpoint = bootstrap::readEndpointRecord(*paths_);
            token = bootstrap::readAuthToken(*paths_);
        } catch (const std::exception& error) {
            return discoveryError(error);
        }
        if (endpoint.endpoint != paths_->endpoint() || endpoint.protocol_version != 1) {
            return Error::protocol("CommHub endpoint record is incompatible");
        }

        const auto now = Clock::now();
        if (now >= deadline) {
            return Error::timeout("CommHub connection deadline elapsed");
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto handshake_timeout = use_remaining_timeout
                                           ? remaining
                                           : std::min(remaining,
                                                      std::chrono::milliseconds(250));

        runtime::TransportRuntimeOptions runtime_options;
        runtime_options.endpoint = {endpoint.endpoint};
        runtime_options.connector =
            std::make_shared<transport::ZeroMqDealerConnector>();
        runtime_options.node_id = options_.node_id;
        runtime_options.service_name = options_.service_name;
        runtime_options.instance_id = instance_id_;
        runtime_options.service_version = options_.service_version;
        runtime_options.auth_token = std::move(token);
        runtime_options.expected_hub_instance_id = endpoint.hub_instance_id;
        runtime_options.expected_protocol_version = endpoint.protocol_version;
        runtime_options.handshake_timeout = handshake_timeout;
        auto runtime_adapter =
            std::make_shared<runtime::TransportRuntimeAdapter>(
                std::move(runtime_options));
        auto started = runtime_adapter->start();
        if (!started) {
            runtime_adapter->close();
            return started.error();
        }
        return runtime_adapter;
    }

    void install(std::shared_ptr<runtime::TransportRuntimeAdapter> runtime) {
        ReceiveHandler receive;
        ConnectionHandler connection;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            delegate_ = runtime;
            state_ = ConnectionState::Connected;
            receive = receive_handler_;
            connection = connection_handler_;
        }
        runtime->setReceiveHandler(std::move(receive));
        runtime->setConnectionHandler(std::move(connection));
    }

    std::shared_ptr<runtime::TransportRuntimeAdapter> delegate() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return delegate_;
    }

    void setDisconnected() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = ConnectionState::Disconnected;
    }

    ClientOptions options_;
    std::string instance_id_;
    std::unique_ptr<bootstrap::RuntimePaths> paths_;
    mutable std::mutex mutex_;
    std::shared_ptr<runtime::TransportRuntimeAdapter> delegate_;
    ReceiveHandler receive_handler_;
    ConnectionHandler connection_handler_;
    ConnectionState state_{ConnectionState::Disconnected};
};

}  // namespace

std::shared_ptr<IRuntimeAdapter> makeLinuxRuntimeAdapter(
    const ClientOptions& options) {
    return std::make_shared<LinuxRuntimeAdapter>(options);
}

}  // namespace industrial_bus::client
