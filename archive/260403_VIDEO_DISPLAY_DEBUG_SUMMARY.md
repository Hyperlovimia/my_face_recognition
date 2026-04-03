# My Face Recognition 视频显示问题排查与修复总结

## 1. 问题背景

项目路径：

- `/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition`

目标现象：

- 启动 `face_recognition.elf` 后，摄像头画面和 OSD 需要正常显示到屏幕上。

设备实际显示配置：

- 分辨率：`960 x 536`
- 旋转：`90°`
- 屏幕类型：`NT35516`

应用里的显示模式配置位于：

- [setting.h](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/setting.h#L11)
- [setting.h](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/setting.h#L54)

这里默认使用的是 `DISPLAY_MODE_NT35516`，并且宽高为 `960x536`。

## 2. 最初导致“视频无法正常显示”的两个主要原因

### 原因一：系统启动时已经预初始化 VB，但应用仍按“自己初始化 VB”的方式启动

初次运行时日志中出现：

```text
VB is initialized!
vb_set_config failed ret:-1610317806
```

这说明：

- 系统在 boot 时已经初始化了全局 VB。
- 应用再次执行 `kd_mpi_vb_set_config()` 时返回 `K_ERR_VB_BUSY`。
- 旧代码把这个情况当成致命错误，直接中断了初始化流程。

对应代码位置：

- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L78)

### 原因二：固件没有启用 `NT35516` 屏驱，导致 connector 无法打开

后续日志中出现过：

```text
kd_mpi_connector_open, failed(-1).
Create, connector open failed.
```

这说明：

- 应用已经按 `NT35516_MIPI_2LAN_536X960_30FPS` 去初始化 connector。
- 但固件里当时没有打开 `NT35516` 面板驱动。
- 所以 `kd_mpi_connector_open()` 无法成功，显示链路根本没有建立起来。

应用侧选择 `NT35516` connector 的代码在：

- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L15)

而最终需要配套启用的固件配置是：

- [k230_canmv_dongshanpi_defconfig](/home/hyperlovimia/rtos_k230/configs/k230_canmv_dongshanpi_defconfig)

## 3. 运行过程中暴露出的附加稳定性问题

在早期失败日志里，还出现过：

```text
dump dev(0)chn(1) failed.
kd_mpi_vicap_dump_frame failed.
...
Exception 13: Load Page Fault
```

这说明除了主问题外，应用还存在失败路径保护不足的问题：

- `PipeLine::Create()` 的返回值没有被调用方正确拦截。
- `GetFrame()` 在 `dump_frame` 失败后，仍继续尝试使用无效物理地址。
- 随后 `mmap` 失败或得到空地址，最终在后续访问时触发页错误。

相关代码位置：

- [main.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/main.cc#L95)
- [main.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/main.cc#L120)
- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L416)

## 4. 已完成的修复工作

### 4.1 让应用兼容“系统预初始化 VB”

在 `PipeLine::Create()` 中加入了对 `K_ERR_VB_BUSY` 的兼容处理：

- 如果 `kd_mpi_vb_set_config()` 返回 `K_ERR_VB_BUSY`
- 说明 VB 已由系统初始化
- 此时不再报错退出，而是复用现有 VB 配置继续运行

代码位置：

- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L79)
- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L90)

同时增加了状态位，确保只有“本程序自己初始化过的 VB”才会在退出时执行 `kd_mpi_vb_exit()`，避免误释放系统全局媒体资源。

相关清理逻辑位置：

- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L492)

### 4.2 修复初始化失败后的崩溃链

对应用运行时的错误路径进行了保护：

- `pl.Create()` 失败时，线程立即退出，不再继续执行识别流程
- `pl.GetFrame()` 失败时，循环立即终止，不再使用无效地址构造 tensor
- `PipeLine::GetFrame()` 中加入对 `dump_frame` 失败、无效物理地址、`mmap` 失败的检查
- `ReleaseFrame()` 和 `Destroy()` 仅在资源真实创建后才释放，避免重复释放或空释放

关键代码位置：

- [main.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/main.cc#L95)
- [main.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/main.cc#L120)
- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L416)
- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L453)
- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L492)

### 4.3 启用 `NT35516` 显示驱动

最终决定性的修复是：在板级 defconfig 中启用 `NT35516` LCD panel driver。

已修改：

- [k230_canmv_dongshanpi_defconfig](/home/hyperlovimia/rtos_k230/configs/k230_canmv_dongshanpi_defconfig)

新增配置：

```config
CONFIG_MPP_DSI_ENABLE_LCD_NT35516=y
```

这一步使得应用中选择的 `NT35516_MIPI_2LAN_536X960_30FPS` 能在运行时成功打开 connector，显示链路才真正成立。

### 4.4 优化 connector 失败提示

为了后续排查更直接，应用侧还增加了更明确的报错信息：

- 当 connector 打不开时，打印 `connector_type` 和 `connector_name`
- 提示优先检查固件中是否启用了对应 panel driver

代码位置：

- [video_pipeline.cc](/home/hyperlovimia/rtos_k230/src/rtsmart/examples/ai/my_face_recognition/src/video_pipeline.cc#L139)

## 5. 为什么之前“看起来像是显示坏了”，实际上不是

最初从现象上看是“视频没显示出来”，但真正根因并不单一：

1. VB 已预初始化，应用初始化流程提前失败。
2. 即使绕过 VB 问题，固件里也没有启用 `NT35516` 屏驱，导致显示 connector 无法打开。
3. 在更早的失败路径里，应用还会因为未检查取帧失败而直接崩溃。

也就是说，“视频无法正常显示”并不是单纯的屏幕硬件坏了，而是：

- 应用初始化逻辑与系统固件启动方式不匹配；
- 显示面板驱动配置不完整；
- 失败路径缺少保护，掩盖了真实根因。

## 6. 最终成功运行的关键信号

最新成功日志中，以下几行说明修复已经生效：

```text
VB is already initialized by system/another app, reuse existing VB configuration.
VICAP to VO: layer=1 configured for 960x536 NV12, rotate90=1
OSD to VO: layer=4 configured for 960x536 BGRA8888, rotate90=1
kd_mpi_vicap_start_stream
init database Done!
```

它们分别说明：

- 应用已能复用系统预初始化的 VB；
- 视频层与 OSD 层都已经按 `960x536` 成功配置；
- VICAP 成功启动数据流；
- 主业务流程进入正常运行阶段。

## 7. 本次问题的最终结论

本次“视频无法正常显示”的根因可以总结为：

1. 应用没有兼容系统预初始化 VB，导致早期初始化失败。
2. 固件没有启用 `NT35516` 面板驱动，导致显示 connector 无法打开。
3. 应用在失败路径上缺少错误保护，进一步引发 `dump_frame` 后的空地址访问崩溃。

最终通过以下方式解决：

1. 在应用中兼容 `VB already initialized` 场景。
2. 在应用中补齐初始化失败和取帧失败的保护逻辑。
3. 在板级配置中启用 `CONFIG_MPP_DSI_ENABLE_LCD_NT35516=y`。
4. 重新构建并部署后，视频与 OSD 已可正常显示，项目恢复正常运行。

