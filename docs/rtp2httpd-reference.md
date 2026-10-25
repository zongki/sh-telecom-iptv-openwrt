# rtp2httpd 参考笔记（只参考，不引入代码）

**决策（2026-10）**：stackia/rtp2httpd 作为本项目的协议与架构参考，
代码全部独立实现。

## 许可边界（必读）

rtp2httpd 是 **GPL-2.0**，本包是 **MIT**。逐行照搬其代码会把本包变成
GPL。因此：

- ✅ 允许：读源码/文档理解**行为与协议**；按协议事实（报文格式、字段
  含义、超时/回退逻辑、服务器地址等事实数据）独立写自己的实现。
- ❌ 禁止：复制或逐句翻译其代码/文档文本；基于其源文件做 diff/patch
  式修改后当作我们的代码。

## 上游与本地副本

- 上游：<https://github.com/stackia/rtp2httpd>（oskar456/rtp2httpd 的
  增强 fork，面向中国大陆 IPTV；文档站 rtp2httpd.com）
- 本地浅克隆：`/tmp/rtp2httpd`（易失）。恢复：
  `git clone --depth 1 https://github.com/stackia/rtp2httpd /tmp/rtp2httpd`

## 阅读地图（按我们的里程碑）

| 我们的里程碑 | 对应参考实现 | 看什么 |
|---|---|---|
| 0.1.x 加固 | `src/rtp_reorder.c`(325行)、`src/rtp_fec.c`(563行) | RTP 乱序恢复（按 seq 重排，消除花屏）；RS 前向纠错（需上游支持，暂低优先级） |
| 0.3 频道表/EPG | `src/epg.c`(279行)、`src/m3u.c`(1780行) | M3U 里 RTP/RTSP URL 自动改写为代理 URL；catchup-source 时移地址处理 |
| 0.4 RTSP 时移 | `src/rtsp.c`(3531行) | 完整 RTSP 客户端：会话建立、重定向（≤5 次）、30s keepalive、interleaved(TCP)/UDP 两种承载、UDP NAT 探测。是 timeshift.py → C 移植的行为基准 |
| 0.5 HLS 反代 | `src/http_proxy.c`(1621行)、`src/http_chunked_decoder.c` | 内网 HLS 代理到公网：chunked 解码、播放列表 URL 重写 |
| 0.6 FCC 快速换台 | `src/fcc.c`(788行)、`fcc_telecom.c`(271行)、`docs/reference/cn-fcc-collection.md` | 见下文协议摘录 |
| 1.0 Web 前端 | `web-ui/`、`src/embedded_web.c`、`openwrt-support/luci-app-rtp2httpd/` | TS 前端构建产物嵌进二进制（单文件分发）；LuCI app + rpcd ACL 是现成模板 |

性能参照：`docs/reference/benchmark.md` 有 rtp2httpd vs udpxy/msd_lite
的对比数据，可当我们组播中继的性能标尺。

## 架构对比（他们的设计 vs 我们 0.1 现状）

- **事件模型**：epoll 多 worker（他们）vs libmicrohttpd
  thread-per-connection + 每组一个组线程（我们）。我们 max_clients=20
  的目标规模下线程模型够用，暂不改造；若将来要支撑 25+ 路并发 1080p
  再评估。
- **同源复用**：双方等价——他们的 worker 内共享组播订阅，我们的
  `mgroup` 引用计数 + 单组线程（`src/mcast.c`），多客户端共享一份
  IGMP 订阅，已达标。
- **慢客户端策略**：行为一致——不拖死组线程。我们 4 MiB 环形 FIFO
  溢出即踢；他们用预分配缓冲池按负载动态共享，慢客户端限流。可借鉴
  的差异：他们踢得温和（先限流），我们直接踢。
- **RTP 剥离**：我们的 `rtp_payload_offset()` 已按上海电信实测处理
  V=2/PT=33/X=1（12B 基头 + 4 字私有扩展）；他们在此之上还做乱序
  恢复与 FEC，是我们 0.1.x 加固的参考。

## 电信 FCC 协议摘录（事实，独立实现）

以下为从 rtp2httpd 源码与文档整理的**协议事实**，供我们独立实现
0.6 里程碑使用。载体是标准 RTCP 反馈包（PT=205 Generic RTP
Feedback），媒体 SSRC 字段填组播组 IP。

**上海电信 FCC 服务器**（来自其社区收集表，不保证可用，需实测）：

- `124.75.26.151:15970`（15970 端口 = 中兴/烽火平台特征）
- `124.75.25.211:7777`、`…212/214/215/216:7777`

（华为平台特征端口是 8027。经验：
FCC IP 常与当地 RTSP 单播源同 IP 不同端口，可拿 RTSP 地址换端口
试探。）

**报文格式**（字节偏移，均网络序）：

1. 请求（C→S，FMT=2，40 字节，24 字节后为填零）：
   RTCP 头 `[V=2,P=0,FMT=2][PT=205][len=9(字)]`，Sender SSRC=0，
   Media SSRC=组播组 IP；FCI：`+12` 版本(0)+保留(4B)，`+16` FCC
   客户端端口(2B)，`+18` 组播组端口(2B)，`+20` 组播组 IP(4B)。
   客户端端口即服务器回推单播加速流的目标 UDP 端口。
2. 响应（S→C，FMT=3，≥36 字节）：FCI：`+12` 结果码(0=成功，非 0
   回退组播)，`+13` 类型(=3 涉及重定向)，`+14` 新信令端口(2B，0=不变)，
   `+16` 新媒体端口(2B)，`+20` 新 FCC 服务器 IP(4B，0=不变)，`+24`
   有效时长(4B, s)，`+28` 加速码率(4B, bps)，`+32` 同步后码率(4B,
   bps)。服务器可借此把客户端重定向到另一台 FCC。
3. 同步通知（S→C，FMT=4，12 字节）：加速推送结束，客户端应回到
   组播流（按序号衔接）并发终止包。
4. 终止（C→S，FMT=5，16 字节）：FCI：`+12` stop 标志(0=正常,
   1=强制)，`+14` 客户端已收到的首个组播 RTP 序号(2B)。

**时序与回退**（其实现的经验值）：

- 客户端发请求后 **80ms** 内无响应 → 放弃 FCC，直接组播（起播体验
  退化为普通 udpxy，不失败）。
- 单播加速流停止到达 **1s** → 判为流中断，回退组播。
- 加速期间以响应中的高码率（speed）追赶，收到 FMT=4 后切回组播并
  发 FMT=5 终止；若超时未收到 FMT=4，也主动回组播。
- 配合做法：换台即对新频道发 FCC 请求；FCC 命中时播放器立刻拿到
  IDR 帧实现毫秒级起播。需要播放器侧配合（不起播优化的话收益有限）。

## 里程碑落地建议

1. **0.1.1（小）**：mcast.c 组线程里加乱序恢复——按 RTP seq 维护小
   重排窗口（几十 ms），过期按序 flush。独立实现，不碰他们的代码。
2. **0.6（中）**：FCC 客户端，作为组播中继的可选加速路径（UCI 开关
   + FCC 地址配置）。信令socket 与组播 socket 并存，80ms 超时回退。
3. 华为 FCC（FMT 5/6/8/9，含 NAT 穿透 FMT 12）上海用不到，需要时
   再研究。
