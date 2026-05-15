# 板端触摸管理入口鉴权改造记录

日期：2026-05-15

## 1. 本轮目标

本轮围绕板端触摸 UI 做了三类增强：

- 将原先开机即显示的 `Import / Signup / Delete` 管理按钮收口到一个统一的 `Manage` 入口
- 为板端管理入口增加 PIN 鉴权、失败冷却与自动锁回
- 解决板端字体缺失导致的中文方框问题，将屏幕显示文案统一改为英文

本轮变更只影响：

- Linux 小核 `face_netd`
- 板端触摸 UI / LVGL 状态机
- `face_netd.ini` 配置格式
- 宿主机生成 PIN 哈希的辅助脚本

本轮**不涉及**：

- RT `face_event / face_video / face_ai` 协议修改
- shared buffer / OSD 混合链路修改
- Web 管理端登录功能

## 2. 最终交互行为

### 2.1 默认锁定态

系统刚进入时，屏幕底部只显示一个按钮：

- `Manage`

此时原有三个管理按钮：

- `Import`
- `Signup`
- `Delete`

默认隐藏，不允许直接操作。

### 2.2 进入管理模式

点击 `Manage` 后，进入 PIN 输入页：

- 使用数字键盘
- 只接受 `6` 位数字
- 输入框为密码掩码显示
- 顶部提供 `Back`

PIN 校验通过后，回到主界面并显示管理按钮：

- `Import`
- `Signup`
- `Delete`
- `Exit Manage`

### 2.3 退出管理模式

管理模式退出有两种方式：

1. 空闲超时自动退出  
2. 点击底部 `Exit Manage` 手动退出

两条路径都复用同一条锁回逻辑，效果等同：

- 隐藏三个管理按钮
- 回到仅显示 `Manage` 的锁定态

### 2.4 错误与冷却

若 PIN 输入错误：

- 未达到阈值时提示剩余尝试次数
- 连续输错 `3` 次后进入冷却

冷却期间：

- 不允许继续尝试解锁
- 主界面状态文案提示剩余冷却秒数

冷却结束后自动恢复到普通锁定态。

## 3. 配置与安全设计

### 3.1 新增配置项

`linux_bridge/face_netd.ini` 新增：

```ini
ui_admin_pin_hash =
```

格式固定为：

```text
pbkdf2_sha256$120000$<salt_hex>$<digest_hex>
```

要求：

- 不接受明文 PIN
- salt 固定 `16` 字节
- digest 固定 `32` 字节
- 派生算法固定为 `PBKDF2-HMAC-SHA256`
- 迭代次数固定为 `120000`

### 3.2 未配置或配置非法时的行为

若 `ui_enabled=1` 且 `ui_admin_pin_hash` 缺失或格式非法：

- `face_netd` 仍继续运行
- 板端 UI 显示 `Admin PIN not configured`
- 管理入口保持不可用

因此不会因为配置问题导致 bridge 进程直接退出。

### 3.3 哈希生成脚本

新增宿主机脚本：

- `linux_bridge/scripts/gen_ui_admin_pin_hash.py`

用途：

- 交互式读取 `6` 位数字 PIN
- 自动生成随机 salt
- 输出可直接写入 `face_netd.ini` 的 `ui_admin_pin_hash=...`

也支持：

```bash
python3 ./scripts/gen_ui_admin_pin_hash.py --pin 123456
```

建议：

- 仓库样例配置不保存真实哈希
- 板端实际部署的 `face_netd.ini` 权限建议设为 `0600`

## 4. 代码落点

本轮主要修改点如下。

### 4.1 `linux_bridge/main.cpp`

新增：

- `ui_admin_pin_hash` 配置加载
- PIN 哈希格式解析
- PBKDF2-HMAC-SHA256 派生
- 常量时间 digest 比较
- 向 `ui_runtime` 传递“是否已配置 PIN”与校验回调

### 4.2 `linux_bridge/ui/ui_runtime.h`

扩展 `runtime_config`，新增：

- `admin_pin_configured`
- `admin_unlock_timeout_ms`
- `admin_fail_limit`
- `admin_cooldown_ms`
- `verify_admin_pin`

### 4.3 `linux_bridge/ui/ui_runtime.cpp`

新增管理授权层状态：

- `locked`
- `pin_entry`
- `unlocked`
- `cooldown`
- `unavailable`

并在原有注册业务状态机之外，增加：

- `Manage` 入口
- PIN 输入页
- 解锁后显示管理按钮
- 冷却与超时锁回
- `Exit Manage` 手动退出

同时把所有板端可见中文文案替换为英文，避免 LVGL 字库不包含汉字时出现方框。

## 5. 板端最终英文文案

本轮确认使用的核心屏幕文案包括：

- `Manage`
- `Exit Manage`
- `Enter 6-digit admin PIN`
- `Incorrect PIN. N tries left`
- `Locked. Retry in Ns`
- `Management unlocked`
- `Management locked`
- `Admin PIN not configured`
- `Capturing preview...`
- `Registration cancelled`
- `Registration complete`

因此当前板端 UI 不再依赖中文字体。

## 6. 构建与交付

本轮变更完成后，已重新构建：

```bash
cd /home/hyperlovimia/k230_sdk/src/reference/ai_poc/my_face_recognition/linux_bridge
./build_face_netd.sh
```

构建产物位于：

- `linux_bridge/out/face_netd`
- `k230_bin/face_bridge/`

部署到板端时，需要同步至少以下文件：

- `face_netd`
- `face_netd.ini`
- `data/img/*`

若未更新板端实际运行的 `face_netd`，则不会看到本轮新增的：

- 管理入口鉴权
- 英文化文案
- `Exit Manage`

## 7. 验证点

本轮应重点验证以下场景：

- 开机后仅显示 `Manage`
- 输入正确 PIN 后显示 `Import / Signup / Delete / Exit Manage`
- 空闲超时后自动退出管理模式
- 点击 `Exit Manage` 后立即退出管理模式
- 连续输错 3 次后进入冷却
- `ui_admin_pin_hash` 缺失时显示 `Admin PIN not configured`
- 屏幕文案全部正常显示，不再出现中文方框

## 8. 本轮边界说明

虽然本次会话中提出了“Web 端也增加登录功能”的需求，但该部分在本轮**尚未实现**，因此本文档不将其视为已交付能力。

当前文档仅记录已经实际完成并编译验证通过的板端触摸管理入口相关改造。
