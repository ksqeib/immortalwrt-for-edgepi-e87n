# immortalwrt-for-edgepi-e87n

为 **EdgePi (Hiveton / Airpi) E87N** 在**官方 immortalwrt** 上做的最小化移植。

基线：`immortalwrt/immortalwrt` master 分支，内核 **6.18**（HEAD `8735c686ae`）。
E87N 硬件：MT7987A、1 GiB DDR4、8 GB eMMC、双 2.5G 网口、双 M.2 NVMe 2230、USB 3.2 A 口、NV3007 1.65" 屏、PWM 风扇。**无无线硬件。**

## 与其它 E87N 移植的区别

| | 本项目 | EN87-openwrt | istoreos-for-edgepi-e87n |
| --- | --- | --- | --- |
| 基线 | 官方 immortalwrt master | immortalwrt-mt798x-6.6 fork | iStoreOS 24.10（6.6） |
| 内核 | 6.18 | 6.6 | 6.6 |
| 改动量 | **360 行新增、0 行删除** | 厂商 HNAT + 无线包 | 15 个 6.6 内核补丁 |
| 无线 | 不涉及（硬件没有） | 装了 MT7921/7922 全套驱动 | 不涉及 |
| 屏幕 | 暂未做 | 有 | 有（未实机验证） |

选择官方 master 而非 fork 的原因：MT7987 支持在 6.18 已进上游，厂商 fork 的价值只剩 HNAT，而官方原生方案（`mtk_eth_soc` + PPE + WED + nftables flowtable）能替代它。

## 已实测通过

- 启动、1 GiB 内存、eMMC 安装（`/overlay` 为 f2fs 挂 `/dev/loop0`）
- 分区名 `kernel`(p4) / `rootfs`(p5)，`sysupgrade` 走 `emmc_do_upgrade`
- 双网口：eth0 外接 RTL8221B（MDIO 地址 **3**）、eth1 内置 2.5G PHY（地址 15），实测 **2500 Mbps**
- MAC 由 eMMC CID 派生，两口不同
- 风扇注册在 `hwmon` 的 `pwmfan`，CPU 温度源为 `cpu_thermal`
- 按键：只有 reset（`pio 1`）。E87N 无 WPS 键，已从 DTS 删除
- LED：内核注册两个可控灯。`pio 4`（绿）= 系统灯，承载 boot/running/failsafe/upgrade；
  `pio 3`（琥珀）= WAN。板上第三个灯上电即常亮、与网线无关，任何 DTS 来源都未声明它，
  充当硬件电源指示，故不定义 `LED_FUNCTION_POWER`
- USB 3.2 口供电与枚举

## 待验证

- **NVMe**：两个 PCIe 控制器的 `reset-gpios` 都指向 pio 36（与官方 h5000m 一致），但尚无 SSD 实测。
- **屏幕**：NV3007 需要把 `fb_nv3007` 驱动移植到 6.18。注意背光**不能**用 `pwm-backlight`（会抢占 GPIO524 导致黑屏），应走 GPIO 直接控制。

## 已结案

- **三个 LED**：板上第三个灯**硬连电源**，不受 GPIO 控制。曾对 pio 0–49 共 50 个引脚
  逐个 export、拉高、拉低（每次 1 秒），该灯全程无反应。因此不定义 `LED_FUNCTION_POWER`——
  电源指示由硬件承担，软件再定义一份是重复。另两个灯定稿为 `pio 4` = `green:status`（系统灯，
  承载 boot/running/failsafe/upgrade）、`pio 3` = `amber:wan`（绑到 `eth1`）。
- **WPS 按键不存在**：`pio 0` 读回恒低，是悬空引脚，不是按键。厂商 u-boot 提交
  `be8c37611` 删掉该节点的做法正确，本移植已同步删除。
- **硬件 offload**：`flow_offloading` 与 `flow_offloading_hw` 由 fw4 的 fullcone 补丁默认开启，
  PPE 表里可见 `BND` 条目带真实硬件计数（实测 `packets=45541 bytes=2897367`），确认生效。
- **USB 存储**：补上 `kmod-usb-storage` 等包后 U 盘可识别并挂载。

## 构建

```sh
git clone -b master https://github.com/immortalwrt/immortalwrt.git
cd immortalwrt
git apply /path/to/patch/e87n-openwrt.patch
cp configs/e87n.config .config
./scripts/feeds update -a && ./scripts/feeds install -a
make defconfig
make download -j16          # 下载与编译分离，避免下载失败掀掉整个 world
make -j16 V=s
```

产物在 `bin/targets/mediatek/filogic/`。

### 两个已知的构建坑

1. **`tools/llvm-bpf` 从源码编 LLVM 会 OOM。** `bridger` 在 filogic 默认包里，需要 BPF 工具链。树顶没有 `llvm-bpf/.llvm-version` 时，`toolchain/Config.in` 的 choice 会落到 `BUILD_LLVM`。解法是下载官方预编译包解包到树顶，`.llvm-version` 一出现就自动改选 `PREBUILT`，那 2.5 小时的编译整段消失。

2. **`mirror2.immortalwrt.org` 会 302 跳到 `sourceforge.net`**，直连极慢。构建时全局导出代理。

详见 `docs/`。

## 刷机

见 `docs/`，以及仓库根目录的 `E87N-移植方案.md`（在上级目录）。
