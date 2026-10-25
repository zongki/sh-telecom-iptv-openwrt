#!/usr/bin/env python3
"""Local fixture mimicking the Shanghai Telecom auth chain shape, so the
C implementation of auth_run_full can be tested end to end on the host.

Usage: auth_server.py <port>   (logs each hit to stdout)

Mimics: 4kLogAuth.do form -> r4kLogAuth (302 redirect, encrytoken) ->
ottauth (authenticator check, usertoken + index.jsp form) -> loadbalance
(top.document.location) -> funcportalauth -> jsSetConfig SessionID/
framecode/IpPort -> portal.jsp (cookie asserted) -> AuthByAjax JSON.
One response uses chunked encoding and one sends a decoy Set-Cookie.

Also serves the 0.5 TVOD shape (getTvodPlayUrl -> playURL that 302s to
a master playlist -> relative variant -> media playlist with an
absolute segment, a 404 segment (skip path) and a chunked segment).
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    hits = []

    def log_message(self, *a):
        pass

    def _send(self, body, status=200, headers=()):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _chunked(self, body, status=200, headers=()):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for i in range(0, len(body), 7):
            part = body[i:i + 7]
            self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
        self.wfile.write(b"0\r\n\r\n")

    def _stb_ua(self):
        assert self.headers.get("User-Agent") == "HMTL RTSP 1.0; CTC/2.0", \
            "HLS fetches must carry the STB UA"

    def do_GET(self):
        u = urlparse(self.path)
        self.hits.append(("GET", u.path))
        if u.path == "/iptv3a/4kLogAuth.do":
            q = parse_qs(u.query)
            assert q["Action"] == ["Login"], q
            assert q["UserID"] == ["12345678@etv1"], q
            assert q["Type"] == ["iptv4k"], q
            self._send(
                b"<html><form action='/iptv3a/r4kLogAuth.do' method='POST'>"
                b"<input name='u' value='UU'><input name='p' value='PP'>"
                b"</form></html>")
        elif u.path == "/r4kStep2.jsp":
            # reached via 302 from the POST below; carries encrytoken.
            # The empty leading "authenticator" mirrors the real server:
            # the client must fill it in place, never append a duplicate
            # (the board got hderror 2901 exactly from that duplicate).
            self._chunked(
                b"<html><script>var encrytoken = \"ETK-XYZ-9\";</script>"
                b"<form action='http://127.0.0.1:%d/ottauth.do' method='post'>"
                b"<input name='authenticator' value=''>"
                b"<input name='uid' value='12345678@etv1'>"
                b"</form></html>" % PORT)
        elif u.path == "/lb.jsp":
            self._send(
                b"<html><form action='/epg/funcportalauth' method='POST'>"
                b"<input name='auth' value='yes'></form></html>",
                headers=[("Set-Cookie", "ziko=decoy; Path=/")])
        elif u.path == "/tvod/entry.m3u8":
            # playURL hop: 302 to the playlist server, like :8006 -> :8112
            self._send(b"moved", 302, [("Location",
                                        "http://127.0.0.1:%d/tvod/pl.m3u8" % PORT)])
        elif u.path == "/tvod/pl.m3u8":
            # master playlist naming a relative variant
            self._stb_ua()
            self._send(b"#EXTM3U\n"
                       b"#EXT-X-STREAM-INF:BANDWIDTH=1000000\n"
                       b"media/med.m3u8\n")
        elif u.path == "/tvod/media/med.m3u8":
            # absolute seg / 404 relative seg / chunked relative seg
            self._stb_ua()
            self._send(b"#EXTM3U\n"
                       b"#EXT-X-TARGETDURATION:10\n"
                       b"#EXTINF:0.1,\n"
                       b"http://127.0.0.1:%d/tvod/seg1.ts\n"
                       b"#EXTINF:0.1,\n"
                       b"seg2.ts\n"
                       b"#EXTINF:0.1,\n"
                       b"seg3.ts\n"
                       b"#EXT-X-ENDLIST\n" % PORT)
        elif u.path == "/tvod/seg1.ts":
            self._stb_ua()
            # r6: TS-shaped body - 200 good packets + 1 broken (0xff) one;
            # vod_read must scrub the broken packet into a null packet,
            # so the client-visible body is 201 good 188B packets
            good = bytes([0x47, 0x10, 0x22, 0x10]) + bytes(184)
            body = good * 200 + (b"\xff" * 188)
            self._send(body)
        elif u.path == "/tvod/media/seg3.ts":
            self._stb_ua()
            good = bytes([0x47, 0x10, 0x23, 0x10]) + bytes(184)
            self._chunked(good * 50)
        elif u.path.startswith("/iptvepg/"):
            assert "JSESSIONID=S-777" in self.headers.get("Cookie", ""), \
                "portal.jsp/AuthByAjax must carry JSESSIONID"
            if u.path.endswith("/portal.jsp"):
                self._send(b"portal-ok")
            elif u.path.endswith("/AuthByAjax.jsp"):
                assert u.query == "action=auth", u.query
                body = json.dumps({"bimsUserToken": "BIMS-TOKEN-1",
                                   "other": 1}).encode()
                self._send(body)
            else:
                self._send(b"?", 404)
        else:
            self._send(b"not found", 404)

    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length", 0))
        form = parse_qs(self.rfile.read(n).decode())
        self.hits.append(("POST", u.path, form))
        if u.path == "/iptv3a/r4kLogAuth.do":
            assert self.headers.get("Referer", "").endswith("4kLogAuth.do")
            assert form == {"u": ["UU"], "p": ["PP"]}, form
            self._send(b"moved", 302,
                       [("Location", "/r4kStep2.jsp")])
        elif u.path == "/ottauth.do":
            auth = form.get("authenticator", [""])[0]
            assert len(auth) >= 64 and all(c in "0123456789abcdef" for c in auth), auth
            self._send(
                b"<html><script>var usertoken='UT-4321';var epggroup='EG-77';</script>"
                b"channelArray.push(ChannelID=\"100\",UserChannelID=\"100\","
                b"ChannelURL=\"igmp://225.1.1.1:5000\",TimeShift=\"1\","
                b"ChannelSDP=\"rtsp://x/SHDXFH_live/0000000001346060\","
                b"TimeShiftURL=\"rtsp://x/LIVE/1346060\","
                b"ChannelFCCPort=\"7777\",ChannelFCCIP=\"124.75.25.214\");"
                b"channelArray.push(ChannelID=\"200\",UserChannelID=\"200\","
                b"ChannelURL=\"igmp://225.1.1.2:5000\",TimeShift=\"1\","
                b"ChannelSDP=\"rtsp://x/SHDXFH_live/0000000006000130\","
                b"TimeShiftURL=\"rtsp://x/LIVE/6000130\");"
                b"<form action='http://127.0.0.1:%d/epg/index.jsp' method='POST'>"
                b"<input name='ut' value='UT-4321'></form></html>" % PORT)
        elif u.path == "/epg/index.jsp":
            self._send(
                b"<html><script>top.document.location='/lb.jsp';</script></html>")
        elif u.path == "/epg/funcportalauth":
            self._send(
                b"<html><script>jsSetConfig('SessionID', 'S-777');"
                b"jsSetConfig('framecode', 'F-42');"
                b"jsSetConfig('IpPort', '127.0.0.1:%d');</script></html>" % PORT)
        elif u.path.endswith("/epg7getChannelByAjax.jsp"):
            assert "JSESSIONID=S-777" in self.headers.get("Cookie", ""), "epg7 needs session cookie"
            assert self.headers.get("Referer", "").endswith("portal.jsp")
            action = form.get("action", [""])[0]
            cate = form.get("cateID", [""])[0]
            if action == "getChannelList":
                if cate == "000409":     # 卫视 category only
                    ch = [{"mixNo": "1", "name": "东方卫视HD",
                           "code": "Umai:CHAN/1346060@BESTV.SMG.SMG",
                           "ID": "ch-1", "isTs": "1"}]
                elif cate == "000406":   # "all" list
                    ch = [{"mixNo": "1", "name": "东方卫视HD",
                           "code": "Umai:CHAN/1346060@BESTV.SMG.SMG",
                           "ID": "ch-1", "isTs": "1"},
                          {"mixNo": "2", "name": "CCTV-1 综合",
                           "code": "Umai:CHAN/9999@BESTV.SMG.SMG",
                           "ID": "ch-2", "isTs": "0"}]
                else:
                    ch = []
                self._send(json.dumps({"data": ch}).encode())
            elif action == "getChannelProg":
                assert form["code"][0].startswith("Umai:CHAN/"), form
                assert form["offset"] == ["0"] and form["limit"] == ["2000"]
                self._send(json.dumps({"data": [
                    {"ID": "pb-1", "name": "新闻", "startTime": "1000", "endTime": "2000"},
                    {"ID": "pb-2", "name": "新闻", "startTime": "1000", "endTime": "2000"},
                    {"ID": "pb-3", "name": "天气预报", "startTime": "2000", "endTime": "3000"},
                ]}).encode())
            elif action == "getPreCurNextProg":
                self._send(json.dumps({"data": {
                    "pre": {"name": "prev", "startTime": 0, "endTime": 1},
                    "cur": {"name": "now playing", "startTime": 1, "endTime": 2},
                }}).encode())
            elif action == "getTvodPlayUrl":
                assert form["channelID"] == ["ch-1"], form
                assert form["playbillID"] == ["pb-1"], form
                assert form["startTime"] == ["1000"], form
                assert form["endTime"] == ["2000"], form
                self._send(json.dumps({"data": {"playURL":
                    "http://127.0.0.1:%d/tvod/entry.m3u8" % PORT}}).encode())
            else:
                self._send(b"{}", 400)
        else:
            self._send(b"not found", 404)


if __name__ == "__main__":
    PORT = int(sys.argv[1])
    HTTPServer(("127.0.0.1", PORT), H).serve_forever()
