# 大小核与 face_gateway 说明（K230，东山派）

**本文件为「人脸工程 + 小核网关 + 大小核联调」的唯一入口**（原 `TEAMMATE_AI_SETUP_PLAYBOOK.md`、`260423_FACE_GATEWAY_USAGE_AND_REMOTE_TEST_PLAN.md`、`BIG_LITTLE_CONTROL_AND_DOOR_LOCK_REF.md` 已合并于此）。

- **大核可执行/协议**：`README.md`、`src/little/README.md`  
- **本目录下其它** `2604xx_*.md` 为历史变更记录，不替代本文。

---

## 一、仓库与 Git 范围

| 项 | 说明 |
|----|------|
| 业务与 Git | 仅 `k230_sdk/.../my_face_recognition` 用 Git 追踪。 |
| `k230_sdk` 其它处 | 常为本机手改（如 Buildroot defconfig），请自行备份。 |
| 路径约定 | 下文用 `K230_SDK_ROOT` 表示 SDK 根（例：`/home/xxx/k230_sdk`），不绑具体用户名。 |

**代码真源**：

- 大核三进程 + **`face_ctrl.elf`**：`my_face_recognition/`，`./build_app.sh` → `k230_bin/`
- 小核 **`face_gateway`** 源码：唯一 **`my_face_recognition/src/little/`**（不要与 SDK 里 `door_lock` 包内旧 gateway 双改）
- Buildroot 包入口：`K230_SDK_ROOT/src/little/buildroot-ext/package/face_gateway/`（`face_gateway.mk` 指向上项）
- **SDK 内除本目录外还有哪些关联赛点**（工具链、MPP/nncase、ipcmsg、`door_lock` 对比等）：`archive/OUTSIDE_MY_FACE_RECOGNITION_IN_SDK.md`  
- **全链路核对（含未随本仓追踪的 k230_sdk 落点、参数表、door_lock 分叉说明）**：同上文件 **§7**

---

## 二、架构总览

### 2.1 两套 IPC

| 范围 | 机制 | 用途 |
|------|------|------|
| 大核内 | `rt_channel` + `lwp_shm` | `face_ai` / `face_video` / `face_event` 与既有通道如 `IPC_FACE_VIDEO_CTRL` |
| 跨小核↔大核 | CDK **IPCMSG**（`/dev/ipcm_user` 等） | 小核 `face_gateway` ↔ 大核 **`face_ctrl.elf`（服务名 `face_ctrl`）** |

### 2.2 数据流（小核控板）

`小核 face_gateway` → `IPCMSG` → **大核 `face_ctrl.elf`** → 与 `face_event` 相同的 **`IPC_FACE_VIDEO_CTRL`** → `face_video` 内 `state` 分支（如查人数/清库与串口 `n`/`d` 一致）。  

- **与串口 `face_event.elf` 二选一**作人机控板，避免两路同时抢 `face_video` 的 ctrl 通道。  
- 小核用 **`send_only`** 时，大核**不回包**；参考 CDK `ipcmsg/sample`。

### 2.3 与 `door_lock` 包（SDK 内）的关系

- `package/door_lock/` 用 `src.mk` 把 UI+gateway 打成大包，其中 `gateway` 与你们 **`src/little` 同构**（HTTP + `ipcmsg`），可作**打包方式**参考。  
- **业务与协议**只维护 **`my_face_recognition`** 这一处。  
- 参考示例：`K230_SDK_ROOT/src/common/cdk/user/component/ipcmsg/sample/`。

### 2.4 `face_ctrl` 现状（v1）

- 源码：`src/face_ctrl_main.cc`，产物：`face_ctrl.elf`（已进 `k230_bin`）。  
- 已映射：`PING`；`GET_DB_COUNT` → 同串口 `n`；`DB_RESET` → 同 `d`；`SHUTDOWN` → 同 `q`；`GET_STATUS` 等为占位/日志。  
- **可后续做**：`GET_STATUS` 真汇总、注册类 CMD 与 `face_video` 的 state 2/3/5 对齐；若需回包要改小核为同步发送或新通道。

---

## 三、给 AI 的任务说明（可直接粘贴给助手）

