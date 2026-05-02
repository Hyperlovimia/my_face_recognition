# CLAUDE.md

## Project Structure

```text
.
├── src/            # RT-Smart 大核源码 (AI推理, 视频流水线, OSD, IPC)
├── linux_bridge/    # Linux 小核桥接程序 (face_netd, MQTT/HTTP)
├── server_pc/      # PC 端后端服务 (FastAPI, SQLite, MQTT)
├── models_src/     # 模型转换与校准 Python 脚本 (ONNX -> KModel)
├── k230_bin/       # 编译产物 (ELF 及其运行脚本, KModel)
├── archive/        # 历史变更记录与调试文档
├── cmake/          # 交叉编译配置
├── build/          # CMake 编译中间产物
├── utils/          # 辅助工具及预编译模型备份
├── CMakeLists.txt  # 项目构建配置
└── Makefile        # 顶层编译入口
```

## K230 SDK 参考文档

执行 K230 SDK 相关任务前，优先查阅文档

[K230 SDK 官方文档](https://www.kendryte.com/k230/zh/main/index.html)
[K230核间通讯API参考](https://www.kendryte.com/k230/zh/main/01_software/board/cdk/K230_%E6%A0%B8%E9%97%B4%E9%80%9A%E8%AE%AF_API%E5%8F%82%E8%80%83.html)

# 项目构建流程

## Linux 小核项目

直接执行脚本
```sh
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/linux_bridge
./build_face_netd.sh
```

> 编译完成后，可在 `out/` 目录下看到可执行程序与配套测试文件
> 另外，face_netd.ini 也需要传到板子上

如果电脑端 `server_pc` 运行在 WSL / Docker 中，还需要额外注意：

- `face_netd.ini` 中 `mqtt_url` 必须填写 Windows 主机真实局域网 IPv4
- 不要填写 `vEthernet (WSL)` 的 `172.23.x.x`
- Windows 管理员 PowerShell 中需要额外配置：

```powershell
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=1883 connectaddress=127.0.0.1 connectport=1883
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8000 connectaddress=127.0.0.1 connectport=8000
New-NetFirewallRule -DisplayName "face-mqtt-1883" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 1883
New-NetFirewallRule -DisplayName "face-web-8000" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8000
```

## RT-Smart 大核项目

RT-Smart 大核项目需要使用 Docker 环境编译项目，具体方法如下

### 进入 Kmodel 模型转换环境
```sh
# 进入k230 SDK目录
cd /home/hyperlovimia/k230_sdk
```

```sh
# 激活 docker
docker run -u root -it -v $(pwd):$(pwd) -v $(pwd)/toolchain:/opt/toolchain -w $(pwd) ghcr.io/kendryte/k230_sdk /bin/bash
```

### 进入kmodel推理程序源码目录
```sh
cd src/reference/ai_poc/my_face_recognition
```

> 注：项目 `my_face_recognition` 必须放在 k230_sdk/src/reference/ai_poc/ 目录下

###  执行编译脚本
```sh
./build_app.sh
```

> 编译完成后，可在 `k230_bin/` 目录下看到可执行程序与配套测试文件

## 活体检测（偏严 / 误拒真人）

静默活体在 `face_ai` 里用 **REAL 概率** 与阈值比较：`score >= 阈值` 视为通过。阈值 **越高越严**（真人更容易被判为不通过），**越低越松**。

| 项目 | 位置 | 说明 |
|------|------|------|
| 默认 REAL 阈值 | `src/face_ai_main.cc` 中 `fas_real_thresh_from_env()` | 未设置环境变量时默认为 **0.32** |
| 环境变量（板端常不可用） | `FACE_FAS_REAL_THRESH` | 优先用第 10 个参数 |
| 命令行 | 第 10 / 11 参数 | 阈值为 10；`real0` 为 11（约定 out[0]=REAL 时），见 README |
| REAL 帧间平滑 | `k_fas_ema_alpha_up` / `k_fas_ema_alpha_dn` | **非对称 EMA**：高分帧快速跟上、低分帧缓降，减轻抖动误拒 |

**启用活体时的启动示例**（路径按你板子实际 `/data` 或 `/sharefs` 调整；常见 kmodel 需 **`real0`**，见 README）：

```sh
/data/face_ai.elf /data/face_detection_320.kmodel 0.5 0.2 /data/GhostFaceNet_W1.3_S1_ArcFace_k230.kmodel 60 /data/face_db 0 /data/face_antispoof.kmodel 0.18 real0 &
```

`0.18` 为 REAL 阈值（真人分数常在 0.15～0.30 波动时可从偏低试起）；不写第 10 个参数则用默认 **0.32**。调试时可将 `<debug_mode>` 设为 `2`，串口打印 `REAL_raw` / `REAL_ema`。

若仍大量误拒，除略降阈值外，还需核对 **光照、角度、活体模型与当前相机场景** 是否匹配；阈值过低会抬高攻击通过风险，需现场折中。
