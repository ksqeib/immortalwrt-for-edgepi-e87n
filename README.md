# immortalwrt-for-edgepi-e87n

为 **EdgePi (Hiveton / Airpi) E87N** 在**官方 immortalwrt** 上做的最小化移植。

基线：`immortalwrt/immortalwrt` master 分支，内核 **6.18**（HEAD `8735c686ae`）。
E87N 硬件：MT7987A、1 GiB DDR4、8 GB eMMC、双 2.5G 网口、双 M.2 NVMe 2230、USB 3.2 A 口、NV3007 1.65" 屏、PWM 风扇。**无无线硬件。**

## 与其它 E87N 移植的区别

| | 本项目 | EN87-openwrt | istoreos-for-edgepi-e87n |
| --- | --- | --- | --- |
| 基线 | 官方 immortalwrt master | immortalwrt-mt798x-6.6 fork | iStoreOS 24.10（6.6） |
| 内核 | 6.18 | 6.6 | 6.6 |
| 改动量 | **490 行新增、0 行删除** | 厂商 HNAT + 无线包 | 15 个 6.6 内核补丁 |
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
| `package/e87n-screen/` | 背光用户态包（`screen-ctl` + `screen-test` + init.d + uci），`DEPENDS:=+kmod-fb-tft-nv3007` |
| `package/e87n-display/` | 面板状态页 C 渲染器。自带 Oswald 字模，直写 `/dev/fb0` 画彩色仪表盘 |
| 板级 DTS | `&spi2` 与 `display@0` 节点已放开，`spi-max-frequency = <52000000>` |
| `patch/e87n-openwrt.patch` | 含 `video.mk` 的 `kmod-fb-tft-nv3007` 包定义，以及设备块里的 `video-support` |

`video-support` 必须显式选中：`kmod-fb` 与 `kmod-backlight` 对它是**不带 `+` 的硬依赖**（`video.mk` 里写的是 `DEPENDS:=video-support ...`），会渲染成 `depends on PACKAGE_video-support` 而非 `select`；而它的默认值是 `m if ALL||ALL_KMODS`，两个都不开时为 n。少了它，`kmod-fb` 被 `defconfig` 静默丢弃，整条 fbdev 链（`kmod-fb-tft-nv3007`、`e87n-screen`）跟着消失。上游同类先例是 ipq40xx 的 `ubnt_utr`，把 `video-support` 直接写进 `DEVICE_PACKAGES`。

**背光不能用 `pwm-backlight`。** istoreos 实测：该驱动会抢占 GPIO524
（`PCM_MCK_I2S_MCLK` / `pwm2_0`），结果既点不亮屏，又让该脚无法 export，屏幕全黑。
原厂 HiGoROS 的 `start_display` 也是直接操作 GPIO524。所以 DTS 里 PWM2 保持未 mux、
不定义 backlight 节点，背光交给 `e87n-screen` 的 `screen-ctl`。

驱动来自 EN87 厂商内核镜像恢复出的原厂实现，寄存器序列与 MADCTL 映射取自
原始 Image，不是公开的 LVGL/ArduinoGFX 寄存器表——后者在这块屏上会显示乱码。

驱动只暴露一个运行时热调参数（`/sys/module/fb_nv3007/parameters/madctl`）：
默认 -1，表示按 DTS 里的 `rotate` 自动取 MADCTL；`rotate = <270>` 时取 `MY|MV`
即 0xA0。刷机后可 SSH 直接改这个值试别的朝向，不用重编。

点亮后 `/dev/fb0` 是 428x142 RGB565 裸帧（整帧 121552 字节）。istoreos 另带一个
LVGL 9.4 的 GUI 应用（istoreos 里也叫 `e87n-display`，4 个页面），本仓库未收录；
本仓库的 `package/e87n-display/` 是同名的**另一件事**（自写 C 渲染器，见下方
「屏幕状态页」）。用 `screen-ctl` 可以直接测面板与排线：

```sh
screen-ctl status     # 看 /dev/fb0 在不在
screen-ctl white      # 全屏白，验证面板与排线
screen-ctl on|off     # 背光
screen-ctl snapshot   # 导出当前帧
```

### 让它显示内容

驱动装上后屏幕是亮的但全空——**固件里没有任何程序往 `/dev/fb0` 写字**。
`init.d/screen` 只做一件事：调 `screen-ctl on` 点亮背光。而 `bootargs` 里
`console=ttyS0`，内核消息走串口，面板永远收不到。所以"亮而空"是预期行为。

**两条已验证的显示路径**：

**① fbcon（不用重编固件，最快出文字）**。内核的 framebuffer console 已经编进去了
（`/sys/class/graphics/` 里能看到 `fbcon`），把它绑到 fb0 就能让 console 输出落到面板：

```sh
# 找名字含 "frame buffer device" 的那个 vtcon
for c in /sys/class/vtconsole/vtcon*; do echo "$c: $(cat $c/name)"; done
echo 1 > /sys/class/vtconsole/vtcon1/bind
echo "E87N" > /dev/tty0
```

