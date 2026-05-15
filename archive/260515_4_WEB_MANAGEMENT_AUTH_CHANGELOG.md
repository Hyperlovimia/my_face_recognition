# Web 管理端登录鉴权改造记录

日期：2026-05-15

## 1. 本轮目标

为 `server_pc` 的 Web 管理端增加单管理员登录能力，确保浏览器首次进入系统时只能看到登录页，输入正确密码后才能进入现有管理面板，并支持主动退出登录。

本轮改造覆盖：

- `server_pc/app/main.py`
- `server_pc/web/src/*`
- `server_pc/docker-compose.yml`
- README / Web 前端说明文档

## 2. 最终交互行为

### 2.1 默认入口

浏览器访问 `face-web` 后，默认先进入登录页：

- 显示标题、副标题与密码输入框
- 只需要单个管理员密码，无用户名
- 输入正确密码后才会进入管理面板

### 2.2 登录成功后

登录成功后：

- 加载设备列表、事件、命令分页和图库
- 建立受保护的 `/ws` WebSocket 连接
- 保留现有管理面板所有能力

### 2.3 退出登录

管理面板右上角新增：

- `退出登录`

点击后：

- 清除服务端会话
- 关闭当前 WebSocket
- 前端清空已加载的管理数据
- 返回登录页

### 2.4 会话失效

当前会话只保存在 `face-web` 进程内存中，因此：

- 浏览器刷新后，如果服务未重启，仍可直接进入面板
- `face-web` 进程重启后，会话失效，需要重新登录

## 3. 安全设计

### 3.1 管理员密码来源

Web 管理端密码固定来自环境变量：

```text
FACE_WEB_ADMIN_PASSWORD
```

特点：

- 不写入仓库
- 不持久化到 SQLite
- 不与板端 `ui_admin_pin_hash` 复用

### 3.2 未配置密码时的行为

若 `FACE_WEB_ADMIN_PASSWORD` 缺失或为空：

- `face-web` 启动直接失败
- 不允许以“无密码模式”启动

这样可以避免部署时因漏配而暴露未加锁的管理后台。

### 3.3 会话与 Cookie

登录成功后，服务端创建随机会话 ID，并通过 Cookie 下发：

- Cookie 名：`face_web_session`
- `HttpOnly`
- `SameSite=Lax`
- `Secure` 根据请求 scheme 自动决定

会话只保存在进程内存中，不做数据库持久化。

## 4. 接口与访问控制

### 4.1 新增认证接口

- `POST /api/auth/login`
- `POST /api/auth/logout`
- `GET /api/auth/session`

### 4.2 受保护范围

以下内容现在必须先登录：

- `/api/server-time`
- `/api/devices*`
- `/api/web-data/clear`
- `/ws`

因此本轮不是“只挡前端页面”，而是后端接口与 WebSocket 也真正加上了鉴权。

## 5. 验证点

本轮应重点验证以下场景：

- 未登录访问 `/` 时只能看到登录页
- 未登录直接访问 `/api/devices` 返回未授权
- 未登录建立 `/ws` 连接会被拒绝
- 输入正确密码后能进入管理面板并正常使用原有功能
- 输入错误密码时停留在登录页并显示错误提示
- 点击“退出登录”后立即返回登录页
- 刷新页面时会话仍可复用
- 重启 `face-web` 后需要重新登录

## 6. 部署提醒

启动 Docker 或本机 `uvicorn` 前，先设置例如：

```bash
export FACE_WEB_ADMIN_PASSWORD='change-this-password'
```

不要把真实密码直接写进仓库里的 `docker-compose.yml`、README 或示例脚本。
