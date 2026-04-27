#ifndef QSCRCPY_COMM_BINARY_FRAME_CODEC_H
#define QSCRCPY_COMM_BINARY_FRAME_CODEC_H

#include <cstdint>
#include <functional>
#include <vector>

namespace qsc {

// Wraps proto-encoded payload in a length-prefixed binary frame for
// WebSocket transport.  Frame layout:
//   [magic:4] [payload_len:4] [payload:N]
// Magic = "QTcP" = 0x51544350
// All integers big-endian.
class BinaryFrameCodec {
public:
    static constexpr uint32_t kMagic = 0x51544350;
    static constexpr size_t kHeaderSize = 8;

    using FrameCallback =
        std::function<void(const uint8_t *payload, size_t len)>;

    // Encode a payload into a framed byte array (for sending).
    static std::vector<uint8_t> Encode(const uint8_t *payload,
                                        size_t len);

    // Encode a vector payload
    static std::vector<uint8_t> Encode(const std::vector<uint8_t> &payload);

    // Feed raw bytes from WebSocket; calls cb for each complete frame.
    // Thread-safe if called sequentially from same thread.
    void Feed(const uint8_t *data, size_t len, FrameCallback cb);

    // Reset parser state (e.g. on reconnect).
    void Reset();

private:
    enum class State { kMagic, kHeader, kPayload };

    State m_state = State::kMagic;
    std::vector<uint8_t> m_buffer;
    size_t m_headerRead = 0;
    uint32_t m_pendingLen = 0;
    size_t m_payloadRead = 0;
};

}  // namespace qsc

#endif  // QSCRCPY_COMM_BINARY_FRAME_CODEC_H
