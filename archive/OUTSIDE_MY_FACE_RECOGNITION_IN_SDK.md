# k230_sdk 中除 `my_face_recognition` 外，与本人脸工程相关的位置

**基准目录**：`K230_SDK_ROOT` = `k230_sdk` 根（例：`/home/fearsixsix/k230_sdk`）。  
**业务与协议源码**：以 `K230_SDK_ROOT/src/reference/ai_poc/my_face_recognition` 为唯一真源；下列路径为**构建依赖、打包入口、或参考/易混淆副本**，不替代该目录下维护。

---

## 1. 大核 `build_app.sh` 直接依赖的 SDK 路径

| 路径 | 作用 |
|------|------|
| `K230_SDK_ROOT/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/` | RISC-V musl 交叉工具链；`build_app.sh` 优先从此处查 `riscv64-unknown-linux-musl-gcc`（也支持 `/opt/toolchain/...` 等）。 |
| `K230_SDK_ROOT/src/big/mpp/` | 环境变量 `MPP_SRC_DIR`；CMake 链接 MPP 库、头文件。 |
| `K230_SDK_ROOT/src/big/nncase/` | 环境变量 `NNCASE_SRC_DIR`；nncase 运行时与 `rvv` 等。 |
| `K230_SDK_ROOT/src/big/utils/lib/opencv/` | 环境变量 `OPENCV_SRC_DIR`；OpenCV 头/库。 |
| `K230_SDK_ROOT/src/common/cdk/user/component/ipcmsg/` | `face_ctrl.elf` 使用：`include/`，大核侧库目录 `.../host/lib`，链接 `libipcmsg`；与大小核 **IPCMSG** 协议配套。 |
| `K230_SDK_ROOT/src/reference/ai_poc/my_face_recognition/cmake/` | 工程内 CMake 与 `link.lds` 等，属工程子目录；若只统计「本仓库外」，可忽略。 |

K 模型、测试图等通常来自 SDK 自带人脸演示资源（按需拷贝到板子 `/data` 或本工程 `k230_bin`）：

| 路径 | 说明 |
|------|------|
| `K230_SDK_ROOT/src/big/kmodel/ai_poc/kmodel/` | 官方示例 kmodel 集合（如 `face_detection_320.kmodel`、`face_recognition.kmodel` 等，以你实际使用的文件名为准）。 |
| `K230_SDK_ROOT/src/reference/ai_poc/build_app_sub.sh` | 与**其它** `ai_poc` 子项打包相关的脚本，引用 `.../big/kmodel/ai_poc/...`；**不**是 `my_face_recognition` 的 `build_app.sh`，但同属 `ai_poc` 与同一批模型资产树。 |

---

## 2. 小核 `face_gateway` 编进 rootfs 的「打包入口」（不在本工程目录内）

| 路径 | 内容 |
|------|------|
| `K230_SDK_ROOT/src/little/buildroot-ext/package/face_gateway/face_gateway.mk` | Buildroot 包定义：`FACE_GATEWAY_SITE` 指向 `.../src/reference/ai_poc/my_face_recognition/src/little`（**本地源码**），`cmake-package` 构建。 |
| `K230_SDK_ROOT/src/little/buildroot-ext/package/face_gateway/Config.in` | 菜单项 `BR2_PACKAGE_FACE_GATEWAY`。 |
| `K230_SDK_ROOT/src/little/buildroot-ext/Config.in` | 通过 `source .../face_gateway/Config.in` 把该包挂进小核 ext Buildroot。 |

在目标 **defconfig** 中需有 `BR2_PACKAGE_FACE_GATEWAY=y` 才会进镜像；常用编辑文件为  
`K230_SDK_ROOT/src/little/buildroot-ext/configs/k230_evb_defconfig`（若顶层未单独指定其它 Buildroot defconfig，东山派等板型常与 `k230_evb` 对应，**以你本机 `configs/*` 与 `parse.mak` 为准**，详见 `archive/BIG_LITTLE_GUIDE.md` 第十节）。

---

## 3. 易与本人脸工程混淆的「同构参考」（勿双改）

| 路径 | 说明 |
|------|------|
| `K230_SDK_ROOT/src/little/buildroot-ext/package/door_lock/src/gateway/` | 门锁等演示里**另一份** `face_gateway` 风格工程（`door_lock` IPC 等），与 **`my_face_recognition/src/little` 不是同一份业务**。协议、HTTP 落点以本人脸工程为准；此处仅作打包/对端服务名参考。 |

---

## 4. 顶层/全局配置中可能触达小核的「间接相关」

| 路径 | 说明 |
|------|------|
| `K230_SDK_ROOT/configs/k230_canmv_dongshanpi_defconfig`（等） | 选板、Linux/UBoot 等；是否用哪份 `Buildroot` defconfig 看其中的 `CONFIG_BUILDROOT_DEFCONFIG` / `CONFIG_BOARD_NAME`（以实际文件与官方更新为准）。 |
| `K230_SDK_ROOT/src/little/buildroot-ext/board/`、`.../post_copy_rootfs/` | rootfs 生成后复制脚本；**一般不必**为 `face_gateway` 的 8080 改 DTS/脚本，除非你有自定义 post 步骤。 |

---

## 5. IDE / 本机元数据（可选）

| 路径 | 说明 |
|------|------|
| `K230_SDK_ROOT/.idea/vcs.xml`（若存在） | 可能将 Git 根映射到 `.../my_face_recognition`；与编译无关。 |

---

## 6. 维护原则（防分叉）

