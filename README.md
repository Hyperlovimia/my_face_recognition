# 轻量化人脸识别示例

## 概述

本示例在 K230 / RT-Smart 上实现轻量级人脸识别，支持人脸检测、人脸识别、人脸注册、注册人数查询、数据库清空、三进程协作与可选静默活体检测。

当前推荐使用三进程模式：

- `face_ai.elf`：人脸检测、识别、数据库操作，可选加载活体模型
- `face_video.elf`：视频采集、显示与 OSD
- `face_event.elf`：单串口交互入口、事件输出、考勤日志与 RT 对外桥接中心

v1 新增一套“电脑服务器 + 网页 + 反向控制”的落地链路：

```text
浏览器
  <-HTTP/WebSocket->
电脑端 face-web
  <-MQTT->
Linux 小核 face_netd
  <-IPCMSG->
RT-Smart 大核三进程
```

说明：

- `MQTT` 只用于“电脑服务器 <-> Linux 小核”这一段
- `IPCMSG` 用于“Linux 小核 <-> RT-Smart 大核”控制与事件桥接
- `sharefs` 仅用于手动部署 `face_netd`，不承载实时消息
- v1 不做图片和实时视频，只做“状态、事件、控制”

## 编译

若 `make prepare_sourcecode` 报 `gzip: stdin: unexpected end of file`，是 **网络把压缩包下断了**；官方用 `wget | tar` 流式解压，容易失败。可改用本目录脚本 **分步下载再解压**（与 Makefile 中 kmodel / nncase / utils 地址一致，支持续传到 `k230_sdk/.k230_download_cache/`）：

```bash
cd src/reference/ai_poc/my_face_recognition
chmod +x download_prepare_bundles.sh
./download_prepare_bundles.sh
./build_app.sh
```

在开发机 SDK 环境中执行：

```bash
cd /home/hyperlovimia/k230_sdk
docker run -u root -it -v $(pwd):$(pwd) -v $(pwd)/toolchain:/opt/toolchain -w $(pwd) ghcr.io/kendryte/k230_sdk /bin/bash
```

```bash
cd src/reference/ai_poc/my_face_recognition
./build_app.sh
```

项目必须位于 `k230_sdk/src/reference/ai_poc/my_face_recognition`。

## 小核 Linux 网关（可选）

小核（Buildroot）上跑的 HTTP 服务 **`face_gateway`** 与本人脸工程放在**同一仓库**，源码目录为：

- `src/little/`（`CMakeLists.txt`、HTTP + IPCMSG 客户端）

在完整 SDK 中通过 **Buildroot 包** `src/little/buildroot-ext/package/face_gateway` 编进小核根文件系统；业务路径已指向上述 `src/little`，无需再维护独立的 `face_gateway` 工程目录。

- **大小核、face_gateway、队友/AI 搭环境、上板/联调（唯一入口）**：`archive/BIG_LITTLE_GUIDE.md`
- **k230_sdk 里除本目录外，与构建/小核包相关的落点**（工具链、MPP、ipcmsg、Buildroot 包、`door_lock` 对比）：`archive/OUTSIDE_MY_FACE_RECOGNITION_IN_SDK.md`
- 小核侧说明与接口列表：见 `src/little/README.md`

```bash
# 板端小核：仅测 HTTP、暂不连大核 IPC
face_gateway --no-ipc
# 与大核 face_ctrl 对接后（先起 face_ai、face_video、face_ctrl，再起小核网关）：
face_gateway --ipc-service face_ctrl --ipc-port 110
```

大核侧已提供 **`face_ctrl.elf`**（IPCMSG 服务名 `face_ctrl`，与 `face_event` 一样经 `IPC_FACE_VIDEO_CTRL` 驱动 `face_video`）。与串口 **`face_event` 二选一**作交互入口，避免两条线同时发控制。小核与大核通过 **IPCMSG** 协作；细节见 `archive/BIG_LITTLE_GUIDE.md`。

