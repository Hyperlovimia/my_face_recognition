# 板端触摸注册 UI 最终实现记录（Offscreen Shared Buffer + RT OSD）

日期：2026-05-15

## 1. 背景与结论

`my_face_recognition` 的板端触摸注册 UI，最初按“小核 `face_netd` 直接使用 Linux DRM plane 显示 LVGL”的思路接入。

这条路径在宿主机构建层面是成立的，但在板端联调时出现了一个关键现象：

- Linux 侧 DRM plane 可以枚举
- atomic commit 可以返回成功
- 但屏幕上既看不到 UI，也看不到强制绘制的纯色调试块

这说明问题已经不再是：

- `face_netd` 是否启动
- 触摸节点是否存在
- plane 编号是否选对
- UI 页面是否创建成功

而是更底层的事实：

- **当前项目运行形态下，Linux 小核这条 DRM overlay 输出路径并不进入最终可见显示链路**

因此本轮最终放弃“Linux 直接上屏”，改为：

- **Linux 小核只负责触摸、UI 状态机、姓名输入与注册会话控制**
- **RT 大核 `face_video` 继续作为唯一显示拥有者**
- **Linux 将 UI 渲染到共享 buffer**
- **RT 将共享 buffer 混合进现有 OSD 后再统一上屏**

这也是当前版本的最终实现结论。

## 2. 最终架构

最终链路如下：

```text
触摸屏
  -> Linux face_netd / LVGL
  -> Offscreen ARGB8888 shared buffer
  -> RT face_event 分配/发布共享面信息
  -> RT face_video 每帧读取并混合到 draw_frame
  -> VO OSD(K_VO_OSD3)
  -> 屏幕最终显示
```

职责划分固定为：

- Linux 小核：
  - 触摸输入
  - LVGL 页面
  - UI 状态机
  - 注册预览 / 提交 / 取消命令下发
- RT `face_event`：
  - 共享 UI buffer 生命周期管理
  - 向 `face_video` 和 Linux bridge 发布共享面信息
- RT `face_video`：
  - 读取共享 UI buffer
  - 与现有人脸框 / 预览图 / OSD 一起合成
  - 最终显示到屏幕

## 3. 为什么旧方案被废弃

旧文档 `260513_BOARD_TOUCH_REGISTRATION_UI_CHANGELOG.md` 记录的是第一轮接入方案，其核心假设是：

- `face_netd` 内嵌 UI
- Linux 小核直接通过 `/dev/dri/card0` + DRM plane 把 LVGL overlay 打到屏上

板端实测结果证明这个假设不成立。

现场已经确认过：

- `face_netd` 已启动
- 触摸节点存在
- `face_netd_ui: selected plane=...` 已成功
- 甚至在首帧里强制写入不透明红块后，屏幕仍完全不可见

因此旧方案的关键问题不是“代码没写完”，而是“显示所有权路径假设错误”。

这也是为什么本轮需要用一份新文档记录**最终实现**，并删除旧文档，避免后续再次按错误路径排障。

## 4. 共享 Buffer 契约

本轮新增公共定义文件：

- `src/ui_overlay_shared.h`

共享面固定为 NT35516 当前 OSD 逻辑尺寸：

- 宽：`540`
- 高：`960`
- 像素格式：`ARGB8888`
- stride：`540 * 4 = 2160`
- 双缓冲：`2` 个 slot

共享内存布局为：

```text
[ ui_overlay_shared_header_t ]
[ slot 0: 540 * 960 * 4 ]
[ slot 1: 540 * 960 * 4 ]
```

Header 记录：

- `magic`
- `version`
- `width`
- `height`
- `stride`
- `slot_count`
- `front_index`
- `seq`
- `generation`
- `flags`

更新规则固定为：

1. Linux 总是写入“非 front”的 back slot
2. 完整拷贝本帧像素
3. 发出内存序屏障
4. 自增 `seq`
5. 翻转 `front_index`
6. RT 每帧只读取当前 `front_index` 指向的 slot

本轮**不使用每帧 dirty IPC 通知**，而是让 RT 在显示循环中自然读取当前 front buffer。

## 5. 协议与结构体变更

修改文件：

- `src/ipc_proto.h`

新增桥接消息类型：

- `IPC_BRIDGE_MSG_UI_SHARED_INFO`

新增视频控制操作：

- `IPC_VIDEO_CTRL_OP_UI_ATTACH`

新增结构体：

- `bridge_ui_shared_info_t`
- `ui_overlay_shared_header_t`（在 `ui_overlay_shared.h`）

扩展 `ipc_video_ctrl_t`，用于 `UI_ATTACH`：

- `ui_phys_addr`
- `ui_bytes`
- `ui_width`
- `ui_height`
- `ui_stride`
- `ui_generation`

这意味着：

- `face_netd`
- `face_event.elf`
- `face_video.elf`

必须同源重编并一起部署，不再支持混用旧版协议。

## 6. RT 侧实现

### 6.1 `face_event` 负责共享面生命周期

修改文件：

- `src/face_event_main.cc`

最终做法：

- 由 `face_event` 在启动阶段调用 `kd_mpi_sys_mmz_alloc(...)` 分配**非缓存** MMZ
- 清零 header 与双缓冲
- 生成并写入 `generation`
- 保存 `phys_addr / bytes / geometry`
- 在两类连接建立时主动下发共享面信息：
  - `face_event -> face_video`：发送 `IPC_VIDEO_CTRL_OP_UI_ATTACH`
  - `face_event -> Linux bridge`：发送 `IPC_BRIDGE_MSG_UI_SHARED_INFO`
