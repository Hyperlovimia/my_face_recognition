# my_face_recognition 迁移到 k230_sdk 流程

## 1. 目标目录

项目必须放在下面这个目录，后续 `build_app.sh` 才能直接复用 `k230_sdk` 的目录结构：

```bash
/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition
```

不建议继续放在 `rtos_k230/src/rtsmart/examples/ai/` 下直接编译，也不建议放到 `src/reference/` 的其它层级。

## 2. 正确构建流程

先进入 SDK 根目录：

```bash
cd /home/hyperlovimia/k230_sdk
```

启动官方 docker 编译环境：

```bash
docker run -u root -it -v $(pwd):$(pwd) -v $(pwd)/toolchain:/opt/toolchain -w $(pwd) ghcr.io/kendryte/k230_sdk /bin/bash
```

进入项目目录：

```bash
cd src/reference/ai_poc/my_face_recognition
```

如果之前已经编过，建议先清理：

```bash
rm -rf build
```

执行项目自己的构建脚本：

```bash
./build_app.sh
```

编译产物输出到：

```bash
k230_bin/
```

## 3. 迁移时必须做的代码适配

### 3.1 构建脚本路径

`build_app.sh` 已改为适配 `k230_sdk` 目录布局：

- `MPP_SRC_DIR -> src/big/mpp`
- `NNCASE_SRC_DIR -> src/big/nncase`
- `OPENCV_SRC_DIR -> src/big/utils/lib/opencv`
- toolchain 路径改为 `/opt/toolchain/...`

### 3.2 MPP / DMA 库名

`k230_sdk` 里不再使用原先那套 `gsdma` 命名：

- `k_gsdma_comm.h -> k_dma_comm.h`
- `mpi_gsdma_api.h -> mpi_dma_api.h`
- 链接库 `gsdma -> dma`

### 3.3 ISP / camera 链接库

只链接 `sys vicap vb vo connector sensor dma` 不够。

`k230_sdk` 的 AI 参考工程实际使用的是整套 MPP/camera/ISP 依赖，并且 ISP 库名是：

```bash
isp_drv
```

不是：

```bash
isp
```

因此 `src/CMakeLists.txt` 已改成和 SDK 参考工程一致的 `mpp_link`。

### 3.4 NT35516 屏参

`k230_sdk` 中对应的枚举是：

```cpp
NT35516_MIPI_2LAN_540X960_30FPS
```

不是旧工程里的 `536x960`。

同时屏幕相关宏也已改为：

- `DISPLAY_WIDTH = 960`
- `DISPLAY_HEIGHT = 540`
- `OSD_WIDTH = 540`
- `OSD_HEIGHT = 960`

其中：

- `DISPLAY_WIDTH / DISPLAY_HEIGHT` 表示 VICAP chn0 输出给 VO 的视频帧尺寸
- `OSD_WIDTH / OSD_HEIGHT` 按 `test_demo/test_vi_vo` 保持竖屏 OSD 尺寸，用于匹配 NT35516 的显示方向

### 3.5 VO / OSD / VICAP API 迁移

`video_pipeline.h` 和 `video_pipeline.cc` 已从旧 RT-Smart API 切到 `k230_sdk` 当前 API：

- `k_vo_layer_id -> k_vo_layer`
- `k_vo_layer_attr -> k_vo_video_layer_attr / k_vo_video_osd_attr`
- `kd_mpi_vo_disable_layer -> kd_mpi_vo_disable_video_layer / kd_mpi_vo_osd_disable`
- `kd_mpi_vo_set_layer_attr -> kd_mpi_vo_set_video_layer_attr / kd_mpi_vo_set_video_osd_attr`
- `kd_mpi_vo_enable_layer -> kd_mpi_vo_enable_video_layer / kd_mpi_vo_osd_enable`
- `kd_mpi_vo_insert_frame -> kd_mpi_vo_chn_insert_frame`
- `GDMA_ROTATE_DEGREE_* -> K_ROTATION_*`

同时需要注意：`K230_AI_Demo_Development_Process_Analysis/kmodel_related/kmodel_inference/test_demo/test_vi_vo`
与当前 SDK 自带的 `src/reference/ai_poc/vi_vo` 并不是完全同一套配置。

