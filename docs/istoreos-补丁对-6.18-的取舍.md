# istoreos 那 15 个内核补丁，在 6.18 上还剩什么

istoreos-for-edgepi-e87n 基于 **iStoreOS 24.10 / 内核 6.6**，为了支持 MT7987
往内核里塞了 15 个补丁，共 3635 行。本移植基于**官方 immortalwrt master /
内核 6.18**，逐项核对这些补丁后发现：**12 个已进上游，2 个仍需自己带，1 个不需要。**
其中 2 个里，`999-nv3007-fbtft` 已在本仓库实现，只剩 `790` 未做。

核对方法：在构建机已展开的 6.18.54 内核源码里 grep 每个补丁的核心符号。

## 已经进上游的（12 个，共 3544 行，全部可丢弃）

| 补丁 | 行数 | 6.18 上游证据 |
| --- | --- | --- |
| `360-pinctrl-mediatek-add-mt7987-pinctrl-support` | 793 | `mediatek,mt7987-pinctrl` 在，`pinctrl-mt7987.c` 存在 |
| `361-clk-mediatek-add-mt7987-clock-drivers-support` | 1187 | `COMMON_CLK_MT7987[_ETHSYS]` 在 `filogic/config-6.18` 里 `=y` |
| `740-net-pcs-mtk_lynxi-add-mt7987-support` | 36 | `pcs-mtk-lynxi.c` 支持的 SoC 列表含 `mt7987` |
| `750-net-ethernet-mtk_eth_soc-add-mt7987-support` | 325 | 网口实测已通（双 2.5G，eth1 协商到 2500Mbps） |
| `751-...-revise-hardware-configuration-for-mt7987` | 79 | 同上 |
| `752-net-phy-mediatek-i2p5g-add-support-for-mt7987` | 415 | `mtk-2p5ge.c` 里 27 处 `mt7987`，已编成 `.ko` |
| `753-...-lock-mt7987-firmware-init` | 56 | 同上文件 |
| `754-...-fix-mt7987-led-polarity` | 13 | 同上文件 |
| `755-...-delay-mmd-writes` | 50 | 同上文件 |
| `821-add-pwm-feature-for-mt7987` | 44 | `pwm-mediatek.c` 里 `mt7987_pwm_data` 与 `mediatek,mt7987-pwm` 都在 |
| `831-...-lvts_thermal-Add-MT7987-support` | 87 | `lvts_thermal.c` 里 17 处 `mt7987` |
| `844-cpufreq-mediatek-Add-support-for-MT7987` | 22 | `mediatek-cpufreq.c` 里 2 处 `mt7987` |

前五个（pinctrl / clk / ethernet / pcs）是当年 MT7987 刚支持时的核心补丁，
现在全部进主线——这也是本移植的主补丁只有 8 个文件、476 行新增就能启动的根本原因。

## 仍然需要的（2 个）

| 补丁 | 行数 | 说明 | 本仓库怎么处理 |
| --- | --- | --- | --- |
| `999-nv3007-fbtft` | 307 | NV3007 面板驱动。6.18 的 fbtft 有框架但无此驱动 | `patch/999-nv3007-fbtft.patch` + `package/e87n-screen/`。**已做成可加载模块**：只有 8 行改既有文件（Kconfig 一条 + Makefile 一行），其余 272 行是新增 `fb_nv3007.c` |
| `790-net-phy-realtek-add-led-link-select-for-RTL8221` | 88 | 允许从 DT 配 RTL8221B 的 LED 行为。**6.18 上游没有**（realtek 驱动里 0 处匹配） | 未做。影响的是外接 PHY 的指示灯行为，非功能必需 |

关于 `790` 的实际影响：RTL8221B 默认把 3 个 LED 分别映射到 10M/100M/1G 速度，
若板上只接了其中一个 LED，则**只有在 10M 速率下才会闪**。我们的 E87N 两个 GPIO 灯
都不经 PHY，所以这个补丁目前不影响已实现的功能。日后若要调外接口的指示灯行为再说。

## 不需要的（1 个）

| 补丁 | 行数 | 说明 |
| --- | --- | --- |
| `320-hwrng-add-driver-for-MediaTek-TRNG-SMC` | 133 | 6.18 有 `mtk-rng.c`（`mtk-rng-v2.c` 不存在）。硬件真随机数，非必需 |

## 一句话总结

当初 3635 行的内核改动，在 6.18 上**只剩 307 行值得搬**，而且那 307 行里
**272 行是一个新文件、8 行是两处 Kconfig/Makefile 条目**——正好适合做成可加载模块（`fb_nv3007.ko`）。
