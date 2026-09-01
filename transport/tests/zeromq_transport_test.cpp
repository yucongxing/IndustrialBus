#include "industrial_bus/transport/transport.h"
#include "zeromq_transport.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <unistd.h>

using namespace industrial_bus::transport;

int main() {
    const std::string socket_path =
        "/tmp/industrial_bus_zmq_" + std::to_string(static_cast<long>(::getpid())) + ".sock";
    const Endpoint endpoint{"ipc://" + socket_path};

    ZeroMqRouterListener listener;
    std::mutex mutex;
    std::condition_variable ready;
    std::shared_ptr<IConnection> server;
    ByteBuffer server_received;
    const auto listen_result = listener.listen(endpoint, [&](std::shared_ptr<IConnection> connection) {
        connection->setReceiveHandler([&](ByteBuffer frame) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                server_received = std::move(frame);
            }
            ready.notify_all();
        });
        server = std::move(connection);
    });
    if (!listen_result) {
        std::cerr << "listen failed: " << listen_result.error().message << '\n';
        if (listen_result.error().message.find("Operation not permitted") !=
            std::string::npos) {
            // Some restricted CI/sandbox profiles deny creation of all sockets.
            return 77;
        }
        return 1;
    }

    ZeroMqDealerConnector connector;
    auto connected = connector.connect(endpoint).get();
    if (!connected) {
        std::cerr << "connect failed: " << connected.error().message << '\n';
        return 1;
    }
    auto client = std::move(connected).value();
    const ByteBuffer payload{9, 8, 7};
    if (!client->send(ByteView(payload))) {
        std::cerr << "client send failed\n";
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(2), [&] {
                return server_received == payload;
            })) {
            std::cerr << "server receive timed out\n";
            return 1;
        }
    }
    if (!server) {
        std::cerr << "server peer was not accepted\n";
        return 1;
    }

    ByteBuffer client_received;
    client->setReceiveHandler([&](ByteBuffer frame) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            client_received = std::move(frame);
        }
        ready.notify_all();
    });
    if (!server->send(ByteView(payload))) {
        std::cerr << "server send failed\n";
        return 1;
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(2), [&] {
                return client_received == payload;
            })) {
            std::cerr << "client receive timed out\n";
            return 1;
        }
    }

    const auto close_started = std::chrono::steady_clock::now();
    listener.close();
    const auto close_elapsed = std::chrono::steady_clock::now() - close_started;
    if (close_elapsed >= std::chrono::seconds(1)) {
        std::cerr << "listener close exceeded one second\n";
        return 1;
    }
    client->close();
    std::remove(socket_path.c_str());
}
