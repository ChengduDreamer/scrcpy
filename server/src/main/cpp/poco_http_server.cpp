#include "poco_http_server.h"

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>

#include <android/log.h>

#define TAG "scrcpy-poco"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace scrcpy {

// Simple request handler: responds "hello" to GET /
class HelloHandler : public Poco::Net::HTTPRequestHandler {
public:
    void handleRequest(Poco::Net::HTTPServerRequest &request,
                       Poco::Net::HTTPServerResponse &response) override {
        LOGI("HTTP %s %s", request.getMethod().c_str(),
             request.getURI().c_str());

        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        response.setContentType("text/plain; charset=utf-8");
        response.setContentLength(5);
        std::ostream &out = response.send();
        out << "hello";
    }
};

class HelloFactory : public Poco::Net::HTTPRequestHandlerFactory {
public:
    Poco::Net::HTTPRequestHandler *createRequestHandler(
        const Poco::Net::HTTPServerRequest &) override {
        return new HelloHandler();
    }
};

PocoHttpServer::PocoHttpServer() {}

PocoHttpServer::~PocoHttpServer() { Stop(); }

bool PocoHttpServer::Start(int port) {
    if (m_running) return true;

    try {
        m_socket = std::make_unique<Poco::Net::ServerSocket>(
            static_cast<Poco::UInt16>(port));
        Poco::Net::HTTPServerParams *params =
            new Poco::Net::HTTPServerParams();
        params->setMaxQueued(10);
        params->setMaxThreads(1);

        m_server = std::make_unique<Poco::Net::HTTPServer>(
            new HelloFactory(), *m_socket, params);
        m_server->start();
        m_running = true;
        LOGI("Poco HTTP server started on port %d", port);
        return true;
    } catch (const std::exception &e) {
        LOGE("Poco HTTP server start failed: %s", e.what());
        return false;
    }
}

void PocoHttpServer::Stop() {
    if (!m_running) return;
    m_running = false;
    try {
        m_server->stopAll(true);
    } catch (...) {}
    m_server.reset();
    m_socket.reset();
    LOGI("Poco HTTP server stopped");
}

bool PocoHttpServer::IsRunning() const { return m_running; }

}  // namespace scrcpy
