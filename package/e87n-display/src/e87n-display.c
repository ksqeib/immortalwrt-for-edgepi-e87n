// SPDX-License-Identifier: GPL-2.0
/*
 * EdgePi E87N 面板状态页渲染器。
 *
 * 直接 mmap /dev/fb0 画像素。面板 428x142 RGB565。
 * 字模来自 Oswald.ttf（SIL OFL 1.1），由 tools/raster-font.js 栅格化成
 * src/e87n-font.h 里的覆盖率数组，所以本程序不链 FreeType，也没有任何
 * 运行时字体文件依赖。
 *
 * 布局（左 → 右）：
 *   左栏   网卡列表，每张有 IPv4 的网卡一行：名字 + 地址
 *   右上   温度大字、CPU、内存三格，后两者带用量条
 *   右下   近 240 秒收发速率柱状图
 *
 * 显示什么参考了 btop 的信息集（CPU / 内存 / 网络 / 温度 / 运行时长），
 * 但去掉了磁盘与进程——这台设备上没意义，面板也放不下。
 * 不显示当前时间：看面板的人关心的是负载，不是几点。
 *
 * 用法:
 *   e87n-display            前台刷新（2fps）
 *   e87n-display once       只画一帧
 *   e87n-display daemon     后台常驻（写 /tmp/e87n-display.pid）
 *   e87n-display stop       停掉后台实例
 *   e87n-display dark       清成黑屏
 *   e87n-display test       画棋盘格与四角异色块，验证面板映射
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "e87n-font.h"

#define LCD_W 428
#define LCD_H 142
#define GRAPH_COLS 36
#define GRAPH_LEVELS 14
#define MAX_IF 6

/* ---------- 颜色（RGB565） ---------- */
static uint16_t C_BG_EDGE, C_BG_CENTER, C_PANEL, C_PANEL_DARK;
static uint16_t C_TEXT, C_TEXT_BRIGHT, C_TEXT_MID, C_TEXT_DIM, C_LINE;
static uint16_t C_ACCENT, C_GRAPH_TOP, C_GRAPH_LOW;
static uint16_t C_BAR_LOW, C_BAR_MID, C_BAR_HIGH;

static inline uint16_t rgb565(int r, int g, int b)
{
	if (r < 0) r = 0;
	if (r > 255) r = 255;
	if (g < 0) g = 0;
	if (g > 255) g = 255;
	if (b < 0) b = 0;
	if (b > 255) b = 255;
	return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xf8) >> 3));
}

static inline void unpack565(uint16_t c, int *r, int *g, int *b)
{
	*r = ((c >> 11) & 0x1f) * 255 / 31;
	*g = ((c >> 5) & 0x3f) * 255 / 63;
	*b = (c & 0x1f) * 255 / 31;
}

static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t a)
{
	int dr, dg, db, sr, sg, sb;

	if (a == 0) return dst;
	if (a == 255) return src;
	unpack565(dst, &dr, &dg, &db);
	unpack565(src, &sr, &sg, &sb);
	return rgb565(dr + (sr - dr) * a / 255,
		      dg + (sg - dg) * a / 255,
		      db + (sb - db) * a / 255);
}

/* ---------- 画布 ---------- */
typedef struct {
	uint16_t pix[LCD_W * LCD_H];
} Canvas;

static void blend_px(Canvas *c, int x, int y, uint16_t col, uint8_t a)
{
	if ((unsigned)x >= LCD_W || (unsigned)y >= LCD_H) return;
	c->pix[y * LCD_W + x] = blend565(c->pix[y * LCD_W + x], col, a);
}

static void fill_rect(Canvas *c, int x, int y, int w, int h, uint16_t col)
{
	int x1 = x + w, y1 = y + h, xx, yy;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > LCD_W) x1 = LCD_W;
	if (y1 > LCD_H) y1 = LCD_H;
	for (yy = y; yy < y1; yy++)
		for (xx = x; xx < x1; xx++)
			c->pix[yy * LCD_W + xx] = col;
}