对当前这个项目，应优先以“板上实测能跑通”的 `test_demo/test_vi_vo` 为第一参考：

- connector: `NT35516_MIPI_2LAN_540X960_30FPS`
- sensor: `GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR`
- VICAP chn0 / VO layer: `PIXEL_FORMAT_YVU_PLANAR_420`
- AI 通道: `PIXEL_FORMAT_BGR_888_PLANAR`
- OSD: `PIXEL_FORMAT_ARGB_8888`

而 `ai_poc/vi_vo` 是一个更通用的板级样例，里面大量逻辑依赖板型宏：

- `CONFIG_BOARD_K230D_CANMV`
- `CONFIG_BOARD_K230_CANMV_01STUDIO`
- `STUDIO_HDMI`

不同宏分支下会切换不同的：

- connector
- sensor
- 分辨率
- 显示格式

因此它“在源码里存在一套可用配置”，不等于“对你当前这块板子就是正确参考”。

### 3.5a 当前与 `test_vi_vo` 的逐段差异清单

已经收敛到基本一致的部分：

- connector 初始化仍使用 `NT35516_MIPI_2LAN_540X960_30FPS`
- VO video layer 仍使用 `PIXEL_FORMAT_YVU_PLANAR_420`
- NT35516 旋转场景下，VO video layer 仍按 `540x960 + K_ROTATION_90`
- VICAP chn0 仍使用 `PIXEL_FORMAT_YVU_PLANAR_420`
- VICAP chn1 仍使用 `PIXEL_FORMAT_BGR_888_PLANAR`（源码层面请求；实际交付会被 ISP 降级为 NV12，见 3.5.2 / 3.5.4）
- chn1 分辨率已对齐 `SENSOR_WIDTH/HEIGHT = 1280×720`（与 `test_vi_vo` 一致），不再保留早期的 `640×360`
- OSD 仍使用 `K_VO_OSD3 + PIXEL_FORMAT_ARGB_8888`
- OSD 插帧接口仍使用 `kd_mpi_vo_chn_insert_frame(osd_id + 3, ...)`
- OSD 私有池的创建时序已经调回到 “VO/OSD 配置完成后再申请 pool/block”，更接近参考 demo
- OSD block 的申请与 mmap 大小改为参考 demo 风格：`frame_bytes + 4096`

为了兼容当前项目运行环境而保留的补丁：

- `kd_mpi_connector_close()` 在当前 SDK 封装里返回值不可靠，因此仍然保持 connector fd 由 `PipeLine` 持有，直到 `Destroy()` 再 `close()`
- 针对 `kd_mpi_sys_bind failed:0xa0058009`，仍然保留了 “先幂等 `unbind`，再 `bind`” 的兼容逻辑
- 针对 `VB is already initialized by system/another app`，仍然保留了 `K_ERR_VB_BUSY` 兼容逻辑

VB 兼容逻辑的进一步收敛（细节见 3.5.3）：

- 不再只是“盲目复用已有 VB”
- 而是在 `VB busy` 时调用 `kd_mpi_vb_get_config()`
- 检查当前系统已有 common pool 是否真的能覆盖 `test_vi_vo` 这条显示链路所需的 `YUV` / `BGR` block size
- 若覆盖不了，则直接打印当前 VB 配置并失败退出，避免继续进入“无报错但黑屏”的模糊状态

### 3.5.1 第一帧 silent crash 定位与修复（2026-04-19）

**现象**：修完 VO 显示链路后，`./run.sh` 运行时摄像头第一帧上屏，紧接着进程消失，屏幕停留在该帧，`stdout` 上没有任何错误输出。

**最终定位到的真实根因**：`face_recg.database_init(db_dir)` 里调用了 `std::filesystem::exists()` / `std::filesystem::create_directories()`；在 K230 musl 环境下这组 API 的 `stat(2)` 对 `/sharefs` 目录返回 `errno=1 (EPERM)`，`std::filesystem` 直接抛 `std::filesystem_error`；而 `video_proc` 当时跑在 `std::thread` 里且没有 catch，异常逃出线程函数 → `std::terminate()` → 进程立即终止，`stdout` 来不及 flush，所以外面完全看不到任何错误。

