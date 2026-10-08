#!/usr/bin/env node
/*
 * 把 e87n-display 的状态页在开发机上渲染成 PNG，用来检查布局。
 *
 *   node tools/preview/render.js [输出.png]
 *
 * 字模直接从 package/e87n-display/src/e87n-font.h 解析，坐标与 src/e87n-display.c
 * 里的 render_dashboard 保持一致（含那些具名版面常量）。改完 C 之后同步改这里，
 * 就能先看图再上机，省掉一轮刷机。
 *
 * 指标是造出来的假数据，目的是看排版而不是看数值。
 */
const fs = require('fs');
const path = require('path');
const { writePNG } = require('./png.js');

const ROOT = path.resolve(__dirname, '..', '..');
const FONT_H = path.join(ROOT, 'package', 'e87n-display', 'src', 'e87n-font.h');
const OUT = process.argv[2] || path.join(__dirname, 'dashboard.png');

/* ---- 与 C 里同名同值 ---- */
const LCD_W = 428, LCD_H = 142, GRAPH_COLS = 36, GRAPH_LEVELS = 9;
const HDR_BL = 14, HDR_RULE = 17, LEFT_RULE = 152, LEFT_LBL_BL = 30;
const CELL_LBL_BL = 28, CELL_VAL_BL = 62, CELL_BAR_Y = 69;
const GRAPH_TTL_BL = 88, GRAPH_BASE = 140, GRAPH_STEP = 5;

/* ---------- 解析字模 ---------- */
function parseFont(src) {
  const tiers = {};
  const tblRe = /static const struct e87n_glyph font(\d+)_glyphs\[\] = \{([\s\S]*?)\n\};/g;
  const rowRe = /[{]'([^']*)',\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*font(\d+)_c_(\d+)[}]/g;
  let m;
  const dim = {};
  while ((m = tblRe.exec(src)) !== null) {
    rowRe.lastIndex = 0;
    let r;
    while ((r = rowRe.exec(m[2])) !== null)
      dim[r[7] + '_' + r[8]] = { ch: r[1], w: +r[2], h: +r[3], left: +r[4], top: +r[5], adv: +r[6] };
  }
  const covRe = /static const uint8_t font(\d+)_c_(\d+)\[\] = \{([^}]*)\};/g;
  let c;
  while ((c = covRe.exec(src)) !== null) {
    const d = dim[c[1] + '_' + c[2]];
    if (!d) continue;
    if (!tiers[c[1]]) tiers[c[1]] = [];
    tiers[c[1]].push({ ...d, cov: c[3].split(',').map(Number) });
  }
  return tiers;
}

/* ---------- 颜色（与 init_colors 一致） ---------- */
const rgb565 = (r, g, b) => ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xf8) >> 3);
const unpack565 = c => [((c >> 11) & 0x1f) * 255 / 31 | 0, ((c >> 5) & 0x3f) * 255 / 63 | 0, (c & 0x1f) * 255 / 31 | 0];
function blend565(dst, src, a) {
  if (a === 0) return dst;
  if (a === 255) return src;
  const [dr, dg, db] = unpack565(dst), [sr, sg, sb] = unpack565(src);
  return rgb565(dr + (sr - dr) * a / 255 | 0, dg + (sg - dg) * a / 255 | 0, db + (sb - db) * a / 255 | 0);
}
const C = {
  BG_EDGE: rgb565(6, 11, 17), BG_CENTER: rgb565(20, 29, 38),
  PANEL: rgb565(30, 41, 52), PANEL_DARK: rgb565(12, 19, 26),
  TEXT: rgb565(214, 226, 222), TEXT_BRIGHT: rgb565(240, 247, 236),
  TEXT_MID: rgb565(158, 174, 175), TEXT_DIM: rgb565(96, 112, 116),
  LINE: rgb565(56, 72, 82), ACCENT: rgb565(94, 196, 214),
  GRAPH_TOP: rgb565(120, 220, 240), GRAPH_LOW: rgb565(52, 122, 148),
  BAR_LOW: rgb565(88, 200, 140), BAR_MID: rgb565(232, 190, 72), BAR_HIGH: rgb565(232, 96, 84),
};

