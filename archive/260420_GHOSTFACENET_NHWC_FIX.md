# GhostFaceNet NHWC 输入兼容修复记录

本文档记录 `my_face_recognition` 工程在接入新识别模型 `GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel` 过程中遇到的问题、定位过程、根因分析、修复内容与最终验证结果。

## 1. 背景

项目路径：

```text
/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition
```

原有人脸识别模型为：

```text
face_recognition.kmodel
```

新替换测试模型为：

```text
GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel
```

原模型可正常运行：

```sh
./face_recognition.elf face_detection_320.kmodel 0.6 0.2 face_recognition.kmodel 75 face_db 0
```

替换为新模型后，最初运行命令为：

```sh
./face_recognition.elf face_detection_320.kmodel 0.6 0.2 GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 75 face_db 0
```

程序在首轮识别阶段报：

```text
Memory exhaustion!
```

## 2. 最初现象

### 2.1 板端日志特征

最初日志中可以看到：

- 检测模型已经正常加载
- 新识别模型也能正常加载
- 首帧检测前处理、检测推理、检测后处理都能成功完成
- 一旦进入识别链路，就触发 `Memory exhaustion!`

这说明问题并不是：

- kmodel 文件损坏
- 识别模型无法被 nncase runtime 加载
- 摄像头/VICAP/VO 初始化失败

### 2.2 为什么最开始看起来像“内存耗尽”

`Memory exhaustion!` 这句并不是业务代码打印的，而是 RT-Smart 内核在用户态内存映射失败时打印的，位置在：

- [lwp_user_mm.c](/home/hyperlovimia/k230_sdk/src/big/rt-smart/kernel/rt-thread/components/lwp/lwp_user_mm.c:123)

对应逻辑是 `rt_hw_mmu_map_auto(...)` 返回失败后直接报错退出。

因此它表示的是：

- 某次底层映射/分配失败了

它**不等于**：

- 2GB DDR 被真正全部耗尽

换句话说，最初“像是内存问题”，但并不能直接得出“模型太大、板子内存不够”的结论。

## 3. 进一步定位

为了判断问题发生在识别链路的哪一步，给工程加了更细的诊断日志，主要包括：

- `FaceRecognition` 构造阶段打印模型原始输入 shape 与当前代码解释方式
- 检测后打印检测到的人脸数
- 进入每一张脸的 `face_recg.pre_process()` 前后打点
- 在 `pre_process()` 内打印关键点、仿射矩阵、ai2d builder 创建与 invoke 状态
- 在 `face_recg.inference()` 前后打点

涉及文件：

- [main.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/src/main.cc:278)
- [face_recognition.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/src/face_recognition.cc:146)

### 3.1 诊断日志暴露出的关键信息

调试日志明确显示新模型输入为：

```text
input 0 : u8,1,112,112,3,
```

即原始 shape 为：

```text
[1,112,112,3]
```

而旧的人脸识别代码一直默认输入是 NCHW，构造函数中按：

```text
C = shape[1], H = shape[2], W = shape[3]
```

去解释输入。

因此旧逻辑会把新模型错误地解释成：

```text
C=112, H=112, W=3
```

而不是正确的：

```text
C=3, H=112, W=112
```

### 3.2 精确崩溃点

后续日志进一步证明，程序不是在 `FaceRecognition run()` 阶段崩，而是在识别前处理里构造 affine ai2d builder 时崩：

```text
[recg_pre] affine matrix=[...]
Memory exhaustion!
```

而且连：

```text
[recg_pre] ai2d builder ready...
```

都没来得及打印。

这说明崩溃发生在：

- `Utils::affine_set(...)`
- 更具体地说，是为识别模型构建 ai2d builder 的过程中

## 4. 根因

根因不是“GhostFaceNet 文件略大，板子 2GB 内存不够”。

真实根因是：

### 4.1 新模型输入布局与旧代码假设不一致

新模型 `GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel` 的输入布局为：

- `NHWC`
- 即 `u8 [1,112,112,3]`

而原工程的识别前处理路径默认是：

- `NCHW`
- 即按 `RGB planar` 输出给识别模型

### 4.2 旧 ai2d 前处理路径被错误复用

原来的 `Utils::affine_set(...)` 固定按：

- `NCHW_FMT -> NCHW_FMT`

去构建 ai2d builder。

对应代码在：

- [ai_utils.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/src/ai_utils.cc:213)

这条路径对旧模型成立，但对新 NHWC 模型不成立。结果是在给识别模型构建 affine ai2d 时走进错误布局，最终在底层内存映射/调度阶段失败，表现成 RT-Smart 的：

```text
Memory exhaustion!
```

## 5. 修复方案

本次修复不是去“加内存”，而是把识别链路补成同时兼容：

- 旧 `NCHW` 识别模型
- 新 `NHWC` 识别模型

### 5.1 在 `FaceRecognition` 中识别输入布局

文件：

- [face_recognition.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/src/face_recognition.cc:126)
- [face_recognition.h](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/src/face_recognition.h:188)

新增逻辑：

- 构造函数读取模型原始输入 shape
- 若 shape 形如 `[1,H,W,3]`，则认定当前模型输入为 `NHWC`
- 对于 NHWC 模型，重新计算逻辑输入尺寸为：
  - `C=3`
  - `H=shape[1]`
  - `W=shape[2]`

