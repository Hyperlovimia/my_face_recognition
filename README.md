# 轻量化人脸识别示例

## 概述

本示例在 K230 / RT-Smart 上实现轻量级人脸识别，支持人脸检测、人脸识别、人脸注册、注册人数查询、数据库清空、三进程协作与可选静默活体检测。

当前推荐使用三进程模式：

- `face_ai.elf`：人脸检测、识别、数据库操作，可选加载活体模型
- `face_video.elf`：视频采集、显示与 OSD
- `face_event.elf`：单串口交互入口、事件输出与考勤日志

## 编译

在开发机 SDK 环境中执行：

```bash
cd rtsmart/examples/ai/my_face_recognition
./build_app.sh
```

生成的 ELF 产物在 `k230_bin` 目录中。将以下文件按需同步到板端 `/data`：

- `face_ai.elf`
- `face_video.elf`
- `face_event.elf`
- `face_detection_320.kmodel`
- `face_recognition.kmodel`
- `face_antispoof.kmodel`，仅启用活体检测时需要

## RT-Smart 板端启动

RT-Smart 的 `msh` 不能按常规 Linux shell 使用，不要依赖 `export`、复杂变量展开或仓库脚本自动启动三进程。请在板端串口中按顺序直接输入命令。

启用活体检测时，先后台启动 `face_ai.elf`，最后一个参数传入活体模型：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/face_recognition.kmodel 70 /data/face_db 0 /data/face_antispoof.kmodel &
```

暂不启用活体检测时，使用 8 参数启动 `face_ai.elf`：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/face_recognition.kmodel 70 /data/face_db 0 &
```

然后后台启动视频进程：

```sh
/data/face_video.elf 0 &
```

最后以前台方式启动交互入口：

```sh
/data/face_event.elf /tmp/attendance.log
```

启动后，所有交互命令都在 `face_event.elf` 所在串口输入。`/data/face_db` 不存在时，`face_ai.elf` 会尝试自动创建。

## 参数说明

`face_ai.elf` 参数格式：

```text
face_ai <kmodel_det> <det_thres> <nms_thres> <kmodel_recg> <recg_thres> <db_dir> <debug_mode> [<face_antispoof.kmodel>]
```

| 参数 | 说明 |
| ---- | ---- |
| `kmodel_det` | 人脸检测 kmodel 路径 |
| `det_thres` | 检测置信度阈值，推荐 `0.5` |
| `nms_thres` | NMS 阈值，推荐 `0.2` |
| `kmodel_recg` | 人脸识别 kmodel 路径 |
| `recg_thres` | 识别相似度阈值，推荐 `70` |
| `db_dir` | 人脸数据库目录，推荐 `/data/face_db` |
| `debug_mode` | 调试模式，`0` 为关闭 |
| `face_antispoof.kmodel` | 可选活体模型路径，存在且加载成功时启用活体 |

## 交互命令

| 功能 | 命令 |
| ---- | ---- |
| 查看帮助 | `h` 或 `help` |
| 抓拍注册帧 | `i` |
| 提交注册 | 抓拍后输入人名 |
| 查询注册人数 | `n` |
| 清空人脸数据库 | `d` |
| 退出三进程 | `q` |

注册截图时请确保画面中仅有一张清晰可见的人脸。姓名建议使用可识别英文字符，避免特殊符号。
