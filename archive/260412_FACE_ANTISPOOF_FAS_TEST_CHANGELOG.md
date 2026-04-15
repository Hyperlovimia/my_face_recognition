# 人脸活体（静默）kmodel 独立验证与 WSL 对照变更文档

> 260415 说明：本文是历史归档，描述的是“独立 `fas_test.elf` 验证阶段”的方案。当前主线已将活体并入 `face_ai.elf`，`fas_test.elf`、`src/fas_test.cc` 与 `utils/run_fas_test.sh` 已移除，RT-Smart 板端请不要再按本文的 `fas_test` 上板步骤执行。当前启动方式请以 [260414_FACE_ANTISPOOF_INTEGRATION_CHANGELOG.md](./260414_FACE_ANTISPOOF_INTEGRATION_CHANGELOG.md) 和 [README.md](../README.md) 为准；本文第 6～8 节仍可作为模型转换、PTQ 与预处理对齐参考。

## 1. 文档说明

本文档用于说明 `my_face_recognition` 工程中 **人脸活体（Face Anti-Spoof, FAS）** 相关的一次增量变更：在 **不改动三进程门禁主流程** 的前提下，新增 **板端可独立运行的 kmodel 测试程序** `fas_test.elf`，并补充 **WSL/PC 侧 ONNX 参考推理** 与 **预处理对齐** 能力，便于在实际上板前验证 `face_antispoof.kmodel` 的加载、推理与输出合理性。

本文档属于“变更文档”，描述当前版本新增能力与使用方式，而非开发过程流水账。

本文档重点覆盖以下内容：

- 变更背景与目标
- 功能变更概览
- 详细变更项（源码、构建、脚本）
- **给板端同事的测试说明**（RT-Smart 上如何跑 `fas_test.elf`）
- **队友如何本地生成 `face_antispoof.kmodel`**（`.kmodel` 不进仓库时）
- WSL 对照测试方法与注意事项
- 与既有三进程架构的关系
- 相关文件索引

## 2. 背景与目标

工程已具备人脸检测、识别与三进程协作能力。用户将 `best_model_static.onnx` 转换为 **`face_antispoof.kmodel`** 后，需要：

1. **先独立验证** kmodel 能否在 K230 nncase runtime 上正常加载与推理；
2. **预处理** 与 Python 校准/训练链路一致（BGR→RGB、最长边缩放、反射 padding、归一化、NCHW）；
3. **尽量不侵入** 现有 `face_ai` / `face_video` / `face_event` 主流程；
4. 在 **WSL** 上用同一套预处理跑 **ONNX**，与板端输出做 **趋势对照**。

## 3. 功能变更概览

变更完成后，项目新增或明确具备以下能力：

- 新增可执行文件 **`fas_test.elf`**：命令行传入 kmodel 与单张图片，打印模型 I/O 信息、原始输出、REAL/SPOOF 分数及判定结果（具体类别下标需在代码注释中与训练约定对齐）。
- 新增类 **`FaceAntiSpoof`**（继承 **`AIBase`**）：封装与训练对齐的预处理及 `feed_image` / `forward`，便于后续集成到主流程时复用。
- 扩展 **`AIBase`**：增加 **`dump_model_io()`**、**`model_input_is_float32()`** / **`model_input_is_uint8()`**，便于独立测试与调试时查看 dtype/shape。
- **（可选）** WSL 侧 ONNX 对照：预处理逻辑与 **`models_src/prepare_calib_npy.py`** / **`src/face_antispoof.cc`** 一致即可；**本仓库默认通过 `.gitignore` 忽略 `host_test/`**，不强制随仓库分发脚本目录。
- **`build_app.sh`** 在收集产物时纳入 **`fas_test.elf`**，便于同步到 `k230_bin/` 部署。

**未包含在本次变更中的内容**（留待后续迭代）：

- 将活体结果接入三进程 IPC、门禁开门策略或 UI；
- 在 `face_ai.elf` 内联调用 `FaceAntiSpoof`（当前未编入 `common_ai_src`）。

## 4. 详细变更说明