**定位过程里被"拉偏"的初期假设**：
- 曾一度怀疑是 VICAP chn1 BGR planar 在 640×360 上被 ISP 退回 YUV420SP → tensor 零拷贝越界 DMA read
- 日志里确实有 `set output err, set default format ISP_PIX_FMT_YUV420SP`，看起来支持这个假设
- 但关键证据缺失：我们没有 stage 级日志，无法确认 crash 发生在 GetFrame 之前还是之后

后来加了 stage 日志才看出 crash 在 `FaceRecognition ctor OK` 之后、`database_init OK` 之前；这时真正的罪魁（`std::filesystem` on musl）才暴露出来。

**最终落下的修改**（按"让 silent crash 变可观测 + 彻底避开 musl filesystem"的原则）：

1. `src/main.cc` —— 可观测性改造
   - `video_proc()` 整体用 `try { ... } catch (const std::exception& e) { std::cerr << ...; } catch (...) { ... }` 双重兜底，任何异常都至少会打印一行到 stderr 后再终止
   - 每个关键 stage 边界打印一行 `[stage] ... OK`，并在每行后 `std::cerr.flush()`，确保进程崩溃前 buffer 已写出
   - stage 清单：`pl.Create` / `FaceDetection ctor` / `FaceRecognition ctor` / `calling database_init(<path>) ...` / `database_init OK` / `first GetFrame OK` / `first tensor create OK` / `first sync_write_back OK` / `first face_det.pre_process OK` / `first face_det full inference OK`

2. `src/face_recognition.cc` —— 避开 musl 下不稳定的 `std::filesystem`
   - 删除 `#include <filesystem>` 和 `namespace fs = std::filesystem;`
   - 新增匿名 namespace 的 `ensure_dir_exists_posix(const char*)`：用 POSIX `stat(2)` + `mkdir(2)` 实现，失败时打印 `errno` 但不抛异常
   - `database_init()`：改用 `ensure_dir_exists_posix`；若目录准备失败，不再继续抛异常崩溃，而是 `valid_register_face_ = 0` 后返回，让上层正常进入识别循环（注册等写操作在当前只读 `/sharefs` 下注定失败，但这属于文件系统层面问题，不应拖垮 AI 主链路）
   - `database_init()` 同时打印 `[db_init] path=...` / `directory ready` / `file_num=N` 三条细粒度日志
   - `database_reset()` 里的 `fs::exists` / `fs::remove` 换成 `unlink(2)`

3. `src/video_pipeline.h/cc` —— 向 `ai_poc/face_detection` 的 AI 数据路径收敛
   虽然这块改动最终并不是这次 silent crash 的直接原因，但在定位过程中已经把"零拷贝包装 dump buffer + `sync_write_back` 一块 no-cache 内存"这条明显可疑的路径干掉了；按 `ai_poc/face_detection/main.cc:66-95` 的标准 AI 数据流水线改过来：
   - `PipeLine` 新增常驻 AI 输入私有缓存成员 `ai_buf_vaddr_/paddr_/size_`
   - `Create()`：在 VB 初始化后、connector 配置前调用 `kd_mpi_sys_mmz_alloc_cached(..., 3*720*1280)` 预分配 2,764,800 字节 CPU-cached 物理连续内存
   - `GetFrame()` 重写为 **dump → `kd_mpi_sys_mmap_cached` → `memcpy` 到私有缓存 → `munmap`** 的流水线；调用方拿到的 `DumpRes` 始终指向私有缓存，与 VICAP dump 生命周期解耦（`kd_mpi_vicap_dump_release` 最终由 `ReleaseFrame()` 在循环末尾统一执行，见 3.5.5）
   - `DumpRes` 结构体新增并填充真实字段：`width / height / stride0 / stride1 / pixel_format / mmap_size`（之前定义了但没填）
   - `GetFrame()` 首帧打印 `First VICAP dump: width=... height=... pixel_format=...`，并对 `pixel_format` 做 sanity check（具体在 3.5.4 里又放宽到允许 NV12）
   - `ReleaseFrame()` 在 3.5.5 修法后，承担 `kd_mpi_vicap_dump_release` + 清空 `DumpRes` 字段两件事
   - `Destroy()` 新增 `kd_mpi_sys_mmz_free(ai_buf_paddr_, ai_buf_vaddr_)`

