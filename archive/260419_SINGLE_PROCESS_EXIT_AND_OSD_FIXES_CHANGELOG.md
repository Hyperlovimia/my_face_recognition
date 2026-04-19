# 单进程退出清理与 OSD 显示修复记录

日期：2026-04-19

## 1. 背景

本轮工作围绕 `k230_sdk/src/reference/ai_poc/my_face_recognition` 的单进程入口 `face_recognition.elf` 展开，主要处理了两类问题：

1. 程序第一次启动可运行，但按 `Ctrl+C` 退出后再次启动会重新出现 `dump` 失败，怀疑退出不干净。
2. 运行时 OSD 相关显示异常，包括：
   - 注册截图预览方向错误
   - 注册截图预览尺寸不合适
   - 注册截图预览颜色异常、透明度异常
   - 人脸检测/识别框在竖屏模式下方向错误

最终这些问题都已逐步修复完成。

## 2. `Ctrl+C` 后二次启动 `dump` 失败

### 2.1 现象

- 板子首次启动程序正常
- 按 `Ctrl+C` 结束程序后，再次启动时重新出现 `kd_mpi_vicap_dump_frame failed` 一类报错
- 判断根因是资源退出不完整，VICAP / VO / OSD / VB 链路残留

### 2.2 根因分析

主要原因有两点：

1. `src/main.cc`
   - 单进程入口没有处理 `SIGINT`
   - `Ctrl+C` 时进程被内核直接打死
   - `video_proc()` 里的 `pl.Destroy()` 根本没有机会执行

2. `src/video_pipeline.cc`
   - `PipeLine::Destroy()` 原本是“某一步失败就直接 `return`”
   - 一旦中途某个 API 清理失败，后续 `unbind / close connector / mmz_free / vb_exit` 都不会执行
   - 退出瞬间如果仍持有一帧 `dump_info`，VICAP dump 队列也可能残留占用

### 2.3 修改内容

#### `src/main.cc`

- 新增 `SIGINT` / `SIGTERM` 处理
- 信号处理函数只负责置退出标志，不在 handler 中执行任何 MPP / OpenCV / IO 清理逻辑
- 主线程输入循环改成 `poll(STDIN_FILENO, ...)` 轮询，不再永久阻塞在 `std::getline`
- `video_proc()` 的主循环退出条件统一为“`isp_stop` 或收到退出信号”
- 启动提示改成 `Press 'q+Enter' or Ctrl+C to exit.`

#### `src/video_pipeline.cc`

- `Destroy()` 开头先检查是否仍持有 `dump_info`，如有则优先 `kd_mpi_vicap_dump_release`
- `Destroy()` 改成 best-effort 清理模式：
  - 记录第一个错误码
  - 即使某一步失败，也继续尝试后续清理
  - 最后返回首个错误码，而不是中途直接退出
- 清理顺序收敛为：
  1. release 未归还 dump
  2. stop stream
  3. unbind VI->VO
  4. deinit VICAP
  5. disable OSD / VO
  6. munmap + release OSD block + destroy OSD pool
  7. close connector
  8. free AI mmz buffer
  9. `vb_exit()`（仅本进程初始化过 VB 时）

#### `utils/run.sh`

- 用 `exec ./face_recognition.elf ...` 替代直接启动，避免多一层前台 shell

#### `README.md`

- 补充说明：
  - `q`
  - `Ctrl+C`
  都应触发优雅退出
- 明确指出看到 `PipeLine::Destroy` 日志意味着退出清理路径已执行

### 2.4 结果

- `Ctrl+C` 不再是粗暴杀进程，而是进入可清理退出路径
- 二次启动时不再因为上次异常终止而复现 `dump` 失败

## 3. 注册截图预览方向错误

### 3.1 现象

- 按 `i` 后显示的注册截图方向错误
- 相比底层实时视频，截图被额外旋转了 90°
- 同时还被拉伸压缩

### 3.2 原因

当前板子为竖屏显示：

```c
#define DISPLAY_WIDTH 960
#define DISPLAY_HEIGHT 540
#define DISPLAY_ROTATE 1
#define OSD_WIDTH 540
#define OSD_HEIGHT 960
```

视频层本身已经通过 `VO + K_ROTATION_90` 旋转到竖屏，但注册截图预览最初只是把原始 `1280x720` 图像直接缩放后贴到 OSD 上，未做和视频层一致的方向变换，因此方向与底层视频不一致。

### 3.3 修改内容

在 `src/main.cc` 中新增预览渲染辅助逻辑：

- 对截图先做和视频层一致的 `90°` 方向旋转
- 再按 OSD 尺寸做等比缩放
- 最终贴到 OSD 上显示

### 3.4 结果

- 注册截图方向与底层实时视频保持一致

## 4. 注册截图预览尺寸过大

### 4.1 现象

- 修正方向后，截图一度几乎铺满全屏
- 需求是只显示在一个角落，作为预览小窗

### 4.2 修改内容

