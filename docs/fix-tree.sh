#!/bin/bash
# 把 immortalwrt 树恢复到 HEAD，再用仓库里的补丁重新打上。
#
# 用法：bash fix-tree.sh [/path/to/immortalwrt]
#
# 适用场景：改了 patch/e87n-openwrt.patch 之后，树里是旧内容，
# git apply 会报 "patch does not apply"。本脚本先复位再重打。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/immortalwrt}"

[ -d "$SRC/.git" ] || { echo "找不到 immortalwrt 树: $SRC"; exit 1; }
cd "$SRC"

echo "=== 1. 当前树状态 ==="
git status --porcelain | head -20

echo "=== 2. 恢复所有被修改的跟踪文件 ==="
git checkout -- .
echo "  已恢复"

echo "=== 3. 删掉旧补丁留下的新文件 ==="
rm -f configs/e87n.config
rm -f target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts
rm -f target/linux/mediatek/filogic/base-files/etc/uci-defaults/99-e87n-theme
rm -f target/linux/mediatek/patches-6.18/999-nv3007-fbtft.patch
rm -rf package/e87n-screen
rm -rf package/e87n-display
rmdir configs 2>/dev/null
echo "  已清理"

echo "=== 4. 确认树干净 ==="
n=$(git status --porcelain --untracked-files=no | wc -l)
echo "  已跟踪文件的未提交改动数: $n （应为 0）"

echo "=== 5. 用仓库里的补丁重打 ==="
for p in "$ROOT/patch/e87n-openwrt.patch"; do
	if git apply --check "$p" 2>&1; then
		git apply "$p" && echo "  已应用 $(basename "$p")"
	else
		echo "  无法应用: $p"; exit 1
	fi
done

echo "=== 6. 装内核补丁与屏幕包（与 apply.sh 一致）==="
install -D -m644 "$ROOT/patch/999-nv3007-fbtft.patch" \
	target/linux/mediatek/patches-6.18/999-nv3007-fbtft.patch
cp -r "$ROOT/package/e87n-screen" package/e87n-screen
cp -r "$ROOT/package/e87n-display" package/e87n-display
echo "  已装"

echo "=== 7. 主补丁覆盖的文件与两个包 ==="
for f in configs/e87n.config \
         target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts \
         target/linux/mediatek/filogic/base-files/etc/board.d/01_leds \
         target/linux/mediatek/filogic/base-files/etc/board.d/02_network \
         target/linux/mediatek/filogic/base-files/etc/uci-defaults/99-e87n-theme \
         target/linux/mediatek/filogic/base-files/lib/upgrade/platform.sh \
         target/linux/mediatek/image/filogic.mk \
         package/e87n-screen/Makefile \
         package/e87n-display/Makefile \
         package/e87n-display/src/e87n-display.c; do
	[ -e "$f" ] && echo "  OK  $f" || echo "  缺  $f"
done

echo "=== 8. DTS 关键内容自检 ==="
D=target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts
echo "  行数: $(wc -l < $D)"
grep -q 'cooling-levels = <0 110 175 255>' "$D" && echo "  OK  风扇档位" || echo "  缺  风扇档位"
grep -q 'temperature = <62000>' "$D" && echo "  OK  62C 起步" || echo "  缺  62C 起步"
grep -qE '^&spi2 \{' "$D" && echo "  OK  spi2 面板节点" || echo "  缺  spi2 面板节点"

echo "=== 9. DTS 编译验证（内核源码就绪时才做）==="
K=$(ls -d build_dir/target-*/linux-mediatek_filogic/linux-* 2>/dev/null | head -1)
if [ -n "$K" ]; then
	cpp -nostdinc -I "$K/include" -I "$PWD/target/linux/mediatek/dts" \
	    -undef -x assembler-with-cpp "$D" /tmp/e87n-pre.dts 2>&1 | head -5
	dtc -I dts -O dtb -o /tmp/e87n.dtb /tmp/e87n-pre.dts 2>&1 \
	    | grep -vE "Warning|also defined" | head -10
	[ -f /tmp/e87n.dtb ] && echo "  DTS 编译通过，$(wc -c < /tmp/e87n.dtb) 字节" \
	                     || echo "  DTS 编译失败"
else
	echo "  内核源码未就绪，跳过（需先 make tools/install 或跑过一次编译）"
fi
echo "=== 完成 ==="