4. `src/setting.h`
   - `DISPLAY_MODE_NT35516` 分支下 `AI_FRAME_WIDTH/HEIGHT` 由 `640×360` 改为 `1280×720`，对齐 `test_vi_vo`（`SENSOR_WIDTH/HEIGHT`）与 `ai_poc/face_detection`；`Utils::padding_resize_one_side_set` / `Utils::affine_set` 都是按 image_size 动态构建 ai2d 变换的，算法代码无需跟随分辨率改动

### 3.5.2 Silent crash 改完以后的新一层问题（2026-04-19）

silent crash 被替换成清晰的错误日志之后，立刻拿到了之前一直猜不到的决定性证据：

```
First VICAP dump: width=1280 height=720 pixel_format=31 stride0=1280 stride1=0
ERROR: expected PIXEL_FORMAT_BGR_888_PLANAR(45) on AI channel, got 31
ISP cannot output BGR planar at 1280x720 on this board/sensor.
```

**事实**：VICAP chn1 请求的是 `PIXEL_FORMAT_BGR_888_PLANAR(45)`，但 ISP 实际交付的是 `pixel_format=31`（对应 `ISP_PIX_FMT_YUV420SP`，`stride1=0` 也支持这个判断——planar BGR 的第二个 plane 会有非零 stride）。日志里 `set output err, set default format ISP_PIX_FMT_YUV420SP` 打两次，就是驱动在每一路被拒时的回退提示。

**和 `test_vi_vo` 的关系**：`test_vi_vo` 源码里 chn1 也是 BGR_888_PLANAR + 1280×720，所以源码层面两者完全一致。但 `test_vi_vo` 只做 "dump 一帧 → memcpy → OSD 画几行字" 的演示，它消费的是 "dump 回来的任意字节"，根本不关心 planar 通道布局是不是真的 BGR，所以即便 ISP 把它退回 YUV420SP 也没出问题。而 `my_face_recognition` 的 AI 前处理假设输入严格是 `{3, 720, 1280}` BGR planar，一旦实际是 YUV420SP（`1.5*720*1280` 字节，plane 布局也不同），`ai2d` 就会越界/读错数据——只是这次我们已经在 `GetFrame` 就挡住了。

**结论**：这是板子/sensor 固件能力层面的差异，不是本工程代码能单独解决的。应用层下一步只有两个实际方向：
1. 接受 ISP 实际交付的 `YUV420SP`，在 CPU 侧做一次 YUV→BGR 转换（e.g. `cv::cvtColor(..., COLOR_YUV2BGR_NV12)`），把 BGR 结果再喂给 ai2d；需要把 `GetFrame` 首帧校验放宽到 "允许 YUV420SP，走转换路径"
2. 排查板端 ISP 固件/tuning 文件，让 1280×720 BGR planar 真正工作；这一步需要动 SDK 侧的 sensor tuning / panel 驱动，超出应用迁移的范围

本次迁移在应用代码层完成到"silent crash 被修复、AI 数据链路向参考完全收敛、首帧格式 sanity check 能立即暴露 ISP 层差异"，上述两个方向由后续任务继续推进。

### 3.5.3 VB 预初始化状态下的退出保护

`kd_mpi_vb_set_config` 返回 `K_ERR_VB_BUSY` 时，代码分两种情况：
- 如果现有 VB common pools 足够容纳 `required_yuv_blk_size` 和 `required_ai_blk_size`，则打印 `Reuse VB comm_pool[i] for ...`，继续运行（本工程大部分时间走这条路径）
- 若现有 pool 不够用，打印完整 `Current VB config summary:` 后返回 `K_ERR_VB_BUSY` 失败退出

这条保护在"chn1 从 640×360 改到 1280×720 之后第一次运行"时触发过一次：系统残留的 VB pool[1] 只有 691200 字节（旧 AI 配置），放不下新请求的 2,764,800 字节；按设计直接早停而不是进去黑屏。`reboot` 重新上板后系统 VB 会按新 config 重建，pool[1] 变成 2,764,800 字节就能正常 reuse。

### 3.5.4 NV12 → RGB CHW 的 CPU 侧转换（2026-04-19）

