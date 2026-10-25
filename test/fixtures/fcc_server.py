#!/usr/bin/env python3
"""UDP fixture mimicking a Telecom FCC server (see
docs/rtp2httpd-reference.md for the packet facts):

  one FMT=2 request  -> FMT=3 ok (no redirect, speed fields set)
                      -> burst of 5 accelerated RTP MP2T packets pushed
                         back to the declared client port
                      -> FMT=4 sync notify
                      -> expects FMT=5 (stop 0, first-mcast-seq 0)

Usage: fcc_server.py <port>   (prints DONE when the FMT=5 arrives)
"""
import socket
import struct
import sys
import time

PORT = int(sys.argv[1])
GROUP = "225.1.1.1"
BASE_SEQ = 100

srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
srv.bind(("127.0.0.1", PORT))
srv.settimeout(10)

data, addr = srv.recvfrom(2048)
assert data[0] & 0x1f == 2 and data[1] == 205, data[:4].hex()
assert len(data) == 40, len(data)
assert data[4:8] == b"\x00\x00\x00\x00"            # sender SSRC 0
group_ip = data[20:24]
assert data[8:12] == group_ip, "media SSRC must be the group IP"
assert group_ip == socket.inet_aton(GROUP), group_ip
cli_port, grp_port = struct.unpack("!HH", data[16:20])
assert grp_port == 5000, grp_port
assert data[24:] == b"\x00" * 16, "request tail must be zero"

# FMT=3 ok: result 0, type 3, no redirect, 300s, 8 Mbps / 4 Mbps
fci = (bytes([0, 3]) + struct.pack("!HHH", 0, 0, 0) +
       socket.inet_aton("0.0.0.0") + struct.pack("!III", 300, 8000000, 4000000))
resp = (bytes([0x80 | 3, 205]) + struct.pack("!H", 8) +
        b"\x00\x00\x00\x00" + group_ip + fci)
assert len(resp) == 36, len(resp)
srv.sendto(resp, addr)

ssrc = struct.unpack("!I", group_ip)[0]
# paced burst: >1s end to end, so the host test can tell a stall detector
# that watches media arrival apart from one that only watches the FMT=3
for i in range(5):
    seq = BASE_SEQ + i
    payload = bytes([seq & 0xFF]) * (7 * 188)
    rtp = struct.pack("!BBHII", 0x80, 33, seq, i * 12600, ssrc) + payload
    srv.sendto(rtp, (addr[0], cli_port))
    if i < 4:
        time.sleep(0.3)

# FMT=4 sync notify (12 bytes)
srv.sendto(bytes([0x80 | 4, 205, 0, 2]) + b"\x00" * 4 + group_ip, addr)

stop, _ = srv.recvfrom(64)
srv.close()
assert stop[0] & 0x1f == 5 and stop[1] == 205 and len(stop) == 16, stop[:4].hex()
assert stop[8:12] == group_ip, "FMT=5 media SSRC must be the group IP"
assert stop[12] == 0, "normal stop expected"
assert struct.unpack("!H", stop[14:16])[0] == 0, "no multicast was seen"
print("DONE")
