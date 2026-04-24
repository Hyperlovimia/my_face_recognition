# K230 人脸识别网络通信落地与问题修复记录

日期：2026-04-25

## 1. 背景

本轮工作的目标，是在现有 `my_face_recognition` 三进程架构之上，为项目补齐一套可落地的：

- 电脑端服务器
- 网页查看状态与事件
- 反向远程控制开发板
- 兼容 K230 大小核分工的通信链路

项目路径：

```text
/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition
```

系统约束是：

- 网络能力主要在 Linux 小核侧
- 当前人脸识别主程序运行在 RT-Smart 大核
- 不适合直接让网页或服务器直接控制 RT-Smart 业务进程

因此，本轮采用的总体方案是：

```text
浏览器
  <- HTTP / WebSocket ->
电脑端 face-web
  <- MQTT ->
Linux 小核 face_netd
  <- IPCMSG ->
RT-Smart 大核三进程
```

其中：

- `face_ai.elf` 继续负责检测、识别、数据库操作、活体判断
- `face_video.elf` 继续负责采集、显示、OSD、执行数据库相关控制
- `face_event.elf` 升级为 RT 侧“对外桥接中心”
- `face_netd` 新增为 Linux 小核侧桥接守护进程
- `face-web` 新增为电脑端服务器与网页入口

## 2. 相关提交

本轮网络通信功能及后续修复，主要对应以下 3 个提交：

```text
b035cc9 feat: 网络通信
a4cec00 fix: segmentation fault
f56eb06 fix: 端口设置必须在 [0, 512)
```

其中：

- `feat: 网络通信` 是第一次完整落地
- `fix: segmentation fault` 是 Linux 小核运行期稳定性修复
- `fix: 端口设置必须在 [0, 512)` 是 IPCMSG 端口范围修复

后续在板端与电脑端联调期间，又额外定位并修复了：

- IPCMSG 建连后立即断开的竞态问题
- WSL / Docker 环境下 Windows 未对外暴露 `1883/8000` 导致的 MQTT 不通问题

这两项修复当前已体现在工作区代码与文档中。

## 3. 第一次落地实现：网络通信完整链路

### 3.1 RT-Smart 侧协议扩展

本轮首先扩展了 RT 侧统一协议头文件：

- `src/ipc_proto.h`

新增内容包括：

- `IPC_FACE_VIDEO_REPLY`
- `IPC_FACE_BRIDGE_SERVICE`
- `IPC_FACE_BRIDGE_PORT`
- `IPC_FACE_BRIDGE_MODULE`
- `IPC_REQUEST_ID_MAX`
- `IPC_OP_MESSAGE_MAX`
- `ipc_op_result_t`
- `ipc_video_ctrl_source_t`
- `ipc_bridge_cmd_t`
- `ipc_bridge_msg_kind_t`
- `ipc_video_reply_t`
- `bridge_cmd_req_t`
- `bridge_cmd_ack_t`
- `bridge_event_t`
- `bridge_cmd_result_t`

同时扩展了已有结构体：

- `ipc_ai_reply_t`
  - 增加 `op_result`
  - 增加 `op_message`
- `ipc_evt_t`
  - 增加 `ts_ms`
- `ipc_video_ctrl_t`
  - 增加 `source`
  - 增加 `bridge_cmd`
  - 增加 `request_id`

这一步的目的，是把原本只适合本地串口/本地状态机的控制与事件结构，升级为：

- 可被 Linux 小核桥接
- 可回溯到网页命令 `request_id`
- 能结构化返回“成功/失败/人数/错误原因”

### 3.2 `face_event.elf` 升级为 RT 对外桥接中心

核心文件：

- `src/face_event_main.cc`

本轮将 `face_event.elf` 从“串口交互入口”升级为“RT 侧统一对外控制面”。

主要职责变为：

1. 保留原有 stdin 串口命令
2. 新增 IPCMSG 服务 `face_bridge`
3. 接收 Linux 小核的桥接命令
4. 将桥接命令转换为对 `face_video` 的控制
5. 接收来自 `face_ai` 的识别/陌生人/活体失败事件
6. 将这些事件继续向 Linux 小核转发
7. 接收 `face_video` 的最终执行结果并转发给 Linux 小核

实现细节包括：