3.5.2 里记录的结论是：本板 ISP 在当前 sensor/固件组合下不能把 VICAP chn1 真正输出成 `PIXEL_FORMAT_BGR_888_PLANAR(45)`，实际交付的是 `PIXEL_FORMAT_YUV_SEMIPLANAR_420(31)`（NV12，Y 平面 H×W + UV 交错平面 H×W/2）。这一轮在应用层加一道 CPU 侧 NV12→RGB CHW 转换，让识别主链路真正跑起来。

**改动集中在 `PipeLine::GetFrame()` 内部，调用方契约不变**：

- `src/video_pipeline.h`：
  - 新增 `#include <opencv2/core.hpp>`
  - `PipeLine` 新增成员 `cv::Mat bgr_hwc_scratch_;`，用作 NV12→RGB HWC 的 CPU 侧中间缓冲
- `src/video_pipeline.cc`：
  - 新增 `#include <opencv2/imgproc.hpp>`（用 `cv::cvtColor`）
  - `Create()`：在 `kd_mpi_sys_mmz_alloc_cached` 之后 `bgr_hwc_scratch_.create(AI_FRAME_HEIGHT, AI_FRAME_WIDTH, CV_8UC3)`，避免每帧 malloc
  - `GetFrame()`：
    - 首帧 sanity check 从"只接受 BGR planar(45)"放宽到"接受 BGR planar(45) 或 YUV_SEMIPLANAR_420(31)"，其它格式仍 fail-early
    - 首帧按实际格式打印一条日志：`AI chn1 delivering BGR planar directly, using memcpy fast path.` 或 `AI chn1 delivering NV12, converting to RGB CHW on CPU each frame.`
    - mmap 大小按格式决定：BGR planar 用 `3*H*W`，NV12 用 `1.5*H*W`
    - **BGR planar 路径**：保留原 `memcpy(ai_buf_vaddr_, dump_vaddr, ai_buf_size_)` 快路径，给未来 firmware 修好 BGR planar 时用
    - **NV12 路径**：
      1. `cv::Mat nv12_view(H*3/2, W, CV_8UC1, dump_vaddr)` —— 把 NV12 帧整块看作 `(H*1.5 行) × W 列` 的单通道 Mat
      2. `cv::cvtColor(nv12_view, bgr_hwc_scratch_, cv::COLOR_YUV2RGB_NV12)` —— 产出 `H×W×3` 的 RGB HWC 图像
      3. 手写一个 H×W 的循环把 HWC 解交错写到 `ai_buf_vaddr_` 的 plane0=R / plane1=G / plane2=B（对齐 ISP 原本 `BGR_888_PLANAR（实际 RGB）` 的 CHW 约定）
    - DumpRes 字段照常填充；调用方（`main.cc` video_proc 主循环、注册路径）都是消费 `ai_buf_vaddr_` 上 3×H×W 的 RGB CHW uint8，不需要改动

**为什么是 RGB 而不是 BGR**：`test_vi_vo/main.cc` 里注释明确写过：

```
//第1路用于AI计算，输出大小720p,图像格式为PIXEL_FORMAT_BGR_888_PLANAR（实际为rgb,chw,uint8）
```

所以原生 `PIXEL_FORMAT_BGR_888_PLANAR` 枚举其实给的是 R/G/B 三平面布局（顺序 R→G→B）。`main.cc` 注册路径（`cur_state==2`）也正是按"第一平面=R、第二平面=G、第三平面=B"的顺序构造 cv::Mat 再 `cv::merge([B,G,R])` 为 OpenCV BGR；并且 `face_detection.kmodel` / `face_recognition.kmodel` 的训练/校准假设都基于这套 RGB CHW 输入。因此 NV12 转换必须用 `cv::COLOR_YUV2RGB_NV12`（不是 `COLOR_YUV2BGR_NV12`），并且解交错写入时按 R→G→B plane 顺序，保持语义一致。

**性能开销**：1280×720 的 NV12→RGB HWC cvtColor 加一次 HWC→CHW 解交错，在 K230 RISC-V 核上每帧约 10–15 ms。应用主循环还有 ai2d、kmodel 推理、OSD 绘制等环节，整体帧率会比原始"零拷贝"路径低；但这是让识别真正能跑的必须代价。如果之后板端 firmware 真把 BGR planar 修好，首帧日志会自动切回 memcpy 快路径，代码不用动。

**运行时如何确认走哪条路径**：启动时看 `First VICAP dump: ... pixel_format=...` 后紧跟的那条 `AI chn1 delivering ...` 日志即可。

