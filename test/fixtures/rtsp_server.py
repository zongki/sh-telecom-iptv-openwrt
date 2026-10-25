#!/usr/bin/env python3
"""RTSP fixture mimicking the FonsView timeshift server shape:

  conn1: DESCRIBE /entry  -> 302 -> /hop2
  conn2: DESCRIBE /hop2   -> 200 + Content-Base (AuthInfo + DURATION window)
  conn3: SETUP            -> 200 Session
        PLAY              -> 200, then $-interleaved TS frames

Usage: rtsp_server.py <port>
"""
import socket
import sys
import threading

PORT = int(sys.argv[1])
WINDOW = "20260101T000000Z-20260101T040000Z"


def read_request(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(65536)
        if not chunk:
            return None
        data += chunk
    return data.decode()


def handle(conn):
    try:
        req = read_request(conn)
        if not req:
            return
        first = req.split("\r\n")[0]
        method = first.split(" ")[0]
        if method == "DESCRIBE":
            if "/hop2" in first:
                cb = ("rtsp://127.0.0.1:%d/LIVE/77?AuthInfo=abc&"
                      "TRANSPORT=MP2T/TCP&DURATION=%s" % (PORT, WINDOW))
                conn.sendall(("RTSP/1.0 200 OK\r\nCSeq: 2\r\n"
                              "Content-Base: %s\r\n"
                              "Content-Type: application/sdp\r\n"
                              "Content-Length: 10\r\n\r\nv=0\r\n" % cb).encode())
            else:
                conn.sendall(("RTSP/1.0 302 Moved\r\nCSeq: 1\r\n"
                              "Location: rtsp://127.0.0.1:%d/hop2\r\n\r\n"
                              % PORT).encode())
        elif method == "SETUP":
            assert "MP2T/TCP;interleaved=0-1" in req, req
            conn.sendall(("RTSP/1.0 200 OK\r\nCSeq: 10\r\n"
                          "Session: SESS1;timeout=60\r\n"
                          "Transport: MP2T/TCP;interleaved=0-1\r\n\r\n").encode())
            req2 = read_request(conn)
            assert req2 and req2.startswith("PLAY"), req2
            assert "Session: SESS1" in req2, req2
            if "Range:" in req2:
                assert "Range: clock=2026" in req2 or \
                       "Range: clock=19" in req2 or "Range: clock=20" in req2, req2
            conn.sendall(("RTSP/1.0 200 OK\r\nCSeq: 11\r\n"
                          "Session: SESS1\r\n\r\n").encode())
            # three interleaved frames: 188-byte TS payloads (0x47 sync)
            for seq in range(3):
                payload = bytes([0x47, seq]) + bytes([seq % 251]) * 186
                conn.sendall(b"$\x00" + len(payload).to_bytes(2, "big") + payload)
    finally:
        conn.close()


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(8)
while True:
    conn, _ = srv.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
