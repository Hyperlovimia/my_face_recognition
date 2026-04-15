# 人脸活体检测并入三进程 AI 流程 — 变更文档

## 1. 文档说明

本文档说明 `my_face_recognition` 工程中 **静默活体（Face Anti-Spoof, FAS）** 从「独立测试程序」转为 **与 `face_ai.elf` 主流程一体化** 的变更：包括源码与构建调整、IPC 与 OSD 扩展、启动方式及已删除内容。

本文档属于「变更文档」，描述当前版本行为与使用方式，而非开发流水账。

本文档重点覆盖：

- 变更背景与目标
- 功能与行为概览
- 详细变更项（文件、接口、策略）
- `face_ai` 启动参数与环境变量
- 与 `face_video` / `face_event` 的协作关系（含 **`[SPOOF]`** 考勤留痕）
- 已移除的测试产物与样例资源
- 相关文档索引

## 2. 背景与目标

此前版本（见 [260412 独立验证变更文档](./260412_FACE_ANTISPOOF_FAS_TEST_CHANGELOG.md)）已提供：

- `FaceAntiSpoof` 封装与 `fas_test.elf` 独立上板验证能力；
- PC 侧 ONNX / 校准链路与 `face_antispoof.kmodel` 生成说明。

本次迭代目标为：

1. **删除** 仓库内与「仅用于独立验证」强绑定的测试程序与脚本，减少维护面；
2. **在 `face_ai` 推理循环内** 对与 **识别同源的对齐脸图** 执行与训练一致的预处理与活体推理（非检测框粗裁）；
3. 将活体结果通过 **IPC 应答** 交给 `face_video` 显示；对 **`face_event`**：**活体通过** 上报识别/陌生人事件，**活体未通过** 单独上报 **攻击留痕事件**（串口与考勤日志可区分 `[SPOOF]`）；
4. **注册入库** 仅在活体通过时允许，避免翻拍录入特征。

## 3. 功能变更概览

| 能力 | 说明 |
|------|------|
| 可选活体 | `face_ai` 支持 **8 参数（不启用）** 或 **9 参数（最后一项为活体 kmodel 路径）**；路径无效或加载失败时捕获异常并 **降级为无活体**，进程继续运行。 |
| ROI / 对齐 | 活体输入与 **人脸识别同源**：在 `FaceRecognition::pre_process`（检测五点仿射对齐）之后，从 **`aligned_face_to_bgr`** 导出与识别模型输入一致的 BGR 脸图，再 `feed_bgr_mat` → `forward` → `decode_liveness_scores`，避免「检测框粗裁」与「识别对齐片」不一致。 |
| 分数约定 | 与原文档 /原 `fas_test` 一致：**输出 `[0]=SPOOF`、`[1]=REAL`**；若两维之和接近 1 则视为概率，否则对 logits 做 softmax。 |
| 阈值 | 环境变量 **`FACE_FAS_REAL_THRESH`**（默认 **0.5**）：`REAL` 概率 ≥ 阈值视为 **`is_live=1`**。 |
| 识别与 KPU | **启用活体且未通过时**，**跳过人脸识别推理**（节省 KPU），回复中该脸为陌生人占位；活体通过则照常识别。 |
| 事件与留痕 | **`IPC_CMD_INFER`** 且 **启用活体** 时：**`is_live==0`** 发送 **`IPC_EVT_KIND_LIVENESS_FAIL`**（`score` 为 REAL 概率）；**`is_live==1`** 发送 **`RECOGNIZED`** 或 **`STRANGER`**。便于 `face_event` 写 **`[SPOOF]`** 日志，与 **`[STRANGER]`** / **`[OK]`** 区分。未启用活体时行为与旧版一致（无 `LIVENESS_FAIL`）。 |
| 注册 | **`IPC_CMD_REGISTER_COMMIT`**：需 **恰好一人脸且活体通过** 才执行 `database_add`；否则区分「活体未通过」与「人脸数不对」等日志。 |
| IPC 扩展（AI 应答） | `ipc_face_bundle_t` 增加 **`liveness_real_score`**、**`is_live`**（及对齐填充）。 |
| IPC 扩展（事件通道） | 新增 **`ipc_evt_kind_t`**；**`ipc_evt_t`** 增加 **`evt_kind`**、**`_pad[2]`**，保留 **`is_stranger`**（`STRANGER` 时为 1）。 |
| OSD | **非活体**：红框 + **`SPOOF`** 与 REAL 分数；**活体**：行为与原先白框 + 姓名/unknown 一致。 |
| 构建 | `face_ai.elf` 增加 **`face_antispoof.cc`**；**移除 `fas_test.elf`** 目标；**`build_app.sh`** 不再收集 `fas_test.elf`。 |
| 启动脚本 | **`utils/run_face3.sh`**：若存在 **`FAS_KMODEL`**（默认 `"$BIN_DIR/face_antispoof.kmodel"`）则传入第 9 参数，否则保持 8 参数。 |

