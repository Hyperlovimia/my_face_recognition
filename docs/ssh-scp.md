```sh
ssh-keygen -R 192.168.144.225  # 嵌入式下重刷系统，SSH key 会被重新生成
ssh root@192.168.144.225
```

```sh
scp -r ./sharefs/* root@192.168.144.225:/sharefs/  # 本地 → 远程
scp -r root@192.168.144.225:/sharefs/* ./sharefs/  # 远程 → 本地
```
