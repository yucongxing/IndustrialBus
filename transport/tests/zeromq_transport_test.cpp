#include "industrial_bus/transport/transport.h"
#include "zeromq_transport.h"

#include <gtest/gtest.h>
#include <zmq.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <unistd.h>

using namespace industrial_bus::transport;

TEST(ZeroMqTransportTest, ExchangesFramesAndClosesPromptly) {
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
    if (!listen_result &&
        listen_result.error().message.find("Operation not permitted") !=
            std::string::npos) {
        GTEST_SKIP() << "sandbox denies socket creation: "
                     << listen_result.error().message;
    }
    ASSERT_TRUE(listen_result) << listen_result.error().message;

    ZeroMqDealerConnector connector;
    auto connected = connector.connect(endpoint).get();
    ASSERT_TRUE(connected) << connected.error().message;
    auto client = std::move(connected).value();
    const ByteBuffer payload{9, 8, 7};
    const auto client_send = client->send(ByteView(payload));
    ASSERT_TRUE(client_send) << client_send.error().message;

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(2), [&] {
            return server_received == payload;
        })) << "server receive timed out";
    }
    ASSERT_NE(server, nullptr) << "server peer was not accepted";

    ByteBuffer client_received;
    client->setReceiveHandler([&](ByteBuffer frame) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            client_received = std::move(frame);
        }
        ready.notify_all();
    });
    const auto server_send = server->send(ByteView(payload));
    ASSERT_TRUE(server_send) << server_send.error().message;
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(2), [&] {
            return client_received == payload;
        })) << "client receive timed out";
    }

    const auto close_started = std::chrono::steady_clock::now();
    listener.close();
    const auto close_elapsed = std::chrono::steady_clock::now() - close_started;
    EXPECT_LT(close_elapsed, std::chrono::seconds(1));
    client->close();
    std::remove(socket_path.c_str());
}

TEST(ZeroMqTransportTest, AcceptsReconnectedPeerWithSameRoutingIdentity) {
    const std::string socket_path =
        "/tmp/industrial_bus_zmq_reconnect_" +
        std::to_string(static_cast<long>(::getpid())) + ".sock";
    const Endpoint endpoint{"ipc://" + socket_path};

    ZeroMqRouterListener listener;
    std::mutex mutex;
    std::condition_variable ready;
    std::size_t accept_count = 0;
    std::shared_ptr<IConnection> server;
    ByteBuffer server_received;
    const auto listen_result = listener.listen(
        endpoint, [&](std::shared_ptr<IConnection> connection) {
            connection->setReceiveHandler([&](ByteBuffer frame) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    server_received = std::move(frame);
                }
                ready.notify_all();
            });
            {
                std::lock_guard<std::mutex> lock(mutex);
                server = std::move(connection);
                ++accept_count;
            }
            ready.notify_all();
        });
    if (!listen_result &&
        listen_result.error().message.find("Operation not permitted") !=
            std::string::npos) {
        GTEST_SKIP() << "sandbox denies socket creation: "
                     << listen_result.error().message;
    }
    ASSERT_TRUE(listen_result) << listen_result.error().message;

    zmq::context_t context(1);
    constexpr const char* routing_identity = "reconnecting-test-peer";
    auto connect_and_send = [&](const ByteBuffer& payload) {
        auto dealer = std::make_unique<zmq::socket_t>(context, zmq::socket_type::dealer);
        dealer->set(zmq::sockopt::linger, 0);
        dealer->set(zmq::sockopt::routing_id, routing_identity);
        dealer->connect(endpoint.value);
        const auto sent = dealer->send(zmq::buffer(payload), zmq::send_flags::none);
        EXPECT_TRUE(sent.has_value());
        return dealer;
    };

    const ByteBuffer first_payload{1, 2, 3};
    auto first_dealer = connect_and_send(first_payload);
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(2), [&] {
            return accept_count == 1 && server_received == first_payload;
        })) << "first peer was not accepted";
    }

    std::shared_ptr<IConnection> first_server;
    {
        std::lock_guard<std::mutex> lock(mutex);
        first_server = server;
    }
    ASSERT_NE(first_server, nullptr);
    first_server->close();
    first_dealer->close();

    const ByteBuffer second_payload{4, 5, 6};
    auto second_dealer = connect_and_send(second_payload);
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(2), [&] {
            return accept_count == 2 && server_received == second_payload;
        })) << "reconnected peer with the same routing identity was not accepted";
        EXPECT_NE(server, first_server);
    }

    second_dealer->close();
    listener.close();
    std::remove(socket_path.c_str());
}