static void fill_rect_alpha(Canvas *c, int x, int y, int w, int h, uint16_t col, uint8_t a)
{
	int x1 = x + w, y1 = y + h, xx, yy;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > LCD_W) x1 = LCD_W;
	if (y1 > LCD_H) y1 = LCD_H;
	for (yy = y; yy < y1; yy++)
		for (xx = x; xx < x1; xx++)
			blend_px(c, xx, yy, col, a);
}

static void hline(Canvas *c, int x0, int x1, int y, uint16_t col)
{
	int x, t;

	if (y < 0 || y >= LCD_H) return;
	if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
	if (x0 < 0) x0 = 0;
	if (x1 >= LCD_W) x1 = LCD_W - 1;
	for (x = x0; x <= x1; x++) c->pix[y * LCD_W + x] = col;
}

static void vline(Canvas *c, int x, int y0, int y1, uint16_t col)
{
	int y, t;

	if (x < 0 || x >= LCD_W) return;
	if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
	if (y0 < 0) y0 = 0;
	if (y1 >= LCD_H) y1 = LCD_H - 1;
	for (y = y0; y <= y1; y++) c->pix[y * LCD_W + x] = col;
}



/* ---------- 文字 ----------
 * 字模是预先栅格化的覆盖率位图，尺寸已定，所以绘制函数不带字号参数。
 * y_base 是基线；字模的 left 相对笔位、top 相对基线（负值向上）。
 */
static const struct e87n_glyph *find_glyph(const struct e87n_glyph *tbl, int n, char ch)
{
	int i;

	for (i = 0; i < n; i++)
		if (tbl[i].ch == ch)
			return &tbl[i];
	return NULL;
}

static int text_width(const struct e87n_glyph *tbl, int n, const char *s, int tracking)
{
	int w = 0, cnt = 0;

	for (; *s; s++) {
		const struct e87n_glyph *g = find_glyph(tbl, n, *s);
		w += g ? g->adv : 8;
		cnt++;
	}
	if (cnt > 1) w += tracking * (cnt - 1);
	return w;
}

static void draw_text(Canvas *c, const struct e87n_glyph *tbl, int n,
		      int x, int y_base, const char *s, uint16_t col, int tracking)
{
	int pen = x;

	for (; *s; s++) {
		const struct e87n_glyph *g = find_glyph(tbl, n, *s);
		int gx, gy, bx, by;

		if (!g) { pen += 8; continue; }
		gx = pen + g->left;
		gy = y_base + g->top;
		for (by = 0; by < g->h; by++)
			for (bx = 0; bx < g->w; bx++) {
				uint8_t a = g->cov[by * g->w + bx];
				if (a) blend_px(c, gx + bx, gy + by, col, a);
			}
		pen += g->adv + tracking;
	}
}

static void draw_text_center(Canvas *c, const struct e87n_glyph *tbl, int n,
			     int x0, int x1, int y_base, const char *s, uint16_t col)
{
	int w = text_width(tbl, n, s, 0);

	draw_text(c, tbl, n, x0 + (x1 - x0 - w) / 2, y_base, s, col, 0);
}

static void draw_text_right(Canvas *c, const struct e87n_glyph *tbl, int n,
			    int right, int y_base, const char *s, uint16_t col)
{
	int w = text_width(tbl, n, s, 0);

	draw_text(c, tbl, n, right - w, y_base, s, col, 0);
}