而不再沿用旧的 `shape[1], shape[2], shape[3] -> C,H,W` 解释方式。

### 5.2 扩展 `Utils::affine_set(...)` 支持 NHWC 输出

文件：

- [ai_utils.h](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/src/ai_utils.h:254)
- [ai_utils.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/src/ai_utils.cc:213)

改动点：

- 为 `affine_set(...)` 增加 `output_nhwc` 参数
- 旧路径保持：
  - `NCHW_FMT -> NCHW_FMT`
- 新路径切换为：
  - `NCHW_FMT -> RGB_packed`

并根据输出布局分别构造：

- `NCHW` 输出 shape：`{1, C, H, W}`
- `NHWC` 输出 shape：`{1, H, W, C}`

这样识别前处理就能直接生成符合 GhostFaceNet 输入要求的 `NHWC/RGB_packed` 数据。

### 5.3 补齐 `aligned_face_to_bgr()` 对 NHWC 的兼容

文件：

- [face_recognition.cc](/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/src/face_recognition.cc:207)

原因：

工程里识别链路后续有“从识别输入 tensor 反解出 BGR 图像”的能力，供活体等模块复用。

旧实现只支持：

- `NCHW/RGB planar`

本次同步补成同时支持：

- `NCHW/RGB planar`
- `NHWC/RGB packed`

避免后续三进程场景下再踩同一类输入布局问题。

### 5.4 增加定位日志

为了后续若再接入不同模型能更快判断问题，本次保留了较有价值的诊断日志，包括：

- 识别模型原始输入 shape
- 是否疑似 NHWC
- 当前前处理输出布局
- 每张脸进入识别前处理/推理/检索的阶段日志

## 6. 修复后验证

修复后重新上板运行同样命令：

```sh
./face_recognition.elf face_detection_320.kmodel 0.6 0.2 GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 75 face_db 2
```

日志显示：

```text
[recg_ctor] raw input shape=[1,112,112,3], code assumes NCHW and interprets input_size as C=3 H=112 W=112
[recg_ctor] WARNING: model input looks NHWC; if so, expected logical CHW would be C=3 H=112 W=112, runtime will switch to RGB_packed/NHWC affine output for this model.
...
[recg_pre] ai2d builder ready, invoking affine warp now (output_layout=NHWC/RGB_packed)
[recg_pre] ai2d invoke result=ok
FaceRecognition pre_process took 33.0446 ms
[stage] face[0] face_recg.pre_process OK, entering inference
FaceRecognition run took 203.967 ms
FaceRecognition get_output took 0.005629 ms
[stage] face[0] face_recg.inference OK, entering database_search
FaceRecognition database_search took 0.011518 ms
[stage] face[0] database_search OK, name=unknown score=0
```

并且第二帧继续稳定运行：

```text
FaceRecognition pre_process took 49.9222 ms
FaceRecognition run took 201.445 ms
...
[stage] face[0] database_search OK, name=unknown score=0
```

### 6.1 结论

修复后已经确认：

- 新模型能够在板端正常加载
- 新模型前处理能够正确生成 `NHWC` 输入
- 新模型推理能够正常完成
- 能正常输出 `512` 维特征并进入数据库检索
- 原先的 `Memory exhaustion!` 已消失

因此本次问题已经解决。

## 7. 对 `unknown score=0` 的说明

修复后日志中出现：

```text
name=unknown score=0
```

这是当前数据库为空导致的正常结果，不代表识别链路失败。

日志中同时有：

```text
found 0 pieces of data in db
```

即此时 `face_db` 中尚无人脸特征，因此识别结果只能是 `unknown`。

## 8. 影响与后续建议

### 8.1 本次修复解决的问题

- GhostFaceNet `NHWC` 输入与旧工程 `NCHW` 假设不兼容导致的识别链路崩溃
- 误导性的 `Memory exhaustion!` 症状

### 8.2 当前可确认的代价

新模型虽然已经能跑，但推理耗时明显高于旧模型：

- `FaceRecognition run` 约 `201~204 ms`
- 单帧总耗时约 `358~389 ms`

因此帧率会明显低于旧识别模型。

### 8.3 后续建议

1. 注册至少一张人脸到 `face_db`，验证新模型的特征匹配是否符合预期。
2. 重新评估识别阈值 `75` 是否仍适用于新模型。不同 embedding 模型的相似度分布往往不同，旧阈值不一定可直接复用。
3. 后续若再接入新模型，优先先确认：
   - 输入 dtype
   - 输入 shape
   - 输入布局是 `NCHW` 还是 `NHWC`
   - ai2d 输出布局是否匹配模型要求

## 9. 总结

本次任务表面上看是“更换模型后报内存耗尽”，但实际并不是板载总内存不足，而是：

- 新识别模型输入为 `NHWC`
- 旧工程仍按 `NCHW` 构建识别前处理
- 识别前处理在构建 ai2d builder 时走入错误布局路径
- 底层最终以 `Memory exhaustion!` 的形式表现出来

通过补齐 `FaceRecognition` 对 `NHWC` 输入的识别、扩展 `Utils::affine_set(...)` 支持 `RGB_packed/NHWC` 输出，并同步兼容后续 `aligned_face_to_bgr()` 读取逻辑，最终使 `GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel` 能在当前 `my_face_recognition` 工程中稳定运行。
