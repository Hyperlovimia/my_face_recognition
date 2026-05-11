# 三进程性能优化与活体回归排查记录

日期：2026-05-11

## 1. 文档说明

本文档归档本轮围绕 `my_face_recognition` 三进程程序所做的完整工作，覆盖两部分：

1. 三进程人脸识别相较单进程明显偏慢的根因分析与性能优化实现；
2. 优化完成后，开启活体检测时出现“活体置信度长期接近 1”的回归排查结论。

本文档既记录“改了什么”，也记录“为什么这样改”以及“后续仍需验证什么”。

## 2. 背景

用户反馈：

- 三进程程序 `src/face_ai_main.cc` / `src/face_video_main.cc` 所在链路，人脸识别单帧约 `400ms`；
- 单进程对照程序 `src/reference/ai_poc/crosswalk_detect/main.cc` 类似模型、类似参数、未开启活体时约 `200ms`；
- 之后在优化版本上重新打开活体检测，又出现活体分数长期维持高值的问题。

本轮目标分两步：

1. 在 **不合并三进程、先不改 `1280x720`、不改外部 IPC 协议** 的前提下，吃掉明显的工程损耗；
2. 对优化后新增的活体异常做回归定位，识别是哪个变更最可能影响了活体输入链路。

## 3. 性能问题根因分析

对比单进程与三进程后，本轮确认三进程慢的主要原因并不是“多一个 `face_event` 进程”本身，而是输入链路被拆成了更多复制和同步阶段。

### 3.1 单进程路径的优势

单进程样例中，`PipeLine::GetFrame()` 获取到的 AI 帧几乎可以直接包装成推理输入 tensor，属于接近零拷贝的输入路径。

### 3.2 三进程路径中的主要损耗

三进程正常识别态里至少存在以下热路径成本：

- `VICAP dump -> ai_buf memcpy`
- `ai_buf -> g_infer_pending memcpy`
- `g_infer_pending -> work memcpy`
- `work -> IPC shm memcpy`
- `IPC shm -> face_ai input_tensor memcpy`
- `face_ai` 侧每帧动态创建输入 tensor、`to_host/map/sync`

如果板端 AI 通道实际输出的是 `NV12` 而非 `BGR_888_PLANAR`，则 `video_pipeline.cc` 还会额外执行一段 CPU 端 `NV12 -> RGB HWC -> RGB CHW` 转换，这会进一步放大差距。

### 3.3 与多进程无关但会拖慢识别的点

`src/face_recognition.cc` 中原先 `database_search()` 每帧都会把库中特征逐个重复做 `l2_normalize`。库越大，这一段 CPU 时间越明显。

## 4. 本轮已实现的性能优化

### 4.1 阶段级性能统计

新增：

- `src/perf_stats.h`

并在三条主链路补充统一的阶段聚合统计，继续复用 `FACE_METRICS=1` / `debug_mode>0` 开关，不新增外部参数。

覆盖阶段包括：

- `video_pipeline`: `dump_frame`、`mmap_cached`、`copy_convert`、`munmap`
- `face_video`: `capture_to_pending`、`pending_to_ipc_slot`、`send_recv_wait`、`reply_copy`、`total_time`
- `face_ai`: `shmat_req`、`prepare_input_tensor`、`det_pre`、`det_infer`、`det_post`、`rec_pre`、`rec_infer`、`rec_search`、`reply_send`

目的不是打印逐帧大日志，而是形成稳定的平均耗时与最大耗时窗口统计，便于板端对比优化前后阶段占比。

### 4.2 `face_video` 识别态减少中转拷贝

修改文件：

- `src/face_video_main.cc`

原路径：

- `dump_res -> g_infer_pending`
- `g_infer_pending -> work`
- `work -> ipc_request_encode_buffer`

新路径：

- 预分配 2 个固定 `IPC_CMD_INFER` 请求 shm 槽位；
- 采集线程直接将当前帧编码进可写槽位；
- 推理线程直接挑选“最新完整槽位”执行 `rt_channel_send_recv`；
- 若 AI 线程仍在处理旧槽位，则覆盖更旧槽位，保持“只保留最新帧”的现有语义。

这样在正常识别态里去掉了两次整帧中转拷贝，同时不改现有 `ipc_proto.h` 线协议，也不改 `face_event` / bridge 外部交互格式。

### 4.3 `face_ai` 复用固定输入 tensor

修改文件：

- `src/face_ai_main.cc`

