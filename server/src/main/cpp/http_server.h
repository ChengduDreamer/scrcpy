#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <atomic>
#include <string>
#include <thread>

namespace scrcpy {

// Minimal single-thread HTTP server for testing native code.
// Listens on a port, responds to GET / with a test page.
class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    bool Start(int port);
    void Stop();
    bool IsRunning() const;

private:
    void Run();
    std::string HandleRequest(const std::string &request);
    static std::string MakeResponse(int code, const std::string &body);

    int m_socket = -1;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

}  // namespace scrcpy

#endif  // HTTP_SERVER_H
