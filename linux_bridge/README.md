# linux_bridge

`face_netd` 运行在 K230 Linux 小核，用于把 RT-Smart 大核的人脸事件和控制命令桥接到 MQTT。

## 编译

在开发机执行：

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/linux_bridge
./build_face_netd.sh
```

产物位于 `out/face_netd`。

## 部署

将以下文件复制到板端 `/sharefs/face_bridge/`：

- `out/face_netd`
- `face_netd.ini`

启动：

```sh
cd /sharefs/face_bridge
chmod +x ./face_netd
./face_netd --config ./face_netd.ini
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