- 新增 `bridge_service_loop()`
- 新增 `bridge_handle_message(...)`
- 新增 `forward_bridge_event(...)`
- 新增 `forward_bridge_result(...)`
- 新增 `stdin_loop()`，避免阻塞式 `getline` 干扰远程关闭
- 引入单个在途远程命令约束：同一时刻只允许 `1` 个远程命令执行

远程控制命令统一支持：

- `db_count`
- `db_reset`
- `register_current`
- `shutdown`

### 3.3 `face_video.elf` 支持远程控制结果回传

核心文件：

- `src/face_video_main.cc`

本轮没有重写 `face_video` 业务，而是在原有状态机基础上补了“远程命令结果回传”。

新增内容包括：

- `IPC_FACE_VIDEO_REPLY` 通道连接
- `send_video_reply(...)`
- 对远程命令控制元信息的缓存
- 对 `db_count/db_reset/register_current/shutdown` 的统一回传

行为变化：

- 本地串口命令仍按原方式执行
- 若命令来源是桥接远程命令，则执行结束后自动通过 `ipc_video_reply_t` 回传最终结果

这样避免了“网页命令发出去了，但不知道板端最终是否成功”的问题。

### 3.4 `face_ai.elf` 输出结构化业务结果

核心文件：

- `src/face_ai_main.cc`

本轮补齐了 AI 侧对业务结果的结构化表达。

新增内容包括：

- `set_reply_op(...)`
- 事件时间戳写入
- 对数据库操作和注册结果填充 `op_result/op_message`

重点修复点：

- `db_count` 返回明确人数与成功状态
- `db_reset` 返回成功状态
- `shutdown` 返回成功状态
- `register_current`
  - 注册成功时返回 `"register ok"`
  - 活体失败时返回 `"register rejected: liveness failed"`
  - 多人/无人时返回 `"register failed: need exactly one face"`

这样网页就能看到明确的失败原因，而不只是“操作失败”。

### 3.5 新增 Linux 小核桥接守护进程 `face_netd`

新增目录：

- `linux_bridge/`

新增文件：

- `linux_bridge/main.cpp`
- `linux_bridge/Makefile`
- `linux_bridge/build_face_netd.sh`
- `linux_bridge/face_netd.ini`
- `linux_bridge/README.md`
- `linux_bridge/third_party/mongoose/mongoose.c`
- `linux_bridge/third_party/mongoose/mongoose.h`

`face_netd` 的职责是：

1. 连接 RT 侧 `face_bridge` IPCMSG 服务
2. 连接 MQTT Broker
3. 订阅下行命令 topic
4. 将网页/服务器发来的命令转成桥接命令
5. 将 RT 侧事件和命令结果发布为 MQTT JSON 消息
6. 维护设备在线状态、LWT、心跳、数据库人数缓存

MQTT topic 约定：

- `k230/<device_id>/down/cmd`
- `k230/<device_id>/up/event`
- `k230/<device_id>/up/reply`
- `k230/<device_id>/up/status`

消息格式固定使用 JSON。

### 3.6 新增电脑端服务器 `server_pc`

新增目录：

- `server_pc/`

新增文件包括：

- `server_pc/app/main.py`
- `server_pc/Dockerfile`
- `server_pc/docker-compose.yml`
- `server_pc/mosquitto/mosquitto.conf`
- `server_pc/requirements.txt`
- `server_pc/static/index.html`
- `server_pc/static/app.js`
- `server_pc/static/style.css`

实现方案：

- `FastAPI` 负责 REST API 和 WebSocket
- `Paho MQTT` 负责订阅和下发 MQTT 消息
- `SQLite` 持久化设备状态、事件、命令记录
- 静态网页用于状态查看和命令下发

HTTP / WebSocket 能力包括：

- `GET /api/devices`
- `GET /api/devices/{device_id}/state`
- `GET /api/devices/{device_id}/events?limit=100`
- `POST /api/devices/{device_id}/commands/db-count`
- `POST /api/devices/{device_id}/commands/db-reset`
- `POST /api/devices/{device_id}/commands/register-current`
- `POST /api/devices/{device_id}/commands/shutdown`
- `GET /ws`

网页展示能力包括：