### 4.1 新增 `fas_test.elf`（板端独立测试入口）

**新增文件：**

- `src/fas_test.cc`

**行为摘要：**

- 命令行：`<kmodel_path> <image_path>`。
- 构造 **`FaceAntiSpoof`**，打印模型加载成功、缓存的输入/输出 shape、**`dump_model_io()`** 详情。
- 调用 **`feed_image`** 写入输入，**`forward()`**（内部为 **`try_run` + `try_get_output`**）。
- 解析首个输出为二维 logits/概率：若数值之和接近 1 则按概率解释；否则对两维做 **softmax** 后再打印 **Real / Spoof** 分数与 **REAL/SPOOF** 字符串结果。
- **类别顺序**：默认约定 `output[0]` 为 SPOOF、`output[1]` 为 REAL（与训练导出顺序不一致时需在 `fas_test.cc` 中调整常量并保留注释说明）。

**与三进程关系：**

- 不依赖 MPP、不占用 `face_ai` / IPC；可与三进程 **并行或单独** 在板端运行，用于模型验证阶段。

### 4.2 新增 `FaceAntiSpoof`（预处理 + 推理封装）

**新增文件：**

- `src/face_antispoof.h`
- `src/face_antispoof.cc`

**预处理逻辑（与 `face_antispoof.cc` 注释及 Python 校准一致）：**

1. 读图（BGR）→ 转 RGB；
2. 按 **最长边** 等比缩放到 **`max(模型 H, 模型 W)`**（常见为128）；
3. 使用 **`cv::BORDER_REFLECT101`**（对应 Python **`cv2.BORDER_REFLECT_101`**）居中 pad 到模型输入高宽；
4. **`float32`**，除以 **255.0**；
5. **HWC → CHW**，批维 **`[1,3,H,W]`**；
6. 通过 **`host_runtime_tensor`** 映射写入输入，**`hrt::sync(..., sync_write_back)`**。

**输入 dtype 兼容：**

- 若 kmodel 输入为 **float32**，按上述浮点链路写入；
- 若为 **uint8**，写入 **0～255** 的 NCHW（与 float 校准链路不同，仅作部分量化部署兼容）。

**公开访问器：**

- **`print_cached_shapes()`**、**`last_output_ptrs()`**、**`cached_output_shapes()`** 供测试程序读取输出，避免在 `main` 中访问 `protected` 成员。

### 4.3 扩展 `AIBase`

**修改文件：**

- `src/ai_base.h`
- `src/ai_base.cc`

**新增接口：**

- **`void dump_model_io()`**：打印输入/输出路数、每路 **dtype** 与 **shape**。
- **`bool model_input_is_float32(size_t i = 0) const`**
- **`bool model_input_is_uint8(size_t i = 0) const`**

**说明：** 不改变原有构造、推理与输出映射语义；其他目标（`face_ai.elf`、`face_recognition.elf` 等）仅多链接上述符号，**行为与变更前一致**。

### 4.4 构建系统

**修改文件：**

- `src/CMakeLists.txt`

**变更要点：**

- 新增目标 **`fas_test.elf`**：源文件为 **`fas_test.cc`**、**`face_antispoof.cc`**、**`ai_base.cc`**、**`ai_utils.cc`**；链接 **`nncase`**与 **OpenCV**，**不链接 MPP**。
- **`install`** 目标中增加 **`fas_test.elf`**。

**修改文件：**

- `build_app.sh`

**变更要点：**

- 收集 ELF 列表增加 **`${BUILD_DIR}/bin/fas_test.elf`**，随其他产物一并拷贝到 **`k230_bin/`**。

### 4.5 `utils/run_fas_test.sh`（可选，仅开发机）

**文件：**

- `utils/run_fas_test.sh`

**说明：**

- 脚本使用 **`$(...)`**、**`${VAR:-...}`** 等 POSIX shell 语法，**适用于 WSL/Linux**。
- 根据 `archive/260408_RTSMART_SINGLE_UART_CHANGELOG.md`，RT-Smart **`msh` 不支持**此类脚本；**板端请直接执行 `fas_test.elf`**。脚本头部注释已明确说明。

