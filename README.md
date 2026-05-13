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

在开发机宿主机环境中执行：

```bash
cd /home/hyperlovimia/k230_sdk
make prepare_toolchain
```

```bash
cd src/reference/ai_poc/my_face_recognition
./build_app.sh
```

项目必须位于 `k230_sdk/src/reference/ai_poc/my_face_recognition`。
`build_app.sh` 会优先使用 `k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin`。

生成的编译产物在 `k230_bin` 目录中。将以下文件按需同步到板端 `/data`：

- `face_ai.elf`
- `face_video.elf`
- `face_event.elf`
- `face_detection_320.kmodel`
- `GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel`
- `face_antispoof.kmodel`，仅启用活体检测时需要

## RT-Smart 板端启动

RT-Smart 的 `msh` 不能按常规 Linux shell 使用，不要依赖 `export`、复杂变量展开或仓库脚本自动启动三进程。请在板端串口中按顺序直接输入命令。

启用活体检测时，先后台启动 `face_ai.elf`，最后一个参数传入活体模型：

```sh
/sharefs/face_ai.elf /sharefs/face_detection_320.kmodel 0.38 0.30 /sharefs/GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 68 /sharefs/face_db 0 /sharefs/face_antispoof.kmodel 0.18 real0 &
```

> 说明：`0.18` 为 REAL 概率阈值（真人分数常在 0.15～0.30 间波动时可从偏低试起，翻拍易过再提到 0.22～0.25）；`real0` 表示该 kmodel 约定 **out[0]=REAL**（与常见训练导出一致）。若你的模型确认为 **out[0]=SPOOF**，则不要加 `real0`，只要 9 个参数或仅「模型路径 + 阈值」共 10 个参数即可，详见下文「参数说明」表。

暂不启用活体检测时，使用 8 参数启动 `face_ai.elf`：

```sh
/sharefs/face_ai.elf /sharefs/face_detection_320.kmodel 0.38 0.30 /sharefs/GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 68 /sharefs/face_db 0 &
```

然后后台启动视频进程：

```sh
/sharefs/face_video.elf 0 &
```

最后以前台方式启动交互入口：

```sh
/sharefs/face_event.elf /tmp/attendance.log
```

启动后，所有交互命令都在 `face_event.elf` 所在串口输入。`/sharefs/face_db` 不存在时，`face_ai.elf` 会尝试自动创建。

当前版本默认在 `face_event.elf` 内启用“门状态指示”控制：

- 门打开状态映射到板载 `LED1`
- `LED1` 使用 `BANK0_GPIO6`
- 低电平点亮
- 开门保持：`3s`
- 默认不驱动第二路蜂鸣器/继电器 GPIO
- 默认关闭写后 `GPIO_READ_VALUE` 自校验，避免板载 LED 场景因输出脚读回不可靠误判 `FAULT`

这些值由 [src/door_control_config.h](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/src/door_control_config.h) 的编译期宏控制。若目标板接线、极性或时长不同，请修改该头文件后重新执行 `./build_app.sh`。

程序支持两种退出方式：

- 输入 `q` 后回车，走正常清理退出
- 按 `Ctrl+C`，触发优雅退出并执行 `PipeLine::Destroy()`

如果退出日志里能看到 `PipeLine::Destroy` 相关输出，说明视频链路的清理路径已经真正执行，可直接再次启动，无需重启板子。

## 参数说明

`face_ai.elf` 参数格式：

```text
face_ai <kmodel_det> <det_thres> <nms_thres> <kmodel_recg> <recg_thres> <db_dir> <debug_mode>
    [<face_antispoof.kmodel> [<real_prob_threshold> [real0|idx0]]]
```