原路径每次 `IPC_CMD_INFER` / `IPC_CMD_REGISTER_COMMIT` 都动态创建一个新的 `host_runtime_tensor`，然后再做 `to_host/as_host/map/sync`。

新实现：

- 启动时为固定尺寸 `3 x AI_FRAME_HEIGHT x AI_FRAME_WIDTH` 预创建一个可复用输入 tensor；
- 正常识别态和注册提交在尺寸匹配时直接复用这一个 tensor；
- 每帧仅做 `req pixels -> tensor buffer memcpy`，再 `sync_write_back`；
- 若收到的请求尺寸不匹配，则仍回退到动态分配路径，避免锁死后续扩展。

### 4.4 `NV12 -> RGB CHW` 路径的 CPU 优化

修改文件：

- `src/video_pipeline.cc`

保留原有 `cv::cvtColor(..., cv::COLOR_YUV2RGB_NV21)`，但把后面的手写逐像素 HWC->CHW 拆平面循环改为：

- 在 `ai_buf` 的三个 plane 上建立 `cv::Mat` 视图；
- 使用 `cv::split` 直接把 `RGB HWC` 拆到 `R/G/B` 三个 plane。

这一步的目标是降低 CPU 逐像素搬运开销，同时继续保持：

- `BGR_888_PLANAR` 快路径不变；
- 下游看到的仍是既有约定的 `RGB CHW` 布局。

### 4.5 人脸库特征改为入库即归一化

修改文件：

- `src/face_recognition.cc`
- `src/face_recognition.h`

调整内容：

- `database_init()` 读取数据库后立即归一化并缓存；
- `database_add()` 新增特征时，文件格式保持原样，但内存中改存归一化后的特征；
- `database_search()` 不再对库中每个人的特征逐帧重复做 `l2_normalize`；
- 仅查询特征保留现有单次归一化逻辑。

这部分不能解释全部 `200ms` 差距，但能稳定压低“库越大越慢”的 CPU 开销。

## 5. 本轮实际改动文件

- `src/face_ai_main.cc`
- `src/face_video_main.cc`
- `src/video_pipeline.cc`
- `src/face_recognition.cc`
- `src/face_recognition.h`
- `src/perf_stats.h`

## 6. 编译与验证情况

### 6.1 本地交叉编译

由于原有构建目录存在路径/权限问题，本轮新建了可写目录：

- `build_codex/`

在设置以下环境后完成交叉编译：

- `SDK_SRC_ROOT_DIR=/home/hyperlovimia/k230_sdk`
- `MPP_SRC_DIR=$SDK_SRC_ROOT_DIR/src/big/mpp/`
- `NNCASE_SRC_DIR=$SDK_SRC_ROOT_DIR/src/big/nncase/`
- `OPENCV_SRC_DIR=$SDK_SRC_ROOT_DIR/src/big/utils/lib/opencv/`
- `PATH=$SDK_SRC_ROOT_DIR/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin:$PATH`

已成功构建：

- `face_video.elf`
- `face_ai.elf`
- `face_recognition.elf`

本轮未引入新的编译错误，仅保留项目中已有 warning。

### 6.2 板端验证边界

本轮在当前环境里无法直接上板跑实测，所以还没有拿到以下最终结果：

- 优化前后每个阶段的 `avg_ms/max_ms` 对比
- `BGR planar` 与 `NV12` 两种板端实际输出格式下的对比数据
- 库规模 `0 / 10 / 50+` 时 `database_search` 的耗时曲线

这些仍需板端实跑确认。

## 7. 优化后活体回归问题排查

用户后续反馈：开启活体检测后，活体模型输出的置信度长期维持在很高的值，接近 `1`。

本轮已沿活体输入链路进行了针对性排查：

- `video_pipeline -> face_video IPC -> face_ai input tensor -> face_recg.pre_process -> aligned_face_to_bgr -> face_antispoof`

### 7.1 已基本排除的变更

#### 7.1.1 数据库归一化优化

`src/face_recognition.cc` 的数据库归一化仅作用于识别库检索，不在活体路径上，可排除。

#### 7.1.2 `aligned_face_to_bgr()` 本轮未改

`src/face_recognition.cc` 中 `aligned_face_to_bgr()` 虽然本身存在通道语义值得继续关注的历史问题，但它不是本轮引入的变化，因此不能解释“优化后才出现的活体回归”。

#### 7.1.3 `NV12` 拆平面改动并非首要嫌疑

