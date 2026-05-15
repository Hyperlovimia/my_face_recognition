# `face_event` 门状态指示接入记录

日期：2026-05-13

## 1. 背景

本轮原始目标，是在当前 `my_face_recognition` 三进程项目中，为 `face_event.elf` 增加“识别成功后执行开门动作”的业务闭环：

- `face_ai.elf` 识别授权用户
- `face_event.elf` 接收识别事件
- 通过 GPIO 驱动外部执行机构
- 保持一段时间后自动恢复

最初设计按“继电器 + 蜂鸣器”执行机构展开，但实际板子上并没有板载继电器或蜂鸣器。为了方便现场观察“门已打开”的状态，本轮最终改为：

- 不接外部执行器
- 使用板载 `LED1` 映射“门打开”状态
- 识别成功时点亮 LED，超时后自动熄灭
- 同时保留串口日志与考勤 JSONL 中的结构化留痕

板级信息由现场确认：

- `LED1` 连接 `BANK0_GPIO6`
- `LED1` 为低电平点亮

## 2. 目标

本轮完成后的行为定义如下：

- 仅 `recognized` 事件触发“门打开”
- “门打开”用 `LED1(GPIO6)` 点亮表示
- 点亮保持 `3s`
- 到时自动熄灭，表示“门关闭”
- `stranger` 与 `liveness_fail` 仍只打印日志和写考勤，不驱动 LED
- 若 GPIO 初始化失败、写失败或 pinmux 配置失败，则进入 `FAULT`，停止后续动作，但不影响识别、日志和桥接链路

## 3. 代码改动

### 3.1 新增门状态控制模块

新增文件：

- `src/door_control.h`
- `src/door_control.cc`
- `src/door_control_config.h`

新增模块 `DoorControl`，职责是：

1. 打开 `/dev/gpio`
2. 配置输出脚
3. 在识别成功时拉到“有效电平”
4. 启动 `3s` 软件定时保持
5. 超时后自动恢复为“无效电平”
6. 失败时转入 `FAULT`

实现要点：

- 采用 RT-Smart 用户态 `/dev/gpio` + `ioctl` 访问 GPIO
- 不直接把 `rtdevice.h` 或内核驱动头拉入当前应用
- 在本地代码中镜像最小 GPIO UAPI 常量：
  - `KD_GPIO_DM_OUTPUT`
  - `KD_GPIO_WRITE_LOW`
  - `KD_GPIO_WRITE_HIGH`
- 通过工作线程 + `condition_variable` 实现一次性开门窗口
- 开门窗口内重复 `recognized` 不续期，避免 LED 常亮、也避免未来迁回继电器时变成“门常开”
- 在输出 GPIO 初始化前，显式把 `IO6` pinmux 强制切到 GPIO 功能，避免板级默认复用不是 GPIO 时“日志成功但 LED 不亮”

### 3.2 `face_event` 接入门状态控制

修改文件：

- `src/face_event_main.cc`

接入方式：

- 进程启动时调用 `g_door_control.init(log_base)`
- 在 `evt_recv_loop()` 中，事件完成原有日志写入后，调用 `g_door_control.handle_ipc_event(*ev)`
- 在 `shutdown_event_process()` 中先调用 `g_door_control.shutdown()`，保证退出前尽量恢复到“门关闭/LED 熄灭”状态

这样保持了原有三进程边界不变：

- `face_ai` 仍只负责识别并发送 `ipc_evt_t`
- `face_event` 负责业务闭环
- 不修改 `ipc_proto.h` 的线协议

### 3.3 编译项调整

修改文件：

- `src/CMakeLists.txt`

新增：

- 将 `door_control.cc` 编入 `face_event.elf`

未修改：

- `face_ai.elf`
- `face_video.elf`
- `linux_bridge/face_netd`
- `server_pc`

## 4. 默认配置从“执行器”切换为“板载 LED1”

修改文件：

- `src/door_control_config.h`

最终默认值：

```c
#define FACE_DOOR_ENABLE 1
#define FACE_DOOR_RELAY_PIN 6
#define FACE_DOOR_RELAY_ACTIVE_HIGH 0
#define FACE_DOOR_BUZZER_PIN -1
#define FACE_DOOR_HOLD_MS 3000
#define FACE_DOOR_VERIFY_READBACK 0
#define FACE_DOOR_FORCE_IOMUX_GPIO 1
```

说明：

- `FACE_DOOR_RELAY_PIN` 现在语义上等同于“门状态输出 GPIO”，当前映射到 `LED1`
- `FACE_DOOR_RELAY_ACTIVE_HIGH = 0` 表示低电平有效，对应 `LED1` 低电平点亮
- `FACE_DOOR_BUZZER_PIN = -1` 表示第二路输出关闭
- `FACE_DOOR_VERIFY_READBACK = 0` 表示默认关闭“输出脚自读回校验”
- `FACE_DOOR_FORCE_IOMUX_GPIO = 1` 表示初始化时强制将 `IO6` 切换到 GPIO 复用
- 代码同步调整为“第二路输出可选”，当 `pin < 0` 时直接跳过配置和写入