| 参数        | 说明                             | 取值范围     |
| ----------- | -------------------------------- | ------------ |
| kmodel_det  | 人脸检测kmodel路径               | kmodel 路径         |
| det_thres   | 检测框置信度下限（`face_detection.cc` 中低于此的 anchor 丢弃）。**越高越严、易漏检**；易漏检可试 `0.32`～`0.40` | 0.0~1.0      |
| nms_thres   | NMS 的 IoU 门槛：两框 IoU≥此值则抑制低分框。**略调高**（如 `0.30`～`0.35`）可减轻「好框被旁边候选误杀」 | 0.0~1.0      |
| kmodel_recg | 人脸识别kmodel路径               | kmodel 路径         |
| recg_thres  | 人脸识别分数下限（`cal_cosine_distance` 的 0~100 标尺）。**库内人多或易互认时推荐 `65`～`72`** | 0~100    |
| （库内≥2 人） | `FaceRecognition::database_search` | 除超过 `recg_thres` 外，还要求 **第一名与第二名分差 ≥ 8.5**（默认 `db_top2_margin_`），否则陌生人。帧间对特征做 **短时 EMA**（仅流式 `INFER`）减轻单帧误判 |
| db_dir      | 数据库目录，推荐 `/sharefs/face_db` | 数据库目录路径         |
| debug_mode  | 是否需要调试，0、1、2分别表示不调试、简单调试、详细调试 | 0、1、2 |
| face_antispoof.kmodel | 可选活体模型路径，存在且加载成功时启用活体 | kmodel 路径 |
| real_prob_threshold（第 10 个参数） | 启用活体时：REAL 概率 ≥ 该值判为真人；越高越严。默认 **0.32**（未传第 10 个时，代码内建） | 现场常试 **0.15**～`0.28`，按误拒/放过翻拍折中 |
| real0 或 idx0（第 11 个参数） | 仅当 kmodel 输出为 **out[0]=REAL、out[1]=SPOOF** 时追加（需同时给出第 10 个阈值）。仓库示例 `k230_bin/run_3process.sh` 已按此配置。若加此后「翻拍」也极易通过，说明你的模型实际是 **out[0]=SPOOF**，应去掉 `real0` | 字面量 `real0` / `idx0` |

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

## 门锁联动

`face_event.elf` 现在会直接消费 `face_ai -> face_event` 的识别事件并驱动门状态指示 GPIO：

- 仅 `recognized` 事件会触发开门
- `stranger` 与 `liveness_fail` 只保留日志/告警，不驱动 GPIO
- 授权用户开门后，板载 `LED1(GPIO6)` 点亮，`3s` 后自动熄灭
- 开门窗口内重复识别不会续期开门，避免人脸常驻导致门常开
- 若 `/dev/gpio` 初始化失败、写失败或读回校验不一致，门锁控制会进入 `FAULT`，后续仅保留识别与日志功能，不再继续驱动 GPIO

门锁动作和故障会额外写入考勤 JSONL 的 `meta` 记录，例如 `door_unlock`、`door_relock`、`door_fault`。

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
此时请先确认 RT 控制台已经出现 `face_event: bridge service face_bridge port=301 ready for little-core face_netd`，并确认板端 `/sharefs/face_event.elf` 是本项目当前版本的新二进制。

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
- 网页为 **React + Vite + TypeScript**（源码在 `server_pc/web/`，构建生成到 `server_pc/static/`），WebSocket 实时更新

启动：在 `server_pc` 下执行 `docker compose up --build` 即可。`Dockerfile` 为**多阶段**：构建阶段在镜像内 `npm install` / `npm run build`（需能拉取 `node:20-bookworm-slim`；可先 `docker pull node:20-bookworm-slim` 缓存在本机）。若**无法**访问 Docker Hub，可改为在宿主机用本机 Node 在 `server_pc/web` 里打好 `static/`，并改用本仓库的 `Dockerfile.prebuilt`（只复制 `static/`，不装 Node 镜像，见同目录下文件说明）或等价的单阶段 `COPY static`。

