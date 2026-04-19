# 三进程优化功能变更文档

> 260415 更正：本文早期记录中的 `utils/run_face3.sh`、`utils/watchdog_face3.sh` 以及 `face_event -> face_ai -> face_video` 启动建议仅适用于类 Linux / POSIX shell 假设，不适用于 RT-Smart 板端 `msh`。当前板端正确方式是直接在串口按顺序执行 `face_ai.elf &`、`face_video.elf &`、`face_event.elf`，详见本文第 5 节和 [260408 RT-Smart 单串口适配文档](./260408_RTSMART_SINGLE_UART_CHANGELOG.md)。

## 1. 变更摘要

本次优化的核心目标，是将原本的单进程人脸识别程序拆分为“视频采集显示进程 + AI 推理进程 + 事件/交互进程”三个独立进程，降低采集显示线程被 AI 推理阻塞的风险，并提升系统可维护性、可观测性和异常恢复能力。

优化完成后，项目同时保留了两种运行模式：

- 原单进程模式仍保留，产物为 `face_recognition.elf`
- 新增三进程模式，产物为 `face_video.elf`、`face_ai.elf`、`face_event.elf`

## 2. 架构变化

### 2.1 优化前

优化前使用单进程架构，主要逻辑集中在 `src/main.cc`：

- 同一个进程同时负责摄像头采集、AI 推理、OSD 绘制、数据库操作和终端命令交互
- 视频帧获取后立即在当前流程中执行人脸检测和识别
- 注册、清库、计数等业务逻辑与视频主循环耦合较深
- 一旦 AI 推理耗时波动，容易直接影响采集和显示流畅性

### 2.2 优化后

优化后拆分为三进程协作：

1. `face_video.elf`
   负责视频采集、显示、截图、OSD 绘制，以及与 `face_ai` 的请求交互
2. `face_ai.elf`
   负责检测、识别、注册提交、数据库计数与清空等 AI/数据库能力
3. `face_event.elf`
   负责终端命令输入、事件接收、日志记录，并将业务控制命令转发给 `face_video`

新的逻辑链路如下：

```text
face_event(stdin/日志)
    | 业务控制命令
    v
face_video(采集/显示/OSD)
    | 推理请求
    v
face_ai(检测/识别/数据库)
    | 识别事件/陌生人告警
    v
face_event(日志输出/告警输出)
```

### 2.3 进程间通信方式

三进程版新增了一套 RT-Smart 用户态 IPC 机制：

- 使用 `rt_channel` 进行消息通道通信
- 使用 `lwp_shm` 传输请求体、返回体和事件体
- 使用 `ipc_proto.h` 定义固定的 POD 协议结构
- 使用 `ipc_lwp_syscall.c` 对 `rt_channel_*` 和 `lwp_*` 系统调用做了用户态封装，避免静态链接环境中符号缺失

新增的主要 IPC 通道包括：

- `face_ai_req`：`face_video -> face_ai` 的请求/应答通道
- `face_evt`：`face_ai -> face_event` 的事件通知通道
- `face_video_ctrl`：`face_event -> face_video` 的控制通道

## 3. 功能变更明细

### 3.1 三进程运行能力新增

新增文件：

- `src/face_video_main.cc`
- `src/face_ai_main.cc`
- `src/face_event_main.cc`

对应新增能力：

- 将视频采集显示从 AI 推理中独立出来
- 将终端交互从视频主循环中独立出来
- 为后续按进程维度做性能监控、故障定位和进程拉起提供基础

### 3.2 视频进程改为“采集显示优先”

`face_video.elf` 的职责从“单纯显示”扩展为“三进程模式下的主控视频服务”：

- 持续从 `PipeLine` 获取视频帧
- 在识别状态下，将最新帧交给独立推理线程处理
- 自身不阻塞等待每一帧推理结果，而是优先保证采集和 OSD 刷新
- 接收 `face_event` 发来的状态控制命令，如计数、注册、清库、退出
- 从 `face_ai` 的回复中读取识别结果并绘制 OSD

这一变化的关键价值是：视频采集主循环不再与每帧推理强耦合，显示链路更稳定。

### 3.3 新增异步推理机制

第二次提交在三进程骨架上进一步增强了 `face_video` 的异步处理能力：

- 新增独立的 `ai_infer_worker_thread`
- 采集线程只保留“最新一帧”作为待推理输入
- 通过 `g_infer_capture_seq` / `g_infer_last_displayed_seq` 判断结果是否过期
- 推理期间如果又采集到了更新帧，则旧推理结果会被丢弃，避免显示陈旧结果