联调时可在**板端**加 **`FACE_DEBUG=1`**：小核 `face_gateway` 打 stderr，大核 `face_ctrl.elf` 打 stdout；小核另支持 **`--debug`**。详见 `src/little/README.md`。

## 大核 RT-Smart 产物

生成的编译产物在 `k230_bin` 目录中。将以下文件按需同步到板端 `/data`：

- `face_ai.elf`
- `face_video.elf`
- `face_event.elf`（串口交互；与小核控板二选一时可不用）
- `face_ctrl.elf`（小核/HTTP 经 IPCMSG 控板时于大核运行，默认与 `face_gateway` 服务名、端口一致）
- `face_detection_320.kmodel`
- `GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel`
- `face_antispoof.kmodel`，仅启用活体检测时需要

## RT-Smart 板端启动

RT-Smart 的 `msh` 不能按常规 Linux shell 使用，不要依赖 `export`、复杂变量展开或仓库脚本自动启动三进程。请在板端串口中按顺序直接输入命令。

启用活体检测时，先后台启动 `face_ai.elf`，最后一个参数传入活体模型：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 60 /data/face_db 0 /data/face_antispoof.kmodel &
```

暂不启用活体检测时，使用 8 参数启动 `face_ai.elf`：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 60 /data/face_db 0 &
```

然后后台启动视频进程：

```sh
/data/face_video.elf 0 &
```

**方式 A — 串口交互（原方式）** 以前台启动：

```sh
/data/face_event.elf /tmp/attendance.log
```

启动后，命令在 `face_event.elf` 所在串口输入。

**方式 B — 小核 HTTP 控板** 请**不要**再开 `face_event`，改为在 `face_ai`、`face_video` 已后台运行后启动：

```sh
/data/face_ctrl.elf
```

再在**小核 Linux** 上启动 `face_gateway`（见上文与 `src/little/README.md`）。`/data/face_db` 不存在时，`face_ai.elf` 会尝试自动创建。

程序支持两种退出方式：

- 输入 `q` 后回车，走正常清理退出
- 按 `Ctrl+C`，触发优雅退出并执行 `PipeLine::Destroy()`

如果退出日志里能看到 `PipeLine::Destroy` 相关输出，说明视频链路的清理路径已经真正执行，可直接再次启动，无需重启板子。

## 参数说明

`face_ai.elf` 参数格式：

```text
face_ai <kmodel_det> <det_thres> <nms_thres> <kmodel_recg> <recg_thres> <db_dir> <debug_mode> [<face_antispoof.kmodel>]
```

| 参数        | 说明                             | 取值范围     |
| ----------- | -------------------------------- | ------------ |
| kmodel_det  | 人脸检测kmodel路径               | kmodel 路径         |
| det_thres   | 人脸检测阈值，推荐 `0.5`          | 0.0~1.0      |
| nms_thres   | 人脸检测 NMS 阈值，推荐 `0.2`    | 0.0~1.0      |
| kmodel_recg | 人脸识别kmodel路径               | kmodel 路径         |
| recg_thres  | 人脸识别阈值，推荐 `60`          | 0~100    |
| db_dir      | 数据库目录，推荐 `/data/face_db` | 数据库目录路径         |
| debug_mode  | 是否需要调试，0、1、2分别表示不调试、简单调试、详细调试 | 0、1、2 |
| face_antispoof.kmodel | 可选活体模型路径，存在且加载成功时启用活体 | kmodel 路径 |

## 功能支持

| 功能           | 支持情况 |命令|
| -------------- | -------- |---|
| 打印帮助说明       | ✔        |h/help|
| dump注册帧       | ✔        |i|
| 清空人脸数据库   | ✔        |d|
| 人脸注册         | ✔        |i 后输入姓名 / i <姓名>|
| 注册人数查询     | ✔        |n|
| 退出程序         | ✔        |q / Ctrl+C|

> 注：
> 注册截图时请确保画面中仅有一张清晰可见的人脸。
> 姓名应使用可识别英文字符，避免特殊符号。

## 远程桥接能力

`face_event.elf` 现在除了保留串口入口，还会启动一个 `IPCMSG` 服务：

