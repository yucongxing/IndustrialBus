#include "industrial_bus/transport/transport.h"
#include "loopback_transport.h"

#include <cassert>
#include <memory>
#include <vector>

using namespace industrial_bus::transport;

int main() {
    LoopbackHub hub;
    std::shared_ptr<IConnection> server;
    const auto listening = hub.listen({"loopback://unit"},
                                      [&](std::shared_ptr<IConnection> connection) {
                                          server = std::move(connection);
                                      });
    assert(listening);

    auto connected = hub.connect({"loopback://unit"}).get();
    assert(connected);
    auto client = std::move(connected).value();
    assert(server);

    ByteBuffer received;
    server->setReceiveHandler([&](ByteBuffer frame) { received = std::move(frame); });
    const ByteBuffer sent{1, 2, 3, 4};
    assert(client->send(ByteView(sent)));
    assert(received == sent);

    ByteBuffer reply;
    client->setReceiveHandler([&](ByteBuffer frame) { reply = std::move(frame); });
    assert(server->send(ByteView(sent)));
    assert(reply == sent);

    client->close();
    assert(client->state() == ConnectionState::closed);
    assert(server->state() == ConnectionState::disconnected);
    assert(!server->send(ByteView(sent)));
}