1. 存在 `K230_SDK_ROOT` 与 `K230_SDK_ROOT/src/reference/ai_poc/my_face_recognition/build_app.sh`。  
2. 小核进镜像未默认打开时：在 `.../buildroot-ext/configs/k230_evb_defconfig` 加 `BR2_PACKAGE_FACE_GATEWAY=y`（**CONF=`k230_canmv_dongshanpi_defconfig` 且未另设 `CONFIG_BUILDROOT_DEFCONFIG` 时**，Buildroot 用 **`k230_evb`** 这份，见**第十一节**）。  
3. 大核产物在 `k230_bin/`，**不会**自动进小核 rootfs。  
4. 跨核需：大核起 **`face_ctrl.elf`**，小核 `face_gateway --ipc-service face_ctrl --ipc-port 110`；大核未就绪时可用 `--no-ipc` 在**板端小核**上先验 HTTP。

---

## 四、环境假设

- 大核交叉：`k230_sdk/toolchain/.../riscv64-unknown-linux-musl-gcc`；Docker 勿用**空**目录挂 `/opt/toolchain`。  
- 小核需能 `make buildroot` 或全量 `make`；`face_gateway` 在**板端小核 Linux**上运行（Buildroot 产物）。

---

## 五、目标分层（验收勿混）

| 层级 | 成功判据 |
|------|----------|
| L1 | `build_app.sh` 通过，`k230_bin` 有 `face_ai`/`face_video`/`face_event`/`face_ctrl` 等 |
| L2 | 小核 rootfs 有 `/usr/bin/face_gateway` |
| L3 | 板端小核 `face_gateway --no-ipc` 后，同网 `curl :8080/api/ping` 有 `ok`（**不依赖**大核） |
| L4 | README 方式起大核三进程，画面/串口业务正常 |
| L5 | 大核起 `face_ctrl.elf` + 小核真实 IPC，HTTP 能驱动与 `face_event` 等效的控板 |

---

## 六、操作顺序（必须按序）

1. `cd $K230_SDK_ROOT/src/reference/ai_poc/my_face_recognition && ./build_app.sh`  
2. 在 `k230_evb_defconfig` 增加 `BR2_PACKAGE_FACE_GATEWAY=y`（或按 **§11.4** 为东山派单独 defconfig）。  
3. `cd $K230_SDK_ROOT && make CONF=k230_canmv_dongshanpi_defconfig` 与 `make buildroot` 或全量 `make`。  
4. 将 `k230_bin/*.elf` 与模型拷到大核（如 `/data`）。  
5. **上板先验 L3**：小核 `face_gateway --no-ipc`，`curl` 小核网口 IP:8080（或在小核本机 `curl 127.0.0.1:8080`）。  
6. **L5**：大核起 `face_ai`、`face_video`，**不要**起 `face_event`；起 `face_ctrl.elf`；小核 `face_gateway --ipc-service face_ctrl --ipc-port 110`，再试 `/api/ipc/send?cmd=GET_DB_COUNT` 等。

---

## 七、检查表

```
[ ] K230_SDK_ROOT
[ ] build_app.sh 与 k230_bin 含 face_ctrl.elf
[ ] k230_evb_defconfig 已 BR2（或已独立 defconfig）
[ ] 镜像/板上有 face_gateway
[ ] L3：--no-ipc + curl
[ ] L4：三进程
[ ] L5：face_ctrl + 小核 IPC
```

---

## 八、关键路径速查

| 内容 | 路径 |
|------|------|
| 大核工程 | `K230_SDK_ROOT/src/reference/ai_poc/my_face_recognition` |
| 小核源码 | `.../my_face_recognition/src/little` |
| 协议头 | `src/little/src/face_gateway_protocol.h` |
| 大核 `face_ctrl` | `src/face_ctrl_main.cc` |
| 顶层 CONF | `K230_SDK_ROOT/configs/k230_canmv_dongshanpi_defconfig` |
| 默认要改的 Buildroot | `K230_SDK_ROOT/src/little/buildroot-ext/configs/k230_evb_defconfig` |
| 包 | `.../package/face_gateway/` |

小核可执行与 HTTP 见 **`src/little/README.md`**。

---

## 九、常见失败