- 设备在线状态
- RT 桥接状态
- 数据库人数
- 最近命令结果
- 最近识别事件
- 控制按钮与注册姓名输入框

### 3.7 文档与忽略规则更新

同时更新了：

- `.gitignore`
- `README.md`

补充内容包括：

- 网络通信整体架构说明
- `face_netd` 编译与部署方式
- `server_pc` 启动方式
- `/sharefs/face_bridge/` 部署约定
- v1 的边界与限制

## 4. 第一次实现后的验证情况

在第一次完整落地后，做过如下验证：

### 4.1 RT 侧交叉编译验证

使用交叉工具链对 RT 侧全量编译，成功生成：

- `face_ai.elf`
- `face_video.elf`
- `face_event.elf`
- `face_recognition.elf`

说明：

- 新增桥接协议可通过编译
- `face_event` 的 IPCMSG 接入代码可通过编译
- `face_video` 的远程结果回传代码可通过编译

### 4.2 Linux 小核程序交叉编译验证

初版 `face_netd` 能完成交叉编译，并生成：

- `linux_bridge/out/face_netd`

但初版在工具链与链接策略上仍有问题，后续由用户继续修复。

### 4.3 电脑端服务验证

已验证：

- `server_pc/app/main.py` 可通过 `python3 -m py_compile`
- `docker compose config` 可正确解析
- `face-web` 镜像可完成构建
- `face-web` 容器启动后，`GET /api/devices` 可返回空设备列表

后续还有一个运行期目录问题，也在后面被修复。

## 5. 用户修复的 3 个 bug

在第一次落地后，用户继续完成了 3 个关键修复。这 3 个修复非常重要，直接决定了 Linux 小核桥接程序能否在板端稳定运行。

### 5.1 Bug 1：Linux 小核交叉编译工具链错误

#### 现象

最初给 `face_netd` 使用的是：

```text
riscv64-linux-musleabi_for_x86_64-pc-linux-gnu
```

但用户确认，Linux 小核实际应使用的正确工具链是：

```text
Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0
```

对应当前 `linux_bridge/Makefile` 已改为：

```make
TOOLCHAIN_ROOT ?= $(SDK_ROOT)/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin
CROSS_COMPILE ?= $(TOOLCHAIN_ROOT)/riscv64-unknown-linux-gnu-
```

#### 修复内容

修复文件：

- `linux_bridge/Makefile`
- `linux_bridge/build_face_netd.sh`

#### 影响

该修复保证：

- Linux 小核程序使用与板端环境一致的 glibc 工具链
- 不再沿用此前不匹配的 musl 工具链配置

这是 Linux 侧桥接程序能正常运行的前提。

### 5.2 Bug 2：`face_netd` 运行时段错误

#### 现象

在工具链切换正确之后，Linux 小核程序仍然在板端运行时报段错误：

```text
./face_netd
[  943.289388] face_netd[233]: unhandled signal 11 code 0x1 at 0x0000000000000000 in face_netd[10000+7f000]
...
Segmentation fault
```

#### 取证结论

用户进一步确认后，得到的结论是：

- `face_netd` 与 `libipcmsg.a` 都是 `riscv64`
- 两者都带有 Xuantie glibc 工具链痕迹
- 没有发现“目标架构或 ABI 配错”的证据
- 现场表现为 `epc=0`，更像运行期路径或静态链接相关异常
- 问题更接近“静态链接 + glibc + 板端运行环境”导致的脆弱运行时问题

#### 根因判断

最终判断：

- 这不是“交叉编译器目标架构错误”问题
- 更像是“静态链接策略在板端环境下触发运行期异常”

#### 修复内容

用户已经将构建策略调整为：

- 默认动态链接
- 保留 `STATIC=1` 静态链接开关
- 支持 `DEBUG`
- 支持 `DO_STRIP`

修复文件：

- `linux_bridge/Makefile`
- `linux_bridge/build_face_netd.sh`
- `README.md`

当前构建脚本支持：

```bash
STATIC=0 DEBUG=0 DO_STRIP=1 ./build_face_netd.sh
```

#### 结果

这一修复显著提升了 `face_netd` 在 Linux 小核侧的可运行性与可调试性。

对应提交：

```text
a4cec00 fix: segmentation fault
```

### 5.3 Bug 3：IPCMSG 服务端口超范围

#### 现象