## 4. 详细变更说明

### 4.1 `FaceRecognition`（`face_recognition.h` / `face_recognition.cc`）

- 新增 **`aligned_face_to_bgr(cv::Mat &bgr) const`**：在 **`pre_process` 成功** 后，将 **`ai2d_out_tensor_`**（NCHW uint8，R/G/B 平面与整帧 IPC 一致）映射到 CPU 并转为 **BGR**，供活体分支使用；**不改变** 识别模型输入 tensor 内容，后续 **`inference()`** 仍对同一对齐结果推理。

### 4.2 `FaceAntiSpoof`（`face_antispoof.h` / `face_antispoof.cc`）

- 新增 **`feed_bgr_mat(const cv::Mat &bgr)`**：与 `feed_image` 相同预处理链路（BGR→RGB、缩放、反射 padding、写入输入 tensor），供对齐脸图或文件读入使用。
- 新增 **`decode_liveness_scores(float *real_prob, float *spoof_prob) const`**：在 **`forward()`** 且输出为 2 类后解析概率。
- **`feed_image`** 改为读盘后经 **`feed_bgr_mat`** 复用逻辑。

依赖：头文件增加 **`#include <opencv2/core.hpp>`**（`cv::Mat`）。

### 4.3 `face_ai_main.cc`

- 启动时解析 **`argc == 8` 或 `9`**，可选构造 **`std::unique_ptr<FaceAntiSpoof>`**。
- **逐脸流水线**：**`FaceRecognition::pre_process`**（与检测关键点对齐）→（若启用活体）**`aligned_face_to_bgr` + FaceAntiSpoof** → 按 **`is_live`** 决定是否 **`inference` + `database_search`**。识别输入 tensor 仅供本脸 **`try_run`** 使用；活体写入 **另一路 kmodel 输入**，二者 **不共享缓冲区**。
- **`notify_face_event(..., ipc_evt_kind_t)`**：在 **`IPC_CMD_INFER`** 下，若 **启用活体且 `!is_live`**，发送 **`IPC_EVT_KIND_LIVENESS_FAIL`**（`face_id=-1`，**`score`=REAL 概率**）；若 **`is_live`**，发送 **`STRANGER`** 或 **`RECOGNIZED`**。事件去重缓存按 **`(id, evt_kind)`** 区分；**`RECOGNIZED`** 仍按 **姓名** 区分不同注册人。
- 注册分支根据 **`reply.faces[0].is_live`** 与 **`fas`** 是否启用区分成功、活体拒绝、人脸数错误。

### 4.4 `ipc_proto.h`

- **`ipc_face_bundle_t`**（`face_ai` → `face_video` 应答）新增字段：
  - **`float liveness_real_score`**：REAL 概率；未启用活体时为 **1.0**（循环内显式赋值）。
  - **`uint8_t is_live`**：**1** 表示通过或未启用活体；**0** 表示未通过。
  - **`uint8_t _pad[3]`**：结构对齐用填充。
- **`ipc_evt_kind_t`**（`face_ai` → `face_event`）：**`RECOGNIZED`**、**`STRANGER`**、**`LIVENESS_FAIL`**。
- **`ipc_evt_t`** 增加 **`evt_kind`**、**`_pad[2]`**；**`is_stranger`** 在 **`STRANGER`** 时为 1，与旧逻辑兼容。

**注意**：`face_video`、`face_ai`、`face_event` 须 **同源编译、共用此头文件**，避免 IPC 结构体不一致。

### 4.5 `face_event_main.cc`

- 收到 **`IPC_EVT_KIND_LIVENESS_FAIL`**：串口打印 **`[ALERT] spoof / liveness failed (REAL score=...)`**，日志文件写入 **`[SPOOF] real_score=...`**（与 **`[STRANGER]`**、**`[OK]`** 并列，便于考勤留痕与后续 MQTT/HTTP 解析）。
- **`STRANGER` /已识用户**：仍以 **`evt_kind`** 或 **`is_stranger`** 分支处理，行为与扩展前一致。

### 4.6 `ipc_osd_draw.cc`

- 根据 **`f.is_live`** 选择框颜色与文字：**非活体** 显示 **`SPOOF`** 与 **`liveness_real_score`**；活体分支保持原 **unknown / 姓名:分数** 逻辑。

### 4.7 `src/CMakeLists.txt`

- 定义 **`face_ai_src`**：在原有 `face_ai_main.cc`、`ipc_*`、`common_ai_src` 基础上增加 **`face_antispoof.cc`**。
- **`face_ai.elf`** 仅链接上述源码；**删除 `fas_test.elf` 目标**。
- **`install`** 列表中移除 **`fas_test.elf`**。

### 4.8 `build_app.sh`

- **`collect_outputs`** 的 ELF 列表中 **移除 `fas_test.elf`**（**`k230_bin` 内 kmodel 可由手工同步**，脚本不强制拷贝活体 kmodel）。

