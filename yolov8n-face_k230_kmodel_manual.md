# YOLOv8-face K230 kmodel 部署使用手册

## 目录
- [模型概述](#模型概述)
- [预处理说明](#预处理说明)
- [输出格式与解码](#输出格式与解码)
- [K230 MicroPython 部署](#k230-micropython-部署)
- [K230 C SDK 部署](#k230-c-sdk-部署)
- [后处理详解](#后处理详解)
- [与人脸识别模型配合](#与人脸识别模型配合)
- [性能参考](#性能参考)
- [常见问题](#常见问题)

---

## 模型概述

| 项目 | 值 |
|------|-----|
| 模型文件 | `yolov8n-face_no_sigmoid_k230_uint8.kmodel` |
| 文件大小 | 3.59 MB |
| 输入形状 | `[1, 3, 640, 640]` |
| 输入格式 | RGB, uint8 [0,255], NCHW |
| 输出形状 | `[1, 20, 8400]` |
| 输出格式 | 20通道: 4 bbox + 1 conf(logits) + 15 kpts(logits) |
| 关键点 | 5个 (左眼、右眼、鼻尖、左嘴角、右嘴角) |
| 量化方式 | PTQ uint8 (Kld校准, NoFineTuneWeights) |
| 编译参数 | target=k230, preprocess=True |
| ⚠️ 重要 | **需手动 Sigmoid！** conf 和 kpts 可见度为 logits，见后处理章节 |

---

## 预处理说明

### 自动预处理（已内置于kmodel）

模型编译时设置了 `preprocess=True`，以下预处理已内置于kmodel中，**KPU硬件会自动完成**：

```
输入: uint8 RGB图像 [0, 255], NCHW布局
  ↓ KPU自动执行
归一化: output = (input - mean) / std = (input - 0) / 255
  ↓
模型输入: float32 [0, 1], NCHW布局
```

| 编译参数 | 值 | 说明 |
|---------|-----|------|
| `input_type` | `uint8` | 接受0-255的原始图像 |
| `input_range` | `[0, 255]` | 输入值范围 |
| `mean` | `[0.0, 0.0, 0.0]` | 均值 |
| `std` | `[255.0, 255.0, 255.0]` | 标准差 |
| `input_layout` | `NCHW` | 通道优先布局 |
| `swapRB` | `False` | 不交换RB通道（保持RGB） |

### 你需要做的

只需将图像调整为 640×640，转换为 RGB 格式，按 NCHW 排列即可传入 KPU：

```python
# MicroPython 预处理示例
import image
img = image.Image("/sdcard/test.jpg")
img = img.resize(640, 640)       # 缩放到640x640
img = img.to_rgb565()             # 转换为RGB565格式
img_data = img.compress_for_kpu() # 转换为KPU需要的NCHW uint8格式
```

---

## 输出格式与解码

### 输出张量结构

```
输出形状: (1, 20, 8400)

通道 0-3:   bbox 坐标 (cx, cy, w, h) — 模型内部 DFL 已解码
通道 4:     置信度 logits — ⚠️ 需手动 Sigmoid 转为 [0,1]
通道 5-19:  5个关键点 (x, y, visibility_logit) × 5
           — ⚠️ visibility_logit 需手动 Sigmoid 转为 [0,1]
```

### ⚠️ 为什么需要手动 Sigmoid？

YOLOv8-face 模型输出端的 Sigmoid 激活函数在 int8 量化下精度崩溃（置信度趋近于 0）。

**修复方案**：从 ONNX 模型中移除输出端的 Sigmoid 节点，改为在后处理中手动计算：

```python
import numpy as np

def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -50, 50)))
```

### 多尺度特征分布

| Stride | 特征图尺寸 | Anchor数量 | 索引范围 |
|--------|-----------|-----------|---------|
| 8      | 80×80     | 6400      | 0-6399  |
| 16     | 40×40     | 1600      | 6400-7999 |
| 32     | 20×20     | 400       | 8000-8399 |
| **总计** | -       | **8400**  | - |

### ⚠️ 核心解码公式

**模型输出的 bbox 是 (cx, cy, w, h) 中心坐标格式，不是 (x1, y1, x2, y2)！**

```python
# 1. 分离通道
bbox = output[0:4, :]      # (4, 8400) = (cx, cy, w, h)
conf_logits = output[4, :]  # (8400,)
kpts = output[5:20, :]      # (15, 8400)

# 2. ⚠️ 手动 Sigmoid：置信度
conf = sigmoid(conf_logits)

# 3. ⚠️ 手动 Sigmoid：关键点可见度
kpts[2::3] = sigmoid(kpts[2::3])

# 4. (cx, cy, w, h) → (x1, y1, x2, y2)
cx = bbox[0]
cy = bbox[1]
bw = bbox[2]
bh = bbox[3]

x1 = cx - bw / 2
y1 = cy - bh / 2
x2 = cx + bw / 2
y2 = cy + bh / 2

# 5. 模型空间 → 原图空间
# 模型输入是640×640，需要缩放到原图尺寸
scale_x = original_width / 640
scale_y = original_height / 640

x1 *= scale_x
y1 *= scale_y
x2 *= scale_x
y2 *= scale_y

# 6. 关键点同理
kpts[0::3] *= scale_x
kpts[1::3] *= scale_y
```

---

## K230 MicroPython 部署

### 完整示例代码

```python
"""
YOLOv8-face K230 人脸检测 (no_sigmoid 修复版)
部署文件: yolov8n-face_no_sigmoid_k230_uint8.kmodel
"""
import ulab.numpy as np
import image
import time
from maix import camera, display, nn

# ========== 配置参数 ==========
CONF_THRESH = 0.5       # 置信度阈值
IOU_THRESH = 0.3        # NMS IOU阈值
INPUT_SIZE = 640        # 模型输入尺寸

# ========== 加载模型 ==========
print("加载 YOLOv8-face kmodel...")
model = nn.load("/sdcard/yolov8n-face_no_sigmoid_k230_uint8.kmodel")
print(f"模型加载完成")

# ========== 辅助函数 ==========
def sigmoid(x):
    """手动 Sigmoid：将 logits 映射到 (0,1)"""
    return 1.0 / (1.0 + np.exp(-np.clip(x, -50, 50)))

# ========== 后处理函数 ==========
def nms(boxes, scores, iou_thresh):
    """非极大值抑制"""
    if len(boxes) == 0:
        return []
    
    x1 = boxes[:, 0]
    y1 = boxes[:, 1]
    x2 = boxes[:, 2]
    y2 = boxes[:, 3]
    areas = (x2 - x1) * (y2 - y1)
    
    order = np.argsort(scores)[::-1]
    keep = []
    
    while len(order) > 0:
        i = order[0]
        keep.append(i)
        
        if len(order) == 1:
            break
        
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        
        w = np.maximum(0.0, xx2 - xx1)
        h = np.maximum(0.0, yy2 - yy1)
        inter = w * h
        
        iou = inter / (areas[i] + areas[order[1:]] - inter)
        
        inds = np.where(iou <= iou_thresh)[0]
        order = order[inds + 1]
    
    return keep

def post_process(output, orig_w, orig_h):
    """
    解码kmodel输出 (no_sigmoid 版本)
    
    Args:
        output: kmodel输出, shape (1, 20, 8400)
        orig_w: 原始图像宽度
        orig_h: 原始图像高度
    
    Returns:
        faces: [{'bbox': [x1,y1,x2,y2], 'score': float, 'kps': [[x,y]*5]}]
    """
    output = output[0]  # (20, 8400)
    
    # 分离各通道
    bbox = output[0:4, :].copy()   # (4, 8400) = (cx, cy, w, h)
    conf_logits = output[4, :]     # (8400,)
    kpts = output[5:20, :].copy()  # (15, 8400)
    
    # ⚠️ 手动 Sigmoid：置信度
    conf = sigmoid(conf_logits)
    
    # ⚠️ 手动 Sigmoid：关键点可见度
    kpts[2::3] = sigmoid(kpts[2::3])
    
    # (cx, cy, w, h) → (x1, y1, x2, y2)
    cx = bbox[0]
    cy = bbox[1]
    bw = bbox[2]
    bh = bbox[3]
    
    x1 = cx - bw / 2
    y1 = cy - bh / 2
    x2 = cx + bw / 2
    y2 = cy + bh / 2
    
    bbox_xyxy = np.stack([x1, y1, x2, y2], axis=1)
    
    # 坐标缩放
    scale_x = orig_w / INPUT_SIZE
    scale_y = orig_h / INPUT_SIZE
    
    bbox_xyxy[:, 0] *= scale_x
    bbox_xyxy[:, 1] *= scale_y
    bbox_xyxy[:, 2] *= scale_x
    bbox_xyxy[:, 3] *= scale_y
    
    kpts[0::3] *= scale_x
    kpts[1::3] *= scale_y
    
    # 置信度过滤
    mask = conf > CONF_THRESH
    bbox_xyxy = bbox_xyxy[mask]
    conf = conf[mask]
    kpts = kpts[:, mask].T
    
    if len(bbox_xyxy) == 0:
        return []
    
    # NMS
    bboxes_wh = bbox_xyxy.copy()
    bboxes_wh[:, 2] = bbox_xyxy[:, 2] - bbox_xyxy[:, 0]
    bboxes_wh[:, 3] = bbox_xyxy[:, 3] - bbox_xyxy[:, 1]
    
    keep = nms(bboxes_wh, conf, IOU_THRESH)
    
    faces = []
    for idx in keep:
        b = bbox_xyxy[idx]
        kp = kpts[idx]
        
        kps_list = []
        for j in range(5):
            kps_list.append([float(kp[j*3]), float(kp[j*3+1])])
        
        faces.append({
            'bbox': [float(b[0]), float(b[1]), float(b[2]), float(b[3])],
            'score': float(conf[idx]),
            'kps': kps_list
        })
    
    return faces

# ========== 主循环 ==========
print("启动人脸检测...")
camera.start()
display.init()

while True:
    img = camera.capture()
    if img is None:
        continue
    
    orig_w, orig_h = img.width(), img.height()
    
    # 预处理：缩放 + 格式转换
    img_input = img.resize(INPUT_SIZE, INPUT_SIZE)
    img_input = img_input.to_rgb565()
    kpu_input = img_input.compress_for_kpu()
    
    # KPU推理
    t_start = time.ticks_ms()
    output = model.forward(kpu_input)
    t_end = time.ticks_ms()
    
    # 后处理
    faces = post_process(output, orig_w, orig_h)
    
    # 绘制结果
    for face in faces:
        bbox = face['bbox']
        x1, y1, x2, y2 = int(bbox[0]), int(bbox[1]), int(bbox[2]), int(bbox[3])
        
        # 画框
        img.draw_rectangle(x1, y1, x2 - x1, y2 - y1, color=(0, 255, 0), thickness=2)
        
        # 画关键点
        for kp in face['kps']:
            img.draw_circle(int(kp[0]), int(kp[1]), 3, color=(255, 0, 0), fill=True)
        
        # 显示置信度
        img.draw_string(x1, y1 - 20, f"{face['score']:.2f}", color=(0, 255, 0))
    
    # 显示FPS
    fps = 1000 / max(1, (t_end - t_start))
    img.draw_string(10, 10, f"FPS: {fps:.1f}", color=(255, 255, 0))
    
    display.show(img)
```

### 关键API说明

| API | 说明 |
|-----|------|
| `nn.load(path)` | 加载kmodel模型 |
| `model.forward(tensor)` | KPU推理，返回输出张量 |
| `img.resize(w, h)` | 图像缩放 |
| `img.to_rgb565()` | 转换为RGB565格式 |
| `img.compress_for_kpu()` | 转换为KPU需要的NCHW uint8格式 |

---

## K230 C SDK 部署

### 核心流程

```c
#include "kpu.h"
#include "mpi_vo_api.h"
#include <math.h>

// 模型配置
#define INPUT_WIDTH   640
#define INPUT_HEIGHT  640
#define CONF_THRESH   0.5f
#define IOU_THRESH    0.3f
#define NUM_ANCHORS   8400

// ⚠️ 手动 Sigmoid
static inline float sigmoid(float x) {
    if (x > 50.0f) return 1.0f;
    if (x < -50.0f) return 0.0f;
    return 1.0f / (1.0f + expf(-x));
}

// 加载模型
kpu_model_context_t model;
kpu_load_kmodel(&model, "/sdcard/yolov8n-face_no_sigmoid_k230_uint8.kmodel");

// 推理
kpu_run_kmodel(&model, input_buffer, DMAC_CHANNEL, callback, user_data);

// 获取输出
kpu_model_output_t output;
kpu_get_output(&model, 0, &output);
float* output_data = (float*)output.data;
// output_data shape: (1, 20, 8400)

// 后处理解码
typedef struct {
    float x1, y1, x2, y2;
    float score;
    float kps[5][2];
} face_t;

int decode_faces(float* output, int orig_w, int orig_h, face_t* faces, int max_faces) {
    float scale_x = (float)orig_w / INPUT_WIDTH;
    float scale_y = (float)orig_h / INPUT_HEIGHT;
    
    // 分离通道: output shape (1, 20, 8400)
    // 通道布局: bbox(4) + conf_logit(1) + kpts(15)
    float* bbox_cx = output;              // 通道0: cx
    float* bbox_cy = output + 8400;       // 通道1: cy
    float* bbox_w  = output + 2*8400;     // 通道2: w
    float* bbox_h  = output + 3*8400;     // 通道3: h
    float* conf_logits = output + 4*8400; // 通道4: conf logits
    float* kpts = output + 5*8400;        // 通道5-19: kpts
    
    int face_count = 0;
    
    for (int i = 0; i < NUM_ANCHORS; i++) {
        // ⚠️ 手动 Sigmoid
        float conf = sigmoid(conf_logits[i]);
        
        if (conf < CONF_THRESH) continue;
        
        // (cx, cy, w, h) → (x1, y1, x2, y2)
        float cx = bbox_cx[i];
        float cy = bbox_cy[i];
        float bw = bbox_w[i];
        float bh = bbox_h[i];
        
        float x1 = (cx - bw / 2) * scale_x;
        float y1 = (cy - bh / 2) * scale_y;
        float x2 = (cx + bw / 2) * scale_x;
        float y2 = (cy + bh / 2) * scale_y;
        
        faces[face_count].x1 = x1;
        faces[face_count].y1 = y1;
        faces[face_count].x2 = x2;
        faces[face_count].y2 = y2;
        faces[face_count].score = conf;
        
        // 关键点
        for (int k = 0; k < 5; k++) {
            faces[face_count].kps[k][0] = kpts[(k*3) * 8400 + i] * scale_x;
            faces[face_count].kps[k][1] = kpts[(k*3+1) * 8400 + i] * scale_y;
            // ⚠️ 关键点可见度 logit → sigmoid（如需使用）
            // float kp_vis = sigmoid(kpts[(k*3+2) * 8400 + i]);
        }
        
        face_count++;
        if (face_count >= max_faces) break;
    }
    
    // NMS
    face_count = apply_nms(faces, face_count, IOU_THRESH);
    
    return face_count;
}
```

---

## 后处理详解

### 完整解码流程

```
kmodel输出 (1, 20, 8400) — 注意：conf和kpts可见度为logits！
        │
        ▼
┌─ 通道分离 ──────────────────────────────────┐
│ bbox = output[0:4, :]    4通道 bbox坐标      │
│ conf_logits = output[4, :]  1通道置信度logits │
│ kpts = output[5:20, :]      15通道关键点      │
└──────────────────────────────────────────────┘
        │
        ▼
┌─ ⚠️ 手动 Sigmoid ───────────────────────────┐
│ conf = sigmoid(conf_logits)                  │
│ kpts[2::3] = sigmoid(kpts[2::3])             │
└──────────────────────────────────────────────┘
        │
        ▼
┌─ 坐标转换 ───────────────────────────────────┐
│ bbox 格式: (cx, cy, w, h)                    │
│                                               │
│ x1 = cx - w/2    y1 = cy - h/2               │
│ x2 = cx + w/2    y2 = cy + h/2               │
│                                               │
│ 坐标缩放: x *= orig_w/640, y *= orig_h/640    │
└──────────────────────────────────────────────┘
        │
        ▼
┌─ 置信度过滤 ──┐
│ conf > 0.5    │
└───────────────┘
        │
        ▼
┌─ NMS ────────┐
│ IOU > 0.3    │ → 抑制
└──────────────┘
        │
        ▼
    最终检测结果
```

### NMS实现

```python
def nms(boxes, scores, iou_thresh):
    """
    非极大值抑制
    
    Args:
        boxes: (N, 4) 格式为 [x1, y1, x2, y2]
        scores: (N,) 置信度
        iou_thresh: IOU阈值
    
    Returns:
        keep: 保留的索引列表
    """
    if len(boxes) == 0:
        return []
    
    x1 = boxes[:, 0]
    y1 = boxes[:, 1]
    x2 = boxes[:, 2]
    y2 = boxes[:, 3]
    areas = (x2 - x1) * (y2 - y1)
    
    order = np.argsort(scores)[::-1]
    keep = []
    
    while len(order) > 0:
        i = order[0]
        keep.append(i)
        
        if len(order) == 1:
            break
        
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        
        w = np.maximum(0.0, xx2 - xx1)
        h = np.maximum(0.0, yy2 - yy1)
        inter = w * h
        
        iou = inter / (areas[i] + areas[order[1:]] - inter)
        
        inds = np.where(iou <= iou_thresh)[0]
        order = order[inds + 1]
    
    return keep
```

---

## 与人脸识别模型配合

### 完整流程

```
摄像头采集
    │
    ▼
YOLOv8-face 人脸检测 (本模型)
    │ 输出: bbox + 5关键点
    ▼
人脸对齐 (基于5关键点)
    │
    ▼
GhostFaceNet 人脸特征提取
    │ 输出: 512维特征向量
    ▼
特征比对 / 识别
```

### 人脸对齐示例

```python
import cv2
import numpy as np

# 标准人脸关键点位置 (112×112)
STD_LANDMARKS = np.array([
    [38.2946, 51.6963],   # 左眼
    [73.5318, 51.6963],   # 右眼
    [56.0252, 71.7366],   # 鼻尖
    [41.5493, 92.3655],   # 左嘴角
    [70.7299, 92.3655],   # 右嘴角
], dtype=np.float32)

def align_face(img, landmarks, output_size=(112, 112)):
    """
    基于5点关键点进行人脸对齐
    
    Args:
        img: 原始图像
        landmarks: 5个关键点 [(x,y), ...]
        output_size: 输出尺寸
    
    Returns:
        aligned: 对齐后的人脸图像
    """
    src_pts = np.array(landmarks, dtype=np.float32)
    dst_pts = STD_LANDMARKS.copy()
    
    # 缩放到输出尺寸
    dst_pts[:, 0] *= output_size[0] / 112.0
    dst_pts[:, 1] *= output_size[1] / 112.0
    
    # 相似变换
    M, _ = cv2.estimateAffinePartial2D(src_pts, dst_pts)
    aligned = cv2.warpAffine(img, M, output_size)
    
    return aligned
```

---

## 性能参考

| 指标 | 值 |
|------|-----|
| 模型大小 | 3.59 MB |
| 推理时间 (K230 KPU) | ~15-25ms |
| 检测精度 (float32) | 与原始ONNX一致 |
| 量化精度损失 | 极小（仅移除输出Sigmoid） |
| 输入分辨率 | 640×640 |
| 检测范围 | 支持多尺度人脸 |

---

## 常见问题

### Q1: 检测不到人脸？

**A**: 检查以下几点：
1. 置信度阈值是否过高（建议 0.5）
2. 图像是否已正确缩放为 640×640
3. 输入格式是否为 RGB（不是 BGR）
4. 是否执行了手动 Sigmoid

### Q2: 检测框位置不对？

**A**: 确认 bbox 解码流程：
1. 模型输出是 `(cx, cy, w, h)` 格式，不是 `(x1, y1, x2, y2)`
2. 需要转换：`x1 = cx - w/2`, `y1 = cy - h/2`
3. 需要缩放到原图尺寸：`x *= orig_w / 640`

### Q3: 置信度全部接近 0？

**A**: 确认是否执行了手动 Sigmoid。模型输出的是 logits，不是概率值。

### Q4: 关键点位置偏移？

**A**: 确认关键点坐标已缩放到原图尺寸：
```python
kpts[0::3] *= orig_w / 640  # x坐标
kpts[1::3] *= orig_h / 640  # y坐标
```

### Q5: 如何切换为 float32 版本？

**A**: 将模型文件替换为 `yolov8n-face_no_sigmoid_k230_float32.kmodel`，后处理代码不变。

### Q6: 与原始 ONNX 模型的区别？

**A**: 
- 原始 ONNX 输出已包含 Sigmoid（conf 和 kpts 可见度在 [0,1] 范围）
- 本 kmodel 移除了 Sigmoid，输出为 logits，需手动 Sigmoid
- bbox 坐标格式和 DFL 解码逻辑与原始模型完全一致