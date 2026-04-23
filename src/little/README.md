# face_gateway（小核 Linux 网关）

与本仓库大核三进程（`face_ai` / `face_video` / `face_event`）配套的**小核**程序：在 Buildroot Linux 上跑 HTTP，便于局域网调试；后续通过 **IPCMSG** 与（待实现的）大核 `face_ctrl` 服务通信。

## 源码位置（唯一维护点）

- 本目录：`k230_sdk/src/reference/ai_poc/my_face_recognition/src/little`
- 协议常量：`src/face_gateway_protocol.h`（与大核 `face_ctrl` 约定对齐用）
- Buildroot 包（仅打包入口，不重复放业务代码）：
  `k230_sdk/src/little/buildroot-ext/package/face_gateway/`

大核侧仍用 `./build_app.sh` 生成 `k230_bin/*.elf`；小核侧用下面方式编进镜像。

## 功能概要

- 小核上提供 HTTP 服务（默认 `8080`）
- 上报本机网络接口、系统信息
- IPCMSG 客户端骨架，默认连接服务名 `face_ctrl`（大核侧需后续实现）
- 首页带简单按钮，便于浏览器联调

## 编进小核镜像

1. 在目标 defconfig 中开启：

   `BR2_PACKAGE_FACE_GATEWAY=y`

2. 重编小核根文件系统 / 完整镜像。  
   `face_gateway.mk` 已指向本目录，无需再单独拷贝 `face_gateway` 工程。

## 板端安装路径

- `/usr/bin/face_gateway`

## 运行示例

```sh
face_gateway
```

默认：`0.0.0.0:8080`，IPC 目标 `face_ctrl`，port `110`。

```sh
face_gateway --port 8080
face_gateway --ipc-service face_ctrl --ipc-port 110
face_gateway --no-ipc
```

无板子时仅调 HTTP/网页，可用 **mock**（不访问 `/dev/ipcm_user`，并模拟 IPC 侧数据）：

```sh
face_gateway --mock
```

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
- `GET /api/ipc/send?cmd=0x1001&module=0x1&payload=hello`
- `GET /api/help`

## 本机快速验证（小核上或带端口的测试环境）

```sh
curl http://127.0.0.1:8080/api/ping
curl http://127.0.0.1:8080/api/status
curl http://127.0.0.1:8080/api/network
```

## 说明

- 大核 ↔ 小核跨核通信依赖 IPCMSG 与**大核 `face_ctrl` 服务**；当前大核三进程仍使用 `rt_channel` 内部通信，与 `face_ctrl` 的对接见仓库 `archive/260423_FACE_GATEWAY_USAGE_AND_REMOTE_TEST_PLAN.md`。
- 勿将 `door_lock` 等无关服务当长期替代，除非你明确在做调试并清楚副作用。
