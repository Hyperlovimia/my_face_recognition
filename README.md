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

最后以前台方式启动交互入口：

```sh
/data/face_event.elf /tmp/attendance.log
```

启动后，所有交互命令都在 `face_event.elf` 所在串口输入。`/data/face_db` 不存在时，`face_ai.elf` 会尝试自动创建。

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

## 推荐启动顺序

1. 开发机启动 `server_pc`，确认 `1883` 和 `8000` 可访问
2. 板端 RT-Smart 启动 `face_ai.elf`
3. 板端 RT-Smart 启动 `face_video.elf`
4. 板端 RT-Smart 启动 `face_event.elf`
5. 板端 Linux 小核启动 `face_netd`
6. 浏览器打开 `http://<电脑IP>:8000`

## v1 边界

当前版本有意保持简单：

- 不做实时视频和抓拍图
- 不做 TLS、账号权限、多租户
- 默认单板、单电脑、同一受信任局域网
- 调试阶段不要同时从串口和网页并发下发命令
