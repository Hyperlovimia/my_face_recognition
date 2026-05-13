# 板端触摸注册 UI 接入记录

日期：2026-05-13

## 1. 背景

当前 `my_face_recognition` 项目的人脸添加入口主要有两类：

- 串口输入 `i` / `i <姓名>`
- Web 端按钮触发远程注册

这两种方式都能工作，但现场使用时仍有明显短板：

- 板子本地屏幕只能看到视频与识别 OSD，不能直接在屏幕上完成注册
- 串口两步注册依赖人为切换到终端输入姓名
- Web 端虽然可用，但并不适合“站在设备前面直接录脸”的场景

结合 K230 官方门锁 POC 以及本项目当前三进程架构，本轮目标是补齐一条“板端本地触摸注册”链路：

- 板子屏幕出现注册按钮
- 点击后抓拍当前人脸
- 屏幕弹出姓名输入键盘
- 本地输入英文/拼音姓名并提交
- 与现有串口 / Web 注册并存

## 2. 目标

本轮定义的落地目标如下：

1. 不新增新的公网协议，不改 MQTT / Web JSON 结构
2. 不新建第二条 Linux -> RT-Smart IPCMSG 业务链路
3. 直接把最小 UI 集成进现有 `face_netd`
4. UI 只覆盖“板端添加人脸”能力，不扩展删除 / 导入 / OTA
5. 板端正在注册时，远端注册类命令返回 `busy`
6. 修复两步注册的“预览帧与最终提交帧不一致”问题

## 3. 总体方案

本轮实际采用的方案是：

```text
触摸屏
  -> face_netd 内嵌 UI
  -> 复用 face_netd 现有命令队列
  -> IPCMSG face_bridge
  -> face_event.elf
  -> face_video.elf / face_ai.elf
```

关键取舍如下：

- **不单独起新的 UI 进程**
  - 避免再维护一条新的 IPCMSG 客户端连接
  - 直接复用 `face_netd` 现有 bridge 命令队列与结果回传
- **不改 RT / Linux 线协议**
  - 继续使用已有的：
    - `REGISTER_PREVIEW`
    - `REGISTER_COMMIT`
    - `REGISTER_CANCEL`
- **只做最小双页 UI**
  - 主页：`Register` 按钮 + 状态提示
  - 编辑页：姓名输入框 + `lv_keyboard` + `Cancel`

## 4. 代码改动

### 4.1 大核：修正两步注册使用冻结预览帧

修改文件：

- `src/face_video_main.cc`

原来的两步注册流程是：

1. `REGISTER_PREVIEW` 抓拍并显示预览小窗
2. `REGISTER_COMMIT` 到来时，重新使用提交瞬间的 `dump_img`

这会导致一个问题：

- 如果用户点了预览后，停顿几秒再输入姓名
- 或后续换成屏幕键盘，姓名输入耗时更长
- 最终注册的人脸图可能已经不是预览时那一帧

本轮修复方式：

- 新增 `register_commit_src`
- `state == 2` 时：
  - 抓拍当前帧
  - 除了写 `register_preview_src` 用于 OSD 预览
  - 同时把当前 BGR 图冻结到 `register_commit_src`
- `state == 3` 时：
  - 若 `register_commit_src` 有效，则优先使用它构建 CHW 输入
  - 不再默认取提交时刻的 live 帧
- `state == 6` / `db_reset` / 提交完成时：
  - 清理 `register_commit_src`
  - 清理 `register_preview_src`

同时额外调整了预览保留逻辑：

- 在两步注册会话未结束前，预览图不再只保留 `2s`
- 进入注册会话后可持续显示，直到提交 / 取消 / 被其它状态打断

这一步解决的是**板端 UI 能否真正可靠使用**的根问题。

### 4.2 小核：在 `face_netd` 中接入板端 UI 会话

修改文件：

- `linux_bridge/main.cpp`

本轮在 `face_netd` 里新增了 UI 接入点，而不是重写 MQTT / IPC 业务：

#### 4.2.1 新增配置项

扩展 `Config`：

- `ui_enabled`
- `ui_touch_device`
- `ui_preview_timeout_ms`
- `ui_overlay_profile`

并在 `face_netd.ini` 增加默认配置：

```ini
ui_enabled = 1
ui_touch_device = /dev/input/event0
ui_preview_timeout_ms = 30000
ui_overlay_profile = dongshanpi_nt35516
```

#### 4.2.2 命令来源区分

新增：

