# sh-telecom-iptv — 上海电信 IPTV 软终端（OpenWrt 软件包）

本项目是 [zongki/sh-telecom-iptv](https://github.com/zongki/sh-telecom-iptv)
（Python 版，Docker 容器发布、依赖外部 udpxy）的 **OpenWrt 原生移植**，
同一作者。以单个守护进程直接部署在 OpenWrt 上：内置组播中继
（udpxy 兼容端点），免装 udpxy、免 Docker；认证/EPG/时移/点播与
Python 版行为对齐。当前版本 **1.1.0**：
组播中继（含 RTP 乱序恢复 + FCC 快速换台）+ 认证链（4kLogAuth →
ottauth → EPG 会话，后台自动续期）+ 频道表/节目单/搜索 + RTSP 时移
回看 + 按节目 HLS 点播 + **AC-3→AAC 音频转码**（4K 台浏览器可听可
调音量）+ **Web 前端**（`/`，mpegts.js 播放器，随包分发）+ UCI 配置
+ procd 服务。
安装后组件：守护进程 `/usr/bin/iptvd`，UCI 配置
`/etc/config/sh-telecom-iptv`，服务 `/etc/init.d/sh-telecom-iptv`，
Web 静态文件 `/usr/share/sh-telecom-iptv/web/`。

## OpenWrt 集成

本仓库只包含本软件包，不含也不需要发布 OpenWrt 构建系统。按下面
四步把它集成进 OpenWrt。

### 1. 放入构建树

clone（或直接拷贝）到一份现成 OpenWrt 源码树的 `package/` 目录：

```
cd <openwrt>/package
git clone https://github.com/zongki/sh-telecom-iptv.git
```

包声明 `DEPENDS:=+libuci +libmicrohttpd`，依赖由包系统自动解析。

### 2. 启用并编译

```
cd <openwrt>
make menuconfig        # Multimedia 之下选 sh-telecom-iptv：
                       #   y = 编进固件，M = 单独出包
# 或脚本方式：
echo CONFIG_PACKAGE_sh-telecom-iptv=y >> .config && make defconfig

make -j$(nproc) package/sh-telecom-iptv/compile   # 只编本包
# make -j$(nproc)                                 # 整编固件时自动带上
```

产物在 `bin/packages/<arch>/base/`（25.12 为 .apk，19.07~21.02 老树
为 .ipk）。需要 4K 台的 AC-3→AAC 转码时，构建树另需
`CONFIG_BUILD_PATENTED=y` 并在设备上安装 ffmpeg 包（推荐轻量的
`ffmpeg-custom` 变体，配置见 1.1 节与 `docs/ffmpeg-custom-aac.patch`），
缺了只是回退直通、不影响其他功能。

### 3. 安装到设备

```
# OpenWrt 25.12（apk）
scp bin/packages/<arch>/base/sh-telecom-iptv-*.apk root@<设备IP>:/tmp/
ssh root@<设备IP> "apk add --no-network --allow-untrusted /tmp/sh-telecom-iptv-*.apk"

# OpenWrt 19.07 ~ 21.02（opkg，产物为 .ipk）
opkg install /tmp/sh-telecom-iptv-*.ipk
```

安装即完成集成：服务由 procd 托管（崩溃自动拉起）并随安装启用；
包内 uci-defaults 自动创建 `iptv` 防火墙 zone（放行该入接口的
IGMP/组播 UDP，fw3/fw4 通吃）。`/etc/config/sh-telecom-iptv` 是
conffile，升级保留本地配置。

### 4. 首次配置

最少两步（UCI 细节见下文"配置"节与 conf 文件内注释；网络侧的
VLAN 接口/静态路由准备见 conf 头部示例）：

```
uci set sh-telecom-iptv.main.device='wan.85'     # 组播源接口（IPTV VLAN）
uci set sh-telecom-iptv.account.userid='...'     # 上海电信账号三元组，
uci set sh-telecom-iptv.account.sn='...'         # 填齐即启用认证链/
uci set sh-telecom-iptv.account.mac='...'        # EPG/时移/点播
uci commit sh-telecom-iptv
/etc/init.d/sh-telecom-iptv restart
```

之后 `http://<设备IP>:4022/` 是 Web 前端，`/status` 看认证与频道表
状态（冷启动约 30s 出频道表）。服务管理：
`/etc/init.d/sh-telecom-iptv {start|stop|restart|enable}`；改 UCI
经 reload trigger 自动重启生效；日志 `logread | grep iptvd`。

### 主机端测试

重排窗口、认证链与 epg 的主机端测试（不改产品代码，直接测内部
状态机 / 全链）：

```
cd package/sh-telecom-iptv
T='src/log.c src/util.c src/crypto/md5.c src/crypto/aes.c -lpthread'
gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/reorder_test \
    test/reorder_test.c src/log.c src/util.c src/fcc.c -lpthread && /tmp/reorder_test
gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/json_test \
    test/json_test.c src/json.c src/util.c && /tmp/json_test
gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/auth_vector_test \
    test/auth_vector_test.c src/auth.c src/httpc.c $T && /tmp/auth_vector_test
gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/auth_chain_test \
    test/auth_chain_test.c src/auth.c src/httpc.c $T && /tmp/auth_chain_test
gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/epg_chain_test \
    test/epg_chain_test.c src/epg.c src/json.c src/auth.c \
    src/httpc.c $T && /tmp/epg_chain_test
gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/ts_test \
    test/ts_test.c src/ts.c src/log.c -lpthread && /tmp/ts_test
gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/vod_test \
    test/vod_test.c src/vod.c src/epg.c src/json.c src/auth.c \
    src/httpc.c $T && /tmp/vod_test
gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/fcc_test \
    test/fcc_test.c src/fcc.c src/log.c src/util.c -lpthread && /tmp/fcc_test
gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/xcode_test \
    test/xcode_test.c src/xcode.c src/log.c -lpthread && /tmp/xcode_test
```

auth_vector_test 的期望值由 Python 版（原项目）生成（authenticator =
AES-128-ECB(MD5("123456"), JSON)，IP 为逗号分隔三位补零）；
auth_chain_test 起本地 fixture（`test/fixtures/auth_server.py`，
模拟 7 步链含 302/chunked/假 Set-Cookie）跑通全链后销毁。

Web 前端不带浏览器侧单测：`node test/web_mock.js` 起一个按本包
API 形状响应的 mock（含静态文件伺服），浏览器打开
`http://127.0.0.1:18960/` 即可脱离守护进程调试前端。

## 版本兼容（25.12 → 19.07）

| 项 | 策略 |
|---|---|
| 内核 | 纯用户态，无 kmod 依赖 |
| 加密 | MD5 / AES-128-ECB 内置（已与 Python 版对拍一致），避开 libmbedtls 在 19.07↔25.12 的包名/ABI 变化 |
| 依赖库 | 仅 `+libuci +libmicrohttpd`；libuci API 与 MHD 所用 API 子集（0.9.59→1.x）在区间内稳定 |
| C 标准 | C99，gcc 7（19.07）与 gcc 14（25.12）均可；epoch 一律 `int64_t`，规避 musl 1.1 的 32 位 time_t |
| init | procd（`USE_PROCD`、respawn、`procd_set_param file`、`procd_add_reload_trigger`），19.07 起语义一致 |
| 防火墙 | 示例 zone/forwarding 用 UCI 写法，fw3(19.07/iptables) 与 fw4(25.12/nftables) 均可直接吃 |
| 包格式 | 同一 Makefile 在 19.07 产出 .ipk、25.12 产出 .apk，无需修改 |

19.07 实际构建需在 19.07 分支树上执行（本树只出 25.12 产物）。

## 配置（/etc/config/sh-telecom-iptv）

见随包安装的 `/etc/config/sh-telecom-iptv` 注释：`device`（组播源接口，如
wan.85）为空则中继不启动；`port` 默认 4022；`rtp` auto/always/never。

网络侧（VLAN 85 接口、静态路由、iptv zone）见同文件头部示例，
与商用网关上的部署同构，仅接口名不同。

## HTTP 接口

| 路径 | 说明 |
|---|---|
| `/udp/G:P` · `/rtp/G:P`（亦接受 `G/P`） | 组播转 HTTP 直播流（RTP 自动剥离，输出裸 TS） |
| `/status` | 纯文本状态（uptime、活跃组、客户端数、auth/epg） |
| `/playlist.m3u` | **动态**：认证启用时由合并频道表生成（URL 指向本机中继）；否则回退静态 `/etc/iptv/channels.m3u` |
| `/api/channels` | 合并频道表 JSON（no/name/pid/igmp/ts/hd/ac3/cats） |
| `/api/programs/<pid>` | 频道节目单（过去 7 天 + 未来 1 天，15min 缓存，epg7 去重） |
| `/api/now/<pid>` | 当前/前后节目（epg7 getPreCurNextProg 透传） |
| `/api/search?q=` | 全网节目搜索（后台索引，30min 刷新，最新在前，最多 200 条） |
| `/ts/<pid>.ts?back=N` | 时移回看：N 秒前起播（裸 TS，mp2t） |
| `/ts/<pid>.ts?start=T` | 按时间点播（T=unix 秒或毫秒，亦接受 `utc=T`；受运营商滚动缓冲钳制） |
| `/api/ts_window/<pid>` | 频道回看缓冲窗口（DESCRIBE DURATION，实测滚动 1-4 小时） |
| `/vod/<pid>.ts?bill=&start=&end=` | **按节目点播**（epg7 getTvodPlayUrl 的 HLS 中继，7 天内已播节目，串联裸 TS） |
| `/api/vod_speed` | POST `sid=<id>&speed=<0.25-4>`（query 或表单体）：设置点播倍速（流按此节奏发段，前端配合 playbackRate） |
| `/` | **Web 前端**（`/usr/share/sh-telecom-iptv/web/`，未安装时回退内建索引页） |

慢客户端（4 MiB FIFO 溢出）会被踢掉而不是拖死组线程；
`max_clients` 默认 20，沿用原项目的经验值。

## 相关项目

- **[zongki/sh-telecom-iptv](https://github.com/zongki/sh-telecom-iptv)**
  （原项目，同一作者）：Python 实现，Docker 容器发布，依赖外部
  udpxy 做组播转 HTTP。本项目的认证/EPG/时移/点播行为基准；本版
  以 C 重写并原生集成组播中继，免去 udpxy 依赖。
- **stackia/rtp2httpd**（GPLv2）：同类 IPTV 转发服务器（组播
  RTP/RTSP→HTTP、FCC 快速换台、HLS 反代）。**只参考其协议事实与
  架构思想，代码独立实现**，本包保持 MIT。协议摘录（含电信 FCC
  报文格式）、阅读地图、许可边界见 `docs/rtp2httpd-reference.md`。

## 认证链（0.2）

account 节填入 `userid`/`sn`/`mac` 后，守护进程后台跑完整认证链
（pre4kLogAuth → r4kLogAuth → ottauth → EPG index/loadbalance →
funcportalauth → portal.jsp → AuthByAjax），每 3h 自动重认证
（会话实测 4h 过期），失败每 60s 重试。`/status` 显示
`auth: ok (age Ns, http://IpPort/iptvepg/framecode)`。

会话状态（usertoken/JSESSIONID/bimsUserToken）目前只留存在内存，
供 0.3 EPG / 0.4 时移取用；进程重启会重新走一遍链（幂等）。
全部端点为纯 HTTP，无 TLS 依赖（DEPENDS 不变）。

## 频道表与节目单（0.3）

认证成功后，epg 模块自动合并频道表：ottauth channelArray（组播/
时移地址）+ epg7 getChannelList（频道名/编号/节目单 ID），长短号
零填充匹配；epg7 独有频道丢弃，ottauth 独有频道按 PID 命名表补
9000+ 编号，另叠加人工验证的静态频道；分类按 epg7 各分类表打标。
节目单走 epg7 getChannelProg（7 天回看 + 1 天预告），整段 15min
缓存并按（起止/名称）去重；分类按 epg7 各分类表打标（每个分类拉
一次表、本地短号匹配，对齐 python _apply_categories）；搜索索引
在后台 8 线程并发构建（30min 刷新，对齐 python SEARCH_WORKERS），
`/api/search` 为子串匹配、最新在前。

## 时移回看（0.4）

RTSP 客户端（timeshift.py → C）：DESCRIBE 沿 302 链解析出
Content-Base（55554 流端点，URL 内嵌 AuthInfo 与
`DURATION=<UTC>-<UTC>` 滚动缓冲窗口）→ SETUP
`MP2T/TCP;interleaved=0-1` → PLAY（`Range: clock=<UTC>-` 定位）→
解 `$` interleaved 帧输出裸 TS。`/ts` 端点规则与原项目一致：
`back=N` 相对回看、`start/utc=T` 按时间点播（毫秒自动归秒、
临近当前时刻退到直播边缘、早于缓冲起点静默钳制）；凭据过期
（401/454）时同步强刷认证并重试一次（timeshift_url 内嵌的
AuthInfo 随频道表刷新）。

## 按节目点播（0.5）

epg7 `getTvodPlayUrl` 下发带鉴权的 HTTP HLS playURL（:8006 入口
302 → :8112 播放列表 → :8114 约 10s TS 分片，支持 Range）。
`/vod` 端点拉取播放列表（跟随重定向与主/变体嵌套，分片 URL
相对/绝对均可）后按序抓取分片，以 **EXTINF 时长的实时节奏**
串联成一路裸 TS（下载超前时补间隔，防止播放器缓冲无限膨胀；
分片失败跳过不中断；请求带机顶盒 UA `HMTL RTSP 1.0; CTC/2.0`）。
`bill` 为节目单条目 ID（`/api/programs` 的 id 字段），`start/end`
为节目起止 unix 秒（start 也接受毫秒自动归秒；机顶盒即走此
接口实现 7 天回看，不受 RTSP 时移缓冲限制）。getTvodPlayUrl
失败时强刷认证重试一次。`/api/vod_speed` 维护 sid → 倍速会话，
`/vod?sid=` 的发段节奏随之缩放（0.25-4x，变速即时生效、无需
重连），与前端 playbackRate 匹配。（本包早期版本带过 ffmpeg
AC-3 频道转码包装，1.1 以独立 xcode 模块重新实现，见 1.1 节。）

## FCC 快速换台（0.6）

换台（新组加入）时向配置的 FCC 服务器（UCI `list fcc_server`，
并行全发、首个成功者胜出）发电信 RTCP FMT=2 请求（PT=205，
media SSRC=组播组 IP，声明本机接收端口），服务器以 FMT=3 确认
（可重定向到另一台，跟随一次）并向该端口高码率推送带 IDR 的
单播加速流；中继在加速期间只跟踪组播 RTP 序号不转发，输出
加速流；收到 FMT=4 同步通知（或 1s 停推）后回 FMT=5 终止，并
按 RTP 序号无缝切回组播（重排窗/序号过滤器丢弃已发过的序号）。
**80ms 内无 FMT=3 即放弃 FCC、纯组播**——服务器不可达最多损失
80ms 起播延迟，绝不影响播放。协议事实（报文格式、超时经验值）见
`docs/rtp2httpd-reference.md`，实现独立。

## Web 前端（1.0）

单文件前端（`files/web/index.html` + `mpegts.js` 1.8 dist，
Apache-2.0）安装到 `/usr/share/sh-telecom-iptv/web/`，由 iptvd 直接伺服
（单段路径白名单，无目录穿越；`/`→index.html，缺省回退内建
索引页；开发时可用 `IPTVD_WEB=<dir>` 环境变量指向源码树）。
功能：频道列表（分类筛选/搜索/遥控器式数字跳台）、mpegts.js
直播、时移滑块（back=N）、节目单 7 天浏览 + 一键按节目点播
（自绘进度条、拖动定位、0.5-2x 变速经 `/api/vod_speed` 不重连
变速）、全网节目搜索点播、"正在播"轮询、键盘快捷键
（↑↓换台、←→时移/±10s、空格暂停、L 回直播、M 静音、F 全屏、
数字键跳台）。AC-3 音轨频道（4K 台）由服务端 xcode 转码后有声
可调（需板上装 ffmpeg，见 1.1 节；未装则维持无声直通）。

## AC-3→AAC 音频转码（1.1）

4K 频道（CCTV-4K 124、9063~9071 卫视4K 等，频道表 `ac3` 标记，
9999 奥林匹克4K 除外——它音轨本就不是 AC-3）为 HEVC+AC-3，浏览器
MSE 无 ac-3 解码许可，音轨进不了 MediaSource：画面正常但无声、
音量键失效。xcode 模块在 `/udp` `/ts` `/vod` 三个流端点上把这些
频道的流经 ffmpeg 子进程（`-c:v copy -c:a aac -b:a 256k -ac 2`，
5.1 必须降混双声道——mpegts.js 声明 HE-AAC 而 Chrome 的 HE-AAC
不支持 5.1，6 声道会拖垮整个 MediaSource 会话；每流一个进程，与
原项目 `xcode.aac_restream` 同构，点播含分片下载中止接口
vod_halt）。行为细节：

- xcode 只需要 ffmpeg 的一个很小子集（mpegts demux/mux、AC-3 解码、
  HEVC 探测、原生 AAC 编码、swresample 降混）。**推荐用 feed 自带的
  `libffmpeg-custom`/`ffmpeg-custom` 裁剪变体**替代 `libffmpeg-full`：
  实测 aarch64 上 libavcodec 从 7.7MB 降到 1.2MB（apk 6.6MB→0.9MB，
  整套占用约省 13MB），转码 CPU 持平（~5%）。所需包配置与 custom
  白名单的一行补丁（上游漏了原生 aac encoder）见
  `docs/ffmpeg-custom-aac.patch`。
  若沿用 `ffmpeg`/`libffmpeg-full`：构建侧两个前提（默认都不满足）：
  `.config` 开 `CONFIG_BUILD_PATENTED=y`（否则 h264/hevc 解码器与
  解析器被禁，ffmpeg 6 下 `-c:v copy` 探测拿不到视频参数、视频流
  停滞）；ffmpeg 构建须无 v4l2m2m 家族（路由器无 /dev/video 设备，
  而它注册在软解之前会毒化 find_stream_info——feed Makefile 本地
  补丁 --disable）。custom 变体因 `--disable-everything` 白名单制
  天然无此问题。
- 未装 ffmpeg 时告警一次并直通原始流（等价 1.0 行为）。
- ffmpeg 中途退出则该 HTTP 响应正常收尾，客户端重连/重选台即可。
- UCI `option xcode '0'` 可整体关闭（默认开）。
- 频道表未建成（开机头几分钟）时不转码，同 FCC 的已知边界。

## 里程碑

- [x] 0.1.0 组播中继 + UCI/procd + udpxy 兼容端点
- [x] 0.1.1 加固：RTP 乱序恢复（64 槽重排窗，40ms 超时冲刷，
      `reorder` UCI 开关，默认开；本版）
- [x] 0.2 认证链（4kLogAuth/ottauth/EPG 会话，auth.py → C；本版）
- [x] 0.3 频道表/节目单/搜索（epg.py → C；本版）
- [x] 0.4 RTSP 时移（timeshift.py → C）
- [x] 0.5 HLS 点播（vod.py → C；getTvodPlayUrl + 分片串联中继 +
      倍速会话；ffmpeg 转码包装砍掉）
- [x] 0.6 FCC 快速换台（电信 RTCP FMT2/3/4/5，80ms 超时
      回退组播，序号无缝回切；协议见 `docs/rtp2httpd-reference.md`）
- [x] 1.0 Web 前端（单文件 + mpegts.js 随包分发，
      直播/时移/点播/搜索/遥控器键盘）
- [x] 1.1 AC-3→AAC 音频转码（本版。4K 台浏览器有声可调；
      ffmpeg 缺席自动回退直通，python xcode.py → C）