因此当前板端行为变为：

- 识别成功 -> `GPIO6` 输出低电平 -> `LED1` 点亮 -> 表示门打开
- 3 秒后 -> `GPIO6` 输出高电平 -> `LED1` 熄灭 -> 表示门关闭

## 5. Bug 修复补充

在首次切换到板载 `LED1` 方案后，现场出现过一个典型问题：

- 串口日志已经出现 `door unlock` / `door relock`
- 说明识别事件、状态机和 3 秒自动回锁逻辑都已生效
- 但 `LED1` 实际没有点亮

同时还出现过另一类日志：

- `face_event: door control faulted, ignore recognized event name=...`

这说明门控模块已提前进入 `FAULT`，后续识别成功事件虽然还能被 `face_event` 正常打印和桥接，但不会再驱动 LED。

最终定位到有两个板级相关原因：

1. **输出脚自读回校验不适合板载 LED 场景**

- 最初版本在 GPIO 输出后使用 `GPIO_READ_VALUE` 立即读回做自校验
- 但 K230 SDK 自带 GPIO 用例更常见的是“一个输出脚接另一个输入脚再读”，并不是“同一个输出脚自读自身”
- 对板载 `LED1(GPIO6)` 这种场景，输出后立即自读并不稳定，可能被误判为故障并转入 `FAULT`

因此后续修复中将：

- `FACE_DOOR_VERIFY_READBACK` 默认改为 `0`
- 不再把输出脚自读回作为默认故障判定依据

2. **`IO6` 在 `k230_canmv_dongshanpi` 板级默认 pinmux 中不是 GPIO**

- 本地 DTS 检查发现，`k230_canmv_dongshanpi.dts` 中 `IO6` 默认 `SEL=1`
- 同文件中，真正配置成 GPIO 的脚通常是 `SEL=0`
- 这解释了为什么“软件日志成功，但 LED 没亮”：GPIO 控制已执行，但 pad 本身并未复用到 GPIO 功能

因此后续修复中在 `DoorControl::init()` 前半段增加：

- 使用 `/dev/mem + mmap` 访问 `IOMUX`
- 将 `IO6` 的 `SEL` 显式切换为 GPIO
- 同时打开 `OE/IE`
- 启动日志增加 `iomux pin 6 forced to gpio old=... new=...`

修复后现场现象变为：

- `recognized`
- `door unlock ...`
- `LED1` 点亮约 3 秒
- `door relock reason=timeout`
- `LED1` 熄灭

说明该问题已经从“软件逻辑故障”收敛并修复为“板级 pinmux + 不合适的默认校验策略”问题。

## 6. 文档更新

修改文件：

- `README.md`

补充内容包括：

- 当前默认不再使用继电器/蜂鸣器，而是映射到板载 `LED1`
- `LED1` 使用 `BANK0_GPIO6`
- 低电平点亮
- 默认保持 `3s`
- 默认关闭写后 `GPIO_READ_VALUE` 自校验
- 默认强制 `IO6` pinmux 切换到 GPIO
- 若后续接入其它执行器，可修改 `src/door_control_config.h` 后重新编译

## 7. 日志与留痕

本轮没有改动 `attendance_log` JSON 结构本身，但门状态控制会额外写入 `meta` 事件，便于后续回溯：

- `door_control_ready`
- `door_unlock ...`
- `door_relock ...`
- `door_fault ...`
- `door_control_disabled`
- `iomux pin 6 forced to gpio old=... new=...`（串口启动日志）

这意味着即使现场只有 LED 指示，也可以通过日志确认：

- 什么时候识别触发了“开门”
- 什么时候自动“关门”
- 是否发生了 GPIO / pinmux 故障

## 8. 构建验证

本轮已在宿主机完成交叉编译验证。

执行命令：

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition
./build_app.sh
```

验证结果：

- `face_event.elf` 成功编译并链接
- `face_ai.elf`、`face_video.elf`、`face_recognition.elf` 未受回归影响
- 新产物已同步到：
  - `build/bin/face_event.elf`
  - `k230_bin/face_event.elf`

## 9. 当前结论

本轮已经把“识别成功后执行开门动作”的闭环，在无外部执行器的板卡条件下落成了可观察版本：

- 业务语义上仍是“开门/关门”
- 硬件表现上暂由板载 `LED1` 代替执行器显示状态
- 软件层保留了完整的：
  - 授权触发
  - 超时恢复
  - 异常容错
  - 日志留痕
- 同时额外覆盖了板载 LED 场景下的两个易踩坑：
  - 输出脚自读回误判 `FAULT`
  - pad 默认 pinmux 不是 GPIO 导致“日志成功但灯不亮”

后续如果板子新增真实继电器、蜂鸣器或其它门锁执行器，只需要调整 `src/door_control_config.h` 中的 GPIO 编号与极性，并重新编译，即可复用本轮已经完成的状态机与容错框架，无需再改动 `face_event` 主流程。
