# GhostFaceNet W1.3 S1 ArcFace kmodel 使用说明

## 模型信息

| 属性 | 值 |
|------|-----|
| 模型名称 | GhostFaceNet W1.3 S1 ArcFace |
| 模型文件 | GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel |
| 文件大小 | 4.64 MB |
| 目标平台 | 嘉楠科技 K230 |
| 量化方式 | PTQ uint8 |

---

## 输入规格

| 参数 | 值 |
|------|-----|
| 形状 | `[1, 112, 112, 3]` |
| 类型 | `uint8` |
| 范围 | `[0, 255]` |
| 布局 | `NHWC` |
| 颜色格式 | RGB |

---

## 输出规格

| 参数 | 值 |
|------|-----|
| 形状 | `[1, 512]` |
| 类型 | `float32` |
| 说明 | 512维人脸特征向量 |

---

## 使用方法

### 预处理

输入为原始 RGB 图像像素值（uint8, 0-255），预处理已由模型内置，无需手动执行归一化。

```python
import cv2
import numpy as np

def preprocess(image):
    """
    Args:
        image: BGR 格式图像
    
    Returns:
        input_data: shape=[1,112,112,3], dtype=uint8
    """
    img_rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    img_resized = cv2.resize(img_rgb, (112, 112))
    input_data = np.expand_dims(img_resized, axis=0)
    return input_data.astype(np.uint8)
```

### 推理

```python
import numpy as np
from nncase import Runtime

runtime = Runtime()
with open('GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel', 'rb') as f:
    runtime.load_model(f.read())

input_data = preprocess(image)
runtime.set_input_tensor(0, input_data)
runtime.run()

output = runtime.get_output_tensor(0)
feature = output.flatten()
```

### 后处理

输出特征需要 L2 归一化后才能用于相似度计算：

```python
feature_normalized = feature / np.linalg.norm(feature)
```

### 相似度计算

```python
def cosine_similarity(feature1, feature2):
    return np.dot(feature1, feature2)

threshold = 0.3
is_same = cosine_similarity(feature1, feature2) > threshold
```

---

## 注意事项

1. 输入必须是 RGB 格式，不是 BGR
2. 输入必须是 uint8 类型，范围 [0, 255]
3. 输入尺寸必须为 112×112
4. 输出特征必须进行 L2 归一化
5. 推荐相似度阈值：0.6
