#!/bin/sh
# fan-sweep : 在设备上逐档扫风扇 PWM 并记录温度。
#
# 为什么需要它：温度压不下来时，先要分清是「内核曲线给的速度太低」还是
# 「风扇本身吹不动」。把 PWM 从低到高扫一遍、每档停一会儿记温度：
#   温度随 PWM 明显下降 -> 曲线问题，抬 cooling-levels 即可；
#   到 255 也不降       -> 是风扇/风道的散热上限，改曲线只会更吵。
#
# 关键前提：必须先停掉内核热管理。mode=enabled 时 thermal governor 在持续
# 治理，会不断把 pwm1 改回当前档位值——这就是「pwm 写不动」的原因。
# 脚本用 trap 保证退出（含 Ctrl-C）时恢复原状态。
#
# 空载 vs 满载：**空载测不出风扇的作用**。空载时 CPU 发热少，散热片靠自然
# 对流就够，风扇吹与不吹只差 1 度左右，全落在测量误差里。要看风扇有没有用，
# 必须一边跑满四核一边扫档位。
#
# 用法（设备上）：
#   sh fan-sweep                    空载，默认档位，每档 20 秒
#   sh fan-sweep load               满载（四核跑满），默认档位，每档 40 秒
#   sh fan-sweep load 60 0 128 255  满载，每档 60 秒，自定义档位
#   FAN_SWEEP_ZONE=/sys/class/thermal/thermal_zone1 sh fan-sweep ...
set -u

MODE_ARG=""
case "${1:-}" in
	load) MODE_ARG=load; shift ;;
esac

if [ -n "$MODE_ARG" ]; then
	HOLD="${1:-40}"
else
	HOLD="${1:-20}"
fi
if [ "$#" -gt 1 ]; then shift; LEVELS="$*"; else LEVELS="0 64 128 192 255"; fi

# --- 找 pwmfan 的 hwmon：编号不固定，按 name 找 ---
PWM_DIR=""
for h in /sys/class/hwmon/hwmon*; do
	[ -r "$h/name" ] || continue
	if [ "$(cat "$h/name" 2>/dev/null)" = "pwmfan" ]; then PWM_DIR="$h"; break; fi
done
[ -n "$PWM_DIR" ] || { echo "找不到 pwmfan 的 hwmon（kmod-hwmon-pwmfan 没加载？）" >&2; exit 1; }

# --- 找带 active trip 的热区（可用 FAN_SWEEP_ZONE 覆盖）---
TZ="${FAN_SWEEP_ZONE:-}"
if [ -z "$TZ" ]; then
	for z in /sys/class/thermal/thermal_zone*; do
		[ -e "$z/trip_point_0_temp" ] || continue
		TZ="$z"; break
	done
fi
[ -n "$TZ" ] || { echo "找不到带 active trip 的 thermal zone" >&2; exit 1; }

PWM="$PWM_DIR/pwm1"; EN="$PWM_DIR/pwm1_enable"
MODE="$TZ/mode";    TEMP="$TZ/temp"

OLD_MODE="$(cat "$MODE" 2>/dev/null || echo enabled)"
OLD_EN="$(cat "$EN" 2>/dev/null || echo 2)"

echo "pwmfan   : $PWM_DIR"
echo "thermal  : $TZ  ($(cat "$TZ/type" 2>/dev/null))"
echo "初始状态 : pwm1=$(cat "$PWM" 2>/dev/null)  pwm1_enable=$OLD_EN  mode=$OLD_MODE"
echo "模式     : ${MODE_ARG:-空载}"
echo "逐档测试 : $LEVELS   每档 ${HOLD} 秒"
echo

LOAD_PIDS=""
stop_load() {
	[ -n "$LOAD_PIDS" ] || return 0
	# yes 会 fork 出子进程，按进程组杀不适用于 busybox，直接按名杀自己起的那些
	for pid in $LOAD_PIDS; do kill "$pid" 2>/dev/null || true; done
	killall yes 2>/dev/null || true
	LOAD_PIDS=""
}
restore() {
	stop_load
	printf '\n恢复 : mode=%s pwm1_enable=%s\n' "$OLD_MODE" "$OLD_EN"
	echo "$OLD_MODE" > "$MODE" 2>/dev/null || true
	echo "$OLD_EN"   > "$EN"   2>/dev/null || true
}
trap restore INT TERM EXIT

echo disabled > "$MODE" 2>/dev/null || { echo "停用热管理失败，中止（未改动任何东西）" >&2; exit 1; }
echo 1 > "$EN" 2>/dev/null || true

if [ -n "$MODE_ARG" ]; then
	n=0
	while [ "$n" -lt 4 ]; do
		yes > /dev/null 2>&1 &
		LOAD_PIDS="$LOAD_PIDS $!"
		n=$((n + 1))
	done
	echo "已在后台拉起 4 个 yes（退出时自动清理）"
	echo "先跑满速预热 30 秒，让温度到稳态……"
	echo 255 > "$PWM" 2>/dev/null || true
	sleep 30
	echo
fi

printf '%-6s %-12s %s\n' PWM 温度 相对上一档
prev=""
for p in $LEVELS; do
	if ! echo "$p" > "$PWM" 2>/dev/null; then
		echo "写 pwm1=$p 失败" >&2
		continue
	fi
	sleep "$HOLD"
	raw="$(cat "$TEMP" 2>/dev/null || echo 0)"
	cur="$(awk -v t="$raw" 'BEGIN{printf "%.1f", t/1000}')"
	delta=""
	[ -n "$prev" ] && delta="$(awk -v a="$prev" -v b="$cur" 'BEGIN{printf "%+.1fC", b-a}')"
	printf '%-6s %-12s %s\n' "$p" "${cur}C" "$delta"
	prev="$cur"
done

if [ -n "$MODE_ARG" ]; then
	echo
	echo "提示：满载扫描里 255 档的温度是这台机器的散热上限参考值。"
fi
