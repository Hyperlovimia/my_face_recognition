# 板端触摸输入节点识别修复记录

日期：2026-05-15

## 1. 现象

在 `my_face_recognition` 当前版本中：

- UI 画面已经可以正常显示到屏幕
- `signup / import / delete` 图标按钮可见
- 但点击屏幕任意位置，UI 没有任何响应

现场日志表现为：

```text
face_netd_ui: waiting for RT shared overlay info
...
face_netd_ui: shared overlay ready generation=...
```

但没有任何按钮事件，也没有触摸输入日志。

## 2. 第一轮判断与排查方向

最初怀疑点有两类：

1. 触摸坐标映射错误
2. `face_netd` 读取的 `/dev/input/event*` 节点并不是触摸设备

为此先做了两步增强排查：

- 在 Linux 侧触摸回调里加入“按下即打印”的日志
- 为触摸输入层增加自动探测 `/dev/input/event*` 的能力，而不是只依赖固定的 `event0`

目标是先确认：

- 触摸事件是否真的进入 `face_netd`
- 若 `ui_touch_device` 配错，程序能否自动找到正确节点

## 3. 关键板端信息

板端实际输入设备信息如下：

```text
/dev/input/event0 -> gpio_keys
/dev/input/event1 -> GKTW50SCED1R0
```

对应 `/proc/bus/input/devices`：

```text
N: Name="gpio_keys"
H: Handlers=kbd event0

N: Name="GKTW50SCED1R0"
H: Handlers=event1
```

这说明：

- `event0` 是按键，不是触摸屏
- `event1` 才是实际触摸设备

## 4. 暴露出的真实根因

即使把配置改成：

```ini
ui_touch_device = /dev/input/event1
```

`face_netd` 仍然报：

```text
face_netd_ui: probe input dev=/dev/input/event1 failed: EVIOCGBIT failed: Success
```

这条日志本身就暴露了异常点：

- 错误文案写着 `failed`
- 但系统错误却是 `Success`

最终确认根因不是：

- 板子没有触摸节点
- 触摸驱动没有加载
- `event1` 不是触摸设备

而是我们自己的探测代码把 `ioctl(EVIOCGBIT(...))` 的**成功返回值误判成了失败**。

也就是说，`event1` 实际是正确节点，但程序逻辑把它提前拒绝了。

## 5. 最终修复内容

修改文件：

- `linux_bridge/ui/k230_port.cpp`

### 5.1 修正 `EVIOCGBIT` 返回值判断

原逻辑错误地用：

- `ioctl(...) != 0` 视为失败

这会把“返回正数字节数”的成功情况误判为失败。

最终改为：

- `ioctl(...) < 0` 才视为失败

受影响位置包括：

- `EVIOCGBIT(0, ...)`
- `EVIOCGBIT(EV_KEY, ...)`
- `EVIOCGBIT(EV_ABS, ...)`

这是本次问题的**核心修复点**。

### 5.2 增强触摸状态识别

为了兼容不同触摸驱动的上报方式，补充支持：

- `BTN_TOUCH`
- `BTN_TOOL_FINGER`
- `ABS_MT_TRACKING_ID`
- `ABS_PRESSURE`

避免仅靠 `BTN_TOUCH` 判断按下状态。

### 5.3 增强触摸坐标映射

不再假设原始触摸坐标一定等于物理分辨率 `1080 x 1920`，而是：

- 启动时读取输入设备的 `ABS_X / ABS_Y` 或 `ABS_MT_POSITION_X / Y` 范围
- 用真实 raw range 映射到逻辑 UI 尺寸 `540 x 960`

这样可以减少不同触摸 IC / 驱动下的坐标失真问题。

### 5.4 增加运行期诊断日志

新增三类日志：

1. 触摸设备探测日志
2. 被跳过的非触摸节点能力日志
3. 每次触摸按下时的 raw / mapped 坐标日志

典型日志格式：

```text
face_netd_ui: touch ok dev=/dev/input/event1 name="GKTW50SCED1R0" ...
face_netd_ui: touch press raw=(...) mapped=(...)
```

## 6. 触摸设备探测策略

修复后 Linux 侧输入初始化流程为：

1. 先尝试 `face_netd.ini` 中配置的 `ui_touch_device`
2. 如果该节点不可用或不满足触摸能力条件
3. 自动扫描 `/dev/input/event*`
4. 按能力特征筛选可能的触摸设备

这让程序不再强依赖固定的：

- `/dev/input/event0`

从而适配不同板卡或不同内核枚举顺序。

## 7. 验证结果

本轮修复后，问题已确认解决。

结论如下：

- UI 可正常显示
- `face_netd` 能正确识别触摸节点
- 触摸按下日志可以打印
- 板端按钮点击恢复正常

因此本轮最终结论是：

- **问题根因不是 LVGL 页面逻辑**
- **也不是 RT shared buffer 显示链路**
- **而是 Linux 小核触摸输入探测代码对 `EVIOCGBIT` 成功返回值的误判**

## 8. 对后续维护的建议

后续若再遇到“UI 可见但点击无响应”，建议按下面顺序排查：

1. 看是否出现 `touch ok dev=...`
2. 看是否出现 `touch press raw=(...) mapped=(...)`
3. 若没有 `touch ok`，先检查 `/proc/bus/input/devices`
4. 若有 `touch press` 但按钮仍不响应，再检查坐标映射与 `flip_x / flip_y`

这次修复后，触摸链路排查已经有了可复用的基础日志，不需要再回到“盲猜 event 节点”的方式。