/* 横向缩放以填满 target_w：大数字靠它撑满格子 */
static void draw_text_fit(Canvas *c, const struct e87n_glyph *tbl, int n,
			  int x, int y_base, int target_w, const char *s, uint16_t col)
{
	int natural = text_width(tbl, n, s, 0);
	double ratio;
	int pen = x;

	if (natural <= 0) return;
	ratio = (double)target_w / (double)natural;
	if (ratio > 1.30) ratio = 1.30;
	if (ratio < 0.60) ratio = 0.60;
	if (natural > target_w) ratio = (double)target_w / (double)natural;

	for (; *s; s++) {
		const struct e87n_glyph *g = find_glyph(tbl, n, *s);
		int gx, gy, by, bx, outw, adv;

		if (!g) { pen += (int)(8 * ratio); continue; }
		adv = (int)(g->adv * ratio + 0.5);
		outw = (int)(g->w * ratio + 0.5);
		gx = pen + (int)(g->left * ratio);
		gy = y_base + g->top;
		if (outw < 1) outw = 1;
		for (by = 0; by < g->h; by++)
			for (bx = 0; bx < outw; bx++) {
				int sx = (int)(bx / ratio);
				uint8_t a;
				if (sx >= g->w) sx = g->w - 1;
				a = g->cov[by * g->w + sx];
				if (a) blend_px(c, gx + bx, gy + by, col, a);
			}
		pen += adv;
	}
}

/* ---------- 网卡 ---------- */
struct if_entry {
	char name[IFNAMSIZ];
	char ip[24];
};

/*
 * 枚举 /sys/class/net 下所有拿到 IPv4 的网卡，跳过 lo。
 * 排序：br-lan、wan 优先，其余按名字。逻辑口名不写死——不同固件的
 * 命名不一样（br-lan / wan / lan / eth0 / end0 都见过），一律现查。
 */
static int iface_rank(const char *n)
{
	if (!strcmp(n, "br-lan")) return 0;
	if (!strcmp(n, "wan")) return 1;
	if (!strncmp(n, "eth", 3)) return 2;
	if (!strncmp(n, "lan", 3)) return 3;
	return 4;
}

static int get_ipv4(const char *iface, char *out, size_t n)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq ifr;
	struct sockaddr_in *sin;

	if (fd < 0) return -1;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
	if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) { close(fd); return -1; }
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	snprintf(out, n, "%s", inet_ntoa(sin->sin_addr));
	close(fd);
	return 0;
}

static int has_ipv4(const char *iface)
{
	char ip[24];

	return get_ipv4(iface, ip, sizeof(ip)) == 0;
}

static void sort_ifaces(struct if_entry *e, int n)
{
	int i, j;

	for (i = 1; i < n; i++) {
		struct if_entry key = e[i];

		j = i - 1;
		while (j >= 0 && (iface_rank(e[j].name) > iface_rank(key.name) ||
				  (iface_rank(e[j].name) == iface_rank(key.name) &&
				   strcmp(e[j].name, key.name) > 0))) {
			e[j + 1] = e[j];
			j--;
		}
		e[j + 1] = key;
	}
}

static int collect_ifaces(struct if_entry *out, int max)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *de;
	int n = 0;

	if (!d) return 0;
	while ((de = readdir(d)) != NULL && n < max) {
		if (de->d_name[0] == '.') continue;
		if (!strcmp(de->d_name, "lo")) continue;
		if (!has_ipv4(de->d_name)) continue;
		snprintf(out[n].name, sizeof(out[n].name), "%s", de->d_name);
		if (get_ipv4(de->d_name, out[n].ip, sizeof(out[n].ip)) < 0)
			continue;
		n++;
	}
	closedir(d);
	sort_ifaces(out, n);
	return n;
}

/* ---------- 指标 ---------- */
typedef struct {
	int cpu, mem;
	float temp;
	char uptime[16];
	char iface[IFNAMSIZ];          /* 速率曲线盯的那张网卡 */
	int graph_h[GRAPH_COLS];
	struct if_entry ifs[MAX_IF];
	int n_ifs;
	unsigned long long prev_total, prev_idle;
	unsigned long long last_net_bytes;
	double last_net_time, next_sample;
} Metrics;

static int read_u64(const char *path, unsigned long long *v)
{
	FILE *fp = fopen(path, "r");
	unsigned long long x = 0;
	int ok;

	if (!fp) return -1;
	ok = fscanf(fp, "%llu", &x) == 1;
	fclose(fp);
	if (ok) *v = x;
	return ok ? 0 : -1;
}

