```sh
ifconfig wlan0 up
wpa_passphrase "Redmi K60" "hyperlovimia" > /etc/wpa_supplicant.conf
wpa_supplicant -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf -B
udhcpc -i wlan0
```

自动连接：加一个新的启动脚本，比如 `/etc/init.d/S41wifi`，内容直接复用你现在手动执行成功的命令：

```sh
#!/bin/sh

case "$1" in
  start)
    ifconfig wlan0 up
    [ -f /etc/wpa_supplicant.conf ] || exit 1
    wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf
    udhcpc -i wlan0
    ;;
  stop)
    killall wpa_supplicant 2>/dev/null
    ifconfig wlan0 down
    ;;
esac

exit 0
```

然后执行：

```sh
wpa_passphrase "Redmi K60" hyperlovimia > /etc/wpa_supplicant.conf
chmod 600 /etc/wpa_supplicant.conf
chmod +x /etc/init.d/S41wifi
reboot
```

如果你只是先在当前板子上验证，直接改运行中的 rootfs：
- `/etc/wpa_supplicant.conf`
- `/etc/init.d/S41wifi`

如果你想让重新编译出来的固件也自带这个功能，就把同样的文件放到源码 overlay 里：
- [board/common/post_copy_rootfs/etc/network/interfaces](/home/hyperlovimia/k230_sdk/board/common/post_copy_rootfs/etc/network/interfaces:1) 是当前 rootfs 模板来源之一
- 你可以新增 `k230_sdk/board/common/post_copy_rootfs/etc/init.d/S41wifi`
- 也可以新增 `k230_sdk/board/common/post_copy_rootfs/etc/wpa_supplicant.conf`
