# SCRFD 2.5G 人脸检测 kmodel 使用说明

## 模型信息

| 属性 | 值 |
|------|-----|
| 模型名称 | SCRFD 2.5G BNKPS |
| 模型文件 | scrfd_2.5g_bnkps_shape640x640_k230.kmodel |
| 目标平台 | 嘉楠科技 K230 |
| 量化方式 | PTQ uint8 |

---

## 输入规格

| 参数 | 值 |
|------|-----|
| 形状 | `[1, 3, 640, 640]` |
| 类型 | `uint8` |
| 范围 | `[0, 255]` |
| 布局 | `NCHW` |
| 颜色格式 | RGB |
| 预处理 | 模型内置归一化 |

**预处理公式（已内置）**：
```
(x - 127.5) / 128.0
```

---

## 输出规格

模型输出 9 个张量：

| 序号 | 名称 | 形状 | 说明 |
|------|------|------|------|
| 0 | score_8 | `[1, 12800, 1]` | stride=8 的置信度 |
| 1 | score_16 | `[1, 3200, 1]` | stride=16 的置信度 |
| 2 | score_32 | `[1, 800, 1]` | stride=32 的置信度 |
| 3 | bbox_8 | `[1, 12800, 4]` | stride=8 的边界框 |
| 4 | bbox_16 | `[1, 3200, 4]` | stride=16 的边界框 |
| 5 | bbox_32 | `[1, 800, 4]` | stride=32 的边界框 |
| 6 | kps_8 | `[1, 12800, 10]` | stride=8 的关键点 |
| 7 | kps_16 | `[1, 3200, 10]` | stride=16 的关键点 |
| 8 | kps_32 | `[1, 800, 10]` | stride=32 的关键点 |

**说明**：
- `score_*`: 人脸置信度，范围 [0, 1]
- `bbox_*`: 边界框偏移量 `[dx1, dy1, dx2, dy2]`，相对于 anchor
- `kps_*`: 5个关键点坐标 `[x0, y0, x1, y1, ..., x4, y4]`，相对于 anchor

---

## 使用方法

### 预处理

```python
import cv2
import numpy as np

def preprocess(image):
    """
    Args:
        image: BGR 格式图像 (OpenCV 默认)
    
    Returns:
        input_data: shape=[1,3,640,640], dtype=uint8
    """
    img_rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    img_resized = cv2.resize(img_rgb, (640, 640))
    img_chw = img_resized.transpose(2, 0, 1)
    input_data = np.expand_dims(img_chw, axis=0)
    return input_data.astype(np.uint8)
```

### 推理

```python
import numpy as np
from nncase import Runtime

runtime = Runtime()
with open('scrfd_2.5g_bnkps_shape640x640_k230.kmodel', 'rb') as f:
    runtime.load_model(f.read())

input_data = preprocess(image)
runtime.set_input_tensor(0, input_data)
runtime.run()

outputs = []
for i in range(9):
    outputs.append(runtime.get_output_tensor(i))
```

### 解码人脸框

