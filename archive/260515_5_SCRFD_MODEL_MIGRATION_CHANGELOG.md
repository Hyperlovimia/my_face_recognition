# 人脸检测模型迁移至 SCRFD 工作记录

日期：2026-05-15

## 1. 本轮目标

将项目所有人脸检测模型统一替换为 SCRFD 2.5G BNKPS，覆盖单进程和三进程两个版本，确保人脸检测、关键点提取、识别对齐全链路正确运行。

替换前模型：YOLOv8n-face（存在 int8 量化 Sigmoid 精度崩溃问题，无法正常使用）

替换后模型：`scrfd_2.5g_bnkps_shape640x640_k230.kmodel`

模型文档：`scrfd_kmodel使用说明.md`

## 2. 模型规格

| 属性 | 值 |
|------|-----|
| 模型名称 | SCRFD 2.5G BNKPS |
| 输入形状 | `[1, 3, 640, 640]` |
| 输入类型 | `uint8`，范围 `[0, 255]` |
| 输入布局 | `NCHW`，RGB |
| 预处理 | 模型内置归一化 `(x - 127.5) / 128.0` |
| 输出张量数 | 9 个（3 个尺度 × {score, bbox, kps}） |
| 关键点 | 5 点（左眼、右眼、鼻尖、左嘴角、右嘴角） |
| 检测方式 | anchor-free 中心点解码 |
| 推荐阈值 | 置信度 0.5，NMS 0.4 |

### 2.1 输出张量布局

| 序号 | 名称 | 形状 | 说明 |
|------|------|------|------|
| 0 | score_8 | `[1, 12800, 1]` | stride=8 置信度 |
| 1 | score_16 | `[1, 3200, 1]` | stride=16 置信度 |
| 2 | score_32 | `[1, 800, 1]` | stride=32 置信度 |
| 3 | bbox_8 | `[1, 12800, 4]` | stride=8 边界框偏移 |
| 4 | bbox_16 | `[1, 3200, 4]` | stride=16 边界框偏移 |
| 5 | bbox_32 | `[1, 800, 4]` | stride=32 边界框偏移 |
| 6 | kps_8 | `[1, 12800, 10]` | stride=8 关键点 |
| 7 | kps_16 | `[1, 3200, 10]` | stride=16 关键点 |
| 8 | kps_32 | `[1, 800, 10]` | stride=32 关键点 |

## 3. 修改文件清单

### 3.1 核心修改

#### `src/face_detection.cc`

**改动范围**：`post_process()` 和 `draw_result()` 两个函数。

**post_process 改动**：

- 新增匿名命名空间内的 SCRFD 专用数据结构：
  - `ScrfCandidate`：候选框结构体（含 bbox、score、5 点关键点）
  - `scrfd_nms_comparator()`：按置信度降序排列的比较函数
  - `scrfd_box_iou()`：计算两个候选框的 IoU
- 实现 3 尺度 anchor-free 中心点解码：
  - `strides = {8, 16, 32}`，`num_anchors = 2`
  - 中心点：`cx = (w + 0.5) * stride`，`cy = (h + 0.5) * stride`
  - 边界框：`x1 = (cx - dx1 * stride) * scale`，以此类推
  - 关键点：`kpx = (cx + kps[...] * stride) * scale`，以此类推
- 按置信度阈值 `obj_thresh_` 过滤候选框
- 使用 `qsort` + 贪心 NMS（IoU ≥ `nms_thresh_` 时抑制）
- 结果边界框 clamp 到帧尺寸范围内

**draw_result 改动**：

- 关键点绘制新增 `DISPLAY_ROTATE` 分支（竖屏顺时针 90° 旋转）：
  ```cpp
  if (DISPLAY_ROTATE) {
      x0 = static_cast<int32_t>((ref_h - py) / ref_h * src_w);
      y0 = static_cast<int32_t>(px / ref_w * src_h);
  }
  ```
- 边界框绘制新增 `DISPLAY_ROTATE` 分支（与关键点旋转逻辑一致）：
  ```cpp
  if (DISPLAY_ROTATE) {
      x = static_cast<int>((ref_h - (b.y + b.h)) / ref_h * src_w);
      y = static_cast<int>(b.x / ref_w * src_h);
      w = static_cast<int>(b.h / ref_h * src_w);
      h = static_cast<int>(b.w / ref_w * src_h);
  }
  ```

#### `src/face_detection.h`

无改动。现有接口（`Bbox`、`SparseLandmarks`、`FaceDetectionInfo`、`FaceDetection` 类）与 SCRFD 完全兼容。

#### `src/main.cc`

无改动。`FaceDetection` 构造函数签名未变，命令行参数传递方式不变。

### 3.2 编译配置清理

#### `src/CMakeLists.txt`

- 移除了 SCRFD 验证专用的可执行文件 target（`face_recognition_scrfd.elf`）
- `install()` 中不再包含 SCRFD 验证 ELF
- `common_ai_src` 保持不变，`face_detection.cc` 继续被单进程和三进程共享

#### `build_app.sh`

- `collect_outputs()` 中的 ELF 列表移除了 SCRFD 验证条目
- 收集的 ELF 保持为：`face_recognition.elf`、`face_video.elf`、`face_ai.elf`、`face_event.elf`

