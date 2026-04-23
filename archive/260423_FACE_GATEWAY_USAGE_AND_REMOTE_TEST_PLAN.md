# my_face_recognition + face_gateway 当前状态与后续开发说明

本文档基于当前代码状态整理，时间点为 2026-04-23。

## 1. 当前真实状态

你的工程已经收敛为**两大路径**（小核业务已并入本仓库，不再使用独立的 `face_gateway` 顶仓目录）：

- 大核 RT-Smart 业务源码（三进程 + 资源）：
  `k230_sdk/src/reference/ai_poc/my_face_recognition/`
- 小核 Linux 网关业务源码（**唯一维护处**）：
  `k230_sdk/src/reference/ai_poc/my_face_recognition/src/little/`
- 小核 Buildroot 包入口（仅指向上述目录，不重复存业务）：
  `k230_sdk/src/little/buildroot-ext/package/face_gateway/`

这意味着：

- 大核继续负责摄像头、推理、识别、数据库操作
- 小核负责联网、HTTP 接口、对外状态展示
- 小核与 `my_face_recognition` **同库同版本**，`face_gateway.mk` 已改为指向 `.../my_face_recognition/src/little`

## 2. 现在已经完成的部分

### 2.1 大核侧

`my_face_recognition` 当前已经有三进程方案：

- `face_ai.elf`
- `face_video.elf`
- `face_event.elf`

编译方式仍然是：

```bash
cd /home/fearsixsix/k230_sdk/src/reference/ai_poc/my_face_recognition
./build_app.sh
```

主要产物在 `k230_bin/`：

- `face_ai.elf`
- `face_video.elf`
- `face_event.elf`
- `face_detection_320.kmodel`
- `face_recognition.kmodel`
- `face_antispoof.kmodel`

### 2.2 小核侧

`face_gateway`（可执行名不变）当前具备：

- 源码与 `my_face_recognition` 同仓：`src/little/`
- Buildroot 外部位：`package/face_gateway/`
- HTTP 服务与简单首页
- IPCMSG 客户端骨架；支持 `--no-ipc`、`--mock`（无 `/dev/ipcm_user` 时调 HTTP/契约）

当前小核包入口文件：

- `/home/fearsixsix/k230_sdk/src/little/buildroot-ext/package/face_gateway/Config.in`
- `/home/fearsixsix/k230_sdk/src/little/buildroot-ext/package/face_gateway/face_gateway.mk`

当前 `face_gateway` 支持的基础接口：

- `GET /`
- `GET /api/ping`
- `GET /api/status`
- `GET /api/system`
- `GET /api/network`
- `GET /api/ipc/last`
- `GET /api/ipc/send?...`

## 3. 当前最重要的判断

你现在最大的“未打通点”不是目录，而是通信机制。

### 3.1 大核当前用的是 RT-Smart 内部 IPC

`my_face_recognition` 现有三进程之间使用的是：

- `rt_channel`
- `lwp_shm`

相关通道目前包括：

- `IPC_FACE_AI_CHANNEL`
- `IPC_FACE_EVT_CHANNEL`
- `IPC_FACE_VIDEO_CTRL`

也就是说，当前大核内部已经有一套“进程间通信”，但这套通信是给 RT-Smart 内部的 `face_ai / face_video / face_event` 用的。

### 3.2 小核当前准备的是跨核 IPCMSG

`face_gateway` 当前准备对接的是 Linux <-> RT-Smart 的 `IPCMSG` 服务，默认目标是类似：

- `face_ctrl`

所以现在的真实情况是：

- 大核内部通信已经有了
- 小核跨核通信骨架也有了
- 但“大核对外暴露给小核的 IPCMSG 服务”还没有真正实现

这就是为什么现在 `face_gateway` 还更适合做网络连通性和接口框架测试，而不是直接拿到完整识别状态。

## 4. 当前建议的最终架构

推荐你后续按这个结构继续做：

1. 大核保留现有三进程架构
2. 新增一个大核 `face_ctrl` 控制服务
3. 小核 `face_gateway` 通过 `IPCMSG` 连接 `face_ctrl`
4. Web 页面只访问小核 HTTP API

其中第 2 步是关键。

## 5. 大核最推荐的实现方式

按你现在的代码状态，最稳的做法不是直接把小核逻辑塞进 `face_ai.elf`，而是新加一个“大核桥接层”。

建议新增一个大核进程，例如：

- `face_ctrl.elf`