在 `src/main.cc` 的预览渲染逻辑中：

- 限制预览最大尺寸为：
  - `OSD_WIDTH / 2`
  - `OSD_HEIGHT / 2`
- 保持等比缩放
- 固定绘制到左上角，并保留边距

### 4.3 结果

- 注册截图预览变为角落小窗，不再遮挡大部分画面

## 5. 注册截图预览颜色异常与半透明

### 5.1 现象

后续在修复预览过程中又出现了两类显示问题：

1. 图片严重偏蓝
2. 图片半透明

### 5.2 分析过程

这个问题分成了两层：

1. 采集到的预览图三通道语义是否与当前假设一致
2. OSD 缓冲的 4 通道字节顺序是否与 OpenCV 默认 `BGRA` 一致

实践中发现：

- 单纯使用 `cv::cvtColor(..., COLOR_BGR2BGRA)` 写入 OSD 会导致颜色错误和透明度异常
- OSD 实际消费的是原始 `ARGB8888` 字节序，而不是 OpenCV 默认解释下的 `BGRA`
- 同时，预览图本身表现出来更接近 `RGB` 语义而不是之前代码里假设的 `BGR`

### 5.3 修改内容

#### `src/main.cc`

为注册预览新增专用转换函数：

- 手工将预览图逐像素打包成 OSD 需要的 `ARGB8888`
- `alpha` 固定写为 `255`
- 按最终验证结果采用：
  - `R = src[0]`
  - `G = src[1]`
  - `B = src[2]`

这样避免了：

- 误用 `BGRA`
- alpha 字节位置错误
- 红蓝通道颠倒

#### `src/video_pipeline.cc`

在排查过程中，还对 `YUV420SP -> RGB` 路径做过一次收敛：

- 从 `NV12` 路径切换到更贴近板端实际表现的 `NV21/VU` 路径

### 5.4 结果

- 注册截图预览不再半透明
- 注册截图预览颜色最终与实时视频一致

## 6. 人脸检测框 / 识别框方向错误

### 6.1 现象

- 预览图片方向修正后，检测框仍然方向不对
- 一度出现：
  - 人脸向上，框向下
  - 人脸向左，框向右
- 本质上是框坐标做了错误方向的 90° 映射

### 6.2 原因

模型输出的人脸框坐标仍然处于原始 AI 图像坐标系（`AI_FRAME_WIDTH x AI_FRAME_HEIGHT`）下，而 OSD 是竖屏坐标系（`OSD_WIDTH x OSD_HEIGHT`）。如果仍然直接按：

```cpp
x = bbox.x / ref_w * dst_w;
y = bbox.y / ref_h * dst_h;
```

进行线性映射，那么得到的框只适用于未旋转场景；一旦底层视频显示已经旋转，就会出现“框和视频方向不一致”的问题。

### 6.3 修改内容

#### `src/face_recognition.cc`

新增 `bbox_to_osd_rect(...)`：

- 在 `DISPLAY_ROTATE` 场景下，先做和视频一致的旋转坐标变换
- 再映射到 OSD 画布大小
- 最后裁剪到有效范围，避免越界

`FaceRecognition::draw_result()` 改为使用这个转换后的 `cv::Rect`

#### `src/ipc_osd_draw.cc`

三进程版本同样新增 `bbox_to_osd_rect(...)`

- 确保 `face_video + face_ai + face_event` 路径也使用同样的旋转后坐标逻辑
- 避免单进程和三进程行为不一致

### 6.4 坐标映射最终形式

在 `DISPLAY_ROTATE` 场景下，最终采用的映射为：

```cpp
x = (ref_h - (bbox.y + bbox.h)) / ref_h * dst_w;
y = bbox.x / ref_w * dst_h;
w = bbox.h / ref_h * dst_w;
h = bbox.w / ref_w * dst_h;
```

### 6.5 结果

- 检测框 / 识别框方向最终与视频一致
- 解决了“上下左右颠倒”的问题

## 7. 本轮涉及的主要文件

本次对话中主要改动或分析过的文件：

- `src/main.cc`
- `src/video_pipeline.cc`
- `src/face_recognition.cc`
- `src/ipc_osd_draw.cc`
- `utils/run.sh`
- `README.md`

另外也参考和对照了：

- `src/setting.h`
- `src/face_video_main.cc`
- `archive/260409_MIGRATION_TO_K230_SDK.md`
- `archive/260408_RTSMART_SINGLE_UART_CHANGELOG.md`

## 8. 最终状态

截至本次对话结束，已完成以下修复：

- `Ctrl+C` 退出后再次启动不再因退出不干净而触发 `dump` 失败
- 注册截图预览方向正确
- 注册截图预览大小合适，只显示在角落
- 注册截图预览颜色正确
- 注册截图预览不再半透明
- 人脸检测 / 识别框方向正确

当前项目在单进程入口上的退出行为、截图预览行为、OSD 渲染行为已经明显稳定下来，板上实际使用体验与此前相比已大幅改善。
