#ifndef QSCRCPY_COMM_MESSAGE_DISPATCHER_H
#define QSCRCPY_COMM_MESSAGE_DISPATCHER_H

#include <QObject>
#include <functional>
#include <type_traits>
#include <unordered_map>

#include "messages.pb.h"

namespace qsc {

// Type-safe dispatcher for WsMessage.oneof body cases.
// Handlers register for a specific body type; dispatcher routes
// incoming WsMessages to the matching handler.
class MessageDispatcher {
public:
    using DefaultHandler =
        std::function<void(const proto::WsMessage &)>;

    // Handler for CtrlCommand
    using CtrlCommandHandler =
        std::function<void(const proto::CtrlCommand &, uint32_t seq)>;

    // Handler for CtrlResponse
    using CtrlResponseHandler =
        std::function<void(const proto::CtrlResponse &, uint32_t seq)>;

    // File transfer handlers
    using FileAnnounceHandler =
        std::function<void(const proto::FileAnnounce &, uint32_t seq)>;
    using FileChunkHandler =
        std::function<void(const proto::FileChunk &, uint32_t seq)>;
    using FileDoneHandler =
        std::function<void(const proto::FileDone &, uint32_t seq)>;
    using FileRequestHandler =
        std::function<void(const proto::FileRequest &, uint32_t seq)>;

    // Clipboard
    using ClipDataHandler =
        std::function<void(const proto::ClipData &, uint32_t seq)>;

    // System
    using HeartbeatHandler =
        std::function<void(const proto::Heartbeat &, uint32_t seq)>;
    using DeviceInfoHandler =
        std::function<void(const proto::DeviceInfo &, uint32_t seq)>;

    // ── Register handlers ──
    void OnCtrlCommand(CtrlCommandHandler handler);
    void OnCtrlResponse(CtrlResponseHandler handler);
    void OnFileAnnounce(FileAnnounceHandler handler);
    void OnFileChunk(FileChunkHandler handler);
    void OnFileDone(FileDoneHandler handler);
    void OnFileRequest(FileRequestHandler handler);
    void OnClipData(ClipDataHandler handler);
    void OnHeartbeat(HeartbeatHandler handler);
    void OnDeviceInfo(DeviceInfoHandler handler);

    // Fallback for unhandled body types
    void SetDefaultHandler(DefaultHandler handler);

    // Dispatch a received WsMessage to the registered handler.
    // Returns false if no handler matched (and no default handler).
    bool Dispatch(const proto::WsMessage &msg);

private:
    CtrlCommandHandler   m_ctrlCmdHandler;
    CtrlResponseHandler  m_ctrlRespHandler;
    FileAnnounceHandler  m_fileAnnHandler;
    FileChunkHandler     m_fileChunkHandler;
    FileDoneHandler      m_fileDoneHandler;
    FileRequestHandler   m_fileReqHandler;
    ClipDataHandler      m_clipHandler;
    HeartbeatHandler     m_hbHandler;
    DeviceInfoHandler    m_devInfoHandler;
    DefaultHandler       m_defaultHandler;
};

}  // namespace qsc

#endif  // QSCRCPY_COMM_MESSAGE_DISPATCHER_H