它的职责是：

- 对小核：提供 `IPCMSG` 服务 `face_ctrl`
- 对大核内部：调用已有 `rt_channel` 能力
- 把小核控制命令翻译成你现有三进程能理解的动作

这样做的好处是：

- 不会把 `face_ai.elf` 改得过重
- 不会打乱现有三进程结构
- 小核协议和大核内部协议可以分层管理

## 6. 推荐的命令映射

当前最适合先打通的最小命令集：

- `PING`
- `GET_STATUS`
- `GET_DB_COUNT`
- `DB_RESET`
- `SHUTDOWN`

建议映射关系：

- `GET_DB_COUNT` -> 调用大核现有 `IPC_CMD_DB_COUNT`
- `DB_RESET` -> 调用大核现有 `IPC_CMD_DB_RESET`
- `SHUTDOWN` -> 调用大核现有 `IPC_CMD_SHUTDOWN`
- `GET_STATUS` -> 由 `face_ctrl` 自己汇总生成状态结构
- `PING` -> `face_ctrl` 直接本地应答

建议 `GET_STATUS` 返回最小字段：

- `ai_ready`
- `video_ready`
- `event_ready`
- `db_count`
- `last_name`
- `last_score`
- `last_is_live`
- `uptime_sec`

## 7. 小核目前能不能直接用

可以用，但要分清“能用到哪一步”。

### 7.1 现在就能用的部分

当前 `face_gateway` 可以先用于：

- 小核 HTTP 服务联通测试
- 局域网访问测试
- API 路径和 JSON 契约先定下来
- 浏览器端页面联调

### 7.2 现在还不能完全用的部分

当前它还不能真正完成下面这些事情：

- 查询大核识别状态
- 查询当前数据库人数
- 远程触发清库
- 远程触发注册流程

原因不是 `face_gateway` 目录不对，而是大核还没有把这些能力以 `IPCMSG` 服务形式暴露出来。

## 8. 关于独立小核包的当前状态

你现在已经有独立小核包了，这一步已经走对了。

当前事实是：

- `face_gateway` 已经有独立 Buildroot 包
- 这个包已经挂到 `src/little/buildroot-ext/Config.in`
- 但目前还没有发现任何 defconfig 打开了 `BR2_PACKAGE_FACE_GATEWAY=y`

当前我查到的是：

- `k230_evb_doorlock_defconfig` 里有 `BR2_PACKAGE_DOOR_LOCK=y`
- 还没有看到 `BR2_PACKAGE_FACE_GATEWAY=y`

这意味着：

- 新包已经存在
- 但还没有自动进入任何镜像配置

所以你后续真正要上板前，还要补一步：

- 在你要用的 defconfig 里打开 `BR2_PACKAGE_FACE_GATEWAY=y`

## 9. 当前建议的代码维护口径

与 `face_gateway` 相关请**只维护一处**业务源码，避免与旧独立目录双改：

- **主业务源码（唯一准路径）**  
  `k230_sdk/src/reference/ai_poc/my_face_recognition/src/little/`
- **小核包入口**（`FACE_GATEWAY_SITE` 已指向上项）  
  `k230_sdk/src/little/buildroot-ext/package/face_gateway/`

已删除/废弃的独立路径 `.../face_gateway/little` 不再使用；以本仓库 `src/little` 为准。

### 9.1 无板子时的 mock 模式

小核上可直接验证 HTTP 与 `/api/ipc/send` 的 JSON 结构（不连接真实 IPC）：

```sh
face_gateway --mock
```

用于网页、curl、与队友对齐字段；与大核实联时再改用 `face_gateway --ipc-service face_ctrl --ipc-port 110`。

## 10. 上板后的最小使用流程

### 10.1 先编大核程序

```bash
cd /home/fearsixsix/k230_sdk/src/reference/ai_poc/my_face_recognition
./build_app.sh
```

### 10.2 板端启动大核三进程

启用活体时：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/face_recognition.kmodel 70 /data/face_db 0 /data/face_antispoof.kmodel &
/data/face_video.elf 0 &
/data/face_event.elf /tmp/attendance.log
```

不启用活体时：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/face_recognition.kmodel 70 /data/face_db 0 &
/data/face_video.elf 0 &
/data/face_event.elf /tmp/attendance.log
```

### 10.3 把小核包编进镜像

这一步现在还不是“自动完成”的，后面要记得做：

- 在你实际使用的 defconfig 里加入 `BR2_PACKAGE_FACE_GATEWAY=y`

