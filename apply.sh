#!/bin/bash
# 把 E87N 支持应用到一棵干净的官方 immortalwrt master 树上。
#
# 用法：bash apply.sh /path/to/immortalwrt
#
# 做三件事：
#   1. git apply patch/e87n-openwrt.patch
#      （新增 DTS、config 种子、99-e87n-theme；改 filogic.mk / 01_leds /
#        02_network / platform.sh / video.mk）
#   2. 把内核补丁 patch/999-nv3007-fbtft.patch 装进
#      target/linux/mediatek/patches-6.18/。构建时内核解包后会自动套用它，
#      编出 fb_nv3007.ko。
#   3. 把 package/e87n-screen/ 拷进树的 package/。它是屏幕背光用户态包
#      （screen-ctl + init.d + uci），OpenWrt 会自己发现并打包。
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$ROOT/immortalwrt}"

[ -d "$SRC/.git" ] || { echo "ERROR: $SRC 不是 immortalwrt 源码树"; exit 1; }
cd "$SRC"

echo "==> 检查树是否干净（只看已跟踪文件；未跟踪的杂项如 .idea/ 不拦）"
DIRTY="$(git status --porcelain --untracked-files=no)"
if [ -n "$DIRTY" ]; then
	echo "ERROR: 工作区有未提交改动，请先 git checkout -- . 或换个干净副本"
	echo "$DIRTY" | head
	exit 1
fi

echo "==> [1/3] 应用主补丁"
git apply "$ROOT/patch/e87n-openwrt.patch"
echo "    OK  DTS / config / filogic.mk / 01_leds / 02_network / platform.sh / video.mk"

echo "==> [2/3] 安装内核补丁（NV3007 面板驱动）"
KP="$SRC/target/linux/mediatek/patches-6.18/999-nv3007-fbtft.patch"
install -D -m644 "$ROOT/patch/999-nv3007-fbtft.patch" "$KP"
echo "    OK  $(basename "$KP") ($(wc -l < "$KP") 行)"

echo "==> [3/3] 安装屏幕用户态包"
rm -rf "$SRC/package/e87n-screen"
cp -r "$ROOT/package/e87n-screen" "$SRC/package/e87n-screen"
echo "    OK  package/e87n-screen ($(find "$SRC/package/e87n-screen" -type f | wc -l) 个文件)"

echo "==> 校验"
fail=0
for f in configs/e87n.config \
         target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts \
         target/linux/mediatek/patches-6.18/999-nv3007-fbtft.patch \
         package/e87n-screen/Makefile; do
	if [ -f "$f" ]; then
		echo "    OK  $f"
	else
		echo "    缺  $f"
		fail=1
	fi
done
[ "$fail" = 0 ] || exit 1

echo "==> 完成。接着执行："
echo "    cp configs/e87n.config .config"
echo "    ./scripts/feeds update -a && ./scripts/feeds install -a"
echo "    make defconfig"
echo "    make download -j\$(nproc)"
echo "    make -j\$(nproc) V=s"