**② 直接写帧（内容完全可控）**。整帧 428x142 RGB565 小端，121552 字节。
电脑上用 `tools/mk-frame.pl` 生成，传到设备用 `screen-ctl raw` 显示：

```sh
perl tools/mk-frame.pl /tmp/f.raw orient   # 或 text/bars/checker/gradient/gray/white/black
scp /tmp/f.raw root@<路由器IP>:/tmp/
ssh root@<路由器IP> "screen-ctl raw /tmp/f.raw"
```

`orient` 图案是专为对朝向做的：fb 是横的 428x142，面板物理是竖的 142x428，
驱动靠 MADCTL 的 MV 位交换行列，所以画出来的东西可能转了 90 度或镜像。
四角红/绿/蓝/白各不相同、黄块只占左上半区，看一眼就知道映射关系。

**③ 设备端内置工具（重编固件后常驻）**。`package/e87n-screen` 里的
`screen-test` 能在设备上直接画色带、棋盘格和 orient 图案，不需要传文件：

```sh
screen-test bars      # 8 条竖色带
screen-test orient    # 朝向测试
screen-test checker   # 棋盘格
screen-test info      # fb 尺寸、驱动热调参数、fbcon 状态
```

它先在临时文件上量字节数，不符就中止、不写 fb——因为图案由 `awk` 的
`printf "%c"` 逐字节产出，而部分 awk 实现对数值 0 不输出 NUL 字节，
那样 `black`/`checker` 会静默写出半帧错位数据。

**关于 istoreos 的屏幕 GUI**：istoreos 带一个 LVGL 9.4 应用（在那边也叫
`e87n-display`，自研 45870 行 + 内嵌 lvgl 26MB，读 `/proc`、`/sys` 画系统概况/
时间/网速/圆弧四页）和一个 LuCI 界面（`luci-app-e87n`，含 `screen.js` 与
`cgi-bin/e87n`）。本仓库未收录这两个，因为前者远大于本移植的全部改动量，后者依赖
风扇脚本包（而我们的风扇走内核 thermal）。本仓库**自己的** `package/e87n-display/`
与它无关，是自写的 C 渲染器，见下方「屏幕状态页」。

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
for s in CONFIG_PACKAGE_video-support CONFIG_PACKAGE_kmod-fb \
         CONFIG_PACKAGE_kmod-fb-tft-nv3007 CONFIG_PACKAGE_e87n-screen \
         CONFIG_LUCI_LANG_zh_Hans; do
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

### 屏幕状态页

面板点亮后是空的——没有任何程序往 `/dev/fb0` 写字。`package/e87n-display` 负责
把状态画上去：直接 mmap `/dev/fb0`，画彩色的大字号仪表盘。

```sh
e87n-display once      # 只画一帧
e87n-display           # 前台刷新（2fps）
e87n-display daemon    # 后台常驻，开机由 init.d/e87n-display 拉起
e87n-display stop      # 停掉并解除清屏
e87n-display dark      # 清成黑屏
e87n-display test      # 画棋盘格与四角异色块，验证面板映射
```

从左到右分三栏：左栏是 56px 的温度大字与时钟（带秒块）；右上是一条 240 秒的
速率柱状图，标 MAX/MIN；右下是 CPU / MEMORY / CLIENTS 三个指标，前两个带进度条
（低于 50% 绿、50-79% 黄、80% 以上红），底行显示 WAN 地址与链路状态。

**字模是预先烘好的。** 字形来自 `Oswald.ttf`（SIL OFL 1.1），由
`tools/raster-font.js` 在开发机上栅格化成 `src/e87n-font.h` 里的覆盖率数组
（56/34/14/10px 四档）。所以渲染器**不链 FreeType、运行时也不带字体文件**，
依赖只剩 libc。改字模时才需要跑一次：

```sh
node tools/raster-font.js \
  package/e87n-display/files/usr/share/e87n-display/Oswald.ttf \
  package/e87n-display/src/e87n-font.h
```

**不抄 EN87 的 `display-control`。** 那包是一个 744 行的 FreeType 渲染器加一个
2.1 MB 的专有 AArch64 二进制（`files/usr/sbin/display`），还依赖
`libstdcpp`。外观参考它，代码是自己写的，也没有那份二进制。

**渲染器与 fbcon 只能择一。** fbcon 绑定后会在 `write()` 时重绘整个面板，把
像素盖掉，所以 `e87n-display` 启动时会先解绑 fbcon。`e87n-screen` 的
`/etc/config/screen` 因此把 `status` 默认为 0，把 `/dev/fb0` 让给本包。

`e87n-screen` 里还留着一个 fbcon 版 `screen-status`，作为没有 C 渲染器时的回退
（内核自带 8x16 字体与 VT 转义解析，428x142 上是 53 列 x 8 行）。要改用回退版：

```sh
uci set screen.global.status=1
uci set e87n-display.settings.enabled=0
uci commit
/etc/init.d/screen restart
/etc/init.d/e87n-display restart
```

面板上只出现 ASCII：内核字体只有拉丁字形，写中文会显示成方块，所以标签都用英文。
要关掉状态页只留背光：

```sh
uci set e87n-display.settings.enabled=0
uci commit e87n-display
/etc/init.d/e87n-display restart
```