这一机制属于典型的“最新帧优先”策略，适合实时视频业务，目标是降低推理滞后对显示体验的影响。

### 3.4 共享内存请求槽复用

第二次提交新增了固定推理请求共享内存槽：

- `face_video` 在启动时预分配一块固定大小的共享内存
- 对于高频 `IPC_CMD_INFER` 请求，优先复用同一个 `shmid`
- 仅在固定槽不可用时退回到“每次请求新分配共享内存”

对应改动主要体现在：

- `src/ipc_shm.cc`
- `src/ipc_shm.h`
- `src/face_video_main.cc`

其意义是减少每帧推理时的共享内存分配与回收开销，进一步降低 IPC 成本。

### 3.5 AI 服务化

`face_ai.elf` 将原本位于单进程中的检测、识别和数据库操作封装成了独立服务：

- 支持 `IPC_CMD_INFER`
- 支持 `IPC_CMD_DB_COUNT`
- 支持 `IPC_CMD_DB_RESET`
- 支持 `IPC_CMD_REGISTER_COMMIT`

返回结果使用 `ipc_ai_reply_t` 统一封装，包括：

- 状态码
- 注册人数
- 检测到的人脸数量
- 每张人脸的框、关键点、检测分数、识别结果

这样做后，AI 能力对上层调用方表现为一个稳定的“请求-应答服务”，便于扩展和调试。

### 3.6 新增事件进程与考勤日志能力

`face_event.elf` 是本次优化的重要新增点，负责：

- 读取标准输入命令
- 向 `face_video` 发送业务控制命令
- 接收 `face_ai` 推送的识别事件
- 将识别成功和陌生人事件写入日志文件

当前事件输出包括：

- `[OK] id=... name=... score=...`
- `[STRANGER] score=...`

这意味着三进程模式不仅实现了结构拆分，还新增了“事件服务/考勤日志输出”的业务能力。

### 3.7 陌生人告警链路新增

在 `face_ai_main.cc` 中，如果识别结果为陌生人（`id == -1`），会通过事件通道将结果发送给 `face_event`：

- 控制台输出 `[ALERT] stranger`
- 日志文件记录陌生人事件

相比原版本，这一链路把“识别结果展示”和“事件处理”分离开来，更适合接入门禁、考勤或审计场景。

### 3.8 保留原有业务功能，但实现位置发生变化

原有以下功能在三进程模式下仍然保留：

- `h/help` 查看帮助
- `i` 进入注册流程
- 输入姓名完成注册提交
- `d` 清空数据库
- `n` 查询注册人数
- `q` 退出程序

但它们的执行位置发生了变化：

- 命令输入入口从原来的单进程主程序迁移到了 `face_event`
- 业务状态切换由 `face_event -> face_video` 控制
- 实际数据库操作由 `face_ai` 完成

### 3.9 AI 接口容错增强

为适配多进程服务化场景，AI 基础类和检测识别类新增了非崩溃式错误返回机制：

- `AIBase` 新增 `try_run()` 与 `try_get_output()`
- `FaceDetection::pre_process()` / `inference()` 改为返回 `bool`
- `FaceRecognition::pre_process()` / `inference()` 改为返回 `bool`

这带来的变化是：

- AI 失败时可以通过 `ipc_ai_reply_t.status` 报告错误
- 避免直接 `expect()` 导致服务进程异常终止
- 为上层保留重试、降级或记录日志的空间

### 3.10 IPC 协议增强

`ipc_proto.h` 在第二次提交中新增状态码定义：

- `IPC_STATUS_OK`
- `IPC_STATUS_ERR_MAGIC`
- `IPC_STATUS_ERR_PARAM`
- `IPC_STATUS_ERR_SHM`
- `IPC_STATUS_ERR_INFER`
- `IPC_STATUS_ERR_BAD_CMD`

这使得请求失败时不再只能依赖“空回复/异常退出”来判断问题，而可以显式区分参数错误、共享内存问题、推理失败、非法命令等场景。

### 3.11 性能指标输出新增

第二次提交为 `face_video` 和 `face_ai` 增加了可选指标打印能力，可通过 `FACE_METRICS=1` 或 debug 模式开启。

典型指标包括：