### 4.6 WSL 侧 ONNX 对照（可选，`host_test/` 默认不纳入仓库）

若开发者本地保留 **`host_test/`** 目录（已被根目录 **`.gitignore`** 忽略），可在其中放置与板端预处理一致的 **ONNXRuntime** 小脚本，便于打印 **`Raw output`**；**克隆仓库的队友不一定拥有该目录**。无独立脚本时，按 **第 6 节** 用 **`prepare_calib_npy.py`** 的预处理逻辑自行写几行推理即可。

## 5. 给板端同事的测试说明（RT-Smart / K230）

本节供 **负责板子部署与现场验证** 的同事按步骤操作；本测试 **独立于** 人脸识别三进程（`face_ai` / `face_video` / `face_event`），**不必**先拉起三进程即可验证活体 kmodel。

### 5.1 测试在验证什么

- **`fas_test.elf`** 能否在板端 **加载** `face_antispoof.kmodel`；
- 读入一张本地图片后，**预处理 + KPU/nncase 推理** 是否跑通；
- 串口能否看到 **输入/输出 dtype与 shape**、**Raw output**、以及程序给出的 **REAL/SPOOF** 判定（用于与算法同事在 WSL 上 ONNX 结果 **对照趋势**，不要求数值完全一致）。

### 5.2 上板前请准备以下文件

| 文件 | 说明 |
|------|------|
| **`fas_test.elf`** | 由算法/集成侧交叉编译产出，可从 `k230_bin/` 同步；需与当前板子 RT-Smart/SDK 匹配。 |
| **`face_antispoof.kmodel`** | 本次要验证的活体模型（与 `fas_test` 为同一套转换链）。**若仓库未包含该文件**，由算法/集成同事按 **本文第 8 节** 在 PC 上生成后拷贝到板端。 |
| **测试图** | 支持常见格式（如 `.jpg` / `.png`），建议至少一张 **真人脸**、一张 **翻拍/攻击样张**，便于肉眼看判别是否合理。路径与文件名以现场为准。 |

将上述文件拷到板子可读写路径（示例统一写 **`/data/`**，实际可按项目规范调整）。

### 5.3 在 `msh` 里的操作步骤

1. **确认文件已到位**（路径替换为你的实际路径）：

   ```text
   msh /> ls /data/fas_test.elf
   msh /> ls /data/face_antispoof.kmodel
   msh /> ls /data/test.png
   ```

2. **可执行权限**（若需要）：

   ```text
   msh /> chmod +x /data/fas_test.elf
   ```

3. **执行测试**（**两个参数**：先 kmodel，后图片）：

   ```text
   msh /> /data/fas_test.elf /data/face_antispoof.kmodel /data/test.png
   ```

4. **请勿在板端执行**仓库里的 **`utils/run_fas_test.sh`**。该脚本面向 WSL/Linux；RT-Smart **`msh` 不支持**其中用到的 shell 语法，容易失败。板端 **始终直接调用 `fas_test.elf`**。

### 5.4 正常时串口应看到什么（摘要）

- **`Model loaded: ok`**（若 kmodel 无法加载，可能直接异常退出或报错，需检查路径与文件完整性）。
- **`Input shape` / `Output shape`** 与 **`dump_model_io`** 中每路 **dtype、shape**（常见输入为 `float32` 或量化后 `uint8`，输出常见为 **`[1, 2]`**）。
- **`Raw output: [...]`** 两维浮点；随后 **Real score / Spoof score** 与 **`Result: REAL` 或 `SPOOF`**。

若 **真人图与翻拍图** 的 **Result 长期相同或明显反直觉**，请记录 **Raw output** 与 **测试图**，反馈算法侧核对 **训练时类别顺序** 是否与 `fas_test.cc` 中默认约定（`[0]=SPOOF`、`[1]=REAL`）一致。

### 5.5 异常时建议排查项

