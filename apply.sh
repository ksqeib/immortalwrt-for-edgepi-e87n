#!/bin/bash
# 把 E87N 支持应用到一棵干净的官方 immortalwrt master 树上。
# 用法：bash apply.sh /path/to/immortalwrt
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$ROOT/immortalwrt}"

[ -d "$SRC/.git" ] || { echo "ERROR: $SRC 不是 immortalwrt 源码树"; exit 1; }
cd "$SRC"

echo "==> 检查树是否干净"
if [ -n "$(git status --porcelain)" ]; then
	echo "ERROR: 工作区有未提交改动，请先 git checkout -- . 或换个干净副本"
	git status --porcelain | head
	exit 1
fi

echo "==> 应用补丁"
git apply "$ROOT/patch/e87n-openwrt.patch"
echo "    ✓ 6 个文件已改（新增 configs/e87n.config、DTS、99-e87n-hw-offload；改 filogic.mk / 02_network / platform.sh）"

echo "==> 校验关键文件"
for f in configs/e87n.config \
         target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts \
         target/linux/mediatek/filogic/base-files/etc/uci-defaults/99-e87n-hw-offload; do
	[ -f "$f" ] && echo "    ✓ $f" || echo "    ✗ 缺 $f"
done

echo "==> 完成。接着执行："
echo "    cp configs/e87n.config .config"
echo "    ./scripts/feeds update -a && ./scripts/feeds install -a"
echo "    make defconfig"
echo "    make download -j\$(nproc)"
echo "    make -j\$(nproc) V=s"