```python
def decode_faces(outputs, orig_shape, conf_thresh=0.5, nms_thresh=0.4):
    """
    解码 SCRFD 输出，获取人脸框和关键点
    
    Args:
        outputs: 模型输出列表 [score_8, score_16, score_32, 
                  bbox_8, bbox_16, bbox_32, kps_8, kps_16, kps_32]
        orig_shape: 原始图像尺寸 (height, width)
        conf_thresh: 置信度阈值
        nms_thresh: NMS 阈值
    
    Returns:
        faces: 人脸列表，每个元素包含 {'bbox': [x1,y1,x2,y2], 'score': float, 'kps': [[x,y], ...]}
    """
    strides = [8, 16, 32]
    num_anchors = 2
    input_size = 640
    orig_h, orig_w = orig_shape
    
    all_faces = []
    
    for idx, stride in enumerate(strides):
        scores = outputs[idx].to_numpy()[0]
        bboxes = outputs[idx + 3].to_numpy()[0]
        kps = outputs[idx + 6].to_numpy()[0]
        
        height = input_size // stride
        width = input_size // stride
        
        for h in range(height):
            for w in range(width):
                for a in range(num_anchors):
                    i = h * width * num_anchors + w * num_anchors + a
                    score = scores[i, 0]
                    
                    if score > conf_thresh:
                        bbox = bboxes[i]
                        
                        cx = (w + 0.5) * stride
                        cy = (h + 0.5) * stride
                        
                        x1 = (cx - bbox[0] * stride) * orig_w / input_size
                        y1 = (cy - bbox[1] * stride) * orig_h / input_size
                        x2 = (cx + bbox[2] * stride) * orig_w / input_size
                        y2 = (cy + bbox[3] * stride) * orig_h / input_size
                        
                        kp = kps[i].reshape(5, 2)
                        kps_scaled = []
                        for k in range(5):
                            kpx = (cx + kp[k, 0] * stride) * orig_w / input_size
                            kpy = (cy + kp[k, 1] * stride) * orig_h / input_size
                            kps_scaled.append([kpx, kpy])
                        
                        all_faces.append({
                            'bbox': [x1, y1, x2, y2],
                            'score': float(score),
                            'kps': kps_scaled
                        })
    
    return nms(all_faces, nms_thresh)

def nms(faces, thresh=0.4):
    """非极大值抑制"""
    if len(faces) == 0:
        return []
    
    faces.sort(key=lambda x: x['score'], reverse=True)
    
    keep = []
    while len(faces) > 0:
        best = faces[0]
        keep.append(best)
        faces = faces[1:]
        
        remaining = []
        for face in faces:
            iou = compute_iou(best['bbox'], face['bbox'])
            if iou < thresh:
                remaining.append(face)
        faces = remaining
    
    return keep

def compute_iou(box1, box2):
    """计算两个框的 IoU"""
    x1 = max(box1[0], box2[0])
    y1 = max(box1[1], box2[1])
    x2 = min(box1[2], box2[2])
    y2 = min(box1[3], box2[3])
    
    inter = max(0, x2 - x1) * max(0, y2 - y1)
    area1 = (box1[2] - box1[0]) * (box1[3] - box1[1])
    area2 = (box2[2] - box2[0]) * (box2[3] - box2[1])
    union = area1 + area2 - inter
    
    return inter / union if union > 0 else 0
```

### 完整示例

```python
import cv2
import numpy as np
from nncase import Runtime

def detect_faces(image_path, kmodel_path, conf_thresh=0.5):
    # 读取图像
    image = cv2.imread(image_path)
    orig_h, orig_w = image.shape[:2]
    
    # 预处理
    img_rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    img_resized = cv2.resize(img_rgb, (640, 640))
    img_chw = img_resized.transpose(2, 0, 1)
    input_data = np.expand_dims(img_chw, axis=0).astype(np.uint8)
    
    # 推理
    runtime = Runtime()
    with open(kmodel_path, 'rb') as f:
        runtime.load_model(f.read())
    
    runtime.set_input_tensor(0, input_data)
    runtime.run()
    
    # 获取输出
    outputs = []
    for i in range(9):
        outputs.append(runtime.get_output_tensor(i))
    
    # 解码人脸框
    faces = decode_faces(outputs, (orig_h, orig_w), conf_thresh)
    
    return faces

# 使用示例
faces = detect_faces('test.jpg', 'scrfd_2.5g_bnkps_shape640x640_k230.kmodel')
for face in faces:
    print(f"人脸框: {face['bbox']}, 置信度: {face['score']:.3f}")
    print(f"关键点: {face['kps']}")
```

---

## 关键点说明

模型输出 5 个面部关键点：

| 序号 | 位置 |
|------|------|
| 0 | 左眼 |
| 1 | 右眼 |
| 2 | 鼻尖 |
| 3 | 左嘴角 |
| 4 | 右嘴角 |

---

## 注意事项

1. 输入必须是 RGB 格式，不是 BGR
2. 输入必须是 uint8 类型，范围 [0, 255]
3. 输入尺寸必须为 640×640
4. 输入布局为 NCHW（通道在前）
5. 预处理归一化已内置到模型中，无需手动执行
6. 推荐置信度阈值：0.5
7. 推荐 NMS 阈值：0.4