| 现象 | 处理 |
|------|------|
| 找不到 musl 编译器 | 用 `K230_SDK_ROOT/toolchain`；Docker 勿空挂 `/opt/toolchain` |
| 编不出 face_gateway | 未加 `BR2_PACKAGE_FACE_GATEWAY` 或改错 defconfig |
| IPC 连不上 | 先起 `face_ctrl.elf`；核对服务名 `face_ctrl`、port `110`、与网关默认一致；检查 `/dev/ipcm_user` |
| curl 失败 | 网段、防火墙、小核 IP、进程与 8080 |

---

## 十、SDK / Buildroot / 东山派（defconfig 落点）

- `make CONF=k230_canmv_dongshanpi_defconfig` 时，主 `Makefile` 写 `.config`，**`parse.mak`** 决定子系统。  
- 若**没有** `CONFIG_BUILDROOT_DEFCONFIG=...`，则其 stem 来自 **`CONFIG_BOARD_NAME`**。东山派现成文件里为 **`"k230_evb"`** 且**无**单独 `CONFIG_BUILDROOT_DEFCONFIG` → 小核用 **`k230_evb_defconfig`**，**不是**名为 `k230_canmv_dongshanpi` 的 buildroot 文件。  
- 关键行（以本机为准）：`CONFIG_BOARD_K230_CANMV_DONGSHANPI=y`，`CONFIG_BOARD_NAME="k230_evb"`，`CONFIG_UBOOT_DEFCONFIG`/`CONFIG_LINUX_DEFCONFIG`/`CONFIG_LINUX_DTB` 为东山派。  
- **仅东山派**单独 Buildroot：复制 `k230_evb_defconfig` → `k230_canmv_dongshanpi_defconfig`，在顶层加 `CONFIG_BUILDROOT_DEFCONFIG="k230_canmv_dongshanpi"`。  
- **menuconfig**：`make buildroot-menuconfig` / `buildroot-savedefconfig`；改前 `grep CONFIG_BUILDROOT` / `CONFIG_BOARD_NAME` 根目录 `.config`。  
- **post_copy**：`gen_image` 时先试 `board/${CONFIG_BOARD_NAME}/post_copy_rootfs/`，无则用 `board/common/post_copy_rootfs/`。  
- 设备树/HTTP：`face_gateway` 一般**不必**为 8080 改 DTS。

若官方 defconfig 变更，**以你本机 `configs/k230_canmv_dongshanpi_defconfig` 与 `grep` 为准**。

---

## 十一、有板分阶段联调

- **有板、仅小核网**：`face_gateway --no-ipc` 后验 HTTP/JSON。  
- **有板、跨核控板**：大核 `face_ai`+`face_video`+`face_ctrl`（与 `face_event` 二选一），小核 `face_gateway --ipc-service face_ctrl --ipc-port 110`，再调 HTTP。

**分阶段**：

| 阶段 | 大核 | 小核 | 同网设备 |
|------|------|------|----------|
| A 先通网 | — | `face_gateway --no-ipc` | `curl` 小核 IP:8080 / 浏览器 |
| B 大核业务 | 三进程可含 `face_event` | 可与 A 并行 | 大核串口 |
| C 跨核 | `face_ctrl` + 与 `face_event` 二选一 | `face_gateway --ipc-service face_ctrl` | 调 `/api/ipc/send` 等 |

**队友代测（摘要）**：确认小核/大核 IP 与串口、镜像有 `face_gateway`；先 `curl` 小核 8080；大核起 `face_ctrl` 联调后按 C 列测。

---

## 十二、分工话术

- **大核应用**：`build_app.sh`、README 上板。  
- **小核镜像**：`k230_sdk` 里改对 defconfig + `make buildroot`。  
- **L3 联调**：小核能 `curl 8080` 即过，与大核无硬依赖。  
- **L5**：大核 `face_ctrl.elf` + 小核 `face_gateway` 真 IPC。

---

**协议与端点以 `src/little/README.md` 为准**；主 `README` 有板端启动与 **方式 A 串口 / 方式 B 小核** 的说明。  
**SDK 根下其它与本人脸工程相关的目录清单**：`archive/OUTSIDE_MY_FACE_RECOGNITION_IN_SDK.md`。