static int read_str(const char *path, char *out, size_t n)
{
	FILE *fp = fopen(path, "r");

	if (!fp) return -1;
	if (!fgets(out, (int)n, fp)) { fclose(fp); return -1; }
	fclose(fp);
	out[strcspn(out, "\r\n")] = '\0';
	return 0;
}

/* 速率曲线盯的网卡：默认路由出口；读不到就退回第一张列出来的网卡 */
static void pick_primary_iface(Metrics *m)
{
	const char *env = getenv("E87N_DISPLAY_IFACE");
	char line[256];
	FILE *fp;

	if (env && *env) {
		snprintf(m->iface, sizeof(m->iface), "%s", env);
		return;
	}
	fp = fopen("/proc/net/route", "r");
	if (fp) {
		fgets(line, sizeof(line), fp);
		while (fgets(line, sizeof(line), fp)) {
			char name[64];
			unsigned dst = 1, mask = 1;

			if (sscanf(line, "%63s %x %*x %*x %*x %*x %*x %x",
				   name, &dst, &mask) == 3 && dst == 0) {
				snprintf(m->iface, sizeof(m->iface), "%s", name);
				fclose(fp);
				return;
			}
		}
		fclose(fp);
	}
	if (m->n_ifs > 0)
		snprintf(m->iface, sizeof(m->iface), "%s", m->ifs[0].name);
	else
		snprintf(m->iface, sizeof(m->iface), "eth0");
}

static int read_mem_percent(void)
{
	FILE *fp = fopen("/proc/meminfo", "r");
	long total = 0, avail = 0;
	char line[256];

	if (!fp) return 0;
	while (fgets(line, sizeof(line), fp)) {
		long val = 0;
		if (sscanf(line, "MemTotal: %ld", &val) == 1) total = val;
		else if (sscanf(line, "MemAvailable: %ld", &val) == 1) avail = val;
	}
	fclose(fp);
	if (total <= 0) return 0;
	if (avail < 0) avail = 0;
	return (int)((100.0 * (double)(total - avail) / (double)total) + 0.5);
}

/* 取所有 thermal zone 里最高的那个读数 */
static float read_temp(void)
{
	float best = -1000.0f;
	int i;

	for (i = 0; i < 16; i++) {
		char path[128];
		long v = 0;
		FILE *fp;
		float t;

		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
		fp = fopen(path, "r");
		if (!fp) continue;
		if (fscanf(fp, "%ld", &v) == 1) {
			t = v > 1000 ? v / 1000.0f : (float)v;
			if (t > best) best = t;
		}
		fclose(fp);
	}
	return best > -999.0f ? best : 0.0f;
}

static int read_cpu_percent(Metrics *m)
{
	FILE *fp = fopen("/proc/stat", "r");
	char cpu[16];
	unsigned long long user, nice, sys, idle, iowait, irq, soft, steal;
	unsigned long long idle_all, total, dt, di;
	int rc;

	if (!fp) return m->cpu;
	rc = fscanf(fp, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
		    cpu, &user, &nice, &sys, &idle, &iowait, &irq, &soft, &steal);
	fclose(fp);
	if (rc < 5) return m->cpu;
	idle_all = idle + iowait;
	total = user + nice + sys + idle + iowait + irq + soft + steal;
	if (!m->prev_total) {
		m->prev_total = total;
		m->prev_idle = idle_all;
		return m->cpu;
	}
	dt = total - m->prev_total;
	di = idle_all - m->prev_idle;
	m->prev_total = total;
	m->prev_idle = idle_all;
	if (!dt) return m->cpu;
	return (int)((100.0 * (double)(dt - di) / (double)dt) + 0.5);
}

static void format_rate(double b, char *out, size_t n)
{
	if (b >= 1073741824.0) snprintf(out, n, "%.1fG", b / 1073741824.0);
	else if (b >= 1048576.0) snprintf(out, n, "%.1fM", b / 1048576.0);
	else if (b >= 1024.0) snprintf(out, n, "%.0fK", b / 1024.0);
	else snprintf(out, n, "%.0f", b);
}

