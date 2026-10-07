# docs

| 文件 | 内容 |
| --- | --- |
| `移植方案.md` | 完整移植分析：血缘、改动依据、风险表、未验证项 |
| `补丁说明.md` | 逐个文件的改动说明、构建机的坑（LLVM OOM、镜像源、下载分离） |
| `diag-on-device.sh` | 设备端诊断脚本，只用 `/sys` 与 `/proc`，不依赖 ethtool/lsblk/lspci |
| `fix-tree.sh` | 补丁内容变过之后，把源码树复位再重新打补丁 |

## 快速上手

```sh
git clone -b master https://github.com/immortalwrt/immortalwrt.git
bash apply.sh "$PWD/immortalwrt"
cd immortalwrt
cp configs/e87n.config .config
./scripts/feeds update -a && ./scripts/feeds install -a
make defconfig
make download -j"$(nproc)"
make -j"$(nproc)" V=s
```

设备端诊断：

```sh
scp docs/diag-on-device.sh root@<路由器IP>:/tmp/
ssh root@<路由器IP> "sh /tmp/diag-on-device.sh"
```