### 3.3 删除的文件

以下 SCRFD 模型验证专用文件已删除（验证通过后不再需要独立版本）：

| 文件 | 说明 |
|------|------|
| `src/face_detection_scrfd.h` | SCRFD 验证版检测头文件 |
| `src/face_detection_scrfd.cc` | SCRFD 验证版检测实现 |
| `src/main_scrfd.cc` | SCRFD 验证版入口 |

### 3.4 三进程版本

**无需任何修改**。原因：

- `face_ai.elf` 通过 `CMakeLists.txt` 中的 `common_ai_src` 共享 `face_detection.cc`
- `face_ai_main.cc` 中 `FaceDetection` 的构造和调用方式与单进程完全一致
- `ipc_proto.h` 中 `ipc_face_bundle_t::sparse_kps[10]` 与 SCRFD 5 点关键点格式匹配
- `ipc_osd_draw.cc` 中 `bbox_to_osd_rect()` 已内置 `DISPLAY_ROTATE` 旋转处理
- `face_video.elf` 和 `face_event.elf` 不直接涉及检测模型

### 3.5 未使用的残留文件

| 文件 | 状态 |
|------|------|
| `src/anchors_640.cc` | 死代码，未被任何文件引用，不在编译列表中。SCRFD 使用 anchor-free 中心点解码，不需要 anchor 数据 |
| `src/anchors_320.cc` | 同上 |

## 4. 修复的问题

### 4.1 YOLOv8n-face int8 量化 Sigmoid 精度崩溃

**现象**：使用 `yolov8n-face_k230_uint8.kmodel` 时完全无法检测到人脸框。

**根因**：int8 量化下 Sigmoid 激活函数精度崩溃，输出置信度全部异常。

**尝试修复**：使用 `yolov8n-face_no_sigmoid_k230_uint8.kmodel`（移除模型内 Sigmoid），在 C++ 后处理中手动添加 Sigmoid。

**最终方案**：YOLOv8n-face 修复后仍不稳定，改用 SCRFD 模型彻底解决。

### 4.2 SCRFD 5 点关键点竖屏错位

**现象**：竖屏模式（`DISPLAY_ROTATE=1`）下，5 点关键点标记与人脸框位置不一致，有时出现方向反转。

**根因**：`draw_result()` 中边界框已做旋转变换，但关键点绘制缺少对应的 `DISPLAY_ROTATE` 分支。

**修复**：在 `draw_result()` 中为关键点坐标添加与边界框一致的顺时针 90° 旋转逻辑。

### 4.3 SCRFD 验证头文件重复定义

**现象**：`face_detection_scrfd.h` 编译报重复定义错误。

**根因**：验证头文件中重复定义了 `Bbox`、`SparseLandmarks`、`FaceDetectionInfo` 等类型。

**修复**：验证文件已删除，正式版本直接复用 `face_detection.h` 中的类型定义。

## 5. 全链路数据流

```
视频帧 (VICAP)
  │  [1, 3, H, W] uint8 NCHW RGB
  ▼
FaceDetection::pre_process()
  │  ai2d resize → [1, 3, 640, 640]
  ▼
FaceDetection::inference()
  │  KPU 推理 → 9 个输出张量
  ▼
FaceDetection::post_process()
  │  3 尺度 anchor-free 解码 → NMS
  │  产出 vector<FaceDetectionInfo>
  │  每个元素: { bbox, sparse_kps.points[10], score }
  │
  ├──► FaceRecognition::pre_process(input_tensor, sparse_kps.points)
  │       │  5 点关键点 → 仿射矩阵 → ai2d 对齐 → [1, 3, 112, 112]
  │       ▼
  │    FaceRecognition::inference() → database_search()
  │
  └──► (三进程) ipc_ai_reply_t → IPC → face_video → ipc_draw_faces_osd()
        (单进程) FaceDetection::draw_result() + FaceRecognition::draw_result()
```

## 6. 验证点

本轮应重点验证以下场景：

- 单进程 `face_recognition.elf` 使用 SCRFD kmodel 能正常检测人脸框
- 竖屏模式（`DISPLAY_ROTATE=1`）下 5 点关键点与人脸框位置一致
- 人脸识别（注册 + 检索）功能正常，关键点对齐无误
- 三进程 `face_ai.elf` 使用 SCRFD kmodel 能正常检测并回传结果
- 三进程 OSD 显示的人脸框位置正确
- 命令行参数 `argv[1]` 传入 SCRFD kmodel 路径后程序正常启动
- 推荐参数：`det_thres=0.5`，`nms_thres=0.4`

## 7. 部署提醒

启动单进程时，第一个参数传入 SCRFD 模型路径：

```bash
./face_recognition.elf \
  /path/to/scrfd_2.5g_bnkps_shape640x640_k230.kmodel \
  0.3 0.2 \
  /path/to/ghostfacenet.kmodel \
  70 \
  /path/to/db_dir \
  0
```

三进程 `face_ai.elf` 同理，第一个参数传入 SCRFD 模型路径。

注意：SCRFD 模型输入要求 640×640 RGB uint8 NCHW，预处理归一化已内置，调用方无需手动归一化。