/* 字节/秒 -> 0..GRAPH_LEVELS 的柱高（每级约 1.78 倍） */
static int rate_to_height(double rate)
{
	int h = 0;

	while (h < GRAPH_LEVELS && rate >= 1024.0) {
		rate /= 1.78;
		h++;
	}
	return h;
}

static double height_to_rate(int h)
{
	double v = 0.0;
	int i;

	for (i = 0; i < h; i++)
		v = v ? v * 1.78 : 1024.0;
	return v;
}

static void format_uptime(char *out, size_t n)
{
	double up = 0.0;
	int d, h, m;

	if (read_str("/proc/uptime", out, n) < 0) {
		snprintf(out, n, "--");
		return;
	}
	up = atof(out);
	d = (int)(up / 86400.0);
	h = (int)(up / 3600.0) % 24;
	m = (int)(up / 60.0) % 60;
	if (d > 0)
		snprintf(out, n, "UP %dd%02d:%02d", d, h, m);
	else
		snprintf(out, n, "UP %02d:%02d", h, m);
}

static double mono_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void metrics_init(Metrics *m)
{
	memset(m, 0, sizeof(*m));
}

static void metrics_update(Metrics *m, double now, int first)
{
	unsigned long long rx = 0, tx = 0, total;
	char path[256];
	float t;
	int rx_ok, tx_ok, i;

	m->cpu = read_cpu_percent(m);
	m->mem = read_mem_percent();
	t = read_temp();
	if (t > 0.0f) m->temp = t;
	format_uptime(m->uptime, sizeof(m->uptime));

	m->n_ifs = collect_ifaces(m->ifs, MAX_IF);
	if (!m->iface[0])
		pick_primary_iface(m);

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", m->iface);
	rx_ok = read_u64(path, &rx) == 0;
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", m->iface);
	tx_ok = read_u64(path, &tx) == 0;
	if (!rx_ok || !tx_ok) return;

	total = rx + tx;
	if (!m->last_net_bytes || first) {
		m->last_net_bytes = total;
		m->last_net_time = now;
		m->next_sample = now + 1.0;
		return;
	}
	if (now >= m->next_sample) {
		double dt = now - m->last_net_time;
		double rate = (dt > 0 && total >= m->last_net_bytes)
			      ? (double)(total - m->last_net_bytes) / dt : 0.0;

		m->last_net_time = now;
		m->last_net_bytes = total;
		m->next_sample = now + 1.0;
		for (i = 0; i < GRAPH_COLS - 1; i++)
			m->graph_h[i] = m->graph_h[i + 1];
		m->graph_h[GRAPH_COLS - 1] = rate_to_height(rate);
	}
}

/* ---------- 背景 ---------- */
static void render_bg(Canvas *c)
{
	int x, y;

	for (y = 0; y < LCD_H; y++) {
		uint8_t ny = (uint8_t)(y * 255 / (LCD_H - 1));
		uint16_t col = blend565(C_BG_EDGE, C_BG_CENTER, ny);

		for (x = 0; x < LCD_W; x++)
			c->pix[y * LCD_W + x] = col;
	}
	/* 左栏压暗，形成分区层次 */
	fill_rect_alpha(c, 0, 0, 148, LCD_H, C_PANEL_DARK, 130);
}

/* ---------- 状态页 ---------- */
#define LEFT_W 148

static uint16_t bar_color(int pct)
{
	if (pct >= 80) return C_BAR_HIGH;
	if (pct >= 50) return C_BAR_MID;
	return C_BAR_LOW;
}