- 服务名：`face_bridge`
- 端口：`301`
- 连接方：Linux 小核 `face_netd`

远程命令固定为：

- `db_count`
- `db_reset`
- `register_current`
- `shutdown`

网页端命令最终会被转换成上述桥接命令，经 `face_event -> face_video -> face_ai` 原有链路执行。

## Linux 小核 face_netd

`face_netd` 是独立手动部署程序，不进入 Linux 镜像。

编译：

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/linux_bridge
./build_face_netd.sh
```

说明：

- 默认是动态链接构建（`STATIC=0`）。
- 如果需要静态链接：`STATIC=1 ./build_face_netd.sh`
- 如果需要调试符号并保留符号表：`DEBUG=1 DO_STRIP=0 ./build_face_netd.sh`

将以下文件复制到板端 `/sharefs/face_bridge/`：

- `linux_bridge/out/face_netd`
- `linux_bridge/face_netd.ini`

启动：

```sh
cd /sharefs/face_bridge
chmod +x ./face_netd
./face_netd --config ./face_netd.ini
```

如果启动后看到 `[IPCMSG]:ioctl connect fail`，通常表示 Linux 小核正在等待 RT-Smart 侧 `face_event.elf` 提供 `face_bridge` 服务。
此时请先确认 RT 控制台已经出现 `face_event: bridge service face_bridge port=301 ready for little-core face_netd`，并确认板端 `/data/face_event.elf` 是本项目当前版本的新二进制。

`face_netd.ini` 里至少需要修改：

- `device_id`
- `mqtt_url`

MQTT topic 约定：

- `k230/<device_id>/up/event`
- `k230/<device_id>/up/reply`
- `k230/<device_id>/up/status`
- `k230/<device_id>/down/cmd`

## 电脑端 server_pc

电脑端服务位于 `server_pc/`，使用：

- `Mosquitto` 作为 MQTT Broker
- `FastAPI + Paho MQTT + SQLite`
- 静态单页网页 + WebSocket 实时更新

启动：

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/server_pc
docker compose up --build
```

### 如果 `server_pc` 运行在 WSL / Docker 中

若电脑端服务运行在 WSL2 中，板子不能直接使用 `vEthernet (WSL)` 的 `172.23.x.x` 地址访问 MQTT。

此时应遵循以下规则：

- `face_netd.ini` 中的 `mqtt_url` 必须填写 Windows 主机真实局域网 IPv4
- 通常应填写 Windows `Wi-Fi` 或 `Ethernet` 网卡的地址，例如 `192.168.160.8`
- 不应填写 `WSL` 虚拟网卡地址

也就是说，`face_netd.ini` 应类似：

```ini
mqtt_url = mqtt://192.168.160.8:1883
```

同时，必须在 Windows 管理员 PowerShell 中额外配置端口映射：

```powershell
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=1883 connectaddress=127.0.0.1 connectport=1883
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8000 connectaddress=127.0.0.1 connectport=8000
```

并放通防火墙：

```powershell
New-NetFirewallRule -DisplayName "face-mqtt-1883" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 1883
New-NetFirewallRule -DisplayName "face-web-8000" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8000
```

推荐验证：

```powershell
netsh interface portproxy show all
Get-NetTCPConnection -LocalPort 1883,8000 -State Listen
```

若配置正确，板端 `face_netd` 应能最终打印：

```text
face_netd: mqtt connected mqtt://192.168.160.8:1883
```

启动后访问：

```text
http://<电脑IP>:8000
```

提供接口：

- `GET /api/devices`
- `GET /api/devices/{device_id}/state`
- `GET /api/devices/{device_id}/events?limit=100`
- `POST /api/devices/{device_id}/commands/db-count`
- `POST /api/devices/{device_id}/commands/db-reset`
- `POST /api/devices/{device_id}/commands/register-current`
- `POST /api/devices/{device_id}/commands/shutdown`
- `GET /ws`

## 完整启动流程

### 1. 开发机编译 RT-Smart 三进程

