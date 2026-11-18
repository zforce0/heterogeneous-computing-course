#!/usr/bin/env python3
"""calib_web.py — 本地网页版双摄配准标定工具 (零第三方依赖)

[Tab 1 选点标定] RGB/IR 交替点对应点 → 自动算出红外视野在可见光中的
                 四边形 (不规则) + 残差
[Tab 2 画面截取] 在可见光大图上直接拖动四边形的 4 个顶点,
                 控制可见光的截取范围 (不规则四边形);
                 四边形内的内容被实时"取出来"变换为红外视角,
                 与红外图叠加/闪烁对比, 直到两画面重合

用法:
  cd twocameraOnMAC
  python3 test/camera_capture_test/calib_web.py          # http://localhost:8765
  python3 test/camera_capture_test/calib_web.py --test   # 内置自检
  python3 test/camera_capture_test/calib_web.py --port 9000 --no-browser
"""

import glob
import json
import os
import socketserver
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler

PORT = 8765
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))


# ---------------------------------------------------------------------------
# 纯 Python 线性代数
# ---------------------------------------------------------------------------

def solve_linear(a, b):
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for col in range(n):
        pivot = max(range(col, n), key=lambda r: abs(m[r][col]))
        if abs(m[pivot][col]) < 1e-12:
            return None
        if pivot != col:
            m[col], m[pivot] = m[pivot], m[col]
        d = m[col][col]
        for j in range(col, n + 1):
            m[col][j] /= d
        for r in range(n):
            if r != col and m[r][col] != 0.0:
                f = m[r][col]
                for j in range(col, n + 1):
                    m[r][j] -= f * m[col][j]
    return [m[i][n] for i in range(n)]


def homography(src, dst):
    """最小二乘 DLT 单应 (h33=1), src→dst, ≥4 对点"""
    ata = [[0.0] * 8 for _ in range(8)]
    atb = [0.0] * 8
    for (x, y), (u, v) in zip(src, dst):
        rows = [
            ([x, y, 1, 0, 0, 0, -u * x, -u * y], u),
            ([0, 0, 0, x, y, 1, -v * x, -v * y], v),
        ]
        for coeffs, rhs in rows:
            for i in range(8):
                for j in range(8):
                    ata[i][j] += coeffs[i] * coeffs[j]
                atb[i] += coeffs[i] * rhs
    h = solve_linear(ata, atb)
    if h is None:
        return None
    return [h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], 1.0]


def apply_h(h, x, y):
    d = h[6] * x + h[7] * y + h[8]
    if abs(d) < 1e-12:
        return None
    return ((h[0] * x + h[1] * y + h[2]) / d,
            (h[3] * x + h[4] * y + h[5]) / d)


def inv3(m):
    """3x3 矩阵求逆 (伴随矩阵法)"""
    a, b, c, d, e, f, g, h, i = m
    ca = e * i - f * h
    cb = -(d * i - f * g)
    cc = d * h - e * g
    det = a * ca + b * cb + c * cc
    if abs(det) < 1e-12:
        return None
    return [ca / det, -(b * i - c * h) / det, (b * f - c * e) / det,
            cb / det, (a * i - c * g) / det, -(a * f - c * d) / det,
            cc / det, -(a * h - b * g) / det, (a * e - b * d) / det]


def compute_quad(pairs, rgb_w, rgb_h, ir_w, ir_h):
    """pairs: [[ir_x, ir_y, rgb_x, rgb_y], ...]
    → 红外四角在可见光中的四边形 + 残差"""
    src = [(p[0], p[1]) for p in pairs]
    dst = [(p[2], p[3]) for p in pairs]
    h = homography(src, dst)          # ir → rgb
    if h is None:
        return {"ok": False, "error": "homography solve failed"}
    if inv3(h) is None:
        return {"ok": False, "error": "homography not invertible"}

    residuals = []
    for i, (s, d) in enumerate(zip(src, dst)):
        m = apply_h(h, *s)
        err = 1e9 if m is None else ((m[0] - d[0]) ** 2 +
                                     (m[1] - d[1]) ** 2) ** 0.5
        residuals.append({"pair": i, "err_px": round(err, 2)})
    max_err = max(r["err_px"] for r in residuals)

    quad = []
    for x, y in [(0, 0), (ir_w, 0), (ir_w, ir_h), (0, ir_h)]:
        m = apply_h(h, x, y)
        if m is None:
            return {"ok": False, "error": "degenerate homography"}
        quad.append([round(m[0], 1), round(m[1], 1)])

    return {
        "ok": True,
        "quad": quad,       # 左上/右上/右下/左下 (rgb 坐标)
        "residuals": residuals,
        "max_err_px": round(max_err, 2),
    }