`face_netd` 板端运行后，出现错误：

```text
face_netd: device_id=k230-dev-01 mqtt_url=mqtt://192.168.160.8:288 heartbeat=5000ms
[IPCMSG]:port must in [0,512)
face_netd: kd_ipcmsg_add_service(face_bridge) failed
face_netd: stopped
```

#### 根因

根因是：

- `src/ipc_proto.h` 中编译期常量 `IPC_FACE_BRIDGE_PORT`
- 初版值为 `2301`
- 但 K230 `ipcmsg` 端口范围要求必须在 `[0, 512)`

因此 `2301` 这个值本身就是非法端口。

#### 修复内容

用户将：

```c
#define IPC_FACE_BRIDGE_PORT 2301u
```

修正为：

```c
#define IPC_FACE_BRIDGE_PORT 301u
```

修复文件：

- `src/ipc_proto.h`

并同步更新：

- `README.md`
- `AGENTS.md`
- `CLAUDE.md`

#### 结果

修复后，`face_netd` 能通过 `kd_ipcmsg_add_service(face_bridge)` 初始化 IPCMSG 服务。

对应提交：

```text
f56eb06 fix: 端口设置必须在 [0, 512)
```

### 5.4 Bug 4：IPCMSG 建连后立即断开

#### 现象

在 RT-Smart 侧 `face_event.elf` 已经成功打印：

```text
face_event: bridge service face_bridge port=301 ready for little-core face_netd
```

且 Linux 小核侧 `face_netd` 也已经能够连接上的情况下，现场日志仍反复表现为：

```text
face_netd: connected to RT bridge service=face_bridge id=5
face_netd: RT bridge disconnected, waiting for reconnect
face_netd: connected to RT bridge service=face_bridge id=5
face_netd: RT bridge disconnected, waiting for reconnect
...
```

RT-Smart 串口则周期性出现：

```text
[IPCMSG]:ioctl connect fail
```

这说明问题已经不再是“端口非法”或“完全无法连接”，而是“连接建立后被立即断开”。

#### 根因

根因位于 `linux_bridge/main.cpp` 的 IPC 监督线程：

- `face_netd` 建连成功后会启动一个后台线程执行 `kd_ipcmsg_run(ipc_id)`
- 主线程则立即轮询 `g_rt.runner_alive`
- 但初版代码里，`g_rt.runner_alive = true` 是在子线程入口 `run_thread_body()` 内部才设置的
- 当主线程调度更快时，会先读到 `false`
- 于是 supervisor 误判接收线程未运行，立刻执行 `kd_ipcmsg_disconnect(ipc_id)`

因此这其实是一个“连接线程启动时序”的竞态问题，而不是 IPCMSG 协议或端口配置错误。

#### 修复内容

本次将 `runner_alive` 的置位时机前移到启动线程之前：

- 在 `std::thread runner(run_thread_body, ipc_id);` 之前先执行 `g_rt.runner_alive.store(true);`
- `run_thread_body()` 中不再重复置位，只在 `kd_ipcmsg_run()` 返回后负责置回 `false`

同时顺手补充了两类诊断信息：

- 启动前检查 `/dev/ipcm_user` 是否存在，提前提示 IPC 驱动未就绪
- `kd_ipcmsg_try_connect()` 重试时输出更明确的等待日志，提示应先确认 RT 侧 `face_event.elf` 已打印 bridge ready

修复文件：

- `linux_bridge/main.cpp`
- `README.md`

#### 结果

修复后：

- `face_netd` 与 `face_event.elf` 的 IPCMSG 连接不再出现“刚连上就断开”的循环
- 板端日志从反复的 `connected -> disconnected` 切换为稳定保持连接
- 之前联调清单中“IPCMSG 连接是否稳定”这一项已经得到实质性修复

### 5.5 Bug 5：WSL / Docker 环境下板子无法连通 MQTT

#### 现象

在 RT-Smart 与 Linux 小核的 IPCMSG 都已经正常后，网页仍长期显示“暂无设备”。

板端 `face_netd` 日志表现为：

