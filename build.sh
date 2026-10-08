#!/bin/bash
# EdgePi E87N —— 在 Linux 构建机上一条命令编出固件。
#
# 用法（构建机上）：
#     bash build.sh
#
# 环境变量（都可以不设，用默认值）：
#     E87N_PROXY    HTTP 代理，默认 http://192.168.31.103:7890。设成空串可关掉。
#     E87N_REPO     本仓库路径，默认脚本所在目录
#     E87N_SRC      immortalwrt 源码树路径，默认 ~/immortalwrt
#     E87N_DL       下载缓存目录，默认 ~/dl
#     E87N_JOBS     并行数，默认 $(nproc)
#
# 脚本是幂等的：重复跑会复位 immortalwrt 树再重新打补丁，
# 保留 dl / build_dir / staging_dir（增量编译），不重新下载已下过的东西。
set -u

REPO="${E87N_REPO:-$(cd "$(dirname "$0")" && pwd)}"
SRC="${E87N_SRC:-$HOME/immortalwrt}"
DL="${E87N_DL:-$HOME/dl}"
JOBS="${E87N_JOBS:-$(nproc)}"
PROXY="${E87N_PROXY-http://192.168.31.103:7890}"
LOG="$HOME/e87n-build.log"

: > "$LOG"
T0=$(date +%s)
log()  { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
step() { log ""; log "===== $* ====="; }
el()   { echo "  (用时 $(( ($(date +%s) - T0) / 60 )) 分 $(( ($(date +%s) - T0) % 60 )) 秒)"; }
die()  { log "✗ $*"; log "  完整日志: $LOG"; exit 1; }

step "0. 环境"
log "  仓库     : $REPO"
log "  源码树   : $SRC"
log "  下载缓存 : $DL"
log "  并行数   : $JOBS"
log "  代理     : ${PROXY:-（不用）}"
[ -f "$REPO/apply.sh" ] || die "找不到 $REPO/apply.sh，确认 E87N_REPO 指向本仓库"
[ -f "$REPO/patch/e87n-openwrt.patch" ] || die "找不到 patch/e87n-openwrt.patch"
[ -f "$REPO/patch/999-nv3007-fbtft.patch" ] || die "找不到 patch/999-nv3007-fbtft.patch"

# ---- 代理：先探通，不通就自动关掉，免得整个流程卡死在超时上 ----
if [ -n "$PROXY" ]; then
	if curl -fsS --connect-timeout 8 -o /dev/null -x "$PROXY" https://github.com 2>/dev/null; then
		export http_proxy="$PROXY" https_proxy="$PROXY" ftp_proxy="$PROXY" all_proxy="$PROXY"
		export no_proxy="localhost,127.0.0.1,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"
		log "  代理可达，已全局导出"
	else
		log "  ! 代理 $PROXY 不可达，本次不用代理"
		PROXY=""
	fi
fi

export GOPROXY="https://mirrors.aliyun.com/goproxy"
export RUSTUP_DIST_SERVER="https://rsproxy.cn"
export RUSTUP_UPDATE_ROOT="https://rsproxy.cn/rustup"
export CARGO_BUILD_JOBS="$JOBS"
mkdir -p "$HOME/.cargo"
cat > "$HOME/.cargo/config" <<'CARGO'
[source.crates-io]
replace-with = 'tuna'
[source.tuna]
registry = "https://mirrors.tuna.tsinghua.edu.cn/git/crates.io-index.git"
CARGO

# ---- 磁盘余量提醒（编译中间产物约 40-60 GB） ----
AVAIL=$(df -BG --output=avail "$HOME" 2>/dev/null | tail -1 | tr -dc '0-9')
if [ -n "${AVAIL:-}" ] && [ "$AVAIL" -lt 60 ]; then
	log "  ! $HOME 只剩 ${AVAIL}G，编完可能不够（建议 60G+）"
fi

step "1. 取 immortalwrt master"
if [ -d "$SRC/.git" ]; then
	git -C "$SRC" fetch --depth 1 origin master >>"$LOG" 2>&1 \
		|| die "fetch 失败（代理/网络？）"
	git -C "$SRC" reset --hard FETCH_HEAD >>"$LOG" 2>&1
	# 清掉上次 apply.sh 留下的未跟踪文件；被 ignore 的 dl/build_dir 不动，
	# 保证增量编译。
	git -C "$SRC" clean -fd >>"$LOG" 2>&1
	log "  已复位到 $(git -C "$SRC" log -1 --format='%h %s')"
else
	git clone --branch master --single-branch --depth 1 \
		https://github.com/immortalwrt/immortalwrt.git "$SRC" >>"$LOG" 2>&1 \
		|| die "clone 失败（代理/网络？）"
	log "  已 clone $(git -C "$SRC" log -1 --format=%h)"
fi

# ---- 共享 dl：跨树复用，避免重复下载 ----
if [ ! -L "$SRC/dl" ]; then
	mkdir -p "$DL"
	[ -d "$SRC/dl" ] && cp -rn "$SRC/dl/." "$DL/" 2>/dev/null
	rm -rf "$SRC/dl" && ln -s "$DL" "$SRC/dl"
	log "  dl 软链到 $DL"
fi
el

step "2. 打 E87N 补丁（apply.sh）"
bash "$REPO/apply.sh" "$SRC" >>"$LOG" 2>&1 || {
	tail -20 "$LOG"; die "apply.sh 失败"; }
log "  OK  主补丁 + 内核补丁 + 屏幕包"
log "  --- 校验 ---"
for f in configs/e87n.config \
         target/linux/mediatek/dts/mt7987a-edgepi-e87n.dts \
         target/linux/mediatek/patches-6.18/999-nv3007-fbtft.patch \
         package/e87n-screen/Makefile; do
	if [ -e "$SRC/$f" ]; then log "    OK  $f"; else die "  缺 $f"; fi
done
el

step "3. llvm-bpf 预编译工具链（避开 2.5 小时的 LLVM 源码编译）"
if [ -f "$SRC/llvm-bpf/.llvm-version" ]; then
	log "  已有 $(cat "$SRC/llvm-bpf/.llvm-version")"
else
	VER=$(sed -n 's/^PKG_VERSION:=//p' "$SRC/tools/llvm-bpf/Makefile")
	TAR="llvm-bpf-${VER}.Linux-x86_64.tar.zst"
	URL="https://downloads.immortalwrt.org/snapshots/targets/mediatek/filogic/${TAR}"
	log "  需要 ${VER}，下载 $TAR"
	if [ ! -f "$DL/$TAR" ]; then
		curl -fL --retry 3 --connect-timeout 15 -o "$DL/$TAR" "$URL" >>"$LOG" 2>&1 \
			|| die "下载失败: $URL"
	fi
	rm -rf "$SRC/llvm-bpf"
	if ! tar --zstd -xf "$DL/$TAR" -C "$SRC" >>"$LOG" 2>&1; then
		tar -I zstd -xf "$DL/$TAR" -C "$SRC" >>"$LOG" 2>&1 || die "解包失败（需要 zstd）"
	fi
	[ -f "$SRC/llvm-bpf/.llvm-version" ] || die "解包后仍无 .llvm-version"
	log "  OK  已就位，kconfig 会自动改选 PREBUILT"
fi
el

step "4. feeds + defconfig"
cd "$SRC" || die "进不去 $SRC"
./scripts/feeds update -a >>"$LOG" 2>&1 || log "  ! feeds update 有失败（继续）"
./scripts/feeds install -a >>"$LOG" 2>&1 || log "  ! feeds install 有失败（继续）"
cp "$REPO/configs/e87n.config" .config
make defconfig >>"$LOG" 2>&1 || die "defconfig 失败"

log "  --- 关键符号落地检查（不存在的包会被 defconfig 静默丢弃）---"
MISSING=0
for s in CONFIG_TARGET_mediatek_filogic_DEVICE_edgepi_e87n \
         CONFIG_PACKAGE_video-support \
         CONFIG_PACKAGE_kmod-fb \
         CONFIG_PACKAGE_kmod-backlight \
         CONFIG_PACKAGE_kmod-fb-tft \
         CONFIG_PACKAGE_kmod-fb-tft-nv3007 \
         CONFIG_PACKAGE_e87n-screen \
         CONFIG_PACKAGE_e87n-display \
         CONFIG_LUCI_LANG_zh_Hans; do
	if grep -q "^$s=y$" .config; then log "    OK   $s"; else log "    丢弃 $s"; MISSING=1; fi
done
# 依赖自诊断：kmod-fb 依赖一个不带 + 的硬依赖 video-support，
# 缺了它整条 fbdev 链会被 defconfig 静默丢弃（见 configs/e87n.config 注释）。
if [ "$MISSING" != 0 ]; then
	if ! grep -q "^CONFIG_PACKAGE_video-support=y$" .config; then
		log "  ! video-support 未落地 —— 它是 kmod-fb 的硬依赖，缺了会连带丢弃整条 fbdev 链"
	fi
	die "有符号没落地，先看上面的清单"
fi
grep -q '^CONFIG_USE_LLVM_BUILD=y' .config && die "LLVM 仍走源码编译，检查步骤 3"
log "  BPF 工具链走预编译，OK"
el

step "5. 内核解包 + 打内核补丁（先跑这步，补丁行号有问题会立刻暴露）"
make target/linux/prepare V=s >>"$LOG" 2>&1
if grep -qE "Hunk #[0-9]+ FAILED|patch failed|Reversed \(or previously applied\)" "$LOG"; then
	log "  ✗ 内核补丁有问题，末尾日志："
	tail -40 "$LOG" | tee -a "$LOG"
	die "补丁未干净应用。多为 hunk 行号漂移，用 patch -p1 -l 或 git apply -C1 放宽后重跑"
fi
if grep -qE "Hunk #[0-9]+ succeeded at .* offset [0-9]+ lines" "$LOG"; then
	log "  ! 补丁有行号偏移但已成功应用（6.18 与 6.6 的行号差，属预期）："
	grep -E "Hunk #[0-9]+ succeeded at" "$LOG" | sed 's/^/      /'
fi
FBT="$SRC/build_dir/target-*/linux-mediatek_filogic/linux-*/drivers/staging/fbtft"
if ls $FBT/fb_nv3007.c >/dev/null 2>&1; then
	log "  OK  驱动源码已进内核树"
else
	log "  ! 没在 build_dir 里找到 fb_nv3007.c（可能路径不同，继续）"
fi
el

step "6. 下载源码"
make download -j"$JOBS" >>"$LOG" 2>&1 || log "  ! 第一轮下载有失败，下面补漏"
make download -j1 >>"$LOG" 2>&1 || log "  ! 补漏仍有失败（可能仍能编）"
log "  dl 共 $(ls -1 "$DL" 2>/dev/null | wc -l) 个文件，$(du -sh "$DL" 2>/dev/null | cut -f1)"
el

step "7. 编译（-j$JOBS）"
make -j"$JOBS" V=s >>"$LOG" 2>&1
RC=$?
el
if [ "$RC" -ne 0 ]; then
	log "✗ 编译失败 rc=$RC，错误摘要："
	grep -nE "Error [0-9]|make\[[0-9]\]: \*\*\*|No rule to make|undefined reference|Hunk #[0-9]+ FAILED" "$LOG" \
		| tail -25 | tee -a "$LOG"
	log "  完整日志: $LOG"
	exit "$RC"
fi

step "8. 产物"
OUT="$SRC/bin/targets/mediatek/filogic"
ls -lh "$OUT"/*e87n* "$OUT"/sha256sums 2>/dev/null | tee -a "$LOG"

log ""
log "--- 屏幕相关包是否进固件 ---"
if grep -qiE 'kmod-fb-tft-nv3007|e87n-screen' "$OUT"/*.manifest 2>/dev/null; then
	grep -hiE 'kmod-fb-tft-nv3007|e87n-screen|^kmod-fb ' "$OUT"/*.manifest | sed 's/^/    /'
	log "  OK  屏幕驱动与背光包都在 manifest 里"
else
	log "  ✗ manifest 里没有屏幕相关包，翻 $LOG 找 kmod-fb-tft-nv3007 的编译记录"
fi

log ""
log "================================"
log "完成，总用时 $(( ($(date +%s) - T0) / 60 )) 分钟"
log "产物目录: $OUT"
log "日志: $LOG"
log ""
log "取回固件（在你自己的机器上跑）："
log "  scp root@<构建机>:$OUT/*e87n*sysupgrade.bin ."
log "先别刷机，先编 initramfs 或直接看 manifest；刷机前确认能进 uboot/tftp"
