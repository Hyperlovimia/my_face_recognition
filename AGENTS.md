# 项目 build 流程

当项目使用 k230_sdk 时，采取以下方法编译项目

## 进入 Kmodel 模型转换环境
```sh
# 进入k230 SDK目录
cd /home/hyperlovimia/k230_sdk
```

```sh
# 激活 docker
docker run -u root -it -v $(pwd):$(pwd) -v $(pwd)/toolchain:/opt/toolchain -w $(pwd) ghcr.io/kendryte/k230_sdk /bin/bash
```

## 进入kmodel推理程序源码目录
```sh
cd src/reference/ai_poc/my_face_recognition
```

> 注：项目 `my_face_recognition` 必须放在 k230_sdk/src/reference/ai_poc/ 目录下

##  执行编译脚本
```sh
./build_app.sh
```

> 编译完成后，可在 `k230_bin/` 目录下看到可执行程序与配套测试文件