```bash
cd /home/hyperlovimia/k230_sdk
docker run -u root -it -v $(pwd):$(pwd) -v $(pwd)/toolchain:/opt/toolchain -w $(pwd) ghcr.io/kendryte/k230_sdk /bin/bash
cd src/reference/ai_poc/my_face_recognition
./build_app.sh
```

### 2. 开发机编译 Linux 小核 `face_netd`

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/linux_bridge
./build_face_netd.sh
```

### 3. 将文件同步到板端

RT-Smart 侧至少需要同步到 `/data`：

- `face_ai.elf`
- `face_video.elf`
- `face_event.elf`
- `face_detection_320.kmodel`
- `GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel`
- `face_antispoof.kmodel`，仅启用活体检测时需要

Linux 小核侧至少需要同步到 `/sharefs/face_bridge`：

- `linux_bridge/out/face_netd`
- `linux_bridge/face_netd.ini`

### 4. 电脑端启动 `server_pc`

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/server_pc
docker compose up --build
```

如果电脑端运行在 WSL / Docker 中，还必须在 Windows 管理员 PowerShell 中额外执行：

```powershell
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=1883 connectaddress=127.0.0.1 connectport=1883
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8000 connectaddress=127.0.0.1 connectport=8000
New-NetFirewallRule -DisplayName "face-mqtt-1883" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 1883
New-NetFirewallRule -DisplayName "face-web-8000" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8000
```

### 5. 设置 `face_netd.ini`

编辑 `linux_bridge/face_netd.ini`：

- `device_id` 填设备标识
- `mqtt_url` 填电脑主机局域网 IPv4，例如 `mqtt://192.168.160.8:1883`

不要填写：

- `127.0.0.1`
- `vEthernet (WSL)` 的 `172.23.x.x`
- WSL 内部容器地址

### 6. 板端 RT-Smart 启动三进程

启用活体检测时：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 60 /data/face_db 0 /data/face_antispoof.kmodel &
/data/face_video.elf 0 &
/data/face_event.elf /tmp/attendance.log
```

不启用活体检测时：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 60 /data/face_db 0 &
/data/face_video.elf 0 &
/data/face_event.elf /tmp/attendance.log
```

正常情况下，`face_event.elf` 会打印：

```text
face_event: bridge service face_bridge port=301 ready for little-core face_netd
```

### 7. 板端 Linux 小核启动 `face_netd`

```sh
cd /sharefs/face_bridge
chmod +x ./face_netd
./face_netd --config ./face_netd.ini
```

正常情况下应最终看到：

```text
face_netd: connected to RT bridge service=face_bridge id=...
face_netd: mqtt connected mqtt://<电脑IP>:1883
```

### 8. 浏览器打开网页

```text
http://<电脑IP>:8000
```

网页正常时应能够看到：

- 已上线设备
- 在线状态与 RT bridge 状态
- 事件列表
- 命令执行记录

### 9. 推荐联调验证

在电脑端可以开一个 MQTT 订阅窗口：

```bash
docker exec -it face-mosquitto mosquitto_sub -h 127.0.0.1 -t 'k230/+/up/#' -v
```

然后在网页依次测试：

1. `db-count`
2. `register-current`
3. `db-reset`
4. `shutdown`

## 常见问题

### 网页一直显示“暂无设备”

优先检查：

1. `face_netd` 是否已经打印 `mqtt connected ...`
2. `face_event.elf` 是否已经打印 `bridge service face_bridge port=301 ready ...`
3. 若电脑端运行在 WSL / Docker 中，Windows 是否已配置 `portproxy` 和防火墙规则
4. `face_netd.ini` 中的 `mqtt_url` 是否填写 Windows 主机局域网 IPv4，而不是 WSL 虚拟网卡地址
5. `docker compose logs -f mosquitto face-web` 中是否能看到来自板子的 MQTT 连接

## v1 边界

当前版本有意保持简单：

- 不做实时视频和抓拍图
- 不做 TLS、账号权限、多租户
- 默认单板、单电脑、同一受信任局域网
- 调试阶段不要同时从串口和网页并发下发命令
