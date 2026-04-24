# 轻量化人脸识别示例

## 概述

本示例在 K230 / RT-Smart 上实现轻量级人脸识别，支持人脸检测、人脸识别、人脸注册、注册人数查询、数据库清空、三进程协作与可选静默活体检测。

当前推荐使用三进程模式：

- `face_ai.elf`：人脸检测、识别、数据库操作，可选加载活体模型
- `face_video.elf`：视频采集、显示与 OSD
- `face_event.elf`：单串口交互入口、事件输出与考勤日志

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
- `face_recognition.kmodel`
- `face_antispoof.kmodel`，仅启用活体检测时需要

## RT-Smart 板端启动

RT-Smart 的 `msh` 不能按常规 Linux shell 使用，不要依赖 `export`、复杂变量展开或仓库脚本自动启动三进程。请在板端串口中按顺序直接输入命令。

启用活体检测时，先后台启动 `face_ai.elf`，最后一个参数传入活体模型：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/face_recognition.kmodel 70 /data/face_db 0 /data/face_antispoof.kmodel &
```

暂不启用活体检测时，使用 8 参数启动 `face_ai.elf`：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/face_recognition.kmodel 70 /data/face_db 0 &
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
| recg_thres  | 人脸识别阈值，推荐 `70`          | 0~100    |
| db_dir      | 数据库目录，推荐 `/data/face_db` | 数据库目录路径         |
| debug_mode  | 是否需要调试，0、1、2分别表示不调试、简单调试、详细调试 | 0、1、2 |
| face_antispoof.kmodel | 可选活体模型路径，存在且加载成功时启用活体 | kmodel 路径 |

## 功能支持

| 功能           | 支持情况 |命令|
| -------------- | -------- |---|
| 打印帮助说明       | ✔        |h/help|
| dump注册帧       | ✔        |i|
| 清空人脸数据库   | ✔        |d|
| 人脸注册         | ✔        |输入人脸名称|
| 注册人数查询     | ✔        |n|
| 退出程序         | ✔        |q / Ctrl+C|

> 注：
> 注册截图时请确保画面中仅有一张清晰可见的人脸。
> 姓名应使用可识别英文字符，避免特殊符号。
