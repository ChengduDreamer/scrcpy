#include "http_server.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

#define TAG "scrcpy-http"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace scrcpy {

HttpServer::HttpServer() {}

HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Start(int port) {
    if (m_running) return true;

    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return false;
    }

    // Allow immediate rebind
    int opt = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(m_socket, reinterpret_cast<sockaddr *>(&addr),
             sizeof(addr)) < 0) {
        LOGE("bind() failed: %s", strerror(errno));
        close(m_socket);
        return false;
    }

    if (listen(m_socket, 5) < 0) {
        LOGE("listen() failed: %s", strerror(errno));
        close(m_socket);
        return false;
    }

    m_running = true;
    m_thread = std::thread(&HttpServer::Run, this);
    LOGI("HTTP server started on port %d", port);
    return true;
}

void HttpServer::Stop() {
    if (!m_running) return;
    m_running = false;
    // Shutdown socket to unblock accept()
    shutdown(m_socket, SHUT_RDWR);
    close(m_socket);
    if (m_thread.joinable()) m_thread.join();
    LOGI("HTTP server stopped");
}

bool HttpServer::IsRunning() const { return m_running; }

void HttpServer::Run() {
    while (m_running) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int client =
            accept(m_socket, reinterpret_cast<sockaddr *>(&clientAddr),
                   &clientLen);
        if (client < 0) {
            if (m_running)
                LOGE("accept() failed: %s", strerror(errno));
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
        LOGI("accepted connection from %s:%d", ip,
             ntohs(clientAddr.sin_port));

        // Read request (small buffer to be thread-stack-friendly)
        char buf[1024] = {};
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            LOGE("recv failed: %s (n=%zd)", strerror(errno), n);
            close(client);
            continue;
        }
        buf[n] = '\0';
        LOGI("received %zd bytes", n);

        std::string response = HandleRequest(std::string(buf));
        ssize_t sent = send(client, response.data(), response.size(), 0);
        LOGI("sent %zd / %zu bytes", sent, response.size());
        if (sent < 0) {
            LOGE("send failed: %s", strerror(errno));
        }

        // Shutdown write to signal EOF to client before close
        shutdown(client, SHUT_WR);
        close(client);
    }
}

std::string HttpServer::HandleRequest(const std::string &request) {
    // DEBUG: log first line
    std::istringstream ss(request);
    std::string firstLine;
    std::getline(ss, firstLine);
    LOGI("HTTP request: %s", firstLine.c_str());

    // Serve test page for GET /
    if (request.find("GET / ") == 0 || request.find("GET / HTTP") == 0) {
        std::string body = R"(<!DOCTYPE html>
<html>
<head><title>scrcpy-native test</title></head>
<body>
<h1>scrcpy-native HTTP Server</h1>
<p>Native C++ code is running!</p>
<ul>
  <li>Library: libscrcpy_native.so</li>
  <li>Thread: )" + std::to_string(
              std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                           R"(</li>
</ul>
</body>
</html>)";

        std::string resp;
        resp += "HTTP/1.1 200 OK\r\n";
        resp += "Content-Type: text/html; charset=utf-8\r\n";
        resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        resp += "Connection: close\r\n";
        resp += "\r\n";
        resp += body;
        return resp;
    }

    return MakeResponse(404, "Not Found");
}

std::string HttpServer::MakeResponse(int code, const std::string &body) {
    const char *msg = code == 200 ? "OK" : "Not Found";
    std::string resp;
    resp += "HTTP/1.1 " + std::to_string(code) + " " + msg + "\r\n";
    resp += "Content-Type: text/plain; charset=utf-8\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;
    return resp;
}

}  // namespace scrcpy
