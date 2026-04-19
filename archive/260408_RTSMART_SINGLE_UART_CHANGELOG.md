# RT-Smart 单串口适配功能变更文档

## 1. 文档说明

本文档用于说明 `my_face_recognition` 三进程项目在 RT-Smart 单串口环境下的适配变更，重点描述本次版本相对原三进程实现的功能变化、运行方式变化和删除项说明。

本文档重点覆盖以下内容：

- 变更背景
- 功能变更概览
- 详细变更项
- 当前 RT-Smart 环境下的正确启动方式
- 已删除内容及原因

本文档属于“变更文档”，用于描述当前版本的功能状态，而不是开发过程记录。

## 2. 背景问题

在最初的三进程实现中，项目虽然已经完成了：

- `face_event` 负责交互与事件接收
- `face_ai` 负责推理与数据库操作
- `face_video` 负责视频采集、显示和 OSD

但在 RT-Smart 实际上板时，暴露出两个关键问题：

1. 板端只有一个串口终端，无法像 Linux 一样方便地开多个交互终端。
2. `face_ai` 只会在启动时尝试连接一次 `face_evt`，如果 `face_event` 最后才以前台方式启动，则事件链路无法恢复。

另外，`q` 退出时原先只会停止 `face_video`，`face_ai` 仍会留在后台，导致整组进程不能完整退出。

## 3. 功能变更概览

本次变更的核心目标，是让三进程人脸识别项目能够在 RT-Smart 单串口环境下稳定运行，并补齐事件链路与退出链路。

变更完成后，系统新增或明确具备以下能力：

- 支持单串口场景下以 `face_event` 作为最终前台交互进程
- 支持 `face_ai` 在 `face_event` 晚启动时自动恢复事件链路
- 支持识别成功事件与陌生人事件统一上报
- 支持事件输出冷却，降低串口刷屏
- 支持 `q` 后三进程整组退出
- 移除不适用于 RT-Smart `msh` 的 shell 启动/守护脚本

## 4. 详细变更说明

### 4.1 `face_ai` 支持事件通道自动重连

修改文件：

- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc)

本次新增了事件通道状态管理逻辑：

- 新增全局事件通道句柄 `g_evt_ch`
- 新增重连节流时间 `g_evt_last_try_ms`
- 新增 `ensure_evt_channel()`，在事件通道未建立时自动尝试重连
- 新增 `reset_evt_channel()`，当发送失败时关闭失效通道，等待后续重连

效果：

- `face_ai` 即使先于 `face_event` 启动，也不会永久丢失事件链路
- 当 `face_event` 后续启动成功后，`face_ai` 会在下一次事件发送时自动连接 `face_evt`
- 如果 `face_event` 退出又重启，`face_ai` 也能重新连接

相关代码位置：

- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc#L25)
- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc#L57)
- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc#L66)

### 4.2 `face_ai` 事件发送逻辑增强

修改文件：

- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc)

原先 `face_ai` 只会对陌生人发事件。现在调整为：

- 已识别用户也会上报事件
- 陌生人继续上报告警事件
- 统一通过 `notify_face_event()` 发送

效果：

- `face_event` 中已有的 `[EVT]` / `[OK]` 输出逻辑真正开始生效
- 不再只有陌生人告警，识别成功同样会打到前台和日志文件中

相关代码位置：

- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc#L144)
- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc#L427)

### 4.3 新增事件冷却，避免串口刷屏

修改文件：

- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc)

为了避免连续识别同一张脸时每帧都输出一条事件，本次新增了事件冷却缓存：

- 按 `id` / `name` / `is_stranger` 做事件去重
- 冷却时间为 3 秒
- 仅在冷却时间外再次发事件

效果：

- 串口输出更可读
- 日志文件不会高速刷满
- 单串口交互时不会被事件输出完全淹没

相关代码位置：

- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc#L28)
- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc#L85)
- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc#L128)

### 4.4 新增 AI 关闭命令，补齐 `q` 退出链路

修改文件：

- [ipc_proto.h](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/ipc_proto.h)
- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc)
- [face_video_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_video_main.cc)

本次新增了：

- `IPC_CMD_SHUTDOWN`

调整了退出流程：

1. 用户在 `face_event` 中输入 `q`
2. `face_event` 向 `face_video` 发送 `IPC_VIDEO_CTRL_OP_QUIT`
3. `face_video` 退出视频循环前，向 `face_ai` 发送 `IPC_CMD_SHUTDOWN`
4. `face_ai` 收到后打印 `face_ai: shutdown requested` 并退出

效果：

- `q` 后不再只是停掉视频进程
- 三进程会按完整链路退出
- `face_ai.elf` 不再残留在后台

相关代码位置：