/* 一个「标签 + 大数字 + 进度条」的格子 */
static void draw_metric_cell(Canvas *c, int x0, int x1, const char *label,
			     const char *value, int pct, int with_bar)
{
	draw_text_center(c, font10_glyphs, FONT10_COUNT, x0, x1, 40, label, C_TEXT_DIM);
	draw_text_fit(c, font34_glyphs, FONT34_COUNT, x0 + 8, 74,
		      x1 - x0 - 16, value, C_TEXT);
	if (with_bar) {
		int w = x1 - x0 - 22, bx = x0 + 11, fillw;

		if (w < 10) w = 10;
		fill_rect(c, bx, 80, w, 5, C_PANEL);
		fillw = pct * w / 100;
		if (fillw > 0)
			fill_rect(c, bx, 80, fillw, 5, bar_color(pct));
	}
}

static void render_dashboard(Canvas *c, const Metrics *m)
{
	char buf[64], maxs[16], mins[16];
	double mx = 0.0, mn = 1e99;
	int i, k, spacing, y0;

	render_bg(c);

	/* 标题行：左 EDGEPI，右 运行时长（不显示时钟） */
	draw_text(c, font14_glyphs, FONT14_COUNT, 6, 13, "EDGEPI", C_ACCENT, 1);
	draw_text_right(c, font10_glyphs, FONT10_COUNT, LCD_W - 6, 12,
			m->uptime, C_TEXT_DIM);

	/* 左栏：网卡列表 */
	if (m->n_ifs == 0) {
		draw_text(c, font10_glyphs, FONT10_COUNT, 8, 34, "NO LINK", C_TEXT_DIM, 0);
	} else {
		spacing = (LCD_H - 26) / m->n_ifs;
		if (spacing > 40) spacing = 40;
		if (spacing < 20) spacing = 20;
		y0 = 26 + (LCD_H - 26 - spacing * m->n_ifs) / 2 + 10;
		for (i = 0; i < m->n_ifs; i++) {
			int yb = y0 + i * spacing;

			draw_text(c, font14_glyphs, FONT14_COUNT, 8, yb,
				  m->ifs[i].name, C_ACCENT, 0);
			draw_text(c, font10_glyphs, FONT10_COUNT, 8, yb + 13,
				  m->ifs[i].ip, C_TEXT_MID, 0);
		}
	}

	/* 左右分栏竖线 */
	vline(c, LEFT_W, 4, LCD_H - 4, C_LINE);

	/* 右栏分三格：温度 / CPU / 内存 */
	{
		int x0 = LEFT_W + 6;
		int colw = (LCD_W - x0 - 6) / 3;

		snprintf(buf, sizeof(buf), "%.1fC", (double)m->temp);
		draw_metric_cell(c, x0, x0 + colw, "TEMP", buf, 0, 0);

		snprintf(buf, sizeof(buf), "%d%%", m->cpu);
		draw_metric_cell(c, x0 + colw, x0 + colw * 2, "CPU", buf, m->cpu, 1);

		snprintf(buf, sizeof(buf), "%d%%", m->mem);
		draw_metric_cell(c, x0 + colw * 2, LCD_W - 6, "MEM", buf, m->mem, 1);
	}

	/* 速率曲线，标题行在曲线之上 */
	for (i = 0; i < GRAPH_COLS; i++) {
		double v = height_to_rate(m->graph_h[i]);
		if (v > mx) mx = v;
		if (v < mn) mn = v;
	}
	if (mn > 1e98) mn = 0.0;
	format_rate(mx, maxs, sizeof(maxs));
	format_rate(mn, mins, sizeof(mins));

	snprintf(buf, sizeof(buf), "%s  RX+TX  MAX %s/s  MIN %s/s",
		 m->iface, maxs, mins);
	draw_text(c, font10_glyphs, FONT10_COUNT, LEFT_W + 6, 60, buf, C_TEXT_MID, 0);

	/* 曲线基线 */
	hline(c, LEFT_W + 6, LCD_W - 6, LCD_H - 8, C_LINE);

	for (k = 0; k < GRAPH_COLS; k++) {
		int h = m->graph_h[k];
		int x = LEFT_W + 8 + k * 7;

		if (x + 5 > LCD_W - 4) break;
		for (i = 0; i < h; i++) {
			int y = LCD_H - 10 - i * 4;
			uint16_t col;

			if (y - 3 < 66) break;
			col = (i >= h - 3) ? C_GRAPH_TOP : C_GRAPH_LOW;
			fill_rect(c, x, y - 3, 5, 3, col);
		}
	}
}

