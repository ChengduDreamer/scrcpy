#ifndef POCO_WEBSOCKET_SERVER_H
#define POCO_WEBSOCKET_SERVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

#include <Poco/Net/WebSocket.h>

namespace Poco {
namespace Net {
class HTTPServer;
class ServerSocket;
}  // namespace Net
}  // namespace Poco

namespace scrcpy {

// Poco Net HTTPServer + WebSocket upgrade. Handles binary proto frames
// between QtScrcpy (client) and this process.
//
// Architecture (3 threads, all server-lifetime):
//   Handler thread (Poco-managed): receiveFrame → enqueue to work queue
//   Work thread:                   dequeue → parse → callback (business logic)
//   Send thread:                   dequeue → sendFrame (exclusive WebSocket write)
//
// This separation ensures:
//   - receiveFrame and sendFrame never run concurrently on the same socket
//   - handler thread stays responsive (no blocking on business logic)
//   - send operations are serialized (no frame interleaving)
//   - thread lifetime is independent of connection lifecycle (no races)
//
// Endpoint URL path is fixed: ws://host:port/mirror
class PocoWebsocketServer {
public:
    using BinaryMessageHandler = std::function<void(const uint8_t* data, size_t len)>;
    using ConnectionStateHandler = std::function<void(bool connected)>;

    // Queue capacity (frames, not bytes). Tune for memory vs throughput.
    static constexpr size_t kMaxSendQueueSize = 512;
    static constexpr size_t kMaxWorkQueueSize = 256;

    PocoWebsocketServer();
    ~PocoWebsocketServer();

    bool Start(int port);
    void Stop();
    bool IsRunning() const;

    // Enqueue binary data for async sending. Thread-safe, non-blocking.
    bool SendBinary(const void* data, size_t len);
    bool HasConnection() const;

    // Register a handler for incoming binary messages.
    void SetOnBinaryMessage(BinaryMessageHandler handler);

    // Register a handler for connection state changes.
    void SetOnConnectionStateChanged(ConnectionStateHandler handler);

    // Connection lifecycle management (called by WebSocketRequestHandler).
    void AddConnection(const Poco::Net::WebSocket& ws);
    void RemoveConnection(const Poco::Net::WebSocket& ws);

    // Enqueue received binary data for async processing (called by handler thread).
    void EnqueueWork(std::vector<uint8_t> data);

    // Route PING response through the send queue (called by handler thread).
    void SendPong(std::vector<uint8_t> payload);

private:
    struct SendItem {
        std::vector<uint8_t> data;
        bool isPong{false};   // true = PONG control frame
        bool isClose{false};  // true = CLOSE control frame (graceful shutdown)
    };

    // HTTP server
    std::unique_ptr<Poco::Net::ServerSocket> m_socket;
    std::unique_ptr<Poco::Net::HTTPServer> m_server;
    std::atomic<bool> m_running{false};

    // Active WebSocket connection (single client)
    mutable std::mutex m_connectionMutex;
    std::optional<Poco::Net::WebSocket> m_connection;

    // Binary message handler callback
    BinaryMessageHandler m_onBinaryMessage;

    // Connection state change callback
    ConnectionStateHandler m_onConnectionStateChanged;

    // --- Send queue + dedicated send thread (server-lifetime) ---
    // Ensures sendFrame runs exclusively, never concurrent with receiveFrame.
    mutable std::mutex m_sendMutex;
    std::condition_variable m_sendCv;
    std::queue<SendItem> m_sendQueue;
    std::thread m_sendThread;

    // --- Work queue + dedicated work thread (server-lifetime) ---
    // Decouples handler thread from business logic processing.
    mutable std::mutex m_workMutex;
    std::condition_variable m_workCv;
    std::queue<std::vector<uint8_t>> m_workQueue;
    std::thread m_workThread;

    void SendLoop();
    void WorkLoop();

    void NotifyConnectionState(bool connected);
    size_t GetSendQueueDepth() const;
    size_t GetWorkQueueDepth() const;
};

}  // namespace scrcpy

#endif  // POCO_WEBSOCKET_SERVER_H