`src/video_pipeline.cc` 的 `cv::split` 替换手写逐像素循环，在语义上保持了和旧实现相同的 `RGB HWC -> RGB CHW` 拆分顺序。

如果运行日志打印的是：

- `AI chn1 delivering BGR planar directly`

则这段 `NV12` 逻辑根本不会执行，可直接排除。

### 7.2 最可疑变更：`face_video` 双固定槽位直发

最可疑的是 `src/face_video_main.cc` 中本轮新增的双固定 shm 槽位直发逻辑，具体在：

- 采集线程直接编码到固定槽位
- 推理线程直接发送该槽位对应的 `shmid`

提交前的旧行为是：

- 采集线程把帧写入 `g_infer_pending`
- 推理线程复制到 `work`
- 再在 `send_recv` 前即时编码成请求 shm

提交后的新行为改成了“采集侧提前编码 + 推理侧只选择最新槽位发送”。这不会改模型参数，但会改 **送到 `face_ai` 的那一帧的生命周期与时序**。

如果活体分数长期黏在接近 `1`，而不是随机坏掉，现象上更像：

- 活体看到的是滞后的真实人脸帧；
- 或者显示帧与活体推理帧不再严格同步；
- 或者槽位在边界时刻被复写，导致 `face_ai` 读到的不是当前预期帧。

因此这组改动是当前第一嫌疑点。

### 7.3 次可疑变更：`face_ai` 固定输入 tensor 复用

第二嫌疑点是 `src/face_ai_main.cc` 中新增的固定输入 tensor 复用逻辑。

提交前：

- 每帧动态创建输入 tensor
- 走 `to_host/as_host/map/sync`

提交后：

- 复用固定 tensor
- 改为 `hrt::map(tensor, map_write) -> memcpy -> unmap -> sync_write_back`

从代码语义上看，这一改动对检测、识别、活体共享同一输入 tensor，并不只针对活体。但如果该 runtime 路径在当前平台上对缓存/映射语义更敏感，也有可能导致输入内容与旧路径不一致，因此被列为第二嫌疑点。

## 8. 当前结论

截至本轮对话结束，结论如下：

1. 三进程较单进程慢，主因已经定位为输入链路中的多段复制、同步及可能存在的 `NV12` CPU 转换，而不是第三个 `face_event` 进程本身。
2. 本轮性能优化已经落地到代码，并完成了本地交叉编译验证。
3. 优化后出现的活体分数长期接近 `1`，目前最可疑的是：
   - `src/face_video_main.cc` 的双固定 infer 槽位直发逻辑；
   - 其次是 `src/face_ai_main.cc` 的固定输入 tensor 复用逻辑。
4. `database_search` 归一化优化不在活体路径上，可排除。
5. `video_pipeline.cc` 的 `NV12 -> cv::split` 改动只有在板端实际走 `NV12` 时才值得继续怀疑；若当前启动日志显示是 `BGR planar`，则它不是这次活体回归的来源。

## 9. 建议的后续验证顺序

为了尽快把活体回归钉死，建议按以下顺序做板端 A/B 实验：

1. 先临时强制 `face_video` 回退到旧的 `g_infer_pending -> work -> rpc_ai_impl` 路径，禁用双固定槽位直发。
2. 若活体恢复正常，则根因基本可锁定在 `face_video` 这组改动。
3. 若问题仍在，再强制 `face_ai` 不走固定输入 tensor，回退到每帧动态创建 input tensor 的旧路径。
4. 若板端实际输出的是 `NV12`，再单独比较旧的手写拆平面与新的 `cv::split` 实现。

推荐先做第 1 步，因为它最有机会直接解释“活体分数稳定偏高而不是随机异常”这一现象。

## 10. 相关提交与文档

- 提交：`9f2adf9 chore: 提升检测帧性能`
- 相关既有文档：
  - `archive/260407_THREE_PROCESS_CHANGELOG.md`
  - `archive/260414_FACE_ANTISPOOF_INTEGRATION_CHANGELOG.md`
  - `archive/260419_THREE_PROCESS_OSD_AND_INFER_SYNC_CHANGELOG.md`
  - `archive/260502_RECOGNITION_HYSTERESIS_AND_EMA_CHANGELOG.md`

## 11. 备注

本轮只完成了：

- 根因分析
- 性能优化实现
- 本地编译验证
- 活体回归定位

尚未在本轮内继续提交“活体回归修复补丁”。后续若通过板端 A/B 验证锁定具体嫌疑点，建议另起一份变更文档记录最终修复方案与回归结果。
