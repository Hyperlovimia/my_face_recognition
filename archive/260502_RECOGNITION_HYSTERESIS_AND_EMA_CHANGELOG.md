# 识别阈值滞回与特征 EMA 调优 — 变更文档

日期：2026-05-02

## 1. 文档说明

本文档说明 `my_face_recognition` 工程中针对 **`face_ai.elf` 流媒体识别** 的两类现象所做的逻辑优化：

1. **同一人相似度在时间上缓慢走低**（与对齐抖动、光照及识别特征短时 EMA 等相关）；
2. **已注册人员在相似度略低于/略高于识别阈值附近频繁交替出现 `recognized` 与 `stranger(unknown)`**，以及日志中与 **`liveness_fail` 事件穿插**的观感问题。

本文档属于「变更文档」：描述行为、文件与可调参数，便于上板验收与后续维护。

本文档重点覆盖：

- 问题背景与优化目标
- 功能与策略概览（表格）
- 详细变更项（`face_recognition` / `face_ai_main`）
- 环境变量与调参建议
- 与其它模块（IPC、`face_event`、活体）的边界说明
- 相关文档索引

## 2. 背景与目标

### 2.1 现象归纳

- **分数波动**：单帧余弦相似度随姿态、光照、模糊与对齐在 **识别阈值**（命令行第 6 参数，如 `60`/`70`）附近摆动时，`database_search` 使用 **严格大于阈值**（`v_score_top1 > obj_thresh_`）判定通过，容易在边界上出现 **一帧通过、下一帧陌生人**。
- **EMA 与观感**：`face_ai` 对 **单人单帧** 特征有短时 EMA，再与库检索；α 偏大时，多帧偏差的特征向量会被持续平滑，**向劣质对齐方向漂移**，表现为相似度 **缓慢下降**。
- **多事件交错**：启用活体时，`LIVENESS_FAIL` 与识别事件分路发送属 **设计行为**；识别侧阈值抖动会进一步放大「既像认出又像陌生人」的日志观感。

### 2.2 目标

1. 在 **不放宽** `FaceRecognition::database_search` 主阈值公式的前提下，对流式结果增加 **少量滞回（hysteresis）**，抑制阈值附近的 **无意义翻转**；
2. **库内两人且歧义（ambiguous）** 时 **不** 做滞回提拔，保留原有两可判决安全性；
3. **多人同帧** 时 **禁用** 滞回并清空状态，降低槽位错配导致的 **串脸** 风险；
4. **略降** 识别特征 EMA 的混合强度，减轻长期 **被 EMA 拖偏** 的程度，同时保留抗单帧噪声能力。

## 3. 功能变更概览

| 项目 | 说明 |
|------|------|
| 识别滞回 | **仅** `IPC_CMD_INFER`、**单帧检测人数为 1**、且 **`ambiguous_match == false`** 时生效：若本帧检索结果为陌生人，但 **top1 仍为此前已确认的同一注册 ID**，且 **score ≥ 识别阈值 − 滞回带宽**，则 **仍输出为该人**；score 仍为当前帧与 top1 模板相似度（不改变 `database_search` 原始分）。 |
| 滞回带宽 | 默认 **5**（与现有 `score` 0～100 标尺一致）；环境变量 **`FACE_REC_HYST`** 覆盖，代码内限制约 **2～12**。 |
| 多人同帧 | 当 **`det_results.size() > 1`** 时，**清空所有槽位**滞回状态；逐脸推理仍按原逻辑（多人时本就不使用槽位特征 EMA）。 |
| 歧义匹配 | `database_search` 对库内 ≥2 人且 top1/top2 过近且未触发「强 top1」豁免时标记 **`ambiguous_match`**；**清除** 该槽滞回，**不允许** 滞回提拔为已识用户。 |
| 弱检测 | 原 **极低置信检测**（`det_weak`）分支跳过识别；现 **同步清除** 该人脸槽位的滞回状态。 |
| 特征 EMA | `k_rec_feat_ema_alpha` **0.4 → 0.34**，`k_rec_feat_ema_alpha_cap` **0.58 → 0.48**（仅影响单人单帧 EMA 路径）。 |
| IPC / 事件 | **未改动** `ipc_proto.h`；`face_event` 仍依据 `face_ai` 送来的 **最终** `rec.id` / `evt_kind` 打印 **`RECOGNIZED` / `STRANGER` / 活体失败**。 |
| 活体 | **未改动** REAL 阈值与活体 EMA；活体未通过仍可能 **跳过识别**，与本次识别滞回相互独立。 |

## 4. 详细变更说明

### 4.1 `FaceRecognitionInfo` 与 `database_search`（`face_recognition.h` / `face_recognition.cc`）

- **`FaceRecognitionInfo` 扩展字段**：
  - **`top1_id`**：库内相似度 **最高者下标**；**无库** 或 **未进入检索** 时为 **-1**。**陌生人** 分支仍保留 **top1 相似度** 于 **`score`**，并写入 **`top1_id`**，供滞回判断「仍是同一人模板」。
  - **`ambiguous_match`**：由原先「两可」逻辑赋值；为 **true** 时表示 **不宜** 做滞回提拔。
- **`database_search`**：在分支判定前统一写入 **`result.top1_id`**、**`result.ambiguous_match`**；**通过** 与 **未通过** 分支均保证 **`top1_id`** 与 **`ambiguous`** 语义一致。
- **新增 API**（供 `face_ai` 使用）：
  - **`float recognition_threshold() const`**：即构造参数识别阈值。
  - **`int registered_face_count() const`**：当前注册人数。
  - **`string registered_name_at(int idx) const`**：按下标取注册姓名（越界返回空串）；滞回提拔时用 **库内权威姓名**。