- `face_video` 的采集帧数、推理成功数、推理失败数、过期结果数、应用结果数
- `face_ai` 的 IPC 接收次数、推理错误回复次数

这有助于现场分析：

- 视频线程是否持续采帧
- AI 请求是否存在失败
- 最新帧策略下是否出现较多结果过期

## 4. 构建与部署方式变化

### 4.1 CMake 构建目标增加

`src/CMakeLists.txt` 从原来只构建 `face_recognition.elf`，调整为同时构建：

- `face_recognition.elf`，单进程原版，保留
- `face_video.elf`
- `face_ai.elf`
- `face_event.elf`

这说明三进程优化并不是替换原程序，而是在保留旧方案的基础上新增一套可并行维护的实现。

### 4.2 构建脚本输出调整

`build_app.sh` 的行为发生变化：

- 原来只收集 `face_recognition.elf` 并拷贝 `utils/*`
- 现在改为只收集 ELF，并支持收集四个可执行文件
- `utils/` 脚本不再自动打包到 `k230_bin`

这意味着部署时需要更明确地区分：

- 编译产物目录 `k230_bin`
- 仓库中的辅助脚本目录 `utils`

### 4.3 关于三进程启动脚本的更正

早期曾尝试新增 `utils/run_face3.sh` 与 `utils/watchdog_face3.sh` 来自动启动和守护三进程，但该方案依赖 `/bin/sh` / POSIX shell 能力，例如变量展开、命令替换、shell 函数、循环、`trap`、PID 文件和通用进程管理命令。

RT-Smart 板端实际使用 `msh`，不能按常规 Linux shell 使用这些脚本能力。因此这些脚本不应作为板端启动方式保留，后续已删除，避免误导部署人员。

### 4.4 当前板端启动原则

三进程启动不再通过脚本自动完成，而是在单串口中手动输入三条命令：

- 先后台启动 `face_ai.elf`
- 再后台启动 `face_video.elf`
- 最后以前台方式启动 `face_event.elf`

这样 `face_event.elf` 能占用唯一串口作为交互入口，`face_ai.elf` 的事件通道也能在 `face_event.elf` 晚启动后自动恢复。

## 5. 使用方式变化

### 5.1 单进程模式

旧模式仍可使用：

```bash
./face_recognition.elf face_detection_320.kmodel 0.6 0.2 face_recognition.kmodel 75 face_database 0
```

旧的 `utils/run.sh` 已删除。RT-Smart 板端请直接输入 ELF 命令，不再通过脚本启动。

### 5.2 三进程模式

RT-Smart 单串口三进程模式必须按以下顺序手动启动：

启用活体检测时：

```sh
/sharefs/face_ai.elf /sharefs/face_detection_320.kmodel 0.5 0.2 /sharefs/face_recognition.kmodel 70 /sharefs/face_db 0 /sharefs/face_antispoof.kmodel &
/sharefs/face_video.elf 0 &
/sharefs/face_event.elf /tmp/attendance.log
```

暂不启用活体检测时：

```sh
/sharefs/face_ai.elf /sharefs/face_detection_320.kmodel 0.5 0.2 /sharefs/face_recognition.kmodel 70 /sharefs/face_db 0 &
/sharefs/face_video.elf 0 &
/sharefs/face_event.elf /tmp/attendance.log
```

交互方式也发生了变化：

- 命令输入终端从 `face_recognition.elf` 转移到 `face_event.elf`
- `face_video.elf` 主要负责采集和显示，不再承担标准输入交互

## 6. 对项目的实际收益

本次三进程优化带来的收益主要有：

- 降低视频采集与显示被 AI 推理阻塞的概率
- 提升系统模块边界清晰度，便于定位问题
- 让命令交互、日志记录、AI 推理职责分离
- 为陌生人告警、考勤日志、守护重启等工程化能力打下基础
- 保留单进程版本，降低切换风险

## 7. 兼容性与注意事项

1. 三进程模式下，交互命令必须在 `face_event` 所在终端输入。
2. 三进程模式对启动顺序有要求：`face_ai` 后台、`face_video` 后台、`face_event` 前台。
3. `build_app.sh` 不再自动把 `utils/` 拷贝进 `k230_bin`，板端部署时只需要同步 ELF 和模型文件。
4. 本次提交保留了单进程实现，因此旧流程理论上仍可回退使用。
5. `.idea/` 目录在第二次提交中一并进入版本库，但这部分属于开发环境文件，不构成功能层面的核心变更。