- `enum class PendingCmdSource`
- `PendingCmd.source`

用于区分：

- `remote`：来自 MQTT / Web
- `ui`：来自板端触摸 UI

这样做的原因是：

- UI 发出的命令结果不应该再二次发布回 MQTT reply
- UI 失败时应直接回灌给本地 UI，而不是走网页提示

#### 4.2.3 并发策略：板端会话优先

新增：

- `is_registration_or_reset_cmd(...)`

并在 `parse_and_enqueue_command(...)` 中加入会话门控：

- 当 `ui_is_session_active()` 为真时
- 远端以下命令直接返回失败：
  - `db_reset`
  - `register_current`
  - `register_preview`
  - `register_commit`
  - `register_cancel`

返回原因固定为：

- `registration session busy`

保留不受影响的命令：

- `db_count`
- `db_face_list`
- `db_face_image`
- `attendance_log_fetch`

这样可以保证：

- 板端录脸时，网页不会把会话抢走
- 只读查询仍能正常工作

#### 4.2.4 结果 / 事件回灌到 UI

在 `handle_bridge_result(...)` 中：

- 先调用 `ui_on_bridge_result(result)`
- 如果 `request_id` 不是 `ui_...`，再继续走原来的 MQTT reply 发布

在 `handle_bridge_event(...)` 中：

- 补充调用 `ui_on_bridge_event(ev)`

这样实现后：

- 板端 UI 可以根据 RT 回包切换状态
- MQTT / Web 行为保持原有兼容

### 4.3 新增小核 UI 模块

新增目录：

- `linux_bridge/ui/`

新增文件：

- `linux_bridge/ui/ui_runtime.h`
- `linux_bridge/ui/ui_runtime.cpp`
- `linux_bridge/ui/k230_port.h`
- `linux_bridge/ui/k230_port.cpp`
- `linux_bridge/ui/lv_conf.h`

#### 4.3.1 `ui_runtime.*`

职责：

- 管理 UI 业务状态机
- 构建 LVGL 页面
- 复用 `face_netd` 命令下发接口
- 消费 RT 返回的 `bridge_cmd_result_t`
- 消费识别事件 `bridge_event_t`

当前定义的内部状态为：

- `idle`
- `preview_requested`
- `editing_name`
- `commit_pending`

固定业务流程：

1. 主界面点击 `Register`
2. 生成 `ui_<ts>_<seq>` 请求号
3. 下发 `REGISTER_PREVIEW`
4. 成功后进入键盘页
5. 输入姓名后下发 `REGISTER_COMMIT`
6. 取消或超时则下发 `REGISTER_CANCEL`

当前实现还额外处理了两个边界：

- **取消后清理挂起请求**
  - 避免晚到的 preview 回包把 UI 又拉回输入页
- **超时自动取消**
  - 默认 `30s`
  - 超时后本地状态回到 `idle`

#### 4.3.2 `k230_port.*`

职责：

- 初始化 DRM UI plane
- 初始化触摸输入
- 提供 `LVGL display / indev` 端口
- 做最小的坐标映射

当前板型只支持：

- `dongshanpi_nt35516`

映射参数由 `overlay_profile` 固定选择，不做通用配置系统。

### 4.4 小核构建系统改造

修改文件：

- `linux_bridge/Makefile`
- `linux_bridge/ui/k230_port.h`
- `linux_bridge/ui/k230_port.cpp`
- `linux_bridge/ui/lv_conf.h`

新增内容包括：

- UI 源文件编译
- `buf_mgt.cpp` 复用
- `libdisp/src/disp.c` 复用
- LVGL 源码编译
- `libdrm` 链接

本轮补充修正了两处实际构建问题：

1. **`custom_tick_get` C / C++ linkage 冲突**
   - `k230_port.h` 中原先按 C++ 默认方式声明
   - `lv_conf.h` 又在 `extern "C"` 语境下重新声明
   - 导致 `k230_port.cpp` 编译时报：
     - `conflicting declaration of 'uint32_t custom_tick_get()' with 'C' linkage`
   - 修复方式：
     - 为 `k230_port.h` 与 `lv_conf.h` 增加 `__cplusplus` 保护
     - 将 `custom_tick_get` 定义统一为 `extern "C"`

