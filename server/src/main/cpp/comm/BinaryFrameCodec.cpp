#include "QtScrcpyComm/BinaryFrameCodec.h"

#include <algorithm>
#include <cstring>

namespace qsc {

std::vector<uint8_t> BinaryFrameCodec::Encode(const uint8_t *payload,
                                               size_t len) {
    std::vector<uint8_t> frame(kHeaderSize + len);

    // Magic: "QTcP"
    frame[0] = 'Q';
    frame[1] = 'T';
    frame[2] = 'c';
    frame[3] = 'P';

    // Length: big-endian
    uint32_t plen = static_cast<uint32_t>(len);
    frame[4] = static_cast<uint8_t>((plen >> 24) & 0xFF);
    frame[5] = static_cast<uint8_t>((plen >> 16) & 0xFF);
    frame[6] = static_cast<uint8_t>((plen >> 8) & 0xFF);
    frame[7] = static_cast<uint8_t>(plen & 0xFF);

    if (len > 0) {
        std::memcpy(&frame[kHeaderSize], payload, len);
    }
    return frame;
}

std::vector<uint8_t> BinaryFrameCodec::Encode(
    const std::vector<uint8_t> &payload) {
    return Encode(payload.data(), payload.size());
}

void BinaryFrameCodec::Feed(const uint8_t *data, size_t len,
                             FrameCallback cb) {
    if (!cb) return;

    const uint8_t *p = data;
    const uint8_t *end = data + len;

    while (p < end) {
        switch (m_state) {
        case State::kMagic: {
            // Collect bytes until we match magic
            while (m_buffer.size() < 4 && p < end) {
                m_buffer.push_back(*p++);
            }
            if (m_buffer.size() >= 4) {
                if (m_buffer[0] == 'Q' && m_buffer[1] == 'T' &&
                    m_buffer[2] == 'c' && m_buffer[3] == 'P') {
                    m_headerRead = 4;
                    m_state = State::kHeader;
                    m_buffer.clear();
                } else {
                    // Shift one byte and retry
                    m_buffer.erase(m_buffer.begin());
                }
            }
            break;
        }
        case State::kHeader: {
            while (m_buffer.size() < 4 && p < end) {
                m_buffer.push_back(*p++);
            }
            if (m_buffer.size() >= 4) {
                m_pendingLen = 0;
                m_pendingLen |= static_cast<uint32_t>(m_buffer[0]) << 24;
                m_pendingLen |= static_cast<uint32_t>(m_buffer[1]) << 16;
                m_pendingLen |= static_cast<uint32_t>(m_buffer[2]) << 8;
                m_pendingLen |= static_cast<uint32_t>(m_buffer[3]);
                m_buffer.clear();
                m_payloadRead = 0;
                if (m_pendingLen == 0) {
                    cb(nullptr, 0);
                    m_state = State::kMagic;
                } else {
                    m_state = State::kPayload;
                }
            }
            break;
        }
        case State::kPayload: {
            size_t remain = m_pendingLen - m_payloadRead;
            size_t take = std::min(remain, static_cast<size_t>(end - p));
            m_buffer.insert(m_buffer.end(), p, p + take);
            p += take;
            m_payloadRead += take;
            if (m_payloadRead >= m_pendingLen) {
                cb(m_buffer.data(), m_buffer.size());
                m_buffer.clear();
                m_state = State::kMagic;
            }
            break;
        }
        }
    }
}

void BinaryFrameCodec::Reset() {
    m_state = State::kMagic;
    m_buffer.clear();
    m_headerRead = 0;
    m_pendingLen = 0;
    m_payloadRead = 0;
}

}  // namespace qsc
