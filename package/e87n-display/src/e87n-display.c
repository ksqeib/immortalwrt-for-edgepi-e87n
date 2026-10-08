// SPDX-License-Identifier: GPL-2.0
/*
 * EdgePi E87N 面板状态页渲染器。
 *
 * 直接 mmap /dev/fb0 画像素。面板 428x142 RGB565。
 * 字模来自 Oswald.ttf（SIL OFL 1.1），由 tools/raster-font.js 栅格化成
 * src/e87n-font.h 里的覆盖率数组，所以本程序不链 FreeType，也没有任何
 * 运行时字体文件依赖。
 *
 * 用法:
 *   e87n-display            前台刷新（2fps）
 *   e87n-display once       只画一帧
 *   e87n-display daemon     后台常驻（写 /tmp/e87n-display.pid）
 *   e87n-display stop       停掉后台实例
 *   e87n-display dark       清成黑屏
 *   e87n-display test       画棋盘格，验证面板映射
 *
 * 为什么不用内核 fbcon：fbcon 是单色文本层，字体是内核编死的 8x16，
 * 颜色、字号、布局都改不了，做不出仪表盘。
 *
 * 为什么不用 EN87 的 display-control：那是一个 744 行 FreeType 渲染器
 * 加一个 2.1 MB 的专有 AArch64 二进制。本程序把字形预先烘成数组，
 * 源码全部可读，依赖只剩 libc。
 */
#define _GNU_SOURCE
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

static void put_px(Canvas *c, int x, int y, uint16_t col)
{
	if ((unsigned)x >= LCD_W || (unsigned)y >= LCD_H) return;
	c->pix[y * LCD_W + x] = col;
}

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

