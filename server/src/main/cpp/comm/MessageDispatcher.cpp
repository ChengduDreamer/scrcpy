#include "QtScrcpyComm/MessageDispatcher.h"

namespace qsc {

void MessageDispatcher::OnCtrlCommand(CtrlCommandHandler h) {
    m_ctrlCmdHandler = std::move(h);
}
void MessageDispatcher::OnCtrlResponse(CtrlResponseHandler h) {
    m_ctrlRespHandler = std::move(h);
}
void MessageDispatcher::OnFileAnnounce(FileAnnounceHandler h) {
    m_fileAnnHandler = std::move(h);
}
void MessageDispatcher::OnFileChunk(FileChunkHandler h) {
    m_fileChunkHandler = std::move(h);
}
void MessageDispatcher::OnFileDone(FileDoneHandler h) {
    m_fileDoneHandler = std::move(h);
}
void MessageDispatcher::OnFileRequest(FileRequestHandler h) {
    m_fileReqHandler = std::move(h);
}
void MessageDispatcher::OnClipData(ClipDataHandler h) {
    m_clipHandler = std::move(h);
}
void MessageDispatcher::OnHeartbeat(HeartbeatHandler h) {
    m_hbHandler = std::move(h);
}
void MessageDispatcher::OnDeviceInfo(DeviceInfoHandler h) {
    m_devInfoHandler = std::move(h);
}
void MessageDispatcher::SetDefaultHandler(DefaultHandler h) {
    m_defaultHandler = std::move(h);
}

bool MessageDispatcher::Dispatch(const proto::WsMessage &msg) {
    uint32_t seq = msg.seq();
    using B = proto::WsMessage;

    switch (msg.body_case()) {
    case B::kCtrlCmd:
        if (m_ctrlCmdHandler) { m_ctrlCmdHandler(msg.ctrl_cmd(), seq); return true; }
        break;
    case B::kCtrlResp:
        if (m_ctrlRespHandler) { m_ctrlRespHandler(msg.ctrl_resp(), seq); return true; }
        break;
    case B::kFileAnn:
        if (m_fileAnnHandler) { m_fileAnnHandler(msg.file_ann(), seq); return true; }
        break;
    case B::kFileChunk:
        if (m_fileChunkHandler) { m_fileChunkHandler(msg.file_chunk(), seq); return true; }
        break;
    case B::kFileDone:
        if (m_fileDoneHandler) { m_fileDoneHandler(msg.file_done(), seq); return true; }
        break;
    case B::kFileReq:
        if (m_fileReqHandler) { m_fileReqHandler(msg.file_req(), seq); return true; }
        break;
    case B::kClipData:
        if (m_clipHandler) { m_clipHandler(msg.clip_data(), seq); return true; }
        break;
    case B::kHb:
        if (m_hbHandler) { m_hbHandler(msg.hb(), seq); return true; }
        break;
    case B::kDevInfo:
        if (m_devInfoHandler) { m_devInfoHandler(msg.dev_info(), seq); return true; }
        break;
    default:
        break;
    }

    if (m_defaultHandler) {
        m_defaultHandler(msg);
        return true;
    }
    return false;
}

}  // namespace qsc