### 4.9 `utils/run_face3.sh`

- 新增默认 **`FAS_KMODEL="${FAS_KMODEL:-${BIN_DIR}/face_antispoof.kmodel}"`**。
- 若文件存在则启动 **`face_ai.elf` 时追加第 9 参数**；否则打印提示并以 **8 参数** 启动（无活体）。
- 注释中说明 **`FACE_FAS_REAL_THRESH`** 与 **`FAS_KMODEL`**。

## 5. 已删除或迁出仓库的内容

| 项 | 说明 |
|----|------|
| **`src/fas_test.cc`** | 独立 kmodel 验证 `main`，已由 `face_ai` 内嵌流程替代。 |
| **`utils/run_fas_test.sh`** | 面向 WSL 的辅助脚本，与板端 `msh` 无关，已移除。 |
| **`fas_test.elf`** | CMake /安装 / `build_app.sh` 均不再产出或打包。 |
| **`models_src/calib_images/*` 样例图** | 仓库内示例校准图已删除；PTQ 前可 **`mkdir -p models_src/calib_images`** 并放入自备图片，或使用任意 **`--input_dir`**（流程仍见 [260412 文档第 8 节](./260412_FACE_ANTISPOOF_FAS_TEST_CHANGELOG.md)）。 |

## 6. 使用说明

### 6.1 `face_ai.elf` 命令行

```text
face_ai <kmodel_det> <det_thres> <nms_thres> <kmodel_recg> <recg_thres> <db_dir> <debug_mode> [<face_antispoof.kmodel>]
```

- **8 参数**：不加载活体模型，行为与旧版三进程一致（`is_live` 恒为 1，`liveness_real_score` 为 1.0）。
- **9 参数**：最后一项为 **`face_antispoof.kmodel`** 路径；加载失败则打印警告并 **无活体** 继续运行。

### 6.2 环境变量

| 变量 | 含义 | 默认 |
|------|------|------|
| **`FACE_FAS_REAL_THRESH`** | REAL 概率达到该值视为活体通过 | `0.5` |

### 6.3 与 `run_face3.sh` 的配合

- 将生成的 **`face_antispoof.kmodel`** 放到与脚本 **`BIN_DIR`** 一致的部署目录（默认与 ELF 同目录），文件名为默认的 **`face_antispoof.kmodel`**，或通过 **`FAS_KMODEL`** 指定路径。
- 若板上 **暂不部署活体模型**，只要不放置该文件（或指向不存在路径），脚本会自动以 **无活体** 方式启动。

### 6.4 `face_event` 日志与事件类型（启用活体时）

| `evt_kind` | 串口（摘要） | 日志文件行前缀 | `score` 含义 |
|------------|--------------|----------------|--------------|
| **`LIVENESS_FAIL`** | `[ALERT] spoof / liveness failed ...` | **`[SPOOF] real_score=...`** | 静默模型输出的 **REAL 概率** |
| **`STRANGER`** | `[ALERT] stranger ...` | **`[STRANGER] score=...`** | 识别相似度等（与原逻辑一致） |
| **`RECOGNIZED`** | `[EVT] id=... name=...` | **`[OK] id=...`** | 识别相似度等 |

日志路径由启动 **`face_event.elf`** 时的参数指定（如 **`run_face3.sh`** 中的 **`LOG_FILE`**，默认 **`/tmp/attendance.log`**）；将路径配置到 SD 卡即可持久化 **翻拍/攻击留痕**。

## 7. 与既有文档的关系

- **模型转换、预处理对齐、PTQ 与 kmodel 生成**：仍以 [260412 人脸活体 kmodel 独立验证与 WSL 对照变更文档](./260412_FACE_ANTISPOOF_FAS_TEST_CHANGELOG.md) **第 6～8 节** 为主；其中关于 **`fas_test.elf` 上板步骤** 已被 **`face_ai` 一体化流程** 替代，可忽略独立可执行文件的描述。
- 三进程架构与单串口说明：[260407 三进程变更文档](./260407_THREE_PROCESS_CHANGELOG.md)、[260408 单串口适配文档](./260408_RTSMART_SINGLE_UART_CHANGELOG.md)。

## 8. 结论

本次变更在保留 **`FaceAntiSpoof`** 与训练一致预处理的前提下，将 **活体结果写入 `face_video` 应答（OSD）** 与 **`face_event` 事件通道**：**活体未通过** 时单独上报 **`LIVENESS_FAIL`**，在考勤日志中体现为 **`[SPOOF]`**，与陌生人、已识用户事件区分，满足 **攻击留痕** 与后续 **网络推送** 扩展需求；并对 **注册入库** 施加 **活体门控**。同时 **移除独立 `fas_test` 工具链与仓库内校准样例图**，部署与维护路径收敛到 **`face_ai.elf` + 可选第 9 参数**；**`utils/face_antispoof.kmodel`** 可由仓库跟踪并手工同步至 **`k230_bin`** 等与脚本一致的部署目录。