### 10.4 小核侧启动网关

如果镜像里已经带了 `face_gateway`，建议先做第一阶段验证：

```sh
face_gateway --no-ipc
```

这一阶段只验证：

- HTTP 服务是否起来
- 局域网是否能访问
- `/api/ping`、`/api/network` 是否正常

等大核 `face_ctrl` 服务补好后，再进入第二阶段：

```sh
face_gateway --ipc-service face_ctrl --ipc-port 110
```

### 10.5 局域网测试

同网段 PC 上先测：

```bash
curl http://<BOARD_IP>:8080/api/ping
curl http://<BOARD_IP>:8080/api/status
curl http://<BOARD_IP>:8080/api/system
curl http://<BOARD_IP>:8080/api/network
```

浏览器直接访问：

```text
http://<BOARD_IP>:8080/
```

## 11. 你现在没有板子时，最值得做什么

没有板子时，最值得做的不是继续搬目录，而是把“通信方案”本身先敲定。

当前最推荐的顺序：

1. 固定 `face_gateway` 协议头
2. 设计 `face_ctrl` 的 `IPCMSG` 服务接口
3. 在大核新增 `face_ctrl` 桥接进程
4. 让 `face_gateway` 的 `/api/status` 真正调用 `GET_STATUS`
5. 再补 `/api/face/db_count`、`/api/face/db_reset`

## 12. 没有板子时，你自己能做的测试

### 12.1 编译验证

你自己先验证：

```bash
cd /home/fearsixsix/k230_sdk/src/reference/ai_poc/my_face_recognition
./build_app.sh
```

### 12.2 小核源码级测试

当前没有板子时，你至少可以先稳定下面这些 HTTP 接口和返回格式：

- `/api/ping`
- `/api/status`
- `/api/system`
- `/api/network`
- `/api/ipc/last`

### 12.3 mock 测试

`--mock` 已在本仓 `src/little` 中实现：不访问 `/dev/ipcm_user`，`/api/ipc/send` 在进程内累加统计并返回 `ok: true`，`/api/status` 中带 `"mock": true`。

## 13. 建议你发给队友的代测清单

### 13.1 请队友先确认

- 小核 IP 地址
- 小核 Linux 串口日志
- 大核 RT-Smart 串口日志
- 板端是否已有 `face_ai.elf`、`face_video.elf`、`face_event.elf`
- 板端镜像里是否已有 `face_gateway`

### 13.2 第一阶段代测

如果 `face_gateway` 已经进镜像，让队友先跑：

```sh
face_gateway --no-ipc
```

然后在 PC 上执行：

```bash
curl http://<BOARD_IP>:8080/api/ping
curl http://<BOARD_IP>:8080/api/status
curl http://<BOARD_IP>:8080/api/network
```

让队友回传：

- `face_gateway` 启动日志
- 三条 `curl` 输出
- 浏览器首页截图

### 13.3 第二阶段代测

让队友在 RT-Smart 侧启动三进程：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/face_recognition.kmodel 70 /data/face_db 0 &
/data/face_video.elf 0 &
/data/face_event.elf /tmp/attendance.log
```

让队友回传：

- 三个进程启动日志
- 摄像头画面是否正常
- 是否能注册人脸
- `n` 查询人数是否正常
- `d` 清库是否正常

### 13.4 第三阶段代测

等你补完 `face_ctrl` 以后，再让队友切到：

```sh
face_gateway --ipc-service face_ctrl --ipc-port 110
```

再测：

- `/api/status`
- `/api/ipc/last`
- 后续新增的 `/api/face/db_count`
- 后续新增的 `/api/face/db_reset`

## 14. 结论

你当前的方向没有问题，而且现在结构已经比前面清晰很多了：

- 大核业务放在 `my_face_recognition/`
- 小核 **HTTP 网关** 业务源码放在 `my_face_recognition/src/little/`
- 小核打包入口放在 `src/little/buildroot-ext/package/face_gateway/`（`face_gateway.mk` 指向前者）

你接下来真正要做的，不再是纠结目录，而是把这两层打通：

- 大核内部现有 `rt_channel` 能力
- 小核对接所需的 `IPCMSG` 服务

一句话概括你现在的阶段：

- 目录和包结构已经基本就位
- HTTP 网关骨架已经有了
- 真正还差的是一个大核 `face_ctrl` 桥接服务，以及把新包加进实际 defconfig
