# face_gateway（小核 Linux 网关）

与本仓库大核三进程（`face_ai` / `face_video` / `face_event` 或 `face_ctrl`）配套的**小核**程序：在 Buildroot Linux 上跑 HTTP，经 **IPCMSG** 与 `face_ctrl.elf` 联调时控板。

## 源码位置（唯一维护点）

- 本目录：`k230_sdk/src/reference/ai_poc/my_face_recognition/src/little`
- 协议常量：`src/face_gateway_protocol.h`（与大核 `face_ctrl` 约定对齐用）
- Buildroot 包（仅打包入口，不重复放业务代码）：
  `k230_sdk/src/little/buildroot-ext/package/face_gateway/`

大核侧仍用 `./build_app.sh` 生成 `k230_bin/*.elf`；小核侧用下面方式编进镜像。

## 功能概要

- 小核上提供 HTTP 服务（默认 `8080`）
- 上报本机网络接口、系统信息
- IPCMSG 客户端，默认连接服务名 `face_ctrl`（大核起 `face_ctrl.elf`）
- 首页带简单按钮，便于浏览器联调

## 编进小核镜像

1. 在目标 defconfig 中开启：

   `BR2_PACKAGE_FACE_GATEWAY=y`

2. 重编小核根文件系统 / 完整镜像。  
   `face_gateway.mk` 已指向本目录，无需再单独拷贝 `face_gateway` 工程。

## 板端安装路径

- `/usr/bin/face_gateway`

## 运行示例（均在**板子小核 Linux**上执行）

默认：`0.0.0.0:8080`，IPC 目标 `face_ctrl`，port `110`。

```sh
face_gateway
face_gateway --port 8080
face_gateway --ipc-service face_ctrl --ipc-port 110
```

**仅验 HTTP、暂不连大核 IPC**（例如大核程序未就绪时，在板端小核上先 `curl` 通网口）：

```sh
face_gateway --no-ipc
```

**跨核控板**（先在大核按主 `README` 起 `face_ai`、`face_video`、`face_ctrl.elf`，再在小核执行）：

```sh
face_gateway --ipc-service face_ctrl --ipc-port 110
```

**调试**：`FACE_DEBUG=1` 与 `--debug` 等价。小核 **stderr** 打 `[face_gateway]` 前缀日志；大核 `FACE_DEBUG=1` 时 `face_ctrl.elf` 在 **stdout** 打细日志。

**板上自测示例**（终端 1 起 `face_gateway`，终端 2 或同网 PC）：

```sh
curl "http://127.0.0.1:8080/api/ping"
curl "http://127.0.0.1:8080/api/ipc/send?cmd=PING"
```

同网电脑访问时把 `127.0.0.1` 换成小核网口 IP（板端 `ip a` 查看）。

## HTTP 接口

- `GET /`
- `GET /api/ping`
- `GET /api/status`
- `GET /api/system`
- `GET /api/network`
- `GET /api/ipc/last`
- `GET /api/ipc/send?cmd=GET_STATUS`
- `GET /api/ipc/send?cmd=GET_DB_COUNT`
- `GET /api/ipc/send?cmd=DB_RESET`
- `GET /api/face/db_count`（等同 `.../api/ipc/send?cmd=GET_DB_COUNT`）
- `GET /api/face/db_reset`（等同 `.../api/ipc/send?cmd=DB_RESET`）
- `GET /api/ipc/send?cmd=0x1001&module=0x1&payload=hello`
- `GET /api/help`

## 说明

- 大核 ↔ 小核跨核通信依赖 IPCMSG 与**大核 `face_ctrl` 服务**；与 `face_ctrl` 的对接与分阶段验收见 `archive/BIG_LITTLE_GUIDE.md`。
- 勿将 `door_lock` 等无关服务当长期替代，除非你明确在做调试并清楚副作用。