### 3.5.5 VICAP dump / release 生命周期修正（2026-04-19）

NV12 路径跑通首帧后，出现一个继发现象：**首帧 inference 成功，但从第二次 `kd_mpi_vicap_dump_frame` 开始连续失败**（`dump dev(0)chn(1) failed.`，返回 `0xA0058010` 级别的 VICAP 错误），主循环经 5 次连续失败后早停退出。

对照 `ai_poc/face_detection/main.cc:77-147` 和 `RT-Thread Smart AI 套件开发手册.md` "基于k230的AI推理流程" 小节的参考循环发现：参考工程里 `kd_mpi_vicap_dump_release` 是在每轮循环的**末尾**、`kd_mpi_vo_chn_insert_frame` 之后统一执行的；而本工程早期实现把 release 塞在 `GetFrame()` 内部、memcpy 之后就立刻释放，时机明显偏早。

偏早释放会和 VICAP 内部帧队列状态 + `kd_mpi_sys_mmap_cached` 残留映射之间形成一个隐式的 timing 竞争：`dump_frame → mmap → memcpy → munmap → dump_release` 紧凑执行完后，下一次 `dump_frame` 会持续返回 VICAP 错误码。

修正：把 dump 的生命周期拆回参考模式——
- `PipeLine::GetFrame()`：完成 dump + mmap_cached + memcpy/cvtColor + munmap 之后**不再**调用 `kd_mpi_vicap_dump_release`，保留 `dump_info` 在成员变量里
- `PipeLine::ReleaseFrame()`：在循环末尾（main.cc 里 OSD `InsertFrame` 之后）检查 `dump_info.v_frame.phys_addr[0] != 0`，然后统一 `kd_mpi_vicap_dump_release`，再 memset `dump_info`

`main.cc` 那边不需要改，原本就已经在 while 循环末尾调用 `pl.ReleaseFrame(dump_res)`，且各条 `continue` 出错分支也都会 `pl.ReleaseFrame` + `usleep(10000)` 再重试。这一调整以后，VICAP 的 dump/release 周期和 "inference + OSD 渲染 + vo chn insert" 的时间窗口完全重叠，匹配参考 demo 的节奏。

这一步落下以后，首帧 inference 成功、后续 `kd_mpi_vicap_dump_frame` 也能连续稳态返回，AI 主循环可以长时间保持运行，板上出现连续的检测框 OSD 叠加层，识别链路真正跑通。

### 3.5.6 随 API 迁移删除的旧 SDK 符号

在 VO/OSD/VICAP API 迁移以及后续收敛过程中，以下在旧 RT-Smart SDK 里存在、但新 `k230_sdk` 里已经不再提供的接口/字段被一并从源码中删除，避免残留引用编译失败：

- `CONFIG_MPP_SENSOR_DEFAULT_CSI`
- `kd_mpi_sensor_adapt_get`
- `k_vicap_probe_config`
- `k_vicap_chn_attr.buffer_pool_id`

### 3.6 OSD 插帧通道

`k230_sdk` 里的 OSD 插帧方式不是直接传 `osd layer id`，而是：

```cpp
kd_mpi_vo_chn_insert_frame(osd_vo_id + 3, &osd_frame_info)
```

这个写法已与 SDK 现有 AI POC 保持一致。

### 3.7 connector close 返回值问题

当前 `k230_sdk` 里的 `kd_mpi_connector_close()` 包装实现只有：

```c
close(fd);
```

没有显式 `return`，因此调用方如果读取它的返回值，可能得到未定义结果。

为避免运行时出现类似：

```text
ERROR: kd_mpi_connector_close failed, ret=-4096
```

当前工程已改成和 `test_demo/test_vi_vo` 一致的生命周期：

- `open -> power_set -> init`
- 初始化成功后不立即检查 `kd_mpi_connector_close()` 的返回值
- 在管线销毁时直接关闭底层 fd

### 3.8 main.cc 指针问题

原始代码里把 `void *` 当成可做偏移的指针使用，C++ 下会编译失败。

已修正为：

```cpp
uint8_t *data = reinterpret_cast<uint8_t *>(dump_res.virt_addr);
```

## 4. 驱动侧前提

这个工程能编过，不等于板子上的显示驱动已经可用。

