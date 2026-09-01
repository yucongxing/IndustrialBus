#include "industrial_bus/transport/transport.h"
#include "loopback_transport.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace industrial_bus::transport;

TEST(LoopbackTransportTest, ExchangesFramesAndPropagatesClose) {
    LoopbackHub hub;
    std::shared_ptr<IConnection> server;
    const auto listening = hub.listen({"loopback://unit"},
                                      [&](std::shared_ptr<IConnection> connection) {
                                          server = std::move(connection);
                                      });
    ASSERT_TRUE(listening) << listening.error().message;

    auto connected = hub.connect({"loopback://unit"}).get();
    ASSERT_TRUE(connected) << connected.error().message;
    auto client = std::move(connected).value();
    ASSERT_NE(server, nullptr);

    ByteBuffer received;
    server->setReceiveHandler([&](ByteBuffer frame) { received = std::move(frame); });
    const ByteBuffer sent{1, 2, 3, 4};
    const auto client_send = client->send(ByteView(sent));
    ASSERT_TRUE(client_send) << client_send.error().message;
    EXPECT_EQ(received, sent);

    ByteBuffer reply;
    client->setReceiveHandler([&](ByteBuffer frame) { reply = std::move(frame); });
    const auto server_send = server->send(ByteView(sent));
    ASSERT_TRUE(server_send) << server_send.error().message;
    EXPECT_EQ(reply, sent);

    client->close();
    EXPECT_EQ(client->state(), ConnectionState::closed);
    EXPECT_EQ(server->state(), ConnectionState::disconnected);
    EXPECT_FALSE(server->send(ByteView(sent)));
}
