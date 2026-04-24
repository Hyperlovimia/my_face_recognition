# K230 `face_netd` 在 WSL / Docker 环境下无法连通 MQTT 的修复记录

日期：2026-04-25

## 1. 背景

本项目的电脑端服务 `server_pc` 运行在开发机的 WSL 环境中，并通过 `docker compose` 启动：

- `mosquitto`
- `face-web`

板端 Linux 小核 `face_netd` 通过 MQTT 与电脑端通信，配置文件位于：

```text
/home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/linux_bridge/face_netd.ini
```

典型配置为：

```ini
mqtt_url = mqtt://192.168.160.8:1883
```

其中 `192.168.160.8` 是 Windows 主机在局域网中的 Wi-Fi IPv4 地址。

## 2. 现象

联调时出现了以下现象：

### 2.1 网页端一直显示“暂无设备”

网页控制台可以正常打开，`face-web` 与 WebSocket 也都已启动，但设备列表一直为空。

### 2.2 板端 `face_netd` 无法完成 MQTT 建连

小核日志长期停留在：

```text
face_netd: mqtt dial attempt=1 url=mqtt://192.168.160.8:1883
face_netd: mqtt event OPEN id=1 url=mqtt://192.168.160.8:1883
face_netd: mqtt event RESOLVE id=1
face_netd: mqtt pending id=1 url=mqtt://192.168.160.8:1883 elapsed=...
  resolving=0 connecting=1 writable=0 readable=0 send=0 recv=0
```

说明：

- DNS / IP 解析已经完成
- 连接对象已经创建
- 但 TCP 三次握手始终没有完成

### 2.3 电脑端 `mosquitto` 日志中看不到来自板子的连接

`docker compose logs -f mosquitto face-web` 中只有网页访问和 `face-web` 自身的 MQTT 连接，没有来自 K230 的新连接记录。

## 3. 根因

根因不在 `mqtt_url` 字符串本身，也不在 K230 与 RT-Smart 的 IPCMSG 链路。

真正的问题是：

- `server_pc` 运行在 WSL + Docker 环境中
- 在 WSL 内部查看时，`1883` 和 `8000` 显示为 `0.0.0.0` 监听
- 但在 Windows 主机侧查看时，这两个端口实际上只监听在 `127.0.0.1` / `::1`
- Windows 局域网地址 `192.168.160.8` 并没有对外暴露 `1883` 和 `8000`
- 同时系统没有配置 `netsh interface portproxy`

因此板子访问：

```text
192.168.160.8:1883
```

时，TCP 握手被卡在 `connecting=1`，既没有成功，也没有立即失败。

网页“暂无设备”只是这个问题的结果，因为 `face-web` 只有在收到以下 MQTT 上行消息后，才会把设备写入数据库：

- `k230/<device_id>/up/status`
- `k230/<device_id>/up/event`
- `k230/<device_id>/up/reply`

而 `face_netd` 在 MQTT 未连通时，根本无法发送这些消息。

## 4. 解决办法

### 4.1 `mqtt_url` 必须填写 Windows 局域网 IPv4

若电脑端服务运行在 WSL 中：

- 应填写 Windows `Wi-Fi` / `Ethernet` 网卡的真实局域网 IPv4
- 不应填写 `vEthernet (WSL)` 的 `172.23.x.x`
- 也不应填写 WSL 内部地址

例如：

```ini
mqtt_url = mqtt://192.168.160.8:1883
```

### 4.2 在 Windows 主机上配置 `portproxy`

必须在 Windows 管理员 PowerShell 中执行：

```powershell
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=1883 connectaddress=127.0.0.1 connectport=1883
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8000 connectaddress=127.0.0.1 connectport=8000
```

这样 Windows 局域网地址上的：

- `1883`
- `8000`

才能正确转发到 WSL / Docker 中的服务。

### 4.3 放通 Windows 防火墙

同样需要在 Windows 管理员 PowerShell 中执行：

```powershell
New-NetFirewallRule -DisplayName "face-mqtt-1883" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 1883
New-NetFirewallRule -DisplayName "face-web-8000" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8000
```

### 4.4 验证 Windows 对外监听状态

执行：

```powershell
netsh interface portproxy show all
Get-NetTCPConnection -LocalPort 1883,8000 -State Listen
```

应能看到：

- `portproxy` 规则已经存在
- `1883` / `8000` 已不再只绑定到 `127.0.0.1`

## 5. 代码侧辅助修复

为了更快定位此类问题，本次还对 `linux_bridge/main.cpp` 做了辅助增强：

- 显式打印 MQTT 拨号、解析、TCP 连接、收发包过程
- 显式打印 MQTT `CONNECT`、`CONNACK` 与 `PUBLISH` 流程
- 对卡住的 TCP 连接输出周期性 `pending` 状态
- 增加 MQTT 建连超时自动重试

这些日志能区分以下几类问题：

- 卡在 TCP 连接阶段
- 已发出 MQTT `CONNECT` 但未收到 `CONNACK`
- 收到异常 MQTT 包
- Broker 主动拒绝连接

## 6. 验证结果

修复完成后，板端日志变为：

```text
face_netd: mqtt event CONNECT id=1 send CONNECT client_id=face-netd-k230-dev-01 keepalive=15s will_topic=k230/k230-dev-01/up/status
face_netd: mqtt event WRITE id=1 bytes=195 send_pending=0
face_netd: mqtt event READ id=1 bytes=4 recv_len=4
face_netd: mqtt packet cmd=2 len=4
face_netd: mqtt connected mqtt://192.168.160.8:1883
```

说明：

- TCP 握手成功
- MQTT `CONNECT` 已发送
- `CONNACK` 已收到
- `face_netd` 已正式接入 `mosquitto`

最终网页端设备列表恢复正常，可看到：

- 设备上线
- 状态心跳
- 事件上报
- 命令执行结果

## 7. 经验结论

在当前项目中，如果电脑端服务运行在 WSL / Docker 环境：

1. 板子访问电脑时，应始终使用 Windows 主机的局域网 IPv4
2. 若服务仅在 WSL 内监听，还必须额外配置 Windows `portproxy`
3. 如果网页显示“暂无设备”，应优先检查 `face_netd` 是否已经成功打印 `mqtt connected`
4. 若 `face_netd` 长期停留在 `connecting=1`，应优先怀疑 Windows 对 WSL 的对外转发问题