```text
face_netd: mqtt dial attempt=1 url=mqtt://192.168.160.8:1883
face_netd: mqtt event OPEN id=1 url=mqtt://192.168.160.8:1883
face_netd: mqtt event RESOLVE id=1
face_netd: mqtt pending id=1 url=mqtt://192.168.160.8:1883 elapsed=...
  resolving=0 connecting=1 writable=0 readable=0 send=0 recv=0
```

而电脑端 `face-web` 已经能够被浏览器访问，但 `mosquitto` 中看不到来自板子的稳定 MQTT 连接。

#### 根因

根因最终定位为电脑端部署环境问题：

- `server_pc` 运行在 WSL + Docker 中
- WSL 内部虽然显示 `1883` / `8000` 监听在 `0.0.0.0`
- 但 Windows 主机侧实际只监听在 `127.0.0.1` / `::1`
- Windows 局域网地址 `192.168.160.8` 上没有对外暴露 `1883` / `8000`
- 系统也没有配置 `netsh interface portproxy`

因此板子访问：

```text
mqtt://192.168.160.8:1883
```

时，TCP 握手会长期卡在 `connecting=1`，从而无法进入真正的 MQTT `CONNECT / CONNACK` 阶段。

网页端之所以一直“暂无设备”，是因为 `face-web` 只有在收到 `k230/<device_id>/up/status` 等上行消息后，才会创建设备记录。

#### 修复内容

本次修复包含两部分：

1. 部署环境修复

- 在 Windows 管理员 PowerShell 中添加 `portproxy`：

```powershell
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=1883 connectaddress=127.0.0.1 connectport=1883
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8000 connectaddress=127.0.0.1 connectport=8000
```

- 同时放通 Windows 防火墙：

```powershell
New-NetFirewallRule -DisplayName "face-mqtt-1883" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 1883
New-NetFirewallRule -DisplayName "face-web-8000" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8000
```

2. 代码与可观测性修复

- `face_netd` 的 MQTT 建连路径改为显式记录 `OPEN / RESOLVE / CONNECT / READ / WRITE`
- 增加 `pending` 日志和建连超时自动重试
- 完整记录 `CONNECT`、`CONNACK` 与后续包收发过程

修复文件：

- `linux_bridge/main.cpp`
- `README.md`
- `linux_bridge/README.md`
- `AGENTS.md`
- `CLAUDE.md`
- `archive/260425_WSL_PORTPROXY_AND_MQTT_CONNECTIVITY_FIX.md`

#### 结果

修复后，板端日志恢复为：

```text
face_netd: mqtt event CONNECT id=1 send CONNECT ...
face_netd: mqtt packet cmd=2 len=4
face_netd: mqtt connected mqtt://192.168.160.8:1883
```

最终网页端可以正常看到设备上线、状态心跳、事件和命令结果。

## 6. 本轮最终结果

到本次会话结束时，这套网络通信链路已经具备以下落地成果：

### 6.1 架构层面

已经形成完整的四段式通信链路：

```text
网页
  -> face-web
  -> MQTT
  -> face_netd
  -> IPCMSG
  -> RT-Smart 三进程
```

### 6.2 RT 侧能力

RT 侧已经具备：

- 远程命令接入
- 单命令串行执行
- 识别/陌生人/活体失败事件上送
- 注册/清库/人数查询/关机结果回传
- 远程请求与网页 `request_id` 的关联能力

### 6.3 Linux 小核能力

Linux 小核已经具备：

- MQTT 客户端
- IPCMSG 桥接客户端/服务管理
- IPCMSG 连接稳定保持
- 设备在线状态上报
- 远程命令转发
- 事件与结果上送
- 可配置构建策略
- 更符合板端环境的 glibc 动态链接默认构建方式
- 更明确的 IPC 驱动与连接诊断日志
- MQTT 建连过程的细粒度调试日志
- MQTT 建连超时自动重试

### 6.4 电脑端能力

电脑端已经具备：

- MQTT Broker 部署方案
- FastAPI 服务
- WebSocket 实时推送
- SQLite 持久化设备状态与事件
- 单页网页控制台
- 面向 WSL / Docker 环境的 Windows `portproxy` 启动说明

## 7. 本轮修改文件总览

### 7.1 RT-Smart 相关

- `src/ipc_proto.h`
- `src/face_event_main.cc`
- `src/face_video_main.cc`
- `src/face_ai_main.cc`
- `src/CMakeLists.txt`

### 7.2 Linux 小核桥接

