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
- `OSD_WIDTH = 960`
- `OSD_HEIGHT = 540`

### 3.5 VO / OSD / VICAP API 迁移

`video_pipeline.h` 和 `video_pipeline.cc` 已从旧 RT-Smart API 切到 `k230_sdk` 当前 API：

- `k_vo_layer_id -> k_vo_layer`
- `k_vo_layer_attr -> k_vo_video_layer_attr / k_vo_video_osd_attr`
- `kd_mpi_vo_disable_layer -> kd_mpi_vo_disable_video_layer / kd_mpi_vo_osd_disable`
- `kd_mpi_vo_set_layer_attr -> kd_mpi_vo_set_video_layer_attr / kd_mpi_vo_set_video_osd_attr`
- `kd_mpi_vo_enable_layer -> kd_mpi_vo_enable_video_layer / kd_mpi_vo_osd_enable`
- `kd_mpi_vo_insert_frame -> kd_mpi_vo_chn_insert_frame`
- `GDMA_ROTATE_DEGREE_* -> K_ROTATION_*`

同时去掉了新 SDK 中不存在的旧接口和字段：

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

## 7. 结论

当前项目已经完成从原 `rtos_k230` 工程结构到 `k230_sdk` 工程结构的迁移，且已修复：

- 目录结构问题
- 构建脚本路径问题
- DMA 库名差异
- NT35516 屏参差异
- VO/OSD/VICAP API 差异
- MPP 链接库差异
- `main.cc` 的 C++ 指针编译错误

当前状态是：项目已经能够在 `k230_sdk` 的 docker 构建流程下成功编译。
