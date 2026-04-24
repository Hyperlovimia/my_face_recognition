# AGENTS.md

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