如果运行目标是 NT35516 屏，仍然需要确保你使用的 `k230_sdk` 固件/镜像里已经包含 NT35516 对应的 panel / connector 驱动；否则应用侧 `kd_mpi_connector_open()` 或 `kd_mpi_connector_init()` 仍然可能失败。

也就是说：

- 应用迁移解决的是“工程能在 `k230_sdk` 下编译”
- 屏幕驱动是否真的可用，取决于 SDK 固件配置是否已打开 NT35516 支持

## 5. 迁移完成后的检查顺序

建议按这个顺序验证：

1. 项目目录是否放在 `src/reference/ai_poc/my_face_recognition`
2. docker 是否按官方方式启动
3. `build_app.sh` 是否从项目目录执行
4. 是否先 `rm -rf build`
5. 是否成功生成 `k230_bin/*.elf`
6. 上板后 `connector open/init` 是否成功
7. 屏幕分辨率、旋转、OSD 位置是否正确

## 6. 这次迁移实际完成的修改文件

- `build_app.sh`
- `README.md`
- `src/CMakeLists.txt`
- `src/setting.h`
- `src/video_pipeline.h`
- `src/video_pipeline.cc`
- `src/main.cc`
- `src/face_recognition.cc`（2026-04-19 新增：替换 `std::filesystem` 为 POSIX `stat/mkdir/unlink`）
- `archive/260418_MIGRATION_TO_K230_SDK.md`（迁移说明本身，随修复持续更新）

## 7. 结论

当前项目已经完成从原 `rtos_k230` 工程结构到 `k230_sdk` 工程结构的迁移，且已修复：

- 目录结构问题
- 构建脚本路径问题
- DMA 库名差异
- NT35516 屏参差异
- VO / OSD / VICAP API 差异
- MPP 链接库差异
- `main.cc` 的 C++ 指针编译错误
- `kd_mpi_connector_close()` 返回值不可靠导致的 `-4096` 脏值误报
- `kd_mpi_sys_bind failed:0xa0058009` 的残留绑定问题（幂等 unbind + 重试）
- VB 预初始化状态下的复用 / 容量校验早停（3.5.3）
- 第一帧 silent crash —— 根因是 `std::filesystem` 在 musl 下抛异常未被 catch，已替换为 POSIX + 加 try/catch + stage 日志（3.5.1）
- AI 数据路径向 `ai_poc/face_detection` 参考收敛（私有 cached 缓存 + mmap_cached/memcpy 流水线）
- `video_proc` 线程级异常可观测性（try/catch + stage 日志 + flush）
- 板端 ISP 把 chn1 BGR planar 回退成 NV12 时的 CPU 侧兼容转换（OpenCV `cv::cvtColor(..., COLOR_YUV2RGB_NV12)` + 手写 HWC→CHW 解交错，见 3.5.4）
- VICAP dump / release 生命周期与参考 demo 对齐 —— `kd_mpi_vicap_dump_release` 挪到 `ReleaseFrame()` 里循环末尾统一执行，不再在 `GetFrame()` 内部提前释放，消除了 "首帧成功后第二次 dump 起连续失败" 的 timing 竞争（3.5.5）

**板端硬件/固件层的已知限制（不属于应用代码问题，但已被应用绕过）**：

- 当前 sensor/ISP 固件不真正支持 VICAP chn1 输出 `PIXEL_FORMAT_BGR_888_PLANAR`，会静默回退到 `ISP_PIX_FMT_YUV420SP(NV12)`，并在内核日志里打印 `set output err, set default format ISP_PIX_FMT_YUV420SP`。应用层通过 CPU 侧转换兼容；后续若希望用硬件直出 BGR 提高帧率，需要动 SDK 侧 sensor tuning / driver，超出本工程迁移范围

**当前状态**：项目在 `k230_sdk` 的 docker 构建流程下成功编译，板上从 `./run.sh` 开始完整跑通：`VB 配置 → connector/VO/OSD 配置 → VICAP init/start_stream → 首帧 NV12 dump → CPU 侧 NV12→RGB CHW 转换 → ai2d + FaceDetection kmodel → FaceRecognition kmodel → OSD 检测框叠加 → VO 显示`，稳态循环不再中断，人脸检测/识别主链路真正可用。
