#!/bin/sh
# /usr/sbin/fb-sweep : 扫 fb_nv3007 的 MADCTL 覆写值，找出这块面板正的朝向。
#
# 背景：面板显示的图案错位（横条、剪切），而纯色填充正常。那是窗口寻址或
# 行列交换方向不对——纯色看不出回卷，结构化图案才暴露。
#
# 驱动现在只暴露一个模块参数（对应厂商原始实现的 MADCTL 覆写）：
#   /sys/module/fb_nv3007/parameters/madctl
#   -1  = 用驱动内置的按 rotate 取值（默认，rotate=270 -> MY|MV = 0xA0）
#   0x00..0xFF = 强制该值
# 运行时可写，改完立刻生效（驱动每次绘制都重读）。
#
# 用法（在路由器上）：
#   fb-sweep cur                     打印当前值
#   fb-sweep set <m> <frame.raw>     设一个 MADCTL 值并显示
#   fb-sweep auto <frame.raw> [hold] 自动轮扫 8 个值，每 hold 秒换一个
set -u

P=/sys/module/fb_nv3007/parameters/madctl
FB=/dev/fb0

[ -f "$P" ] || { echo "fb-sweep: 找不到 $P（驱动没加载？）" >&2; exit 1; }

get() { cat "$P" 2>/dev/null; }
setp() { printf '%s\n' "$1" > "$P" 2>/dev/null || { echo "写 $P 失败" >&2; return 1; }; }

show_frame() {
	[ -f "$1" ] || { echo "fb-sweep: 读不到 $1" >&2; return 1; }
	cat "$1" > "$FB" 2>/dev/null || { echo "fb-sweep: 写 $FB 失败" >&2; return 1; }
}

good() {
	echo
	echo "  找到正的朝向值后，持久化到 /etc/modules.d/ :"
	echo "    echo 'fb_nv3007 madctl=0xNN' > /etc/modules.d/fb_nv3007"
	echo "  或者写进板级 DTS 的 display@0（需要重编固件）。"
}

case "${1:-}" in
	cur)
		echo "madctl = $(get)"
		;;

	set)
		[ $# -ge 3 ] || { echo "用法: $0 set <madctl> <frame.raw>" >&2; exit 1; }
		setp "$2" || exit 1
		show_frame "$3" && echo "madctl=$2 已应用并显示 $3"
		good
		;;

	auto)
		F="${2:-}"
		HOLD="${3:-4}"
		[ -n "$F" ] || { echo "用法: $0 auto <frame.raw> [hold秒]" >&2; exit 1; }
		for m in 0xA0 0x00 0x20 0x40 0x60 0x80 0xC0 0xE0; do
			setp "$m" || exit 1
			show_frame "$F" || exit 1
			printf '  madctl=%s  （记下这一组的编号）\n' "$m"
			sleep "$HOLD"
		done
		echo "扫完 8 组。把显示正常的那组 madctl 告诉我。"
		good
		;;

	*)
		cat <<'USAGE' >&2
用法:
  fb-sweep cur
  fb-sweep set <madctl> <frame.raw>
  fb-sweep auto <frame.raw> [hold秒]

madctl 取值: 0x00 原生 / 0x20 MV / 0x40 MX / 0x60 MX|MV
             0x80 MY / 0xA0 MY|MV / 0xC0 MY|MX / 0xE0 MY|MX|MV
驱动内置值（rotate=270）是 0xA0。
USAGE
		exit 1
		;;
esac
