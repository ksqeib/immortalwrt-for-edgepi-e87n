#!/bin/bash
# EdgePi E87N —— 在构建机上后台启动 build.sh，然后立刻打印启动确认。
#
# 用法（构建机上）：
#     bash start-build.sh          # 起一次；已在跑就不重复起
#     bash start-build.sh status   # 只看进度，不动进程
#
# 为什么要有这个：编译要一两个小时，SSH 一断前台进程就没了。
# 这里用 setsid 彻底脱离终端，日志写文件，随时可以回来看。
set -u

REPO="${E87N_REPO:-$(cd "$(dirname "$0")" && pwd)}"
H="$HOME"
CONSOLE="$H/run-console.log"
LOG="$H/e87n-build.log"

# 只匹配 build.sh 本身，不匹配 start-build.sh。
# 注意 "start-build.sh" 里含子串 "build.sh"，所以不能用 pgrep -f "build.sh"
# ——那会匹配到本脚本自己的命令行，永远显示"已经在跑"，pkill 时还会把自己杀掉。
# 要匹配的是 "bash <某个路径>/build.sh"，末尾必须是 /build.sh，
# 于是 ".../start-build.sh" 不会被命中。
PAT='bash .*/build\.sh$'

running() { pgrep -f "$PAT" >/dev/null 2>&1; }

show() {
	echo "=== 进程 ==="
	if running; then
		pgrep -af "$PAT"
	else
		echo "  没在跑"
	fi
	echo
	echo "=== 日志文件 ==="
	ls -l "$CONSOLE" "$LOG" 2>&1
	echo
	echo "=== 已完成的步骤 ==="
	grep -a "^\[.*\] ===== " "$LOG" 2>/dev/null | tail -8 || echo "  （还没到任何步骤）"
	echo
	echo "=== e87n-build.log 末尾 25 行 ==="
	tail -25 "$LOG" 2>/dev/null || echo "  （还没写）"
}

if [ "${1:-}" = "status" ]; then
	show
	exit 0
fi

if running; then
	echo "已经在跑了，不重复启动："
	pgrep -af "$PAT"
	echo
	show
	exit 0
fi

echo "=== 启动 $REPO/build.sh（脱离终端，SSH 断开也不影响）==="
setsid bash "$REPO/build.sh" > "$CONSOLE" 2>&1 < /dev/null &
sleep 8

echo
show
echo
echo "看进度：  bash $REPO/start-build.sh status"
echo "跟日志：  tail -f $HOME/e87n-build.log"
echo "停掉：    pkill -f 'bash .*/build\.sh'"
