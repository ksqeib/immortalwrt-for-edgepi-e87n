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
# 用法（设备上）：
#   sh fan-sweep            默认扫 0 / 64 / 128 / 192 / 255，每档 20 秒
#   sh fan-sweep 30         每档 30 秒
#   sh fan-sweep 20 0 100 255   自定义档位
set -u

HOLD="${1:-20}"
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
echo "逐档测试 : $LEVELS   每档 ${HOLD} 秒"
echo

restore() {
	printf '\n恢复 : mode=%s pwm1_enable=%s\n' "$OLD_MODE" "$OLD_EN"
	echo "$OLD_MODE" > "$MODE" 2>/dev/null || true
	echo "$OLD_EN"   > "$EN"   2>/dev/null || true
}
trap restore INT TERM EXIT

echo disabled > "$MODE" 2>/dev/null || { echo "停用热管理失败，中止（未改动任何东西）" >&2; exit 1; }
echo 1 > "$EN" 2>/dev/null || true

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