- **找不到文件 / 打开失败**：检查路径、大小写、存储挂载；`ls` 确认。
- **一加载就失败 / Invalid kmodel**：kmodel 是否与当前芯片/SDK 版本匹配、文件是否损坏、是否拿错文件。
- **`feed_image failed`**：图片路径错误或格式无法被 OpenCV 解码。
- **`inference failed`**：nncase 运行或读输出失败，需结合完整串口日志与 SDK 版本排查。

### 5.6 与三进程人脸识别程序的关系

- **本测试不要求** `face_ai.elf`、`face_video.elf`、`face_event.elf` 已启动；专注验证 **活体单模型**。
- 若现场同时跑着三进程，**一般也可再跑** `fas_test.elf` 做对比，但需注意 **KPU/内存** 占用；如有资源冲突，可先停掉其它占 KPU 的任务再测。

## 6. WSL 浮点 ONNX 对照（可选）

**背景：** **`fas_test.elf`** 为 **RISC-V** 静态链接，**不能在 WSL x86 上直接执行**。若需在 PC 上对照 **浮点 ONNX** 与板端 **kmodel** 的趋势，可在 WSL 安装 **ONNXRuntime** 与 OpenCV 后自行跑推理。

**与仓库策略的关系：** 根目录 **`.gitignore`** 默认包含 **`/host_test/`**，**克隆仓库不一定带有** 任何 `host_test` 脚本；下列方式任选其一即可。

1. **推荐（与仓库脚本一致）：** 在 Python 中 **直接调用** **`models_src/prepare_calib_npy.py`** 里的 **`preprocess_image()`**（或复制其逻辑），得到 **`(1,3,128,128)` `float32`** 张量后，用 **`onnxruntime.InferenceSession`** 对 **`models_src/fas/best_model_static.onnx`** 做 `run`，打印输出并与板端 **`Raw output`** 对照趋势（不必逐位相同）。
2. **若个人本地仍有 `host_test/`：** 可使用其中已对齐预处理的参考脚本（**不随 Git 分发**），团队内自行拷贝即可。

**依赖示例：**

```bash
python3 -m pip install onnxruntime opencv-python-headless numpy
```

**说明：** **`models_src/prep_debug.npy`** 等本地导出文件已被 **`.gitignore`** 忽略，勿提交。

## 7. 验证建议

- **链路正确性：** 板端能加载 kmodel、shape/dtype 与预期一致、无崩溃。
- **与 ONNX 一致性：** 同图在 WSL 与板端 **类别倾向一致**（注意 PTQ 带来的数值偏差）。
- **业务正确性：** 使用真人/翻拍等多张图观察判别是否符合训练标签；若整体反类，调整 **`fas_test.cc`** 中 REAL/SPOOF 下标约定。

## 8. 队友如何本地生成 `face_antispoof.kmodel`

本节面向 **需要在 PC 上从 ONNX 产出 kmodel** 的同事（集成/算法）。当前仓库通过 **`.gitignore`** 忽略 **`utils/face_antispoof.kmodel`**、**`models_src/prep_debug.npy`**、**`models_src/calib_npy/`**、**`models_src/nncase_dump/`** 等生成物，**克隆仓库后不会自带 kmodel**，须按下列步骤在本地生成，再将 **`face_antispoof.kmodel`** 发给板端同事或拷贝到板子（如 `/data/face_antispoof.kmodel`）。

### 8.1 环境与依赖

- **操作系统：** 建议 Linux / WSL（与 Kendryte/nncase 官方文档一致）。
- **Python：** 3.x（与已安装的 `nncase` wheel 版本匹配）。
- **Python 包（示例）：**
  - **`nncase`**：需支持 **`target = k230`** 的编译与 PTQ（版本以 SDK/文档为准）。
  - **`onnx`**：用于 `make_static_onnx.py`。
  - **`numpy`**、**`opencv-python`**（或 headless）：用于 `prepare_calib_npy.py`。

具体安装命令以你们当前 K230 工具链文档为准；若使用虚拟环境，可在仓库根目录下创建 **`/.venv/`**（该目录已被忽略）。

### 8.2 输入约定（与板端 `fas_test` / 校准一致）

