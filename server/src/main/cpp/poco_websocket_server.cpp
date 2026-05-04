#include "poco_websocket_server.h"

#include <android/log.h>

#include <Poco/Buffer.h>
#include <Poco/String.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/WebSocket.h>

#include <cstddef>
#include <string>

#define TAG "scrcpy-poco-ws"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace scrcpy {

namespace {

// Qt / browser clients must use: ws://host:port<kWebSocketPath>
// e.g. ws://127.0.0.1:29747/ws  (query string allowed: /ws?x=1 uses path /ws)
static constexpr char kWebSocketPath[] = "/mirror";

static std::string UriPath(const std::string &uri) {
    const auto q = uri.find('?');
    if (q == std::string::npos) {
        return uri;
    }
    return uri.substr(0, q);
}

static bool IsAllowedWebSocketPath(const std::string &uri) {
    std::string p = UriPath(uri);
    while (p.size() > 1 && p.back() == '/') {
        p.pop_back();
    }
    return Poco::icompare(p, std::string(kWebSocketPath)) == 0;
}

class NotFoundHandler : public Poco::Net::HTTPRequestHandler {
public:
    void handleRequest(Poco::Net::HTTPServerRequest & /*request*/,
                       Poco::Net::HTTPServerResponse &response) override {
        response.setStatus(Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
        response.setContentType("text/plain");
        response.setContentLength(0);
        response.send();
    }
};

// One WebSocket *frame* is always fully received by receiveFrame() (including
// payload); FIN marks the last *fragment of an application message*, which may
// span multiple frames — we reassemble TEXT/BINARY + CONT until FIN, then echo.
constexpr std::size_t kMaxReassembledMessage = 64 * 1024 * 1024;

static void EchoCompleteMessage(Poco::Net::WebSocket &ws,
                                const Poco::Buffer<char> &payload, int textOrBinaryOp) {
    using Poco::Net::WebSocket;
    const int sz = static_cast<int>(payload.size());
    const char *data = sz > 0 ? payload.begin() : "";
    if (textOrBinaryOp == WebSocket::FRAME_OP_BINARY) {
        ws.sendFrame(data, sz, WebSocket::FRAME_BINARY);
    } else {
        ws.sendFrame(data, sz, WebSocket::FRAME_TEXT);
    }
}

// Echo binary/text *messages* (possibly fragmented); PING→PONG; CLOSE ends.
class WebSocketRequestHandler : public Poco::Net::HTTPRequestHandler {
public:
    void handleRequest(Poco::Net::HTTPServerRequest &request,
                       Poco::Net::HTTPServerResponse &response) override {
        using Poco::Net::WebSocket;
        using Poco::Net::WebSocketException;

        try {
            WebSocket ws(request, response);
            constexpr int kMaxPayload = 64 * 1024 * 1024;
            ws.setMaxPayloadSize(kMaxPayload);

            Poco::Buffer<char> frameBuf(0);
            Poco::Buffer<char> messageAccum(0);
            // -1 = not assembling; else FRAME_OP_TEXT or FRAME_OP_BINARY
            int messageOpcode = -1;
            int flags = 0;

            for (;;) {
                frameBuf.resize(0);
                int n = ws.receiveFrame(frameBuf, flags);
                if (n < 0) {
                    continue;
                }
                if (n == 0 && flags == 0) {
                    break;
                }

                const int op = flags & WebSocket::FRAME_OP_BITMASK;
                const bool fin =
                    (flags & WebSocket::FRAME_FLAG_FIN) != 0;

                if (op == WebSocket::FRAME_OP_CLOSE) {
                    break;
                }
                if (op == WebSocket::FRAME_OP_PING) {
                    const char *payload = n > 0 ? frameBuf.begin() : "";
                    ws.sendFrame(payload, n, WebSocket::FRAME_FLAG_FIN |
                                                  WebSocket::FRAME_OP_PONG);
                    continue;
                }
                if (op == WebSocket::FRAME_OP_PONG) {
                    continue;
                }

                // Data message reassembly (TEXT / BINARY / CONT)
                if (op == WebSocket::FRAME_OP_CONT) {
                    if (messageOpcode < 0) {
                        LOGE("CONT frame without leading TEXT/BINARY");
                        break;
                    }
                    if (static_cast<std::size_t>(n) >
                        kMaxReassembledMessage - messageAccum.size()) {
                        LOGE("reassembled message exceeds cap");
                        break;
                    }
                    if (n > 0) {
                        messageAccum.append(frameBuf.begin(),
                                            static_cast<std::size_t>(n));
                    }
                    if (fin) {
                        EchoCompleteMessage(ws, messageAccum, messageOpcode);
                        messageOpcode = -1;
                        messageAccum.resize(0, false);
                    }
                    continue;
                }

                if (op == WebSocket::FRAME_OP_TEXT ||
                    op == WebSocket::FRAME_OP_BINARY) {
                    if (messageOpcode >= 0) {
                        LOGE("new TEXT/BINARY while fragmented message unfinished");
                        break;
                    }
                    messageOpcode = op;
                    messageAccum.resize(0, false);
                    if (static_cast<std::size_t>(n) > kMaxReassembledMessage) {
                        LOGE("first fragment exceeds cap");
                        break;
                    }
                    if (n > 0) {
                        messageAccum.append(frameBuf.begin(),
                                            static_cast<std::size_t>(n));
                    }
                    if (fin) {
                        EchoCompleteMessage(ws, messageAccum, messageOpcode);
                        messageOpcode = -1;
                        messageAccum.resize(0, false);
                    }
                    continue;
                }

                LOGE("unexpected WebSocket opcode 0x%x", op);
                break;
            }
        } catch (const WebSocketException &exc) {
            LOGE("WebSocketException: %s", exc.displayText().c_str());
            switch (exc.code()) {
            case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
                response.set("Sec-WebSocket-Version",
                             WebSocket::WEBSOCKET_VERSION);
                // fallthrough
            case WebSocket::WS_ERR_NO_HANDSHAKE:
            case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
            case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
                response.setStatusAndReason(
                    Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                response.setContentLength(0);
                response.send();
                break;
            default:
                break;
            }
        } catch (const std::exception &e) {
            LOGE("WebSocket handler error: %s", e.what());
        }
    }
};

class WsHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory {
public:
    Poco::Net::HTTPRequestHandler *createRequestHandler(
        const Poco::Net::HTTPServerRequest &request) override {
        const bool wsUpgrade =
            request.find("Upgrade") != request.end() &&
            Poco::icompare(request["Upgrade"], "websocket") == 0;
        if (!wsUpgrade) {
            return new NotFoundHandler();
        }
        if (!IsAllowedWebSocketPath(request.getURI())) {
            LOGI("WebSocket upgrade ignored (path mismatch): %s (need %s)",
                 request.getURI().c_str(), kWebSocketPath);
            return new NotFoundHandler();
        }
        return new WebSocketRequestHandler();
    }
};

}  // namespace

PocoWebsocketServer::PocoWebsocketServer() {}

PocoWebsocketServer::~PocoWebsocketServer() { Stop(); }

bool PocoWebsocketServer::Start(int port) {
    if (m_running) {
        return true;
    }

    try {
        m_socket = std::make_unique<Poco::Net::ServerSocket>(
            static_cast<Poco::UInt16>(port));
        auto *params = new Poco::Net::HTTPServerParams();
        params->setMaxQueued(64);
        params->setMaxThreads(8);

        m_server = std::make_unique<Poco::Net::HTTPServer>(
            new WsHandlerFactory(), *m_socket, params);
        m_server->start();
        m_running = true;
        LOGI("Poco WebSocket server listening on port %d, path %s", port,
             kWebSocketPath);
        return true;
    } catch (const std::exception &e) {
        LOGE("Poco WebSocket server start failed: %s", e.what());
        m_server.reset();
        m_socket.reset();
        return false;
    }
}

void PocoWebsocketServer::Stop() {
    if (!m_running) {
        return;
    }
    m_running = false;
    try {
        if (m_server) {
            m_server->stopAll(true);
        }
    } catch (...) {
    }
    m_server.reset();
    m_socket.reset();
    LOGI("Poco WebSocket server stopped");
}

bool PocoWebsocketServer::IsRunning() const { return m_running; }

}  // namespace scrcpy
