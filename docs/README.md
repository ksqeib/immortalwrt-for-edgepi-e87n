# docs

| 文件 | 内容 |
| --- | --- |
| `移植方案.md` | 完整移植分析：血缘、改动依据、风险表、未验证项 |
| `补丁说明.md` | 逐个文件的改动说明、构建机的坑（LLVM OOM、镜像源、下载分离） |
| `diag-on-device.sh` | 设备端诊断脚本，只用 `/sys` 与 `/proc`，不依赖 ethtool/lsblk/lspci |
| `fix-tree.sh` | 补丁内容变过之后，把源码树复位再重新打补丁 |
| `luci-app-Airpifanctrl-核查.md` | 第三方风扇插件的核查结论：它把 h5000m 与 E87N 当同一设备，两处对 E87N 不适用 |
| `istoreos-补丁对-6.18-的取舍.md` | istoreos 那 15 个内核补丁逐项核对：12 个已进上游、2 个仍需、1 个不需要 |

## 快速上手

`apply.sh` 做三件事：打主补丁、把 `patch/999-nv3007-fbtft.patch` 装进
`target/linux/mediatek/patches-6.18/`、把 `package/e87n-screen/` 拷进树的 `package/`。

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

## 构建 config 说明

`configs/e87n.config` 分两部分：

**目标与硬件**（`CONFIG_TARGET_*`）—— 只选 target 和镜像格式，硬件包全部来自
`filogic.mk` 里的 `Device/edgepi_e87n` 块，保证 `make defconfig` 可复现。

**用户空间定制**（`CONFIG_PACKAGE_*`）—— 界面、语言与屏幕：

| 项 | 真名 | 说明 |
| --- | --- | --- |
| 中文语言开关 | `CONFIG_LUCI_LANG_zh_Hans` | `default-settings-chn` 的硬依赖；各 `luci-i18n-*-zh-cn` 的 DEFAULT 也挂在它上面，开了它才自动带上 |
| 中文默认设置 | `luci-app` → `default-settings-chn` | immortalwrt 特有，来自 `package/emortal/default-settings` |
| 主题 | `luci-theme-material` | |
| 应用 | 见下 | 全部来自 luci feed |

启用的应用：`luci-app-firewall`、`luci-app-package-manager`、`luci-app-advanced-reboot`、
`luci-app-autoreboot`、`luci-app-cloudflared`、`luci-app-ddns-go`、`luci-app-filemanager`、
`luci-app-ttyd`、`luci-app-uhttpd`、`luci-app-wol`，外加 `btop`。

屏幕四行：`video-support`（**必须显式选**，见下）、`kmod-fb`（级联打开整个
fbdev 栈）、`kmod-fb-tft-nv3007`（面板驱动，来自 `patch/999-nv3007-fbtft.patch`）、
`e87n-screen`（背光用户态，来自 `package/e87n-screen/`）。后两者由 `apply.sh`
装进树，缺了会被 `defconfig` 静默丢弃。

`video-support` 是 `kmod-fb` 与 `kmod-backlight` 的硬依赖（`DEPENDS` 里没写 `+`），
而它默认不选。缺了它，整条 fbdev 链会被 `defconfig` 静默丢弃。

### 两个易错点

1. **`luci-app-ddnsgo` 不存在**，真名是 `luci-app-ddns-go`（`feeds/luci/applications/luci-app-ddns-go`）。
2. **`luci-i18n-*` 不是目录**，由 `feeds/luci/luci.mk` 的 `LuciTranslation` 宏按
   `po/<lang>/` 自动生成子包，所以 `find` 找目录会误判为"不存在"。

`make defconfig` 对不存在的 `CONFIG_PACKAGE_xxx` 是**静默丢弃**，不报错。改完 config
务必回查：

```sh
for s in CONFIG_PACKAGE_video-support CONFIG_PACKAGE_kmod-fb \
         CONFIG_PACKAGE_kmod-fb-tft-nv3007 CONFIG_PACKAGE_e87n-screen \
         CONFIG_LUCI_LANG_zh_Hans; do
  grep -q "^$s=y" .config && echo "OK   $s" || echo "丢弃 $s"
done
```
