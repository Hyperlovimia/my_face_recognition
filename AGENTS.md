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

## RT-Smart 大核项目

RT-Smart 大核项目支持在宿主机中直接执行脚本编译，具体方法如下

### 准备工具链
```sh
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition
./build_app.sh
```

> 注：项目 `my_face_recognition` 必须放在 k230_sdk/src/reference/ai_poc/ 目录下
> `build_app.sh` 会优先使用 `k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin`
> 编译完成后，可在 `k230_bin/` 目录下看到可执行程序与配套测试文件
