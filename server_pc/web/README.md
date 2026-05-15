# face-web 前端（React + Vite + TypeScript + Shoelace）

源码在 `web/`，**构建产物**输出到上一级的 `server_pc/static/`（与 FastAPI `FileResponse` / `StaticFiles` 一致）。

## 环境要求

- Node.js 20+（在 **WSL 里必须用 Linux 的 `node` / `npm`**，不要用 `C:\Program Files\nodejs\` 里的 Windows 版。）
- 项目路径用 **WSL 内的路径**（如 `/home/.../server_pc/web`），不要从 **Windows** 在 `\\wsl$\` 下跑 `npm`；否则常出现 `EPERM`、`UNC\wsl.localhost\...` 和日志写到 `C:\Users\...\npm-cache`。

### WSL 下装 Linux 版 Node 并切到对的路径

1. 在 **WSL 终端** 里（提示符是 `user@...:` 不是 `C:\`）执行，确认不是 Windows 的 npm：

   ```bash
   which -a node npm
   # 好的示例：/usr/bin/node 或 ~/.nvm/versions/node/.../node
   # 坏示例：/mnt/c/Program Files/nodejs/npm
   ```

2. 若上一步指向 `/mnt/c/...`，在 Ubuntu/WSL2 中安装本机 Node，例如（选一即可）：

   ```bash
   # 简单方式：发行版仓库
   sudo apt update && sudo apt install -y nodejs npm
   # 或 NodeSource 20.x（更贴近当前 Vite 需求）
   # 见 https://github.com/nodesource/distributions
   ```

3. 清掉在错误环境下生成的目录后重装（仍在 **WSL、项目 Linux 路径** 下）：

   ```bash
   cd /home/你的用户/k230_sdk/src/reference/ai_poc/my_face_recognition/server_pc/web
   rm -rf node_modules
   /usr/bin/npm install   # 或先 hash -r 再用 which npm 确认
   npm run build
   ```

4. 若你曾在 Windows 上误跑过 `npm install`，`node_modules` 可能已损坏，**一定**在 WSL 里用上面 `rm -rf` 再装。

## 一次构建

在 `server_pc/web` 目录：

```bash
cd server_pc/web
npm install
npm run build
```

然后在本机或容器内启动 `uvicorn` 即可。`vite` 的 `emptyOutDir` 会清空并重建 `server_pc/static`（**不要**把数据库放在 `static/` 里）。

运行前还需要提供管理员密码环境变量，否则 `face-web` 会拒绝启动：

```bash
export FACE_WEB_ADMIN_PASSWORD='change-this-password'
```

网页登录成功后会话保存在服务端内存中，因此 `face-web` 进程重启后需要重新登录。

## 开发调试（热更新 + 代理 API）

终端 1（项目根在 `…/my_face_recognition/server_pc`）：

```bash
cd server_pc
export FACE_WEB_ADMIN_PASSWORD='change-this-password'
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

终端 2：

```bash
cd server_pc/web
npm run dev
```

浏览器打开 **http://127.0.0.1:5173**（Vite 将 `/api`、`/ws` 代理到 8000）。不要同时依赖 `http://127.0.0.1:8000/` 的静态页做开发，除非已执行过 `npm run build`。

## Docker

默认在 `server_pc` 下 `docker compose build`：`server_pc/Dockerfile` 多阶段在**镜像内**用 `node:20-bookworm-slim` 执行 `npm install` 与 `npm run build`（**无需**在宿主机先建 `static`）。若拉不到 Node 镜像、只能本机构建，可用 `../build-web.sh` / `../build-web.cmd` 或手动 `npm run build` 后使用 `Dockerfile.prebuilt` 类单阶段只复制 `static/`，见项目根 `README.md`。

## 技术栈

- React 18、Vite 5、TypeScript
- [@shoelace-style/shoelace](https://shoelace.style) 组件与主题（`setBasePath` 指向 jsdelivr 资源，亦可改为私有 CDN）
