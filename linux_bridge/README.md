# linux_bridge

`face_netd` 运行在 K230 Linux 小核，用于把 RT-Smart 大核的人脸事件和控制命令桥接到 MQTT。

## 编译

在开发机执行：

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/linux_bridge
./build_face_netd.sh
```

产物位于 `out/face_netd`。
收集后的部署目录位于 `../k230_bin/face_bridge/`。
脚本收集完成后会自动删除 `.o` 中间产物，因此部署目录最终只保留可运行文件和配置。

## 部署

将 `../k230_bin/face_bridge/` 目录同步到板端 `/sharefs/face_bridge/`。

其中板端实际运行必需的是：

- `face_netd`
- `face_netd.ini`

`.o` 文件属于宿主机交叉编译产生的中间目标文件，不会在板端单独运行，脚本会在构建完成后自动清理。

启动：

```sh
cd /sharefs/face_bridge
chmod +x ./face_netd
./face_netd --config ./face_netd.ini
```

`face_netd.ini` 中的 `mqtt_url` 必须填写电脑主机在局域网中的真实 IPv4。

如果 `server_pc` 运行在 WSL / Docker 中：

- 应填写 Windows `Wi-Fi` / `Ethernet` 网卡地址，例如 `192.168.160.8`
- 不应填写 `vEthernet (WSL)` 的 `172.23.x.x`
- 还必须在 Windows 管理员 PowerShell 中配置端口映射：

```powershell
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=1883 connectaddress=127.0.0.1 connectport=1883
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8000 connectaddress=127.0.0.1 connectport=8000
New-NetFirewallRule -DisplayName "face-mqtt-1883" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 1883
New-NetFirewallRule -DisplayName "face-web-8000" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8000
```

若配置正确，板端启动后应最终看到：

```text
face_netd: mqtt connected mqtt://<电脑IP>:1883
```

## MQTT 约定

- 下行命令：`k230/<device_id>/down/cmd`
- 上行事件：`k230/<device_id>/up/event`
- 上行结果：`k230/<device_id>/up/reply`
- 上行状态：`k230/<device_id>/up/status`

命令 JSON 示例：

```json
{
  "schema": "k230.face.bridge.v1",
  "request_id": "demo-001",
  "cmd": "register_current",
  "name": "alice"
}
```