/* ---------- 画布 ---------- */
class Canvas {
  constructor() { this.pix = new Uint16Array(LCD_W * LCD_H); }
  blend(x, y, col, a) {
    if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
    this.pix[y * LCD_W + x] = blend565(this.pix[y * LCD_W + x], col, a);
  }
  fill(x, y, w, h, col) {
    let x1 = x + w, y1 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x1 > LCD_W) x1 = LCD_W; if (y1 > LCD_H) y1 = LCD_H;
    for (let yy = y; yy < y1; yy++) for (let xx = x; xx < x1; xx++) this.pix[yy * LCD_W + xx] = col;
  }
  fillAlpha(x, y, w, h, col, a) {
    let x1 = x + w, y1 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x1 > LCD_W) x1 = LCD_W; if (y1 > LCD_H) y1 = LCD_H;
    for (let yy = y; yy < y1; yy++) for (let xx = x; xx < x1; xx++) this.blend(xx, yy, col, a);
  }
  hline(x0, x1, y, col) {
    if (y < 0 || y >= LCD_H) return;
    if (x0 > x1) [x0, x1] = [x1, x0];
    if (x0 < 0) x0 = 0; if (x1 >= LCD_W) x1 = LCD_W - 1;
    for (let x = x0; x <= x1; x++) this.pix[y * LCD_W + x] = col;
  }
  vline(x, y0, y1, col) {
    if (x < 0 || x >= LCD_W) return;
    if (y0 > y1) [y0, y1] = [y1, y0];
    if (y0 < 0) y0 = 0; if (y1 >= LCD_H) y1 = LCD_H - 1;
    for (let y = y0; y <= y1; y++) this.pix[y * LCD_W + x] = col;
  }
}

/* ---------- 文字 ---------- */
const findGlyph = (tbl, ch) => tbl.find(g => g.ch === ch);
function textWidth(tbl, s, tracking = 0) {
  let w = 0, n = 0;
  for (const ch of s) { const g = findGlyph(tbl, ch); w += g ? g.adv : 8; n++; }
  if (n > 1) w += tracking * (n - 1);
  return w;
}
function drawText(c, tbl, x, yBase, s, col, tracking = 0) {
  let pen = x;
  for (const ch of s) {
    const g = findGlyph(tbl, ch);
    if (!g) { pen += 8; continue; }
    const gx = pen + g.left, gy = yBase + g.top;
    for (let by = 0; by < g.h; by++) for (let bx = 0; bx < g.w; bx++) {
      const a = g.cov[by * g.w + bx];
      if (a) c.blend(gx + bx, gy + by, col, a);
    }
    pen += g.adv + tracking;
  }
}
const drawTextCenter = (c, t, x0, x1, yb, s, col) =>
  drawText(c, t, x0 + ((x1 - x0 - textWidth(t, s)) >> 1), yb, s, col);
const drawTextRight = (c, t, right, yb, s, col) =>
  drawText(c, t, right - textWidth(t, s), yb, s, col);
function drawTextFit(c, tbl, x, yBase, targetW, s, col) {
  const natural = textWidth(tbl, s);
  if (natural <= 0) return;
  let ratio = targetW / natural;
  if (ratio > 1.30) ratio = 1.30;
  if (ratio < 0.60) ratio = 0.60;
  if (natural > targetW) ratio = targetW / natural;
  let pen = x;
  for (const ch of s) {
    const g = findGlyph(tbl, ch);
    if (!g) { pen += 8 * ratio | 0; continue; }
    const adv = (g.adv * ratio + 0.5) | 0;
    const outw = Math.max(1, (g.w * ratio + 0.5) | 0);
    const gx = pen + (g.left * ratio) | 0, gy = yBase + g.top;
    for (let by = 0; by < g.h; by++) for (let bx = 0; bx < outw; bx++) {
      let sx = (bx / ratio) | 0;
      if (sx >= g.w) sx = g.w - 1;
      const a = g.cov[by * g.w + sx];
      if (a) c.blend(gx + bx, gy + by, col, a);
    }
    pen += adv;
  }
}

/* ---------- 假数据 ---------- */
const M = {
  temp: 53.8, cpu: 12, mem: 41,
  uptime: 'UP 3d04:12', iface: 'eth1',
  rx_rate: 1258291, tx_rate: 348160,          /* 1.2M + 340K */
  graph: [0, 2048, 51200, 300000, 1258291, 340000, 0, 4096, 900000, 2097152,
          700000, 12000, 0, 0, 3000000, 1500000, 200000, 45000, 0, 1024,
          600000, 1800000, 2400000, 900000, 60000, 0, 8000, 400000, 1100000,
          260000, 30000, 0, 2000000, 700000, 120000, 4000],
  ifs: [
    { name: 'br-lan', ip: '192.168.31.87' },
    { name: 'eth1', ip: '192.168.31.87' },
  ],
};

function barColor(pct) {
  if (pct >= 80) return C.BAR_HIGH;
  if (pct >= 50) return C.BAR_MID;
  return C.BAR_LOW;
}
function rateToHeight(rate) {
  let h = 0;
  if (rate < 1024) return 0;
  rate /= 1024;
  while (h < GRAPH_LEVELS && rate >= 4) { rate /= 4; h++; }
  return h;
}
function formatRate(b) {
  if (b >= 1073741824) return (b / 1073741824).toFixed(1) + 'G';
  if (b >= 1048576) return (b / 1048576).toFixed(1) + 'M';
  if (b >= 1024) return (b / 1024).toFixed(0) + 'K';
  return b.toFixed(0);
}