- 退出时释放该 MMZ

这里特意选择由 `face_event` 持有共享面，是因为它本来就是：

- RT 对 Linux bridge 的唯一服务端
- `face_video` 控制入口的上游
- 最适合做“中枢协调者”

这样可以让 Linux 重连和 `face_video` 重连都自然复用现有流程。

### 6.2 `face_video` 负责最终混合显示

修改文件：

- `src/face_video_main.cc`

新增行为：

- 在 `ctrl_recv_loop()` 中识别 `IPC_VIDEO_CTRL_OP_UI_ATTACH`
- 用 `kd_mpi_sys_mmap(ui_phys_addr, ui_bytes)` 映射共享 UI 面
- 校验 header：
  - `magic`
  - `version`
  - `width/height/stride`
  - `slot_count`
  - `generation`
- 在每帧 `draw_frame` 合成结束后，调用 `blend_ui_overlay(draw_frame)`

混合顺序固定为：

1. 人脸框 / 文案
2. 注册预览图
3. 共享 UI overlay
4. `InsertFrame()`

因此最终 UI 一定位于现有 OSD 之上。

Alpha 规则：

- `src_a == 0`：跳过
- `src_a == 255`：直接覆盖
- 否则按标准 alpha blend 混合

字节语义与当前项目 OSD 保持一致：

- `A, R, G, B`

## 7. Linux 侧实现

### 7.1 `face_netd` 接收共享面信息

修改文件：

- `linux_bridge/main.cpp`

新增：

- 处理 `IPC_BRIDGE_MSG_UI_SHARED_INFO`
- 收到后调用 `ui_on_shared_info(...)`
- 输出共享面日志：
  - `phys`
  - `bytes`
  - `generation`
  - `geometry`

这让 Linux 不再自行猜测显示设备，而是等待 RT 主动告知“应该把 UI 写到哪里”。

### 7.2 `ui_runtime` 改为等待共享面后启动

修改文件：

- `linux_bridge/ui/ui_runtime.h`
- `linux_bridge/ui/ui_runtime.cpp`

新增状态：

- `have_shared_info`
- `shared_info`

新行为：

- `face_netd` 启动后，UI 线程先进入等待态
- 如果还没收到共享面信息，只打印：
  - `face_netd_ui: waiting for RT shared overlay info`
- 收到共享面信息后再真正初始化 LVGL backend
- 初始化成功后加载主页

因此 Linux 先启动、RT 后启动的场景也能自然工作。

### 7.3 `k230_port.cpp` 从 DRM 改为 Offscreen Backend

修改文件：

- `linux_bridge/ui/k230_port.h`
- `linux_bridge/ui/k230_port.cpp`

这是本轮最核心的 Linux 改动。

旧版：

- 打开 `/dev/dri/card0`
- 枚举 plane
- atomic commit
- 依赖 Linux DRM overlay 最终可见

新版：

- 不再接触 DRM
- 保留 LVGL
- 保留 `/dev/input/event*` 触摸输入
- 打开 `/dev/mem`
- 按 RT 下发的 `phys_addr` 映射共享面
- 在 `disp_flush()` 中把 LVGL 输出转换为共享 slot 像素
- 每帧写入 back slot 后翻 front buffer

触摸映射也同步改为：

- 物理屏：`1080 x 1920`
- 逻辑 UI：`540 x 960`
- 保留：
  - `flip_x = true`
  - `flip_y = true`

不再沿用旧 DRM profile 的 `offset_x/offset_y` 概念。

## 8. 视觉与交互保持项

本轮没有重写 UI 页面本身，而是保留原有 LVGL 业务结构：

- 主页面：
  - `Register`
  - 状态提示
- 编辑页面：
  - 姓名输入框
  - 键盘
  - `Cancel`

会话逻辑也保持不变：

- `REGISTER_PREVIEW`
- `REGISTER_COMMIT`
- `REGISTER_CANCEL`
- 板端会话进行时远端注册类命令返回 `busy`

因此本轮是**显示后端替换**，不是**UI 业务逻辑重写**。

## 9. 部署要求

本轮至少需要替换以下产物：

- `k230_bin/face_bridge/face_netd`
- `k230_bin/face_event.elf`
- `k230_bin/face_video.elf`

`face_ai.elf` 协议侧未新增 UI 逻辑，但本轮仍建议整套同源部署，避免运行时出现版本混搭。

## 10. 上板关注日志

成功链路下，板端应关注这些日志：

### RT `face_event`

- `face_event: ui overlay shared buffer ready ...`

### RT `face_video`

- `face_video: UI overlay attached ...`

### Linux `face_netd`

- `face_netd: received UI shared overlay info ...`
- `face_netd_ui: shared surface mapped ...`
- `face_netd_ui: offscreen port init complete ...`
- `face_netd_ui: shared overlay ready generation=...`

如果 Linux 先起，应先看到：

- `face_netd_ui: waiting for RT shared overlay info`

随后在 RT 就绪并桥接连上后再进入正常 UI 绘制。

## 11. 本轮结论

本轮真正落地的不是“修复 Linux DRM plane”，而是：

- **确认 Linux DRM 直显路径不适用于当前项目运行形态**
- **把板端触摸注册 UI 改为 Linux offscreen 渲染 + RT OSD 最终显示**
- **保住现有 Linux UI 业务逻辑**
- **复用现有 RT OSD 显示主链路**

因此当前板端 UI 的最终实现路线已经从：

- Linux 直接显示

切换为：

- Linux 负责交互
- RT 负责显示

这是当前工作区内这项功能的正式实现版本。