static void rounded_rect(Canvas *c, int x, int y, int w, int h, int r, uint16_t col)
{
	int yy, xx;

	fill_rect(c, x + r, y, w - 2 * r, h, col);
	fill_rect(c, x, y + r, r, h - 2 * r, col);
	fill_rect(c, x + w - r, y + r, r, h - 2 * r, col);
	for (yy = 0; yy < r; yy++)
		for (xx = 0; xx < r; xx++) {
			int dx = r - 1 - xx, dy = r - 1 - yy;
			if (dx * dx + dy * dy <= r * r) {
				put_px(c, x + xx, y + yy, col);
				put_px(c, x + w - 1 - xx, y + yy, col);
				put_px(c, x + xx, y + h - 1 - yy, col);
				put_px(c, x + w - 1 - xx, y + h - 1 - yy, col);
			}
		}
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

/* 横向缩放以填满 target_w：大数字靠它撑满左栏 */
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

/* ---------- 指标 ---------- */
typedef struct {
	int cpu, mem, clients;
	float temp;
	char time_s[8], sec_s[4], date_s[16];
	char iface[IFNAMSIZ], ip[24], link[8];
	int graph_h[GRAPH_COLS];
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

/* 默认路由所在网口；读不到再退回候选名 */
static int choose_iface(char *out, size_t n)
{
	const char *env = getenv("E87N_DISPLAY_IFACE");
	const char *cands[] = { "br-lan", "eth0", "eth1", "wan", "end0", NULL };
	char path[256], line[256];
	struct stat st;
	FILE *fp;
	int i;

	if (env && *env) {
		snprintf(path, sizeof(path), "/sys/class/net/%s", env);
		if (stat(path, &st) == 0) { snprintf(out, n, "%s", env); return 0; }
	}
	fp = fopen("/proc/net/route", "r");
	if (fp) {
		fgets(line, sizeof(line), fp);
		while (fgets(line, sizeof(line), fp)) {
			char name[64];
			unsigned dst = 1, mask = 1;
			if (sscanf(line, "%63s %x %*x %*x %*x %*x %*x %x",
				   name, &dst, &mask) == 3 && dst == 0) {
				snprintf(out, n, "%s", name);
				fclose(fp);
				return 0;
			}
		}
		fclose(fp);
	}
	for (i = 0; cands[i]; i++) {
		snprintf(path, sizeof(path), "/sys/class/net/%s", cands[i]);
		if (stat(path, &st) == 0) { snprintf(out, n, "%s", cands[i]); return 0; }
	}
	snprintf(out, n, "eth0");
	return -1;
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

static int read_clients(void)
{
	FILE *fp = fopen("/proc/net/arp", "r");
	char line[512];
	int count = 0;

	if (!fp) return 0;
	fgets(line, sizeof(line), fp);
	while (fgets(line, sizeof(line), fp)) {
		unsigned flags = 0;
		char ip[64], hw[64], mac[64], mask[64], dev[64];
		if (sscanf(line, "%63s %63s %x %63s %63s %63s",
			   ip, hw, &flags, mac, mask, dev) == 6 && (flags & 0x2))
			count++;
	}
	fclose(fp);
	return count;
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

/* 字节/秒 -> 0..20 的柱高（每级约 1.78 倍，覆盖 ~3K/s 到 ~80M/s） */
static int rate_to_height(double rate)
{
	int h = 0;

	while (h < 20 && rate >= 1024.0) {
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

static double mono_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void metrics_init(Metrics *m)
{
	memset(m, 0, sizeof(*m));
	snprintf(m->ip, sizeof(m->ip), "--");
	snprintf(m->link, sizeof(m->link), "--");
}

static void metrics_update(Metrics *m, double now, int first)
{
	time_t tt = time(NULL);
	struct tm tmv;
	unsigned long long rx = 0, tx = 0, total;
	char path[256];
	float t;
	int rx_ok, tx_ok, i;

	localtime_r(&tt, &tmv);
	strftime(m->time_s, sizeof(m->time_s), "%H:%M", &tmv);
	strftime(m->sec_s, sizeof(m->sec_s), "%S", &tmv);
	strftime(m->date_s, sizeof(m->date_s), "%a %d %b", &tmv);

	m->cpu = read_cpu_percent(m);
	m->mem = read_mem_percent();
	m->clients = read_clients();
	t = read_temp();
	if (t > 0.0f) m->temp = t;

	if (!m->iface[0]) choose_iface(m->iface, sizeof(m->iface));
	get_ipv4(m->iface, m->ip, sizeof(m->ip));

	snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", m->iface);
	if (read_str(path, m->link, sizeof(m->link)) < 0)
		snprintf(m->link, sizeof(m->link), "--");

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
	fill_rect_alpha(c, 0, 0, 137, LCD_H, C_PANEL_DARK, 120);
}

/* ---------- 状态页 ---------- */
static uint16_t bar_color(int pct)
{
	if (pct >= 80) return C_BAR_HIGH;
	if (pct >= 50) return C_BAR_MID;
	return C_BAR_LOW;
}

static void render_dashboard(Canvas *c, const Metrics *m)
{
	char buf[64], maxs[16], mins[16];
	double mx = 0.0, mn = 1e99;
	int vals[3], i, k;

	render_bg(c);

	/* 分区线：左栏 / 右上曲线区 / 右下指标区 */
	vline(c, 139, 4, 137, C_LINE);
	vline(c, 202, 26, 82, C_LINE);
	hline(c, 143, LCD_W - 2, 84, C_LINE);

	/* --- 左栏 --- */
	draw_text(c, font14_glyphs, FONT14_COUNT, 8, 15, "EDGEPI", C_ACCENT, 1);
	snprintf(buf, sizeof(buf), "%.1f", (double)m->temp);
	draw_text_fit(c, font56_glyphs, FONT56_COUNT, 6, 65, 88, buf, C_TEXT_BRIGHT);
	draw_text(c, font14_glyphs, FONT14_COUNT, 96, 44, "C", C_TEXT_MID, 0);
	draw_text(c, font10_glyphs, FONT10_COUNT, 96, 57, "SYSTEM", C_TEXT_DIM, 0);

	draw_text(c, font34_glyphs, FONT34_COUNT, 6, 104, m->time_s, C_TEXT, 0);
	rounded_rect(c, 74, 86, 34, 22, 2, C_ACCENT);
	draw_text_center(c, font14_glyphs, FONT14_COUNT, 74, 108, 101, m->sec_s, C_BG_CENTER);
	draw_text(c, font10_glyphs, FONT10_COUNT, 6, 122, m->date_s, C_TEXT_DIM, 0);

	/* --- 右上：速率曲线 --- */
	draw_text(c, font14_glyphs, FONT14_COUNT, 146, 15, "LAST 240 SEC", C_TEXT_BRIGHT, 0);
	for (i = 0; i < GRAPH_COLS; i++) {
		double v = height_to_rate(m->graph_h[i]);
		if (v > mx) mx = v;
		if (v < mn) mn = v;
	}
	if (mn > 1e98) mn = 0.0;
	format_rate(mx, maxs, sizeof(maxs));
	format_rate(mn, mins, sizeof(mins));
	snprintf(buf, sizeof(buf), "MAX %s/s", maxs);
	draw_text_right(c, font10_glyphs, FONT10_COUNT, LCD_W - 4, 14, buf, C_TEXT_MID);
	snprintf(buf, sizeof(buf), "MIN %s/s", mins);
	draw_text_right(c, font10_glyphs, FONT10_COUNT, LCD_W - 4, 23, buf, C_TEXT_DIM);

	for (k = 0; k < GRAPH_COLS; k++) {
		int h = m->graph_h[k];
		int x = 206 + k * 6;

		if (x + 4 > LCD_W - 4) break;
		for (i = 0; i < h; i++) {
			int y = 79 - i * 3;
			uint16_t col;

			if (y - 2 < 30) break;
			col = (i >= h - 3) ? C_GRAPH_TOP : C_GRAPH_LOW;
			fill_rect(c, x, y - 2, 4, 3, col);
		}
	}

	/* --- 右下：CPU / MEMORY / CLIENTS --- */
	vals[0] = m->cpu;
	vals[1] = m->mem;
	vals[2] = m->clients;
	{
		const char *labels[3] = { "CPU", "MEMORY", "CLIENTS" };
		int is_pct[3] = { 1, 1, 0 };
		int colw = (LCD_W - 143) / 3;

		for (k = 0; k < 3; k++) {
			int x0 = 143 + k * colw, x1 = x0 + colw;

			draw_text_center(c, font10_glyphs, FONT10_COUNT, x0, x1, 96,
					 labels[k], C_TEXT_DIM);
			if (is_pct[k])
				snprintf(buf, sizeof(buf), "%d%%", vals[k]);
			else
				snprintf(buf, sizeof(buf), "%d", vals[k]);
			draw_text_center(c, font34_glyphs, FONT34_COUNT, x0, x1, 124,
					 buf, C_TEXT);
			if (is_pct[k]) {
				int w = colw - 18, bx = x0 + 9, fillw;

				if (w < 10) w = 10;
				fill_rect(c, bx, 128, w, 5, C_PANEL);
				fillw = vals[k] * w / 100;
				if (fillw > 0)
					fill_rect(c, bx, 128, fillw, 5, bar_color(vals[k]));
			}
		}
	}

	/* 底行：WAN 地址与链路状态 */
	snprintf(buf, sizeof(buf), "%s  %s  %s", m->ip, m->link, m->iface);
	draw_text(c, font10_glyphs, FONT10_COUNT, 146, 140, buf, C_TEXT_MID, 0);
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