- [ipc_proto.h](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/ipc_proto.h#L16)
- [face_video_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_video_main.cc#L23)
- [face_video_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_video_main.cc#L102)
- [face_video_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_video_main.cc#L435)
- [face_ai_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_ai_main.cc#L302)

### 4.5 `face_event` 退出逻辑修复

修改文件：

- [face_event_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_event_main.cc)

问题：

- `face_event` 的事件接收线程阻塞在 `rt_channel_recv(face_evt)` 上
- 退出时主线程会 `join()` 它
- 如果没有唤醒动作，可能导致 `face_event` 无法顺利回到 shell

本次修改：

- 在 `shutdown_event_process()` 中，先向 `face_evt` 自发一个唤醒消息，再关闭通道

效果：

- `face_event` 在 `q` 后更容易干净退出
- 单串口场景下，退出后能回到 `msh`

相关代码位置：

- [face_event_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_event_main.cc#L136)

### 4.6 单串口提示文案更新

修改文件：

- [face_event_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_event_main.cc)

新增提示内容：

- 单串口场景建议：先后台启动 `face_ai` / `face_video`，再前台启动 `face_event`

这样用户在板端直接看到提示时，不会再按多终端思路误操作。

相关代码位置：

- [face_event_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_event_main.cc#L19)
- [face_event_main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognitionsrc/face_event_main.cc#L173)

## 5. 删除项说明

本次会话最终删除了以下文件：

- `utils/run_face3.sh`
- `utils/watchdog_face3.sh`
- `utils/run.sh`

原因不是它们逻辑本身错误，而是：

- 它们基于 `/bin/sh` 的 POSIX shell 语法编写
- RT-Smart 板端实际使用的是 `msh`
- `msh` 不支持这类脚本所依赖的关键语法，包括：
  - 变量赋值展开
  - `$(...)`
  - shell 函数
  - `if/for/while`
  - `trap`
  - `export` 设置运行时环境变量
  - `ps`、`grep`、`killall` 等通用 Linux 进程管理命令

因此在 RT-Smart 系统中：

- `run_face3.sh` 无法直接执行
- `watchdog_face3.sh` 无法直接执行
- `run.sh` 也不作为板端入口保留

既然项目最终目标是面向 RT-Smart 实机运行，继续保留这些脚本反而会误导使用者，因此本轮已删除。

## 6. 当前运行方式

### 6.1 适用场景

当前推荐启动方式适用于：

- RT-Smart 系统
- 仅有一个串口终端
- 三进程模式

### 6.2 启动前准备

确认板端已有以下文件：

- `/sharefs/face_ai.elf`
- `/sharefs/face_video.elf`
- `/sharefs/face_event.elf`
- `/sharefs/face_detection_320.kmodel`
- `/sharefs/face_recognition.kmodel`
- `/sharefs/face_antispoof.kmodel`，仅启用活体检测时需要

`/sharefs/face_db` 不存在时，`face_ai.elf` 会在启动时尝试自动创建。

### 6.3 正确启动顺序

请按以下顺序启动：

启用活体检测时，`face_ai.elf` 使用 9 参数，最后一项为活体模型路径：

```sh
/sharefs/face_ai.elf /sharefs/face_detection_320.kmodel 0.5 0.2 /sharefs/face_recognition.kmodel 70 /sharefs/face_db 0 /sharefs/face_antispoof.kmodel &
/sharefs/face_video.elf 0 &
/sharefs/face_event.elf /tmp/attendance.log
```

暂不启用活体检测时，`face_ai.elf` 使用 8 参数：

```sh
/sharefs/face_ai.elf /sharefs/face_detection_320.kmodel 0.5 0.2 /sharefs/face_recognition.kmodel 70 /sharefs/face_db 0 &
/sharefs/face_video.elf 0 &
/sharefs/face_event.elf /tmp/attendance.log
```

说明：

- `face_ai` 后台启动
- `face_video` 后台启动
- `face_event` 最后前台启动，负责读取串口输入
- 不依赖 `export`、脚本变量或脚本自动判断模型是否存在

这样可以同时满足：

- 单串口交互
- 三进程完整协作
- `face_ai` 事件通道晚绑定自动恢复

### 6.4 运行时交互

在 `face_event` 前台可输入：

- `h` 或 `help`：查看帮助
- `i`：抓拍一帧准备注册
- 随后输入用户名：提交注册
- `n`：查询注册人数
- `d`：清空数据库
- `q`：退出三进程

### 6.5 正常运行时的现象

启动完成后，预期会看到：

- `face_event` 打印帮助说明
- `face_video` 正常初始化视频采集与显示
- `face_ai` 正常初始化数据库
- 在第一次识别事件发生后，`face_ai` 打印类似：

```text
face_ai: connected to face_evt (ch=...)
```

说明事件链路已自动恢复。

### 6.6 正常退出时的现象

输入 `q` 后，预期会看到类似：

- `face_video: AI shutdown acknowledged`
- `face_ai: shutdown requested`

并最终回到 `msh`。

## 7. 当前验证结果

截至本次会话结束，已确认：

- 三进程功能可正常运行
- 单串口场景可正常交互
- `face_event` 可以保留在前台
- `face_ai` 支持 `face_evt` 自动重连
- 识别成功和陌生人事件都能正常上报
- `q` 退出后整组进程可以正确收尾

另外，本地交叉编译已通过，新的 ELF 已生成。

## 8. 相关文档

本次会话相关文档包括：

- [THREE_PROCESS_CHANGELOG.md](./260407_THREE_PROCESS_CHANGELOG.md)
- [RTSMART_SINGLE_UART_CHANGELOG.md](./260408_RTSMART_SINGLE_UART_CHANGELOG.md)

## 9. 结论

本次变更的核心成果，是把原本更偏向“多终端/类 Linux 使用方式”的三进程实现，调整为真正适合 RT-Smart 单串口环境的版本。

最终项目具备以下特点：

- 三进程模式可在 RT-Smart 单串口环境下正常使用
- `face_event` 可作为最终前台交互入口
- 事件链路支持晚启动自动恢复
- 退出链路完整，不再残留 `face_ai`
- 不再保留不适用于 RT-Smart 的 shell 启动/守护脚本