若**不用 Docker**、直接本机起 `uvicorn`，需先在 `server_pc/web` 执行 `npm install && npm run build`，或开发时用 Vite 代理（见 `server_pc/web/README.md`）。

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
Test-NetConnection 127.0.0.1 -Port 1883
Test-NetConnection 127.0.0.1 -Port 8000
```

注意：

- `netsh interface portproxy show all` 只能证明规则存在
- 它不能单独证明 `127.0.0.1:1883` 在重启后仍然真正转发到了 WSL / Docker 中的 `mosquitto`
- 如果板端日志已经出现 `mqtt event CONNECT` 和 `mqtt event WRITE`，但始终没有 `mqtt connected`，通常说明 TCP 前门已经接通，但后端 broker 没有返回 `CONNACK`
- 此时应优先检查 `docker compose ps`、`docker compose logs --tail=50 mosquitto face-web`，以及 Windows 上 `Test-NetConnection 127.0.0.1 -Port 1883`
- 即使 `127.0.0.1:1883` 探活通过，也仍可能出现“TCP 能连上，但 MQTT `CONNECT` 发出后收不到 `CONNACK`”的情况；这时也应把 `portproxy` 的 `connectaddress` 改成当前 WSL IP
- 使用 WSL IP 作为 `connectaddress` 时，要注意 WSL IP 在重启后可能变化

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
- `POST /api/devices/{device_id}/commands/register-preview`
- `POST /api/devices/{device_id}/commands/register-commit`
- `POST /api/devices/{device_id}/commands/register-cancel`
- `POST /api/devices/{device_id}/commands/shutdown`
- `GET /ws`

## 完整启动流程

### 1. 开发机编译 RT-Smart 三进程

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition
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
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=1883 connectaddress=<WSL地址> connectport=1883
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8000 connectaddress=<WSL地址> connectport=8000
New-NetFirewallRule -DisplayName "face-mqtt-1883" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 1883
New-NetFirewallRule -DisplayName "face-web-8000" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8000
```

重启电脑后，建议马上补做：

```powershell
Test-NetConnection <WSL地址> -Port 1883
Test-NetConnection <WSL地址> -Port 8000
```

若不通，说明 `portproxy` 规则虽然还在，但它背后的 `<WSL地址> -> WSL/Docker` 这一跳没有恢复；此时可用 `wsl hostname -I` 取当前 WSL IP，并把 `connectaddress` 改成该 IP 重新写入 `portproxy`。

若 此前填的WSL地址是 `127.0.0.1:1883`，而它看起来是通的，但板端仍然卡在“已发送 MQTT `CONNECT`、始终收不到 `CONNACK`”，也建议直接改为当前 WSL IP 作为 `connectaddress`。这是因为某些环境下 `localhost` 探活成功并不等于真实 MQTT 流量已经正确转发到 Broker。

### 5. 设置 `face_netd.ini`

编辑 `linux_bridge/face_netd.ini`：

- `device_id` 填设备标识
- `mqtt_url` 填电脑主机局域网 IPv4，例如 `mqtt://192.168.160.8:1883`

不要填写：

- `127.0.0.1`
- `vEthernet (WSL)` 的 `172.23.x.x`
- WSL 内部容器地址

### 6. 板端 RT-Smart 启动三进程

易 **漏检人脸** 时，优先把 `face_ai` 第 2、3 个参数由 `0.5` / `0.2` 调成约 **`0.38` / `0.30`**（与 `k230_bin/run_3process.sh` 一致）。

启用活体检测时：

```sh
/sharefs/face_ai.elf /sharefs/face_detection_320.kmodel 0.38 0.30 /sharefs/GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 68 /sharefs/face_db 0 /sharefs/face_antispoof.kmodel &
/sharefs/face_video.elf 0 &
/sharefs/face_event.elf /tmp/attendance.log
```

不启用活体检测时：

```sh
/sharefs/face_ai.elf /sharefs/face_detection_320.kmodel 0.38 0.30 /sharefs/GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 68 /sharefs/face_db 0 &
/sharefs/face_video.elf 0 &
/sharefs/face_event.elf /tmp/attendance.log
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
2. `register-preview` / `register-commit`（或一键 `register-current`）
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
6. 若板端已打印 `mqtt event CONNECT` 和 `mqtt event WRITE` 但没有 `mqtt connected`，Windows 上 `Test-NetConnection 127.0.0.1 -Port 1883` 是否仍然通过

## v1 边界

当前版本有意保持简单：

- 不做实时视频和抓拍图
- 不做 TLS、账号权限、多租户
- 默认单板、单电脑、同一受信任局域网
- 调试阶段不要同时从串口和网页并发下发命令
