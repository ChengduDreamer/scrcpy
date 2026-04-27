#ifndef POCO_HTTP_SERVER_H
#define POCO_HTTP_SERVER_H

#include <atomic>
#include <memory>
#include <string>

namespace Poco {
namespace Net {
class HTTPServer;
class ServerSocket;
}
}  // namespace Poco

namespace scrcpy {

class PocoHttpServer {
public:
    PocoHttpServer();
    ~PocoHttpServer();

    bool Start(int port);
    void Stop();
    bool IsRunning() const;

private:
    std::unique_ptr<Poco::Net::ServerSocket> m_socket;
    std::unique_ptr<Poco::Net::HTTPServer> m_server;
    std::atomic<bool> m_running{false};
};

}  // namespace scrcpy

#endif  // POCO_HTTP_SERVER_H
