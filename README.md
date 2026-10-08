# immortalwrt-for-edgepi-e87n

为 **EdgePi (Hiveton / Airpi) E87N** 在**官方 immortalwrt** 上做的最小化移植。

基线：`immortalwrt/immortalwrt` master 分支，内核 **6.18**（HEAD `8735c686ae`）。
E87N 硬件：MT7987A、1 GiB DDR4、8 GB eMMC、双 2.5G 网口、双 M.2 NVMe 2230、USB 3.2 A 口、NV3007 1.65" 屏、PWM 风扇。**无无线硬件。**

## 与其它 E87N 移植的区别

| | 本项目 | EN87-openwrt | istoreos-for-edgepi-e87n |
| --- | --- | --- | --- |
| 基线 | 官方 immortalwrt master | immortalwrt-mt798x-6.6 fork | iStoreOS 24.10（6.6） |
| 内核 | 6.18 | 6.6 | 6.6 |
| 改动量 | **476 行新增、0 行删除** | 厂商 HNAT + 无线包 | 15 个 6.6 内核补丁 |
| 无线 | 不涉及（硬件没有） | 装了 MT7921/7922 全套驱动 | 不涉及 |
| 屏幕 | 有（做成可加载模块） | 有 | 有（作者未实机验证） |
| 风扇 | 内核 thermal 曲线，无脚本 | 厂商 fancontrol 脚本 | 厂商 fancontrol 脚本 |

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
- 硬件 offload：PPE 表里可见 `BND` 条目带真实硬件计数（`packets=45541 bytes=2897367`）

## 待验证

- **NVMe**：两个 PCIe 控制器都启用，pcie0 的 `reset-gpios` 指向 pio 36（与官方 h5000m 一致），但尚无 SSD 实测。
- **屏幕**：驱动与背光包已就绪，**未上机**。面板排线状态未知（istoreos 作者本人的排线座已损坏，其屏幕功能从未实机验证过）。
- **风扇曲线**：新的 52/68/80 °C 三段曲线是设计值，尚未在实机上跑过温度爬坡验证。

## 已结案

- **三个 LED**：板上第三个灯**硬连电源**，不受 GPIO 控制。曾对 pio 0–49 共 50 个引脚
  逐个 export、拉高、拉低（每次 1 秒），该灯全程无反应。因此不定义 `LED_FUNCTION_POWER`——
  电源指示由硬件承担，软件再定义一份是重复。另两个灯定稿为 `pio 4` = `green:status`（系统灯，
  承载 boot/running/failsafe/upgrade）、`pio 3` = `amber:wan`（绑到 `eth1`）。
- **WPS 按键不存在**：`pio 0` 读回恒低，是悬空引脚，不是按键。厂商 u-boot 提交
  `be8c37611` 删掉该节点的做法正确，本移植已同步删除。
- **USB 存储**：补上 `kmod-usb-storage` 等包后 U 盘可识别并挂载。

## 风扇：只用内核 thermal，不装任何脚本

`mt7987a.dtsi` 里的 `fan: pwm-fan` 由 `cpu-thermal` 的 cooling-maps 按下标驱动，
本移植只改了两处（都在板级 DTS 里，不动 `.dtsi`）：

| 项 | 上游 mt7987a.dtsi | 本移植 | 理由 |
| --- | --- | --- | --- |
| `cooling-levels` | `<0 128 192 255>` | `<0 110 175 255>` | 1 档起转电压。低于约 100 的风扇会「嗡嗡响但不转」，128 偏高、浪费静音空间 |
| 起步温度 | 40 °C | **52 °C** | 路由器待机自然积热就在 42–50 °C，40 °C 起步会在临界点反复启停 |
| 中档 | 85 °C | **68 °C** | 85 °C 才上二档太晚，热量早已传导到 PHY、USB、电容 |
| 满速 | 115 °C | **80 °C** | 115 °C 已贴近 SoC 的 125 °C critical trip，等不到就熔断了 |

滞回 4–5 °C（起步档 4 °C，其余 5 °C），避免温度在档位边缘抖动导致风扇抽搐。

**`critical` trip 保持 `mt7987a.dtsi` 的 125 °C 不动。** 那是 SoC 的硬件保护点，
不是本板的属性；调低它只会让内核更容易主动关机，并不会让板子变凉。

三段曲线：

```
<= 52 °C  0 档   PWM 0     停转
   52 °C  1 档   PWM 110   低速，刚过起转电压
   68 °C  2 档   PWM 175   持续高负载
   80 °C  3 档   PWM 255   全速
```

不采用厂商那套 25 级曲线：它必须配合原厂 `fancontrol` 守护进程才有意义，
而那个守护进程绕过 thermal zone 直接写 `pwm1`。本移植没有任何用户态碰 `pwm1`。

istoreos 那边必须手工强开一堆 FB 符号、还要用 python 往 `kernel-defaults.mk` 里注东西，
我们这边不需要：`CONFIG_STAGING=y` 在 `generic/config-6.18` 里本来就开着。