- **ONNX 输入：** 单路 **`float32`**，形状 **`[1, 3, 128, 128]`**，NCHW，数值范围与训练一致（本链路校准张量为 **`[0, 1]`**）。
- **校准图：** 使用与业务分布相近的人脸图（仓库中可有 **`models_src/calib_images/`** 作为示例目录，也可替换为你们自己的目录）。

### 8.3 三步生成流程（在 `my_face_recognition` 根目录执行）

以下路径可按实际文件位置修改；**`--input` 的 ONNX** 为训练导出的浮点模型（示例名 **`best_model.onnx`**，需自行准备或从内部制品库获取）。

**步骤 A — 导出静态 ONNX（固定 batch 维等）**

```bash
python3 models_src/make_static_onnx.py \
  --input models_src/fas/best_model.onnx \
  --output models_src/fas/best_model_static.onnx
```

**步骤 B — 由图片生成 PTQ 校准用 `.npy`**

脚本预处理与板端 **`face_antispoof.cc`** 一致：BGR→RGB、最长边缩放到 128、**`BORDER_REFLECT_101`** 居中 pad到 **128×128**、除以255、**HWC→CHW**、形状 **`(1,3,128,128)`** `float32`。

```bash
mkdir -p models_src/calib_npy
python3 models_src/prepare_calib_npy.py \
  --input_dir models_src/calib_images \
  --output_dir models_src/calib_npy \
  --limit 32
```

**步骤 C — nncase 编译生成 kmodel**

```bash
python3 models_src/build_fas_kmodel.py \
  --model models_src/fas/best_model_static.onnx \
  --dataset models_src/calib_npy \
  --output utils/face_antispoof.kmodel \
  --samples 32 \
  --dump_dir models_src/nncase_dump
```

- **`--samples`** 不超过 `calib_npy` 内 `.npy` 文件个数。
- **`--dump_dir`** 会生成体积较大的中间文件，目录默认已被 **`.gitignore`** 忽略，可定期手动删除。

成功后终端打印 **`[OK] saved: ...`**，得到 **`utils/face_antispoof.kmodel`**（或你 `--output` 指定的路径）。将该文件交给 **板端同事** 做 **第 5 节** 的 `fas_test.elf` 验证。

### 8.4 可选自检（浮点 ONNX，不经过 nncase）

在已安装 **`onnxruntime`** 的机器上，可按 **第 6 节** 用与 **`prepare_calib_npy.py`** 一致的预处理对 **`best_model_static.onnx`** 推理一遍，确认 **`Raw output`** 合理，再执行步骤 C。此步 **可跳过**，直接上板用 **`fas_test.elf`** 验证亦可。

### 8.5 常见问题

- **`No .npy files found`**：先完成步骤 B，并确认 **`--dataset`** 指向的目录内存在 **`*.npy`**。
- **nncase 报错 / 目标不匹配**：检查 **`nncase` 版本** 与 **`compile_options.target = "k230"`** 是否与当前芯片/SDK 一致。
- **板端加载失败**：确认 kmodel 与板子 **nncase runtime** 为同一套工具链产出；路径、文件完整性可在板端用 **`ls`** 核对。

## 9. 相关文档

- [三进程优化功能变更文档](./260407_THREE_PROCESS_CHANGELOG.md)
- [RT-Smart 单串口适配功能变更文档](./260408_RTSMART_SINGLE_UART_CHANGELOG.md)
- 本文档：[人脸活体 kmodel 独立验证与 WSL 对照变更文档](./260412_FACE_ANTISPOOF_FAS_TEST_CHANGELOG.md)

## 10. 结论

本次变更在 **不改变三进程主业务路径** 的前提下，为 **`face_antispoof.kmodel`** 提供了 **可独立上板验证** 的工具链，并配套 **WSL 侧 ONNX 与预处理对齐** 流程，降低部署与调试成本。后续若要将活体接入门禁或 `face_ai`，可在现有 **`FaceAntiSpoof`** 与 **`AIBase`** 扩展基础上增量集成。
