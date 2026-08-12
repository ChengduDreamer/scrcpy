#!/usr/bin/env python3
"""Minimal injection test for the scrcpy notification relay (127.0.0.1:29748).

Prereq:
  adb forward tcp:29748 tcp:29748
  adb shell "echo -n test-token-123 > /data/local/tmp/mivox_agent_token && chmod 600 /data/local/tmp/mivox_agent_token"

Covers:
  1. wrong token  -> connection must be closed by the relay
  2. non-handshake first frame -> closed
  3. correct token -> accepted (connection stays open)
  4. kNotificationEvent after handshake -> accepted & forwarded to PC
     (with no WS client the relay logs "Dropped notification event: no PC channel";
      watch `adb logcat -s scrcpy-notif-relay`)
  5. oversized frame (> 64KB) -> closed

tc::Message encoding is hand-rolled protobuf (only the fields we need):
  Message.type                  = field 10  (varint)
  Message.notification_handshake= field 520 (msg) -> NotificationHandshake{token=1, agent_version=2}
  Message.notification_event    = field 530 (msg) -> NotificationEvent{key=1, package_name=2, ...}
"""

import socket
import struct
import sys

HOST, PORT = "127.0.0.1", 29748
TOKEN = b"test-token-123"
K_NOTIFICATION_HANDSHAKE = 530
K_NOTIFICATION_EVENT = 540


def varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def tag(field: int, wire: int) -> bytes:
    return varint((field << 3) | wire)


def f_varint(field: int, value: int) -> bytes:
    return tag(field, 0) + varint(value)


def f_bytes(field: int, payload: bytes) -> bytes:
    return tag(field, 2) + varint(len(payload)) + payload


def handshake_msg(token: bytes) -> bytes:
    hs = f_bytes(1, token) + f_bytes(2, b"inject-test/1.0") + f_varint(3, 0)
    return f_varint(10, K_NOTIFICATION_HANDSHAKE) + f_bytes(520, hs)


def event_msg() -> bytes:
    ev = (
        f_bytes(1, b"test-key-1")
        + f_bytes(2, b"com.example.app")
        + f_bytes(3, "示例应用".encode())
        + f_bytes(4, "测试标题".encode())
        + f_bytes(5, "relay injection test body".encode())
        + f_varint(6, 1723000000000)
        + f_varint(7, 0)  # kNotifPosted
    )
    return f_varint(10, K_NOTIFICATION_EVENT) + f_bytes(530, ev)


def send_frame(sock: socket.socket, payload: bytes) -> None:
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def conn_closed_by_peer(sock: socket.socket, timeout: float = 3.0) -> bool:
    sock.settimeout(timeout)
    try:
        data = sock.recv(1)
        return data == b""
    except (ConnectionResetError, ConnectionAbortedError):
        return True
    except socket.timeout:
        return False


def expect_closed(name: str, first_frame: bytes) -> bool:
    s = socket.create_connection((HOST, PORT), timeout=3)
    send_frame(s, first_frame)
    closed = conn_closed_by_peer(s)
    s.close()
    print(f"[{'PASS' if closed else 'FAIL'}] {name}: connection {'closed' if closed else 'still open'}")
    return closed


def main() -> int:
    ok = True

    # 1. wrong token -> rejected
    ok &= expect_closed("wrong token rejected", handshake_msg(b"wrong-token"))

    # 2. first frame is not a handshake -> rejected
    ok &= expect_closed("non-handshake first frame rejected", event_msg())

    # 3. correct token -> accepted, then event frame
    s = socket.create_connection((HOST, PORT), timeout=3)
    send_frame(s, handshake_msg(TOKEN))
    if conn_closed_by_peer(s, timeout=2.0):
        print("[FAIL] correct token: connection closed")
        s.close()
        return 1
    print("[PASS] correct token: handshake accepted")

    # 4. notification event -> relay forwards (or drops w/ log if no PC)
    send_frame(s, event_msg())
    if conn_closed_by_peer(s, timeout=2.0):
        print("[FAIL] event frame: connection closed after event")
        ok = False
    else:
        print("[PASS] event frame: connection alive (check logcat for forward/drop)")

    # 5. oversized frame -> closed
    send_frame(s, struct.pack(">I", 70 * 1024) + b"x" * 1024)
    if conn_closed_by_peer(s):
        print("[PASS] oversized frame: connection closed")
    else:
        print("[FAIL] oversized frame: connection still open")
        ok = False
    s.close()

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