## 屏幕：可加载模块，不是内核补丁

结论是**能做成模块**，但拖出来的是整个 fbdev 栈。

istoreos 的 `999-nv3007-fbtft.patch` 共 307 行，**对既有内核文件的改动只有 8 行**
（`Kconfig` 一条 `config FB_TFT_NV3007` + `Makefile` 一行 `obj-$(CONFIG_FB_TFT_NV3007)`），
其余 272 行是新增 `drivers/staging/fbtft/fb_nv3007.c`。6.18 的 fbtft 框架里面板驱动
是 `tristate`，所以能编成 `fb_nv3007.ko`。

本移植把它落成树内正规形式，而不是外挂补丁：

| 位置 | 内容 |
| --- | --- |
| `patch/999-nv3007-fbtft.patch` | 内核补丁。构建时内核解包后自动套用，编出 `fb_nv3007.ko` |
| `package/e87n-screen/` | 背光用户态包（`screen-ctl` + init.d + uci），`DEPENDS:=+kmod-fb-tft-nv3007` |
| 板级 DTS | `&spi2` 与 `display@0` 节点已放开，`spi-max-frequency = <52000000>` |
| `patch/e87n-openwrt.patch` | 含 `video.mk` 的 `kmod-fb-tft-nv3007` 包定义 |

**背光不能用 `pwm-backlight`。** istoreos 实测：该驱动会抢占 GPIO524
（`PCM_MCK_I2S_MCLK` / `pwm2_0`），结果既点不亮屏，又让该脚无法 export，屏幕全黑。
原厂 HiGoROS 的 `start_display` 也是直接操作 GPIO524。所以 DTS 里 PWM2 保持未 mux、
不定义 backlight 节点，背光交给 `e87n-screen` 的 `screen-ctl`。

驱动自带四个运行时热调参数（`/sys/module/fb_nv3007/parameters/`）：
`madctl_v` / `swap_xy` / `off_x` / `off_y`。作者显然被这块屏折磨过——NV3007 的
MV 行列交换行为与 CGRAM 列偏移缺少可靠文档，各厂商资料不一致，所以刷机后可 SSH 直接调，
不用重编。

点亮后 `/dev/fb0` 是 428x142 RGB565 裸帧（整帧 121552 字节）。istoreos 另带一个
LVGL 9.4 的 GUI 应用（`e87n-display`，4 个页面），本仓库未收录；用 `screen-ctl`
可以直接测面板与排线：

```sh
screen-ctl status     # 看 /dev/fb0 在不在
screen-ctl white      # 全屏白，验证面板与排线
screen-ctl on|off     # 背光
screen-ctl snapshot   # 导出当前帧
```

## 构建

一步到位。`apply.sh` 会打主补丁、装内核补丁、拷屏幕包：

```sh
git clone -b master https://github.com/immortalwrt/immortalwrt.git
bash apply.sh "$PWD/immortalwrt"
cd immortalwrt
cp configs/e87n.config .config
./scripts/feeds update -a && ./scripts/feeds install -a
make defconfig
make download -j16          # 下载与编译分离，避免下载失败掀掉整个 world
make -j16 V=s
```

`make defconfig` 之后回查符号是否真落地（不存在的 `CONFIG_PACKAGE_xxx` 会被**静默丢弃**）：

```sh
for s in CONFIG_PACKAGE_kmod-fb-tft-nv3007 CONFIG_PACKAGE_e87n-screen CONFIG_LUCI_LANG_zh_Hans; do
  grep -q "^$s=y" .config && echo "OK   $s" || echo "丢弃 $s"
done
```

产物在 `bin/targets/mediatek/filogic/`，也由 `.github/workflows/build.yml` 自动出（手动触发）。

### 两个已知的构建坑

1. **`tools/llvm-bpf` 从源码编 LLVM 会 OOM。** `bridger` 在 filogic 默认包里，需要 BPF 工具链。树顶没有 `llvm-bpf/.llvm-version` 时，`toolchain/Config.in` 的 choice 会落到 `BUILD_LLVM`。解法是下载官方预编译包解包到树顶，`.llvm-version` 一出现就自动改选 `PREBUILT`，那 2.5 小时的编译整段消失。workflow 里已处理。

2. **`mirror2.immortalwrt.org` 会 302 跳到 `sourceforge.net`**，直连极慢。构建时全局导出代理。

### 内核补丁的行号已知会漂

`patch/999-nv3007-fbtft.patch` 里两个小 hunk 的行号（`@@ -65,6 +65,13 @@`、
`@@ -17,6 +17,7 @@`）照抄 istoreos 的 6.6 文件，6.18 里行号已变。hunk 内容本身
（Kconfig 条目、Makefile 行）是准确的，但 6.18 的 fbtft 目录里新增了不少面板驱动，
上下文行号需要核对。若 `git apply` 报错，用 `patch -p1 -l` 或 `git apply -C1` 放宽。

详见 `docs/`。