def self_test():
    # 已知: IR(256x192) → RGB 仿射 (缩放+平移)
    sx, sy = 585 / 256.0, 439 / 192.0
    pairs = [[x, y, x * sx + 55, y * sy + 26]
             for (x, y) in [(20, 15), (230, 20), (240, 180), (10, 170)]]
    r = compute_quad(pairs, 640, 480, 256, 192)
    assert r["ok"] and r["max_err_px"] < 0.01
    q = r["quad"]
    assert abs(q[0][0] - 55) <= 1 and abs(q[0][1] - 26) <= 1
    assert abs(q[2][0] - 640) <= 1 and abs(q[2][1] - 465) <= 1
    print("self-test PASS: quad =", q)


# ---------------------------------------------------------------------------
# 网页
# ---------------------------------------------------------------------------

HTML = r"""<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8">
<title>双摄配准标定</title>
<style>
 body { font-family: sans-serif; margin: 12px; background: #222; color: #eee; }
 h2 { margin: 4px 0 10px; }
 .tabs { display:flex; gap:6px; margin-bottom:10px; }
 .tab { padding:8px 20px; background:#333; cursor:pointer; border-radius:6px 6px 0 0; }
 .tab.on { background:#555; font-weight:bold; }
 #bar { display:flex; gap:8px; align-items:center; margin-bottom:10px; flex-wrap:wrap; }
 button { padding:6px 14px; font-size:14px; cursor:pointer; }
 #status { padding:6px 12px; background:#333; border-radius:4px; font-size:15px; }
 #status .rgb { color:#6cf; } #status .ir { color:#f86; }
 .panels { display:flex; gap:16px; flex-wrap:wrap; }
 .panel { background:#181818; padding:8px; border-radius:6px; }
 .panel h3 { margin:0 0 6px; }
 .imgwrap { position:relative; display:inline-block; line-height:0; cursor:crosshair; }
 .imgwrap img { width:100%; display:block; }
 #rgbwrap { width:640px; } #irwrap { width:512px; }
 #irwrap img { image-rendering:pixelated; }
 .pt { position:absolute; width:9px; height:9px; border-radius:50%;
       transform:translate(-50%,-50%); pointer-events:none; }
 .pt .lbl { position:absolute; left:8px; top:-14px; font-size:12px;
            font-weight:bold; color:#fff; text-shadow:0 0 2px #000; }
 .pending { background:#ff3030; } .paired { background:#28c828; }
 #result, .outbox { margin-top:12px; background:#333; padding:10px 14px;
           border-radius:6px; white-space:pre-wrap; font-family:monospace;
           font-size:14px; }
 table { border-collapse:collapse; margin-top:6px; }
 td, th { border:1px solid #555; padding:2px 10px; font-size:13px; }
 .hint { color:#aaa; font-size:13px; margin-top:8px; }
 canvas.view { display:block; background:#000; }
 #rgbview { width:640px; cursor:crosshair; }
 .preview { width:512px; image-rendering:pixelated; }
 .hidden { display:none; }
</style></head><body>
<h2>双摄配准标定 <span id="pairinfo" style="color:#888;font-size:14px"></span></h2>
<div class="tabs">
 <div class="tab on" id="tab-pick" onclick="switchTab('pick')">1. 选点标定</div>
 <div class="tab" id="tab-align" onclick="switchTab('align')">2. 画面截取 (拖四边形)</div>
</div>

<!-- ======================= Tab 1: 选点标定 ======================= -->
<div id="pick">
<div id="bar">
 <button onclick="nav(-1)">&#8592; 上一对</button>
 <button onclick="nav(1)">下一对 &#8594;</button>
 <span id="status">loading...</span>
 <button onclick="undo()">撤销 (右键亦可)</button>
 <button onclick="clearPts()">清空</button>
 <button onclick="compute()" style="background:#2a2;color:#fff">计算 (&#8805;4 对)</button>
</div>
<div class="panels">
 <div class="panel"><h3>可见光 RGB <span style="color:#888">(先点这里)</span></h3>
  <div class="imgwrap" id="rgbwrap"><img id="rgbimg"><svg id="rgbsvg"
   style="position:absolute;left:0;top:0;width:100%;height:100%;pointer-events:none"></svg></div></div>
 <div class="panel"><h3>红外 IR <span style="color:#888">(再点同一位置)</span></h3>
  <div class="imgwrap" id="irwrap"><img id="irimg"></div></div>
</div>
<div id="result"></div>
<div class="hint">
 交替点选: RGB 特征点 &#8594; IR 同一物理位置。建议 4~8 对、覆盖画面四角。
 计算后会自动得到"红外视野在可见光中的四边形", 切到 [2. 画面截取] 拖顶点精调。
</div>
</div>

<!-- ======================= Tab 2: 画面截取 ======================= -->
<div id="align" class="hidden">
<div id="bar">
 <button onclick="nav(-1)">&#8592; 上一对</button>
 <button onclick="nav(1)">下一对 &#8594;</button>
 显示:
 <button onclick="setMode('blend')" id="m-blend">叠加</button>
 <button onclick="setMode('ir')" id="m-ir">只看红外</button>
 <button onclick="setMode('vis')" id="m-vis">只看截取结果</button>
 <label style="font-size:14px"><input type="checkbox" id="blink"
  onchange="setBlink(this.checked)"> 闪烁对比 (对错位最敏感)</label>
 <button onclick="resetQuad()">重置为标定四边形</button>
 <button onclick="saveCalib()" style="background:#26a;color:#fff">保存到 calibration.txt</button>
</div>
<div class="panels">
 <div class="panel"><h3>可见光原图 + 截取四边形
   <span style="color:#888">(拖动 4 个绿点; 深色区域=被丢弃)</span></h3>
  <canvas id="rgbview" class="view" width="640" height="480"></canvas>
 </div>
 <div class="panel"><h3>截取结果 vs 红外 <span style="color:#888">(应重合)</span></h3>
  <canvas id="quadv" class="view preview" width="256" height="192"></canvas>
  <div class="hint" style="max-width:512px">
   四边形内的可见光内容被"取出"并变换为红外视角 (256&#215;192)。
   拖动顶点使叠加/闪烁预览中同一物体完全重合;
   顶点顺序: 0=左上 1=右上 2=右下 3=左下 (对应红外画面四角)。
  </div>
 </div>
</div>
<div id="quadout" class="outbox">-</div>
</div>

<script>
let index = 0, count = 0, names = [];
let pairs = [], pending = null, lastResult = null;
let quad = null;          // [{x,y} x4] 左上/右上/右下/左下
let quadDrag = -1;
let alignMode = 'blend', blinkOn = false, blinkPhase = 0;
let rgbPixels = null;     // 可见光原图像素缓存
let warpCache = null;     // {key, img, blackRatio, h}
let calibQuad = null;     // Tab1 标定出的四边形 (重置用)

const rgbimg = document.getElementById('rgbimg');
const irimg  = document.getElementById('irimg');
const rgbsvg = document.getElementById('rgbsvg');

async function loadIndex(i) {
  const r = await fetch('/api/pairs');
  const j = await r.json();
  count = j.count; names = j.names;
  index = ((i % count) + count) % count;
  rgbimg.src = '/api/image?i=' + index + '&t=rgb&r=' + Math.random();
  irimg.src  = '/api/image?i=' + index + '&t=ir&r=' + Math.random();
  document.getElementById('pairinfo').textContent =
    '  (' + (index+1) + '/' + count + ')  ' + names[index];
  pairs = []; pending = null; lastResult = null;
  redraw(); renderAlign();
}

function switchTab(t) {
  document.getElementById('pick').classList.toggle('hidden', t !== 'pick');
  document.getElementById('align').classList.toggle('hidden', t !== 'align');
  document.getElementById('tab-pick').classList.toggle('on', t === 'pick');
  document.getElementById('tab-align').classList.toggle('on', t === 'align');
}

/* ---------- Tab 1: 选点 ---------- */
function status() {
  const s = document.getElementById('status');
  if (pending === null)
    s.innerHTML = 'pairs:' + pairs.length + ' &#8594; 在 <span class="rgb">RGB</span> 图点下一个点';
  else
    s.innerHTML = 'pairs:' + pairs.length + '+1 &#8594; 在 <span class="ir">IR</span> 图点同一位置';
}

function toImgCoords(ev, img) {
  const r = img.getBoundingClientRect();
  return [Math.round((ev.clientX - r.left) / r.width * img.naturalWidth),
          Math.round((ev.clientY - r.top) / r.height * img.naturalHeight)];
}

function addDot(wrap, pt, cls, idx) {
  const img = wrap.querySelector('img');
  const d = document.createElement('div');
  d.className = 'pt ' + cls;
  d.style.left = (pt[0] / img.naturalWidth * 100) + '%';
  d.style.top  = (pt[1] / img.naturalHeight * 100) + '%';
  d.innerHTML = '<span class="lbl">' + idx + '</span>';
  wrap.appendChild(d);
}

function redraw() {
  document.querySelectorAll('.pt').forEach(e => e.remove());
  rgbsvg.innerHTML = '';
  pairs.forEach((p, i) => {
    addDot(document.getElementById('rgbwrap'), p.rgb, 'paired', i);
    addDot(document.getElementById('irwrap'), p.ir, 'paired', i);
  });
  if (pending) addDot(document.getElementById('rgbwrap'), pending.rgb,
                      'pending', pairs.length);
  if (lastResult && lastResult.ok) {
    const q = lastResult.quad;
    rgbsvg.innerHTML =
      '<polygon points="' + q.map(p => p.join(',')).join(' ') +
      '" fill="none" stroke="#0ff" stroke-width="2"/>';
  }
  status();
}

function onClick(ev, which) {
  const pt = toImgCoords(ev, which === 'rgb' ? rgbimg : irimg);
  if (which === 'rgb' && pending === null) { pending = {rgb: pt}; }
  else if (which === 'ir' && pending !== null) {
    pairs.push({rgb: pending.rgb, ir: pt}); pending = null;
  }
  redraw();
}

function undo() {
  if (pending) pending = null;
  else if (pairs.length) pairs.pop();
  lastResult = null; redraw();
}

async function compute() {
  if (pairs.length < 4) { alert('至少需要 4 对点, 当前 ' + pairs.length); return; }
  const body = {
    rgb_w: rgbimg.naturalWidth, rgb_h: rgbimg.naturalHeight,
    ir_w: irimg.naturalWidth, ir_h: irimg.naturalHeight,
    pairs: pairs.map(p => [p.ir[0], p.ir[1], p.rgb[0], p.rgb[1]])
  };
  const r = await fetch('/api/calculate', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body)});
  const j = await r.json();
  const box = document.getElementById('result');
  if (!j.ok) { box.textContent = '计算失败: ' + j.error; return; }
  lastResult = j;
  calibQuad = j.quad.map(p => ({x: Math.round(p[0]), y: Math.round(p[1])}));
  quad = calibQuad.map(p => ({x: p.x, y: p.y}));
  warpCache = null;
  let tbl = '<table><tr><th>点对</th><th>残差(px)</th></tr>';
  j.residuals.forEach(x => {
    tbl += '<tr><td>#' + x.pair + '</td><td' +
      (x.err_px > 5 ? ' style="color:#f55"' : '') + '>' + x.err_px + '</td></tr>';
  });
  tbl += '</table>';
  box.innerHTML =
    '红外视野四边形 (RGB 坐标, 左上/右上/右下/左下):\n' +
    j.quad.map((p, i) => '  q' + i + '=(' + p[0] + ', ' + p[1] + ')').join('  ') +
    '\nmax_err=' + j.max_err_px + 'px\n' +
    '已同步到 [2. 画面截取], 可拖顶点继续精调。' + tbl;
  redraw(); renderAlign();
}

/* ---------- 通用: 4 点单应 (前端求解, rect → quad) ---------- */
function solve8(ata, atb) {
  const n = 8;
  const m = ata.map((row, i) => row.concat([atb[i]]));
  for (let col = 0; col < n; col++) {
    let piv = col;
    for (let r = col + 1; r < n; r++)
      if (Math.abs(m[r][col]) > Math.abs(m[piv][col])) piv = r;
    if (Math.abs(m[piv][col]) < 1e-12) return null;
    const t = m[col]; m[col] = m[piv]; m[piv] = t;
    const d = m[col][col];
    for (let j = col; j <= n; j++) m[col][j] /= d;
    for (let r = 0; r < n; r++) if (r !== col && m[r][col] !== 0) {
      const f = m[r][col];
      for (let j = col; j <= n; j++) m[r][j] -= f * m[col][j];
    }
  }
  return m.map(row => row[n]);
}

/* pairs: [[u,v,x,y],...] 输出(u,v)→rgb(x,y) 的 3x3 单应 */
function solveH4(pairs4) {
  const ata = Array.from({length: 8}, () => new Array(8).fill(0));
  const atb = new Array(8).fill(0);
  for (const [u, v, x, y] of pairs4) {
    const rows = [
      [[u, v, 1, 0, 0, 0, -x*u, -x*v], x],
      [[0, 0, 0, u, v, 1, -y*u, -y*v], y],
    ];
    for (const [c, rhs] of rows)
      for (let i = 0; i < 8; i++) {
        for (let j = 0; j < 8; j++) ata[i][j] += c[i] * c[j];
        atb[i] += c[i] * rhs;
      }
  }
  const h = solve8(ata, atb);
  return h ? h.concat([1]) : null;
}

/* 像素缓存: 可见光原图 */
function cacheRgbPixels() {
  if (!rgbimg.naturalWidth) return;
  const c = document.createElement('canvas');
  c.width = rgbimg.naturalWidth; c.height = rgbimg.naturalHeight;
  const cx = c.getContext('2d');
  cx.drawImage(rgbimg, 0, 0);
  rgbPixels = cx.getImageData(0, 0, c.width, c.height);
  warpCache = null;
}

/* 双线性采样 */
function samplePx(data, W, H, x, y) {
  if (x < 0 || y < 0 || x > W - 1.001 || y > H - 1.001) return null;
  const x0 = Math.floor(x), y0 = Math.floor(y);
  const fx = x - x0, fy = y - y0;
  const i00 = (y0 * W + x0) * 4;
  const i10 = i00 + 4, i01 = i00 + W * 4, i11 = i01 + 4;
  const out = new Array(3);
  for (let ch = 0; ch < 3; ch++) {
    out[ch] = data[i00+ch]*(1-fx)*(1-fy) + data[i10+ch]*fx*(1-fy) +
              data[i01+ch]*(1-fx)*fy     + data[i11+ch]*fx*fy;
  }
  return out;
}

/* 四边形 → 红外视角 (256x192) 的截取结果 */
function computeQuadWarp() {
  if (!quad || !rgbPixels) return null;
  const key = quad.map(p => p.x + ',' + p.y).join(';');
  if (warpCache && warpCache.key === key) return warpCache;
  // 输出矩形四角 → 四边形顶点 (0左上 1右上 2右下 3左下)
  const h = solveH4([
    [0, 0, quad[0].x, quad[0].y],
    [256, 0, quad[1].x, quad[1].y],
    [256, 192, quad[2].x, quad[2].y],
    [0, 192, quad[3].x, quad[3].y]]);
  if (!h) return null;
  const W = rgbPixels.width, Hh = rgbPixels.height;
  const data = rgbPixels.data;
  const img = new ImageData(256, 192);
  const dst = img.data;
  let black = 0;
  for (let v = 0; v < 192; v++) {
    for (let u = 0; u < 256; u++) {
      const d = h[6]*u + h[7]*v + h[8];
      let px = null;
      if (Math.abs(d) > 1e-12) {
        const x = (h[0]*u + h[1]*v + h[2]) / d;
        const y = (h[3]*u + h[4]*v + h[5]) / d;
        px = samplePx(data, W, Hh, x, y);
      }
      const o = (v * 256 + u) * 4;
      if (px === null) { black++; dst[o+3] = 255; continue; }
      dst[o] = px[0]; dst[o+1] = px[1]; dst[o+2] = px[2]; dst[o+3] = 255;
    }
  }
  warpCache = {key: key, img: img, blackRatio: black / (256*192), h: h};
  return warpCache;
}

/* ---------- Tab 2: 交互与渲染 ---------- */
function setMode(m) {
  alignMode = m; renderAlign();
  ['blend','ir','vis'].forEach(k =>
    document.getElementById('m-'+k).style.background = k===m ? '#2a2' : '');
}
function setBlink(on) {
  blinkOn = on;
  if (on && !setBlink.timer)
    setBlink.timer = setInterval(() => {
      blinkPhase = 1 - blinkPhase; renderAlign();
    }, 700);
  if (!on && setBlink.timer) {
    clearInterval(setBlink.timer); setBlink.timer = null; renderAlign();
  }
}
function resetQuad() {
  if (!calibQuad) { alert('请先在 [1. 选点标定] 计算'); return; }
  quad = calibQuad.map(p => ({x: p.x, y: p.y}));
  warpCache = null; renderAlign();
}

async function saveCalib() {
  if (!quad) { alert('尚无四边形可保存'); return; }
  const r = await fetch('/api/save', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({quad: quad.map(p => [p.x, p.y])})});
  const j = await r.json();
  const out = document.getElementById('quadout');
  if (!j.ok) { alert('保存失败: ' + j.error); return; }
  alert('已写入 ' + j.path + '\n主程序下次启动自动生效 (无需重编译)');
  out.textContent += '\n[已保存到 ' + j.path + ' — 主程序下次启动自动加载]';
}

function drawQuadView() {
  const ctx = document.getElementById('rgbview').getContext('2d');
  ctx.drawImage(rgbimg, 0, 0, 640, 480);
  if (!quad) {
    ctx.fillStyle = 'rgba(0,0,0,0.4)'; ctx.fillRect(0, 0, 640, 480);
    ctx.fillStyle = '#ff8'; ctx.font = '20px sans-serif';
    ctx.fillText('请先在 [1. 选点标定] 计算出四边形', 130, 240);
    return;
  }
  // 四边形外暗化 (evenodd)
  ctx.save();
  ctx.beginPath();
  ctx.rect(0, 0, 640, 480);
  ctx.moveTo(quad[0].x, quad[0].y);
  for (let i = 3; i >= 0; i--) ctx.lineTo(quad[i].x, quad[i].y);
  ctx.closePath();
  ctx.fillStyle = 'rgba(0,0,0,0.55)';
  ctx.fill('evenodd');
  ctx.restore();
  // 四边形边
  ctx.strokeStyle = '#3f3'; ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(quad[0].x, quad[0].y);
  for (let i = 1; i < 4; i++) ctx.lineTo(quad[i].x, quad[i].y);
  ctx.closePath(); ctx.stroke();
  // 顶点
  quad.forEach((p, i) => {
    ctx.beginPath();
    ctx.arc(p.x, p.y, 8, 0, 2 * Math.PI);
    ctx.fillStyle = i === quadDrag ? '#ff0' : '#3f3';
    ctx.fill();
    ctx.strokeStyle = '#fff'; ctx.lineWidth = 1; ctx.stroke();
    ctx.fillStyle = '#fff'; ctx.font = 'bold 13px sans-serif';
    ctx.fillText('' + i, p.x + 10, p.y - 8);
  });
}

function rgbViewPos(ev) {
  const r = document.getElementById('rgbview').getBoundingClientRect();
  return [(ev.clientX - r.left) / r.width * 640,
          (ev.clientY - r.top) / r.height * 480];
}
function quadDown(ev) {
  ev.preventDefault();
  if (!quad) return;
  const [x, y] = rgbViewPos(ev);
  quadDrag = -1;
  quad.forEach((p, i) => {
    if (Math.hypot(p.x - x, p.y - y) < 14) quadDrag = i;
  });
}
function quadMove(ev) {
  if (quadDrag < 0) return;
  ev.preventDefault();
  const [x, y] = rgbViewPos(ev);
  quad[quadDrag].x = Math.max(0, Math.min(x, 640));
  quad[quadDrag].y = Math.max(0, Math.min(y, 480));
  renderAlign();
}
function quadUp() { quadDrag = -1; renderAlign(); }

function renderAlign() {
  drawQuadView();
  const ctx = document.getElementById('quadv').getContext('2d');
  const out = document.getElementById('quadout');
  ctx.clearRect(0, 0, 256, 192);
  if (!quad) {
    ctx.fillStyle = '#333'; ctx.fillRect(0, 0, 256, 192);
    out.textContent = '尚无四边形: 请先在 [1. 选点标定] 计算或直接切图后在左侧拖动。';
    return;
  }
  let mode = alignMode;
  if (blinkOn) mode = blinkPhase ? 'ir' : 'vis';
  const w = computeQuadWarp();
  if (!w) { out.textContent = '变换求解失败, 检查四边形是否退化'; return; }
  if (mode !== 'ir') ctx.putImageData(w.img, 0, 0);
  if (mode === 'blend') {
    ctx.globalAlpha = 0.5;
    ctx.drawImage(irimg, 0, 0, 256, 192);
    ctx.globalAlpha = 1.0;
  } else if (mode === 'ir') {
    ctx.drawImage(irimg, 0, 0, 256, 192);
  }
  const qs = quad.map((p, i) => 'q' + i + '=(' + Math.round(p.x) + ',' +
                Math.round(p.y) + ')').join('  ');
  out.textContent =
    '四边形顶点 (RGB 坐标): ' + qs + '\n' +
    '截取变换 H (输出 (u,v) → RGB (x,y), 供主程序逐像素采样):\n[' +
    w.h.map(v => v.toFixed(6)).join(', ') + ']\n' +
    '越界(黑边)占比: ' + (w.blackRatio * 100).toFixed(1) + '%';
}

const rgbview = document.getElementById('rgbview');
rgbview.addEventListener('mousedown', quadDown);
rgbview.addEventListener('mousemove', quadMove);
window.addEventListener('mouseup', quadUp);
rgbview.addEventListener('contextmenu', ev => ev.preventDefault());

document.getElementById('rgbwrap').addEventListener('click', ev => onClick(ev, 'rgb'));
document.getElementById('irwrap').addEventListener('click', ev => onClick(ev, 'ir'));
document.addEventListener('contextmenu', ev => {
  if (!ev.target.closest('#rgbview')) { ev.preventDefault(); undo(); }
});
rgbimg.onload = () => { cacheRgbPixels(); renderAlign(); };
irimg.onload  = () => renderAlign();
loadIndex(0);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    pairs_list = []

    def log_message(self, fmt, *args):
        pass

    def _json(self, obj, code=200):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            data = HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif self.path.startswith("/api/pairs"):
            self._json({"count": len(self.pairs_list),
                        "names": [os.path.basename(p)
                                  for p in self.pairs_list]})
        elif self.path.startswith("/api/image"):
            from urllib.parse import urlparse, parse_qs
            q = parse_qs(urlparse(self.path).query)
            i = int(q["i"][0])
            t = q["t"][0]
            rgb = self.pairs_list[i]
            path = rgb if t == "rgb" else rgb.replace("images/",
                                                      "imagesIR/")
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError:
                self._json({"error": "not found"}, 404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        if self.path.startswith("/api/calculate"):
            length = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(length))
            result = compute_quad(req["pairs"], req["rgb_w"],
                                  req["rgb_h"], req["ir_w"], req["ir_h"])
            self._json(result)
        elif self.path.startswith("/api/save"):
            length = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(length))
            try:
                quad = req["quad"]
                assert len(quad) == 4
                vals = [float(v) for pt in quad for v in pt]
            except (KeyError, AssertionError, TypeError, ValueError):
                self._json({"ok": False, "error": "bad quad"})
                return
            path = os.path.join(ROOT, "calibration.txt")
            try:
                with open(path, "w") as f:
                    f.write(
                        "# twocameraOnMAC 双摄配准标定文件\n"
                        "# 由 test/camera_capture_test/calib_web.py 自动生成, "
                        "主程序启动时加载\n"
                        "# (摄像头被动过/重新安装后: 重拍标定图 → "
                        "calib_web.py 选点 → 保存即可)\n"
                        "#\n"
                        "# quad: 红外视野在可见光画面中的四边形顶点\n"
                        "# 坐标基于 640x480 可见光帧, "
                        "顺序: 左上 右上 右下 左下\n"
                        "quad " + " ".join("%.1f" % v for v in vals)
                        + "\n")
            except OSError as e:
                self._json({"ok": False, "error": str(e)})
                return
            self._json({"ok": True, "path": path})
        else:
            self._json({"error": "not found"}, 404)


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    if "--test" in sys.argv:
        self_test()
        return

    global PORT
    args = sys.argv[1:]
    if "--port" in args:
        PORT = int(args[args.index("--port") + 1])
    Handler.pairs_list = sorted(
        glob.glob(os.path.join(ROOT, "captures", "images", "*.jpg")))
    if not Handler.pairs_list:
        print("no captures found under captures/images "
              "(run camera_capture_test first)")
        sys.exit(1)

    url = "http://localhost:%d" % PORT
    print("标定图对: %d 组" % len(Handler.pairs_list))
    print("标定工具已启动: %s   (Ctrl+C 退出)" % url)
    if "--no-browser" not in args:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    with Server(("127.0.0.1", PORT), Handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nbye")


if __name__ == "__main__":
    main()
