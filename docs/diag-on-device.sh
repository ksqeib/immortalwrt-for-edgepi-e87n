#!/bin/sh
# E87N 设备端诊断。只用 /sys、/proc 和 busybox，不依赖 ethtool/lsblk/lspci/lsusb/evtest。
# 用法：把本文件传到路由器，执行 sh diag-on-device.sh

p(){ printf '\n===== %s =====\n' "$1"; }

p "1. 系统与内存"
uname -a
head -1 /etc/openwrt_release 2>/dev/null
free -m | head -2
grep -E ' / | /overlay' /proc/mounts | head -3

p "2. 网口状态（实际 MAC 应两两不同）"
for i in /sys/class/net/*; do
  n=$(basename "$i")
  [ "$n" = "lo" ] && continue
  printf '%-8s MAC=%-18s %-8s carrier=%s speed=%s duplex=%s\n' \
    "$n" "$(cat "$i/address")" "$(cat "$i/operstate")" \
    "$(cat "$i/carrier" 2>/dev/null)" "$(cat "$i/speed" 2>/dev/null)" \
    "$(cat "$i/duplex" 2>/dev/null)"
done
echo "--- IP ---"
ip -4 addr show 2>/dev/null | grep -E '^[0-9]+:|inet '

p "3. MDIO 总线上的 PHY 与驱动"
for d in /sys/bus/mdio_bus/devices/*; do
  [ -d "$d" ] || continue
  drv=$(basename "$(readlink -f "$d/driver" 2>/dev/null)" 2>/dev/null)
  printf '  %s -> %s\n' "$(basename "$d")" "${drv:-无驱动}"
done

p "4. PCIe（关键）"
for c in /sys/bus/platform/devices/*.pcie; do
  [ -e "$c" ] || continue
  echo "  $(basename "$c") driver=$(basename "$(readlink -f "$c/driver" 2>/dev/null)" 2>/dev/null)"
done
echo "--- PCI 设备 ---"
ls /sys/bus/pci/devices/ 2>/dev/null || echo "  无 PCI 设备（空槽，或链路未建立）"
echo "--- dmesg ---"
dmesg | grep -iE "pcie|ltssm|link down" | tail -25

p "5. NVMe"
ls /dev/nvme* 2>/dev/null || echo "  无 /dev/nvme*"
ls /sys/class/nvme/ 2>/dev/null || echo "  无 nvme 类设备"
grep -iE "nvme|pcie" /proc/modules 2>/dev/null

p "6. 存储与分区（sysupgrade 依赖 kernel/rootfs）"
ls /dev/mmcblk* 2>/dev/null
for d in /sys/block/mmcblk0/mmcblk0p*; do
  [ -e "$d" ] || continue
  echo "  $(basename "$d"): $(grep PARTNAME "$d/uevent")  $(cat "$d/size") 扇区"
done
tail -10 /proc/partitions
echo "--- 当前根文件系统 ---"
grep -E ' / | /overlay' /proc/mounts | head -3
echo "--- cmdline ---"
cat /proc/cmdline

p "7. 风扇与温度"
ls /sys/class/hwmon/
for h in /sys/class/hwmon/hwmon*; do
  echo "  $(basename "$h"): name=$(cat "$h/name" 2>/dev/null)"
  for inp in "$h"/pwm* "$h"/fan*_input "$h"/temp*_input; do
    [ -e "$inp" ] && echo "     $(basename "$inp")=$(cat "$inp" 2>/dev/null)"
  done
done
cut -c1-20 /sys/class/thermal/thermal_zone*/type /sys/class/thermal/thermal_zone*/temp 2>/dev/null

p "8. 按键"
grep -E 'Name|Handlers|EV=' /proc/bus/input/devices 2>/dev/null | head -20

p "9. USB"
dmesg | grep -iE "usb|xhci|vbus|dwc" | tail -25
ls /sys/bus/usb/devices/ 2>/dev/null

p "10. board.json 与启动脚本"
head -40 /etc/board.json 2>/dev/null
ls /etc/rc.d/ 2>/dev/null | grep -iE 'network|sysupgrade|fan|gpio|led'

p "11. 设备树板名与内存"
cat /sys/firmware/devicetree/base/model 2>/dev/null; echo
tr '\0' ' ' < /sys/firmware/devicetree/base/compatible 2>/dev/null; echo
dmesg | grep -iE 'Memory:|On node' | head -5

echo
echo "===== 诊断结束 ====="