/* ---------- 与 render_dashboard 同构 ---------- */
function renderDashboard(c, T) {
  const f34 = T['34'], f14 = T['14'], f10 = T['10'];

  for (let y = 0; y < LCD_H; y++) {
    const col = blend565(C.BG_EDGE, C.BG_CENTER, (y * 255 / (LCD_H - 1)) | 0);
    for (let x = 0; x < LCD_W; x++) c.pix[y * LCD_W + x] = col;
  }
  c.fillAlpha(0, 0, LEFT_RULE, LCD_H, C.PANEL_DARK, 130);

  /* 顶栏 */
  drawText(c, f14, 6, HDR_BL, 'EDGEPI', C.ACCENT, 1);
  drawTextRight(c, f10, LCD_W - 6, HDR_BL - 1, M.uptime, C.TEXT_DIM);
  c.hline(6, LCD_W - 6, HDR_RULE, C.LINE);

  /* 左栏 */
  drawText(c, f10, 8, LEFT_LBL_BL, 'NETWORK', C.TEXT_DIM, 1);
  const top = LEFT_LBL_BL + 12, avail = LCD_H - 6 - top;
  let spacing = Math.floor(avail / M.ifs.length);
  if (spacing > 30) spacing = 30;
  if (spacing < 22) spacing = 22;
  const y0 = top + ((avail - spacing * M.ifs.length) >> 1) + 10;
  M.ifs.forEach((it, i) => {
    const yb = y0 + i * spacing;
    drawText(c, f14, 8, yb, it.name, C.ACCENT, 0);
    drawText(c, f10, 9, yb + 12, it.ip, C.TEXT_MID, 0);
  });

  c.vline(LEFT_RULE, 4, LCD_H - 5, C.LINE);

  /* 右栏三格 */
  const x0 = LEFT_RULE + 8, colw = Math.floor((LCD_W - x0 - 6) / 3);
  function cell(cx0, cx1, label, value, pct, withBar) {
    drawTextCenter(c, f10, cx0, cx1, CELL_LBL_BL, label, C.TEXT_DIM);
    drawTextFit(c, f34, cx0 + 8, CELL_VAL_BL, cx1 - cx0 - 16, value, C.TEXT);
    if (withBar) {
      const w = Math.max(10, cx1 - cx0 - 22), bx = cx0 + 11;
      c.fill(bx, CELL_BAR_Y, w, 4, C.PANEL);
      const fillw = pct * w / 100 | 0;
      if (fillw > 0) c.fill(bx, CELL_BAR_Y, fillw, 4, barColor(pct));
    }
  }
  cell(x0, x0 + colw, 'TEMP', M.temp.toFixed(0) + 'C', 0, false);
  cell(x0 + colw, x0 + colw * 2, 'CPU', M.cpu + '%', M.cpu, true);
  cell(x0 + colw * 2, LCD_W - 6, 'MEM', M.mem + '%', M.mem, true);

  /* 曲线 */
  let mx = 0;
  for (const v of M.graph) if (v > mx) mx = v;
  drawText(c, f10, LEFT_RULE + 8, GRAPH_TTL_BL, M.iface + '  RX+TX 240s', C.TEXT_MID, 0);
  drawTextRight(c, f10, LCD_W - 6, GRAPH_TTL_BL,
    formatRate(M.rx_rate + M.tx_rate) + '/s  max ' + formatRate(mx) + '/s', C.TEXT_DIM);

  c.hline(LEFT_RULE + 8, LCD_W - 6, GRAPH_BASE, C.LINE);
  let pitch = Math.floor((LCD_W - 8 - (LEFT_RULE + 8)) / GRAPH_COLS);
  if (pitch < 5) pitch = 5;
  for (let k = 0; k < GRAPH_COLS; k++) {
    const h = rateToHeight(M.graph[k]);
    const x = LEFT_RULE + 10 + k * pitch;
    if (x + 4 > LCD_W - 6) break;
    for (let i = 0; i < h; i++) {
      const y = GRAPH_BASE - 2 - i * GRAPH_STEP;
      if (y - GRAPH_STEP + 2 < GRAPH_TTL_BL + 4) break;
      c.fill(x, y - GRAPH_STEP + 2, 4, GRAPH_STEP, i >= h - 2 ? C.GRAPH_TOP : C.GRAPH_LOW);
    }
  }
}

const src = fs.readFileSync(FONT_H, 'utf8');
const tiers = parseFont(src);
const c = new Canvas();
renderDashboard(c, tiers);
const rgb = new Uint8Array(LCD_W * LCD_H * 3);
for (let i = 0; i < LCD_W * LCD_H; i++) {
  const [r, g, b] = unpack565(c.pix[i]);
  rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
}
console.log('写出', OUT, writePNG(OUT, LCD_W, LCD_H, rgb), '字节');
