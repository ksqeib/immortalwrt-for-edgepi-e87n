#!/usr/bin/env node
/*
 * 把 Oswald.ttf 栅格化成 C 头文件（覆盖率位图），供 e87n-display 使用。
 *
 *   node tools/raster-font.js <字体.ttf> <输出.h>
 *
 * 依赖 opentype.js（npm install opentype.js），只在改字模时在本机跑一次，
 * 产物 src/e87n-font.h 入仓，目标机上不需要 node，也不需要字体文件参与渲染。
 *
 * 为什么这样做：面板要彩色大字号，内核 fbcon 的 8x16 单色字体做不到；
 * 而链 FreeType 就要在设备上装库、带 TTF。预先烘成数组后，渲染器只依赖 libc。
 *
 * 字符集故意不含引号和反斜杠，这样生成的是合法 C 单引号字面量，无需转义。
 * 4x 超采样 + 非零环绕填充，覆盖率 0..255。
 */
const fs = require('fs');
const opentype = require('opentype.js');

const TTF = process.argv[2];
const OUT = process.argv[3];
if (!TTF || !OUT) {
  console.error('用法: node raster-font.js <字体.ttf> <输出.h>');
  process.exit(1);
}

const CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.:%/+-@# _";
const SIZES = [56, 34, 14, 10];

const buf = fs.readFileSync(TTF);
const font = opentype.parse(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));

/* 把 path 展平成多边形（每个子路径一个多边形） */
function flatten(path) {
  const polys = [];
  let cur = [];
  let x = 0, y = 0, sx = 0, sy = 0;

  for (const c of path.commands) {
    if (c.type === 'M') {
      if (cur.length > 1) polys.push(cur);
      cur = [[c.x, c.y]];
      x = sx = c.x;
      y = sy = c.y;
    } else if (c.type === 'L') {
      cur.push([c.x, c.y]);
      x = c.x;
      y = c.y;
    } else if (c.type === 'Q') {
      const N = 16;
      for (let i = 1; i <= N; i++) {
        const t = i / N, mt = 1 - t;
        cur.push([mt * mt * x + 2 * mt * t * c.x1 + t * t * c.x,
                  mt * mt * y + 2 * mt * t * c.y1 + t * t * c.y]);
      }
      x = c.x;
      y = c.y;
    } else if (c.type === 'C') {
      const N = 20;
      for (let i = 1; i <= N; i++) {
        const t = i / N, mt = 1 - t;
        cur.push([mt * mt * mt * x + 3 * mt * mt * t * c.x1 + 3 * mt * t * t * c.x2 + t * t * t * c.x,
                  mt * mt * mt * y + 3 * mt * mt * t * c.y1 + 3 * mt * t * t * c.y2 + t * t * t * c.y]);
      }
      x = c.x;
      y = c.y;
    } else if (c.type === 'Z') {
      if (cur.length > 1) polys.push(cur);
      cur = [];
      x = sx;
      y = sy;
    }
  }
  if (cur.length > 1) polys.push(cur);
  return polys;
}

/* 扫描线 + 4x 超采样 */
function raster(polys, x0, y0, w, h) {
  const SS = 4;
  const out = new Uint8Array(w * h);

  for (let sy = 0; sy < h * SS; sy++) {
    const yy = y0 + (sy + 0.5) / SS;
    const xs = [];

    for (const poly of polys) {
      const n = poly.length;
      for (let i = 0; i < n; i++) {
        const a = poly[i], b = poly[(i + 1) % n];
        if ((a[1] <= yy && b[1] > yy) || (b[1] <= yy && a[1] > yy)) {
          const t = (yy - a[1]) / (b[1] - a[1]);
          xs.push(a[0] + t * (b[0] - a[0]));
        }
      }
    }
    xs.sort((p, q) => p - q);

    const row = new Uint8Array(w * SS);
    for (let i = 0; i + 1 < xs.length; i += 2) {
      let pa = Math.round((xs[i] - x0) * SS);
      let pb = Math.round((xs[i + 1] - x0) * SS);
      if (pa < 0) pa = 0;
      if (pb > w * SS) pb = w * SS;
      for (let px = pa; px < pb; px++) row[px] = 1;
    }
    const oy = (sy / SS) | 0;
    for (let ox = 0; ox < w; ox++) {
      let acc = 0;
      for (let s = 0; s < SS; s++) acc += row[ox * SS + s];
      const v = Math.round(acc * 255 / SS);
      if (v > out[oy * w + ox]) out[oy * w + ox] = v;
    }
  }
  return out;
}

const out = [];
out.push('/* 自动生成，勿手改。');
out.push(' * 来源: Oswald.ttf (SIL Open Font License 1.1)');
out.push(' * 生成: tools/raster-font.js  字号: ' + SIZES.join('/') + 'px');
out.push(' * 覆盖率 0..255，非零环绕填充，4x 超采样。');
out.push(' */');
out.push('#ifndef E87N_FONT_H');
out.push('#define E87N_FONT_H');
out.push('#include <stdint.h>');
out.push('');
out.push('struct e87n_glyph {');
out.push('\tchar ch;');
out.push('\tunsigned short w, h;');
out.push('\tshort left, top;');
out.push('\tshort adv;');
out.push('\tconst uint8_t *cov;');
out.push('};');

for (const size of SIZES) {
  const gl = [];

  for (const ch of CHARS) {
    const g = font.charToGlyph(ch);
    const path = g.getPath(0, 0, size);
    const bb = path.getBoundingBox();
    const left = Math.floor(bb.x1) - 1;
    const top = Math.floor(bb.y1) - 1;
    let w = Math.ceil(bb.x2) - left + 1;
    let h = Math.ceil(bb.y2) - top + 1;
    if (!(w > 0) || !(h > 0)) { w = 1; h = 1; }
    const polys = flatten(path);
    const cov = polys.length ? raster(polys, left, top, w, h) : new Uint8Array(w * h);
    const adv = Math.round(g.advanceWidth * size / font.unitsPerEm);

    gl.push({ ch, w, h, left, top, adv, cov });
  }

  const tag = 'font' + size;
  out.push('');
  out.push('/* ---- ' + size + 'px ---- */');
  for (const g of gl)
    out.push('static const uint8_t ' + tag + '_c_' + g.ch.charCodeAt(0) +
             '[] = {' + Array.from(g.cov).join(',') + '};');
  out.push('static const struct e87n_glyph ' + tag + '_glyphs[] = {');
  for (const g of gl)
    out.push("\t{'" + g.ch + "', " + g.w + ', ' + g.h + ', ' + g.left + ', ' + g.top +
             ', ' + g.adv + ', ' + tag + '_c_' + g.ch.charCodeAt(0) + '},');
  out.push('};');
  out.push('#define ' + tag.toUpperCase() + '_COUNT ' + gl.length);
}
out.push('');
out.push('#endif');

fs.writeFileSync(OUT, out.join('\n'));
console.log('OK 写出 ' + OUT + '  (' + out.join('\n').length + ' 字节, ' +
            SIZES.length + ' 档 x ' + CHARS.length + ' 字形)');