### 4.2 `face_ai_main.cc`

- **静态状态（按检测槽位）**：
  - **`g_rec_sticky_id`**、**`g_rec_sticky_name`**、**`g_rec_sticky_valid`**：记录**上一明确确认**的身份，用于滞回。
- **`rec_hysteresis_band_from_env()`**：读取 **`FACE_REC_HYST`** 并夹紧到 **[2, 12]**。
- **`apply_recognition_hysteresis(slot, nfaces, face_recg, recg_result)`**：
  - **`nfaces != 1`**：直接返回；
  - **`ambiguous_match`**：清该槽滞回并返回；
  - **已识别**：更新 sticky；
  - **陌生人**：若 sticky 有效、**`top1_id` 与 sticky id 一致** 且 **`score ≥ threshold − band`**，则 **提拔** 为已识用户并 **`registered_name_at`** 填名；若 **低于 relax** 或与 top1 **不一致**，按规则 **清 sticky**。
- **调用位置**：在 **`IPC_CMD_INFER`** 分支内完成 **`database_search`** 之后、组装 `reply` / 事件 **之前** 调用，保证 OSD、IPC 与 **`notify_face_event`** 使用 **同一最终识别结果**。
- **帧级清理**：
  - **`sort_det_results_by_quality` 之后**：若本帧 **多于一人**，**整表清空** `g_rec_sticky_valid`；
  - **原 `j >= det_results.size()` 槽位复位循环**：同时对 **`g_rec_sticky_valid[j] = 0`**；
  - **`det_weak`**：对该 **`i`** **`g_rec_sticky_valid[i] = 0`**。
- **启动日志**：在打印 **`db_top2_margin`** 之后增加一行 **`recognition hysteresis band`**（含 **`FACE_REC_HYST`** 说明）。

### 4.3 未修改文件（边界说明）

| 组件 | 说明 |
|------|------|
| `ipc_proto.h` | 事件与应答结构未变。 |
| `face_event_main.cc` / `linux_bridge` | 仅消费 `face_ai` 最终结果，无需随本次改动重编即可兼容 **逻辑层** 行为；**建议** 与 `face_ai` **同源版本** 部署。 |
| `FaceRecognition::database_search` 核心阈值与 margin | **未改** `obj_thresh_`、`db_top2_margin_`、两可判决公式；滞回为 **后处理**。 |

## 5. 环境变量与调参

| 变量 | 默认 | 说明 |
|------|------|------|
| **`FACE_REC_HYST`** | **5** | 滞回带宽（与 `score` 同标尺）。略 **增大** → 阈值附近更「黏」已识用户，但陌生人 **更接近**阈值时也可能被多挂一会儿已识 ID；略 **减小** → 更敏感、更接近原始硬阈值。有效范围在代码中 **约 2～12**。 |

**注意**：RT-Smart `msh` 往往 **无法** `export` 环境变量；若板端需改带宽，通常需 **改默认值重编** 或后续扩展为 **命令行参数**。

## 6. 使用与验收建议

1. **单用户库、正对镜头、光照稳定**：观察 **recognized** 是否仍因 **±1～2 分** 抖动在陌生人之间闪烁；若仍有，可略 **提高** **`FACE_REC_HYST`**（或在 PC/壳层能传 env 时试 **6～8**）。
2. **多用户库、同框易混**：依赖原有 **ambiguous**；若日志出现 **`ambiguous_match` 调试输出**（`debug_mode > 1`），应 **不会** 被滞回「强行认出」。
3. **活体频繁失败**：优先调 **`FACE_FAS_REAL_THRESH` / 命令行 REAL 阈值** 与现场光路；本次改动 **不替代** 活体门控。
4. 重新编译 **`face_ai.elf`** 后，与 `face_video` / `face_event` **共用同一套 `ipc_proto.h` 的版本** 一并部署。

## 7. 与既有文档的关系

- 活体与事件类型（**`LIVENESS_FAIL`** / **`STRANGER`** / **`RECOGNIZED`**）：见 [260414 人脸活体并入三进程变更文档](./260414_FACE_ANTISPOOF_INTEGRATION_CHANGELOG.md)。
- 三进程架构与协作：[260407 三进程变更文档](./260407_THREE_PROCESS_CHANGELOG.md)。
- 工程内 AI 入口与活体阈值说明：**`CLAUDE.md`**（桌面侧规则文档，含 **`FACE_FAS_REAL_THRESH`** 与启动示例）。

## 8. 结论

本次在 **`face_ai_main.cc`** 增加 **按槽位、按帧人数门控的识别滞回**，并在 **`FaceRecognition`** 侧补充 **`top1_id` / `ambiguous_match` 与只读查询接口**，在 **不修改核心检索公式** 的前提下 **减轻识别阈值附近的 jitter**；同时 **小幅降低** 识别特征 EMA 强度以缓解 **相似度被平滑拖低** 的观感。**多人同框** 与 **库内歧义** 路径 **刻意不启用** 滞回或 **主动清状态**，以降低 **串脸与误确认** 风险。部署后可通过 **`FACE_REC_HYST`**（在环境允许时）与 **`recognition hysteresis band`** 启动日志做现场校准。
