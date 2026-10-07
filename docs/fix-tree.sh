#!/bin/bash
# 把 immortalwrt 树恢复到 HEAD，再用新版补丁重新打上。
set -u
H="$HOME"
cd "$H/immortalwrt" || { echo "找不到 $H/immortalwrt"; exit 1; }

echo "=== 1. 当前树状态 ==="
git status --porcelain | head -20

echo "=== 2. 恢复所有被修改的跟踪文件 ==="
git checkout -- .
echo "  已恢复"

echo "=== 3. 删掉旧补丁留下的新文件 ==="
rm -f configs/e87n.config target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts
rmdir configs 2>/dev/null
echo "  已清理"

echo "=== 4. 确认树干净 ==="
echo "  未提交改动数: $(git status --porcelain | wc -l) （应为 0）"

echo "=== 5. 打新补丁 ==="
if git apply --check "$H/e87n-openwrt.patch" 2>&1; then
  git apply "$H/e87n-openwrt.patch" && echo "  补丁已应用"
else
  echo "  补丁仍无法应用"; exit 1
fi

echo "=== 6. 五个文件 ==="
for f in configs/e87n.config \
         target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts \
         target/linux/mediatek/filogic/base-files/etc/board.d/02_network \
         target/linux/mediatek/filogic/base-files/lib/upgrade/platform.sh \
         target/linux/mediatek/image/filogic.mk; do
  [ -f "$f" ] && echo "  OK  $f" || echo "  缺  $f"
done

echo "=== 7. DTS 是否新版 ==="
grep -n "bootargs" target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts
echo "  行数: $(wc -l < target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts) （新版 273）"

echo "=== 8. DTS 编译验证 ==="
K=$(ls -d build_dir/target-*/linux-mediatek_filogic/linux-* 2>/dev/null | head -1)
if [ -n "$K" ]; then
  cpp -nostdinc -I "$K/include" -I "$PWD/target/linux/mediatek/dts" -undef -x assembler-with-cpp \
      target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts /tmp/e87n-pre.dts 2>&1 | head -5
  dtc -I dts -O dtb -o /tmp/e87n.dtb /tmp/e87n-pre.dts 2>&1 | grep -vE "Warning|also defined" | head -10
  [ -f /tmp/e87n.dtb ] && echo "  DTS 编译通过，$(wc -c < /tmp/e87n.dtb) 字节" || echo "  DTS 编译失败"
else
  echo "  内核源码未就绪"
fi
echo "=== 完成 ==="
