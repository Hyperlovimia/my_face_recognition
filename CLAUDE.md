# CLAUDE.md

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