2. **`libdrm` 链接触发错误 `libc.so` linker script**
   - 原 Makefile 通过：
     - `-L$(LITTLE_STAGING_LIB) -ldrm`
     进行链接
   - 这会让 linker 优先命中 `staging/usr/lib/libc.so`
   - 而该文件本质是 GNU ld script，内部引用了宿主机根路径绝对位置：
     - `/lib64xthead/lp64d/libc.so.6`
     - `/usr/lib64xthead/lp64d/libc_nonshared.a`
     - `/lib/ld-linux-riscv64xthead-lp64d.so.1`
   - 最终表现为：
     - `ld` 错误地去宿主根目录查找 glibc 运行时
   - 修复方式：
     - 不再把整个 `staging/usr/lib` 作为全局 `-L` 搜索路径
     - 改为只显式链接 `$(LITTLE_STAGING_LIB)/libdrm.so`

目标是做到：

- 不需要另起 `door_lock` 单独打包
- `face_netd` 自身就能把 UI 一并编进去

## 5. 编译与验证结果

### 5.1 RT-Smart 大核编译

执行：

```sh
./build_app.sh
```

结果：

- `face_video.elf` 成功重新编译
- `face_ai.elf` / `face_event.elf` / `face_recognition.elf` 无回归
- 产物已同步到：
  - `build/bin/`
  - `k230_bin/`

说明：

- 大核“冻结预览帧提交”的改动已通过宿主机构建验证

### 5.2 Linux 小核 UI 编译

执行：

```sh
cd linux_bridge
./build_face_netd.sh
```

结果：

1. **源码编译通过**
   - `ui_runtime.cpp`
   - `k230_port.cpp`
   - `main.cpp`
   - 大量 LVGL 对象文件
   均成功编译

2. **链接通过**
   - `custom_tick_get` 的 C / C++ linkage 冲突已消除
   - `libdrm` 链接路径问题已修复
   - 成功生成：
     - `linux_bridge/out/face_netd`
   - 构建脚本最终输出：
     - `face_netd built at .../linux_bridge/out/face_netd`

这说明：

- 本轮新增 UI 代码已经完成源码级与最终链接级验证
- `face_netd` 小核产物已在当前宿主机成功生成
- 当前阻塞已不再是构建，而是后续上板联调验证

## 6. 已知问题

### 6.1 小核构建问题已解决，转入上板联调阶段

当前工作区中，`face_netd` 的 UI 集成已经完成：

已确认：

- `linux_bridge/out/face_netd` 已成功生成
- 原先两个阻塞点均已修复：
  - `custom_tick_get` 的声明 linkage 冲突
  - `libdrm` 链接时误命中 `staging/usr/lib/libc.so` ld script

因此当前重点不再是宿主机构建环境，而是：

- UI overlay 是否正确显示
- 触摸坐标映射是否与屏幕一致
- 注册预览 / 提交 / 取消链路在板端是否按预期工作

### 6.2 当前 UI 还属于“最小闭环实现”

本轮 UI 已具备：

- 注册按钮
- 键盘输入
- 提交 / 取消
- 超时处理
- 与 RT 回包联动

但以下仍属于后续可优化项：

- 更精细的屏幕布局与视觉资源
- 触摸映射现场标定
- 更细的错误提示文案
- 板端识别态与注册态的更自然联动提示

## 7. 后续建议

建议后续继续按下面顺序推进：

1. **优先上板验证 UI 基础链路**
   - 屏幕按钮是否出现
   - 触摸是否命中
   - 键盘页是否可进入
2. **验证冻结帧注册逻辑**
   - 预览后离开画面再输入姓名
   - 最终注册图必须仍为 preview 时那一帧
3. **验证会话并发策略**
   - 板端注册过程中，Web / MQTT 注册类命令是否稳定返回 `busy`
   - 查询类命令是否仍正常工作
4. **最后再做视觉与交互打磨**

## 8. 本轮结论

本轮已经完成的关键成果是：

- 大核两步注册语义已修正为“preview 冻结帧提交”
- 小核 `face_netd` 已接入板端 UI 会话模型
- 远端与板端并发策略已收敛为“板端优先，远端 busy”
- 小核 UI 模块已经写入项目并完成最终构建
- `face_netd` 构建链路中新增的两类问题已定位并修复：
  - `custom_tick_get` C / C++ linkage 冲突
  - `libdrm` 链接阶段误命中 buildroot staging 的 `libc.so` ld script

因此，这一轮可以定性为：

- **功能设计与代码接入已经完成**
- **RT 侧实现已构建验证**
- **Linux 小核侧也已完成构建验证**
- **下一阶段重点已经转为板端联调与交互验证**