/* ---------- framebuffer ---------- */
typedef struct {
	int fd;
	uint8_t *map;
	size_t map_len;
	int line_length;
	int xoff, yoff;
} Fb;

static int fb_open(Fb *fb, const char *path)
{
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;

	memset(fb, 0, sizeof(*fb));
	fb->fd = open(path, O_RDWR);
	if (fb->fd < 0) {
		perror("open /dev/fb0");
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0 ||
	    ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		perror("framebuffer ioctl");
		close(fb->fd);
		fb->fd = -1;
		return -1;
	}
	if (vinfo.bits_per_pixel != 16) {
		fprintf(stderr, "只支持 16bpp，本机是 %u bpp\n", vinfo.bits_per_pixel);
		close(fb->fd);
		fb->fd = -1;
		return -1;
	}
	if ((int)vinfo.xres < LCD_W || (int)vinfo.yres < LCD_H) {
		fprintf(stderr, "fb 太小: %ux%u，需要至少 %dx%d\n",
			vinfo.xres, vinfo.yres, LCD_W, LCD_H);
		close(fb->fd);
		fb->fd = -1;
		return -1;
	}
	fb->line_length = (int)finfo.line_length;
	fb->xoff = (int)vinfo.xoffset;
	fb->yoff = (int)vinfo.yoffset;
	fb->map_len = finfo.smem_len;
	fb->map = mmap(NULL, fb->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->map == MAP_FAILED) {
		perror("mmap framebuffer");
		close(fb->fd);
		fb->fd = -1;
		fb->map = NULL;
		return -1;
	}
	return 0;
}

static void fb_close(Fb *fb)
{
	if (fb->map && fb->map != MAP_FAILED) munmap(fb->map, fb->map_len);
	if (fb->fd >= 0) close(fb->fd);
}

static void fb_present(const Fb *fb, const Canvas *c)
{
	int y;

	for (y = 0; y < LCD_H; y++) {
		uint8_t *dst = fb->map
			     + (size_t)(y + fb->yoff) * (size_t)fb->line_length
			     + (size_t)fb->xoff * 2;
		memcpy(dst, &c->pix[y * LCD_W], LCD_W * 2);
	}
}

/*
 * 解绑内核 fbcon。fbcon 一旦绑定，写 console 时它会重绘整个面板，
 * 把我们的像素盖掉。名字含 "frame buffer device" 的那个 vtcon 就是它。
 */
static void unbind_fbcon(void)
{
	int i;

	for (i = 0; i < 8; i++) {
		char p[128], name[128];
		FILE *fp;

		snprintf(p, sizeof(p), "/sys/class/vtconsole/vtcon%d/name", i);
		fp = fopen(p, "r");
		if (!fp) continue;
		name[0] = '\0';
		if (fgets(name, sizeof(name), fp) && strstr(name, "frame buffer")) {
			FILE *bf;

			fclose(fp);
			snprintf(p, sizeof(p), "/sys/class/vtconsole/vtcon%d/bind", i);
			bf = fopen(p, "w");
			if (bf) {
				fputs("0", bf);
				fclose(bf);
			}
			continue;
		}
		fclose(fp);
	}
}

/* ---------- 测试图案 ---------- */
static void render_test(Canvas *c)
{
	int x, y;

	for (y = 0; y < LCD_H; y++)
		for (x = 0; x < LCD_W; x++)
			c->pix[y * LCD_W + x] = ((x / 16 + y / 16) & 1)
					      ? C_TEXT_BRIGHT : C_BG_EDGE;
	fill_rect(c, 0, 0, 40, 40, C_BAR_HIGH);
	fill_rect(c, LCD_W - 40, 0, 40, 40, C_BAR_LOW);
	fill_rect(c, 0, LCD_H - 40, 40, 40, C_ACCENT);
	fill_rect(c, LCD_W - 40, LCD_H - 40, 40, 40, C_TEXT_BRIGHT);
}

