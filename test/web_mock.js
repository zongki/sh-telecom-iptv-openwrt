#!/usr/bin/env node
// Frontend dev/mock server: serves files/web/ and answers the iptvd
// HTTP API with fixture data shaped EXACTLY like the C daemon's JSON
// (field names, units and cats-as-comma-string included), so the web
// UI can be developed and browser-tested without the daemon.
//
//   node test/web_mock.js [port]     (default 18960)
const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = parseInt(process.argv[2] || '18960', 10);
const WEB = path.join(__dirname, '..', 'files', 'web');

const MINE = {
  'index.html': ['text/html; charset=utf-8', 'no-store'],
  'mpegts.js': ['application/javascript', 'max-age=3600'],
};

const CHANNELS = [
  { no: 1, name: '东方卫视HD', pid: '1346060', chid: 'ch-1',
    igmp: 'igmp://225.1.1.1:5000', ts: 1, hd: 1, ac3: 0, cats: '卫视,高清' },
  { no: 2, name: 'CCTV-1 综合', pid: '9999', chid: 'ch-2',
    igmp: 'igmp://225.1.1.2:5000', ts: 0, hd: 0, ac3: 0, cats: '央视,标清' },
  { no: 9001, name: '北京卫视4K', pid: '0000000006000130', chid: '',
    igmp: 'igmp://225.1.1.3:5000', ts: 1, hd: 1, ac3: 1, cats: '4K' },
];

function programs(pid) {
  const now = Date.now();
  const day = 86400 * 1000;
  const out = [];
  // two past days + today + tomorrow, three programs each
  for (let d = -2; d <= 1; d++)
    for (let i = 0; i < 3; i++) {
      const st = new Date(now + d * day);
      st.setHours(19 + i, 0, 0, 0);
      const en = st.getTime() + 3600 * 1000;
      out.push({ id: pid + '-pb-' + d + '-' + i,
                 name: '节目' + i + '(' + pid + ')',
                 start: st.getTime(), end: en });
    }
  return out;
}

const srv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  const send = (code, type, body, cc) => {
    res.writeHead(code, { 'Content-Type': type,
                          'Cache-Control': cc || 'no-store' });
    res.end(body);
  };
  const json = (code, obj) => send(code, 'application/json', JSON.stringify(obj));

  if (u.pathname === '/api/channels')
    return json(200, { count: CHANNELS.length, channels: CHANNELS });
  let m = u.pathname.match(/^\/api\/programs\/([^/]+)$/);
  if (m) {
    const pid = decodeURIComponent(m[1]);
    if (!CHANNELS.some(c => c.pid === pid))
      return json(404, { error: 'no such channel' });
    const p = programs(pid);
    return json(200, { pid, count: p.length, programs: p });
  }
  m = u.pathname.match(/^\/api\/now\/([^/]+)$/);
  if (m) {
    const now = Date.now();
    return json(200, { pid: m[1], data: {
      pre: { name: '上一节', startTime: now - 3600e3, endTime: now },
      cur: { name: '正在播的节目', startTime: now, endTime: now + 1800e3 },
      next: { name: '下一节', startTime: now + 1800e3, endTime: now + 5400e3 },
    } });
  }
  if (u.pathname === '/api/search') {
    const q = u.searchParams.get('q') || '';
    const items = [];
    if (q)
      for (const c of CHANNELS)
        for (const p of programs(c.pid))
          if (p.name.includes(q))
            items.push({ pid: c.pid, no: c.no, chname: c.name, name: p.name,
                         id: p.id, start: p.start, end: p.end });
    return json(200, { q, building: 0, age: 10, items: items.slice(0, 200) });
  }
  if (u.pathname === '/api/vod_speed' && req.method === 'POST') {
    const sid = u.searchParams.get('sid') || '';
    const speed = parseFloat(u.searchParams.get('speed') || '1');
    if (!sid || !/^[0-9a-zA-Z]{1,64}$/.test(sid))
      return json(400, { error: 'bad sid' });
    const clamped = Math.min(Math.max(speed, 0.25), 4);
    return json(200, { ok: true, speed: clamped });
  }
  // stream endpoints: enough bytes to keep mpegts.js busy, not real TS
  if (/^\/(udp|rtp|ts|vod)\//.test(u.pathname))
    return send(200, 'video/mp2t', Buffer.alloc(188 * 20, 0x47));

  const name = u.pathname === '/' ? 'index.html' : u.pathname.slice(1);
  if (MINE[name]) {
    try {
      return send(200, MINE[name][0], fs.readFileSync(path.join(WEB, name)),
                  MINE[name][1]);
    } catch (e) { /* fall through */ }
  }
  send(404, 'text/plain', 'not found');
});
srv.listen(PORT, '127.0.0.1',
           () => console.log('web mock on http://127.0.0.1:' + PORT));