- `linux_bridge/main.cpp`
- `linux_bridge/Makefile`
- `linux_bridge/build_face_netd.sh`
- `linux_bridge/face_netd.ini`
- `linux_bridge/README.md`
- `linux_bridge/third_party/mongoose/mongoose.c`
- `linux_bridge/third_party/mongoose/mongoose.h`

### 7.3 电脑端服务器与网页

- `server_pc/app/main.py`
- `server_pc/app/__init__.py`
- `server_pc/Dockerfile`
- `server_pc/docker-compose.yml`
- `server_pc/mosquitto/mosquitto.conf`
- `server_pc/requirements.txt`
- `server_pc/static/index.html`
- `server_pc/static/app.js`
- `server_pc/static/style.css`
- `server_pc/data/.gitkeep`

### 7.4 文档与配置

- `archive/260425_NETWORK_COMMUNICATION_IMPLEMENTATION_AND_FIXES.md`
- `archive/260425_WSL_PORTPROXY_AND_MQTT_CONNECTIVITY_FIX.md`
- `.gitignore`
- `README.md`
- `AGENTS.md`
- `CLAUDE.md`

## 8. 当前建议的使用方式

### 8.1 Linux 小核编译

当前建议优先使用动态链接方式编译：

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/linux_bridge
./build_face_netd.sh
```

如需调试版：

```bash
DEBUG=1 DO_STRIP=0 ./build_face_netd.sh
```

如确有需要再尝试静态版：

```bash
STATIC=1 ./build_face_netd.sh
```

### 8.2 Linux 小核部署

板端部署目录约定为：

```text
/sharefs/face_bridge/
```

建议部署文件：

- `face_netd`
- `face_netd.ini`

启动：

```sh
cd /sharefs/face_bridge
chmod +x ./face_netd
./face_netd --config ./face_netd.ini
```

### 8.3 电脑端启动

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/server_pc
docker compose up --build
```

访问：

```text
http://<电脑IP>:8000
```

## 9. 已解决问题与剩余关注点

### 9.1 已解决

- RT 侧桥接协议缺失
- `face_event` 无远程桥接能力
- `face_video` 无远程结果回传
- `face_ai` 无结构化业务结果
- 缺少 Linux 小核桥接程序
- 缺少电脑端服务与网页
- Linux 小核工具链配置错误
- Linux 小核静态链接导致的运行时段错误风险
- IPCMSG 端口超范围问题
- IPCMSG 建连后立即断开的竞态问题
- WSL / Docker 环境下 Windows 未对外暴露 `1883/8000` 导致的 MQTT 不通问题

### 9.2 仍需板端联调确认

虽然这轮已经完成实现与多处构建验证，但仍建议继续在真实板端检查：

1. `face_event.elf` 与 Linux 小核 `face_netd` 长时间运行下的 IPCMSG 连接是否持续稳定
2. `register_current` 在单人/多人/活体失败场景下的网页提示是否符合预期
3. `shutdown` 后网页离线状态回写是否符合预期
4. 断网重连、Broker 重启后的自动恢复是否符合预期
5. `face_netd.ini` 中 MQTT 地址、端口、设备 ID 是否与局域网环境一致
6. 若电脑端运行在 WSL / Docker 中，Windows `portproxy` 与防火墙规则是否已正确配置

## 10. 总结

本次会话已经把“电脑服务器 + 网页 + 反向控制开发板”的 v1 方案完整落到了代码里，并在此基础上由用户继续修复了 5 个关键问题：

1. Linux 小核交叉编译工具链应切换为 Xuantie glibc 工具链
2. `face_netd` 默认构建策略应从静态链接调整为更稳妥的动态链接
3. IPCMSG 服务端口必须满足 `[0, 512)`，因此桥接端口最终调整为 `301`
4. `face_netd` 的 IPC 监督线程存在启动竞态，导致桥接连接建立后立即断开，需要前移 `runner_alive` 的置位时机
5. 当电脑端服务运行在 WSL / Docker 中时，还必须额外配置 Windows `portproxy` 与防火墙，否则板子无法通过 Windows 局域网地址接入 MQTT

至此，这套功能已经从“方案设计”推进到“工程落地 + 初步可运行 + 已修复关键运行问题”的阶段。