/* ---------- 主循环 ---------- */
static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig)
{
	(void)sig;
	g_run = 0;
}

static void init_colors(void)
{
	C_BG_EDGE = rgb565(6, 11, 17);
	C_BG_CENTER = rgb565(20, 29, 38);
	C_PANEL = rgb565(30, 41, 52);
	C_PANEL_DARK = rgb565(12, 19, 26);
	C_TEXT = rgb565(214, 226, 222);
	C_TEXT_BRIGHT = rgb565(240, 247, 236);
	C_TEXT_MID = rgb565(158, 174, 175);
	C_TEXT_DIM = rgb565(96, 112, 116);
	C_LINE = rgb565(56, 72, 82);
	C_ACCENT = rgb565(94, 196, 214);
	C_GRAPH_TOP = rgb565(120, 220, 240);
	C_GRAPH_LOW = rgb565(52, 122, 148);
	C_BAR_LOW = rgb565(88, 200, 140);
	C_BAR_MID = rgb565(232, 190, 72);
	C_BAR_HIGH = rgb565(232, 96, 84);
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "loop";
	const char *fbfile = getenv("E87N_DISPLAY_FB");
	Fb fb;
	Canvas *c;
	Metrics m;
	struct sigaction sa;
	int fps = 2;

	init_colors();
	if (!fbfile || !*fbfile) fbfile = "/dev/fb0";
	if (getenv("E87N_DISPLAY_FPS")) {
		int f = atoi(getenv("E87N_DISPLAY_FPS"));

		if (f > 0 && f <= 30) fps = f;
	}

	if (!strcmp(mode, "stop")) {
		FILE *fp = fopen("/tmp/e87n-display.pid", "r");

		if (fp) {
			int pid = 0;

			if (fscanf(fp, "%d", &pid) == 1 && pid > 1)
				kill(pid, SIGTERM);
			fclose(fp);
		}
		remove("/tmp/e87n-display.pid");
		printf("e87n-display: 已停止\n");
		return 0;
	}

	if (fb_open(&fb, fbfile)) return 1;

	c = calloc(1, sizeof(Canvas));
	if (!c) {
		fprintf(stderr, "内存不足（需要 %zu 字节）\n", sizeof(Canvas));
		fb_close(&fb);
		return 1;
	}

	if (!strcmp(mode, "dark")) {
		memset(c, 0, sizeof(Canvas));
		fb_present(&fb, c);
		fb_close(&fb);
		free(c);
		return 0;
	}

	if (!strcmp(mode, "test")) {
		render_test(c);
		fb_present(&fb, c);
		fb_close(&fb);
		free(c);
		return 0;
	}

	if (!strcmp(mode, "daemon")) {
		FILE *fp = fopen("/tmp/e87n-display.pid", "w");

		if (fp) {
			fprintf(fp, "%d\n", (int)getpid());
			fclose(fp);
		}
	}

	unbind_fbcon();

	metrics_init(&m);
	metrics_update(&m, mono_seconds(), 1);

	if (!strcmp(mode, "once")) {
		render_dashboard(c, &m);
		fb_present(&fb, c);
		fb_close(&fb);
		free(c);
		return 0;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	while (g_run) {
		double t0 = mono_seconds();
		double target = 1.0 / (double)fps;
		double left;

		metrics_update(&m, t0, 0);
		render_dashboard(c, &m);
		fb_present(&fb, c);

		left = target - (mono_seconds() - t0);
		if (left > 0.0) {
			struct timespec ts;

			ts.tv_sec = (time_t)left;
			ts.tv_nsec = (long)((left - (double)ts.tv_sec) * 1e9);
			nanosleep(&ts, NULL);
		}
	}

	fb_close(&fb);
	free(c);
	remove("/tmp/e87n-display.pid");
	return 0;
}
