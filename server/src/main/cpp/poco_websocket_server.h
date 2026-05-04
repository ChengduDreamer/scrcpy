#ifndef POCO_WEBSOCKET_SERVER_H
#define POCO_WEBSOCKET_SERVER_H

#include <atomic>
#include <memory>

namespace Poco {
namespace Net {
class HTTPServer;
class ServerSocket;
}
}  // namespace Poco

namespace scrcpy {

// Poco Net HTTPServer + WebSocket upgrade. Intended for binary frames between
// QtScrcpy (client) and this process; echoes binary/text and handles PING/CLOSE.
//
// Endpoint URL path is fixed in native code (see kWebSocketPath in
// poco_websocket_server.cpp), e.g. ws://host:port/ws
class PocoWebsocketServer {
public:
    PocoWebsocketServer();
    ~PocoWebsocketServer();

    bool Start(int port);
    void Stop();
    bool IsRunning() const;

private:
    std::unique_ptr<Poco::Net::ServerSocket> m_socket;
    std::unique_ptr<Poco::Net::HTTPServer> m_server;
    std::atomic<bool> m_running{false};
};

}  // namespace scrcpy

#endif  // POCO_WEBSOCKET_SERVER_H
