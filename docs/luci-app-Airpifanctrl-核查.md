# luci-app-Airpifanctrl 核查（2026-10-07）

本地副本：`/d/self/op/openwrt-luci-app-Airpifanctrl`
上游：`https://github.com/ChenMercy/openwrt-luci-app-Airpifanctrl`
HEAD：`f4b0d68 fix: terminate default fan temperature source`

## 它把两块板当同一设备

它的文档标题是「EdgePi E87N / AirPi PWM风扇控制LuCI插件」，正文写
「本仓库由原始AirPi风扇控制程序整理而成」。原始程序来自 h5000m（AirPi），
E87N 只是在同一套代码上验证过一次。

具体混同点：

| 项 | h5000m | E87N | 它的假设 |
| --- | --- | --- | --- |
| WPS 键 | 有（`&pio 0`） | **无** | 假定有 → **错** |
| 无线 | 有（mt7996e） | **无** | 脚本调用 `wlan0` → **错** |
| PWM 通道 | `pwm 1` | `pwm 1` | 一致 |
| 温源 | `thermal_zone0` | `thermal_zone0` | 一致，但它会关掉 |

## 两处对 E87N 明确不适用

**1. `root/usr/bin/wps` 依赖无线。** 该文件被 `init.d` 拷成 `/etc/rc.button/wps`，
被按键触发时第一件事是：

```sh
hostapd_cli -i wlan0 wps_pbc     # 需要 wifi 与 WPS 键
```

E87N 两样都没有。这是从 h5000m 直接搬过来的文件。**若要用这个插件，必须删掉这一项。**
（顺带印证：厂商 uboot 项目提交 `be8c37611` 正是在 E87N 上删掉 WPS 按键节点。）

**2. 它会关掉内核热管理。** `root/etc/init.d/Airpifanctrl` 与 `fancts.sh` 都执行：

```sh
echo disabled > /sys/class/thermal/thermal_zone0/mode
```

然后由守护进程直接写 `pwm1`。而我们的移植依赖内核 cooling-maps 驱动风扇
（`mt7987.dtsi:313` 的 `cooling-maps`，引用 `<&fan 3 3>` 等）。**两者不能共存**：
装了它，内核热管理就失效。

## 与我们的做法对比

| | 我们的移植 | Airpifanctrl |
| --- | --- | --- |
| 风扇驱动 | 内核 `pwm-fan` + cooling-maps | 用户态守护进程直接写 `pwm1` |
| 温控 | 内核 thermal zone 自动 | 脚本每 8 秒读一次再判断 |
| 档位 | `cooling-levels = <0 128 192 255>`（未覆盖，跟官方） | 0/1/2/3 → PWM 64/128/192/255 |
| 界面 | 无 | LuCI 页面 |
| 依赖 wifi | 否 | **是**（wps 脚本） |

设备实测我们的风扇工作正常：`hwmon2 name=pwmfan pwm1=128 pwm1_enable=1`，
CPU 55~56 °C。

## 结论

**首版不要引入这个插件。** 理由：

1. 它的 WPS 脚本需要无线，而 E87N 无无线——留着是死代码，最坏情况是在按 reset 时
   触发一个调用 `wlan0` 的脚本。
2. 它会关掉内核热管理，与我们已验证可用的方案冲突。
3. 它的价值是 LuCI 界面与手动档位。若日后想要，需要先删 `wps` 文件、
   去掉 `thermal_zone0/mode` 那两行，才能与内核 coolings 共存。

其 PWM 路径 `/sys/devices/platform/pwm-fan/hwmon/hwmon2/pwm1` 与我们的设备实测一致，
但它自己的文档也提醒 hwmon 编号会变。