- **大核可执行、协议、小核 `face_gateway` 业务代码**：只改 `my_face_recognition` 内文件，再 `build_app.sh` / 重编 Buildroot。  
- **要让小核带上网关**：动 `face_gateway` 的 **Buildroot 包** `face_gateway.mk` / `Config.in` 与 **defconfig 开关**，不要再去改 `door_lock` 里那份 gateway 当主工程。  
- **大核 `face_ctrl`**：链接与头文件已指向 `common/cdk/.../ipcmsg`；若升级 SDK，核对 `host/lib` 与头文件是否仍匹配。

---

## 7. 全链路核对（仓库内实现 + SDK 内未单独随本仓 Git 追踪部分）

以下为 **2026-04** 基于当前树的一次走读结论；若你升级 SDK 或改 defconfig，请再对表核对。

### 7.1 端到端数据路径

| 步骤 | 位置 | 说明 |
|------|------|------|
| 1 | 小核 `my_face_recognition/src/little/src/main.cpp` | HTTP server；`/api/ipc/send` 等调用 `kd_ipcmsg_send_only`，**仅发不收**（与 `face_ctrl` 侧 `on_ipcmsg` 里忽略 `bIsResp` 一致）。 |
| 2 | CDK `src/common/cdk/user/component/ipcmsg/` | 双方链 `libipcmsg`，经 `/dev/ipcm_user` 等；头文件 `k_ipcmsg.h` / `k_comm_ipcmsg.h` 定义 `u32Module` / `u32CMD` / `pBody`。 |
| 3 | 大核 `src/face_ctrl_main.cc` | `kd_ipcmsg_add_service("face_ctrl", …)` + `kd_ipcmsg_connect` + `kd_ipcmsg_run`；`on_ipcmsg` 里用 `u32CMD` 分派（**未使用** `pBody` 做业务，与纯 cmd 路径一致）。 |
| 4 | 同文件 + `ipc_proto.h` | `send_video_ctrl` → `rt_channel_send` → **`IPC_FACE_VIDEO_CTRL`**，与 `face_event` 同源。 |
| 5 | `src/face_video_main.cc` | 读 `ipc_video_ctrl_t` 的 `op`/`state`；`OP_SET` 且 `state==1` → 查库人数、`state==4` → 清库等，与串口 `n`/`d` 一致。 |

### 7.2 必须一致的跨核参数（小核默认 vs 大核写死）

| 项 | 小核 `main.cpp` 默认 | 大核 `face_ctrl_main.cc` | 结论 |
|----|----------------------|----------------------------|------|
| IPCMSG 服务名 | `face_ctrl` | `face_ctrl` | 一致 |
| Port | `110` | `110` | 一致 |
| RemoteId | `1` | `1` | 一致 |
| CMD 枚举 | `face_gateway_protocol.h` | 宏与头文件枚举**同值**（0x1000–0x1006） | 一致（大核用宏避免交叉 include） |

若启动时改 `--ipc-service` / `--ipc-port` / `--ipc-remote-id`，**大核须同步改** `face_ctrl` 内 `attr` 与 `add_service` 名字，否则会连不上。

### 7.3 `GET_DB_COUNT` / `DB_RESET` / `SHUTDOWN` 与 `face_event` 对齐

| 来源 | 行为 |
|------|------|
| `face_event` 串口 `n` / `d` / `q` | `IPC_VIDEO_CTRL_OP_SET` + `state` 1 / 4；`q` 为 `OP_QUIT` + 先 `usleep` 再发。 |
| `face_ctrl` HTTP 映射 | `GET_DB_COUNT`→`SET,1`；`DB_RESET`→`SET,4`；`SHUTDOWN`→`OP_QUIT,0` + 本进程 `g_stop`，**无** `usleep`。 |

差异：关断时 `face_event` 多 100ms 延时；一般可接受。`GET_STATUS` 在大核仍为**占位日志**，不读 `face_video` 结构体。

### 7.4 Buildroot 与镜像（SDK 树内、常不随 my_face Git）

- `package/face_gateway/face_gateway.mk` 已指向 **`.../my_face_recognition/src/little`**，编进 rootfs 的即该目录。
- 在**本仓库 `k230_evb_defconfig` 样例中未检索到** `BR2_PACKAGE_FACE_GATEWAY=y`：默认镜像**可能不含** `face_gateway`，需在所用 defconfig 中**手工增加**后再 `make buildroot`（与 `BIG_LITTLE_GUIDE` 一致）。

### 7.5 `door_lock` 内嵌 gateway 副本（分叉风险）

- 路径：`src/little/buildroot-ext/package/door_lock/src/gateway/`。
- 与 **`my_face_recognition/src/little`** 的 `main.cpp` **行数不同**、**`face_gateway_protocol.h` 与真源存在 diff**；内容为历史/演示用**另一份拷贝**，**不会**随你改 my_face 自动更新。
- **维护策略**：人脸与 `face_ctrl` 协议以 **`my_face_recognition` 为唯一真源**；勿在 `door_lock` 内改协议后反推 my_face，除非你有意同步两份。

### 7.6 其它 SDK 目录（无直接改 my_face 逻辑，但属构建链）

- **大核**：`build_app.sh` → `MPP` / `nncase` / `opencv` / `toolchain`（见上文第 1 节）。
- **IPC 实现**：除 `ipcmsg` 用户态库外，内核/驱动侧在官方 BSP 中，一般**不必**为人脸工程单独改，除非 `/dev/ipcm_user` 不存在或版本不兼容。

---

**与联调/步骤合并阅读**：`archive/BIG_LITTLE_GUIDE.md`。
