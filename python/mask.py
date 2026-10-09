#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mask.py — 聊天截图自动打码：抹掉头像 + 名字（纯 PIL 实现，不依赖 numpy）
用法: python3 mask.py 图片1 图片2 ... [--outdir DIR] [--debug]

原理（无需 OCR）:
  1) 头像 = 左侧固定列里的大块彩色区域（背景是纯深色，好区分）
  2) 名字 = 内容区里的"灰色小字行"（聊天正文是白色，亮度不同）
  3) 标题栏 = 顶部白色加粗文字行（里面也含名字）
"""
import os
import time
import argparse
from PIL import Image, ImageOps, ImageChops, ImageDraw


# ---------------- 基础工具 ----------------

def runs_of(vals, thr):
    """一维数值列表中连续 >thr 的区间 [(i0,i1), ...]"""
    out = []
    i = 0
    n = len(vals)
    while i < n:
        if vals[i] > thr:
            j = i
            while j + 1 < n and vals[j + 1] > thr:
                j += 1
            out.append((i, j))
            i = j + 1
        else:
            i += 1
    return out


def row_profile(mask, box):
    """按行统计：每行内容像素占比（0..255）"""
    x0, y0, x1, y1 = box
    reg = mask.crop((x0, y0, x1, y1))
    w, h = reg.size
    return list(reg.resize((1, h), Image.BOX).getdata())


def col_profile(mask, box):
    """按列统计：每列内容像素占比（0..255）"""
    x0, y0, x1, y1 = box
    reg = mask.crop((x0, y0, x1, y1))
    w, h = reg.size
    return list(reg.resize((w, 1), Image.BOX).getdata())


def mean_bright_sat(mx, sat, cmask, box):
    """区域里"内容像素"的平均亮度 / 平均饱和度"""
    mreg = cmask.crop(box)
    ma = mreg.resize((1, 1), Image.BOX).getdata()[0]
    if ma == 0:
        return None
    bmul = ImageChops.multiply(mx.crop(box), mreg).resize((1, 1), Image.BOX).getdata()[0]
    smul = ImageChops.multiply(sat.crop(box), mreg).resize((1, 1), Image.BOX).getdata()[0]
    return bmul * 255.0 / ma, smul * 255.0 / ma


def mosaic(img, box, nv=4):
    """对 box 打马赛克, nv = 竖方向切几块"""
    x0, y0, x1, y1 = [int(round(v)) for v in box]
    x0 = max(0, x0); y0 = max(0, y0)
    x1 = min(img.width, x1); y1 = min(img.height, y1)
    if x1 - x0 < 4 or y1 - y0 < 4:
        return
    reg = img.crop((x0, y0, x1, y1))
    w, h = reg.size
    block = max(6, round(h / nv))
    sw = max(1, round(w / block))
    sh = max(1, round(h / block))
    reg = reg.resize((sw, sh), Image.BILINEAR).resize((w, h), Image.NEAREST)
    img.paste(reg, (x0, y0))


def lut(thr, gt=True):
    return [255 if (i > thr if gt else i < thr) else 0 for i in range(256)]


# ---------------- 主流程 ----------------

def process(path, outdir, suffix="_masked", debug=False):
    t0 = time.time()
    im = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    W, H = im.size
    R, G, B = im.split()
    mx = ImageChops.lighter(ImageChops.lighter(R, G), B)        # 最大通道 = 亮度近似
    mn = ImageChops.darker(ImageChops.darker(R, G), B)
    sat = ImageChops.difference(mx, mn)                          # 饱和度近似

    # 背景亮度：取左边缘竖条的中位数
    xb = max(4, int(0.015 * W))
    hist = mx.crop((0, int(0.20 * H), xb, int(0.80 * H))).histogram()
    tot = sum(hist); acc = 0; bgL = 0
    for v, c in enumerate(hist):
        acc += c
        if acc >= tot * 0.5:
            bgL = v
            break
    # 主题自动检测：亮色主题下"内容比背景暗"，判定全部取反
    LIGHT = bgL > 128
    if LIGHT:
        content = ImageChops.lighter(
            mx.point(lut(bgL - 32, gt=False)),
            sat.point(lut(30)))
        content_av = ImageChops.lighter(
            mx.point(lut(bgL - 12, gt=False)),
            sat.point(lut(15)))
    else:
        content = ImageChops.lighter(
            mx.point(lut(bgL + 32)),
            sat.point(lut(30)))
        # 宽松版内容判定：暗色头像（照片类）与背景区分度低，用它来框头像位置
        content_av = ImageChops.lighter(
            mx.point(lut(bgL + 12)),
            sat.point(lut(15)))
    print(f"\n=== {os.path.basename(path)}  {W}x{H}  bgL={bgL}  主题={'亮色' if LIGHT else '深色'}")

    jobs = []

    # ---------- 1) 头像 ----------
    sx0, sx1 = int(0.030 * W), int(0.175 * W)
    y0s, y1s = int(0.055 * H), int(0.985 * H)
    prof = row_profile(content_av, (sx0, y0s, sx1, y1s))
    avatars = []
    for (a, b) in runs_of(prof, 76):          # 行占比 > 0.30
        h = b - a + 1
        if h < int(0.020 * H) or h > int(0.13 * H):
            continue
        aa, bb = a, b                          # 向上下扩展弱边缘
        while aa - 1 >= 0 and prof[aa - 1] > 30:
            aa -= 1
        while bb + 1 < len(prof) and prof[bb + 1] > 30:
            bb += 1
        yy0, yy1 = aa + y0s, bb + y0s
        cols = col_profile(content_av, (sx0, yy0, sx1, yy1 + 1))
        idx = [i for i, v in enumerate(cols) if v > 127]
        if not idx:
            continue
        # 只取最宽的"连续列块"，避免把右边同行的名字文字算进头像宽度
        blocks = []
        cur0 = prev = idx[0]
        for i in idx[1:]:
            if i - prev <= 3:
                prev = i
            else:
                blocks.append((cur0, prev))
                cur0 = prev = i
        blocks.append((cur0, prev))
        b0, b1 = max(blocks, key=lambda t: t[1] - t[0])
        # 亮色主题的坑：导航栏底色会占满整条竖带 → 满宽块不是头像，排除
        if b0 <= 2 and b1 >= len(cols) - 3:
            print(f"  [跳过候选] y=({yy0},{yy1}) 占满整条竖带（导航栏底色）")
            continue
        cx0, cx1 = sx0 + b0, sx0 + b1 + 1
        wd = cx1 - cx0
        if not (0.045 * W <= wd <= 0.22 * W):
            print(f"  [跳过候选] y=({yy0},{yy1}) x=({cx0},{cx1}) w={wd}")
            continue
        avatars.append((cx0, yy0, cx1, yy1))
        jobs.append(((cx0 - 3, yy0 - 3, cx1 + 3, yy1 + 3), "avatar"))
        print(f"  头像 y=({yy0},{yy1}) h={yy1-yy0+1} x=({cx0},{cx1}) w={wd}")

    # ---------- 2) 名字 / 引用（内容区灰色小字） ----------
    av_x1 = max([a[2] for a in avatars], default=0)
    tx0 = int(0.155 * W)
    if av_x1:
        tx0 = min(max(tx0, av_x1 + 2), int(0.175 * W))
    tx1 = int(0.80 * W)
    ty0 = int(0.110 * H)
    tprof = row_profile(content, (tx0, ty0, tx1, H))
    for (a, b) in runs_of(tprof, 3):
        h = b - a + 1
        if h < int(0.008 * H):
            continue
        yy0, yy1 = ty0 + a, ty0 + b
        box = (tx0, yy0, tx1, yy1 + 1)
        mbms = mean_bright_sat(mx, sat, content, box)
        if not mbms:
            continue
        mb, ms = mbms
        cols = col_profile(content, box)
        idx = [i for i, v in enumerate(cols) if v > 13]
        if not idx:
            continue
        cx0, cx1 = tx0 + idx[0], tx0 + idx[-1] + 1
        if LIGHT:
            # 亮色主题：名字(实测≈142)比正文(≈75)【亮】；
            # 正文里的表情会拉高饱和度 → 用 sat/h 双保险排除
            gray = (mb > 110) and (ms < 30) and (h <= 45)
        else:
            gray = (mb < 160) and (ms < 45)
        print(f"  文字行 y=({yy0},{yy1}) h={h} 亮度={mb:.0f} 饱和={ms:.0f}"
              f" -> {'打码' if gray else '跳过'}")
        if gray:
            jobs.append(((cx0 - 8, yy0 - 6, cx1 + 8, yy1 + 6), "text"))

    # ---------- 3) 顶部标题栏（含名字） ----------
    # 深色主题 = 白色加粗字；亮色主题 = 深色字（判定取反、范围放宽）
    if LIGHT:
        hy0, hy1 = int(0.055 * H), int(0.115 * H)
        white = ImageChops.darker(mx.point(lut(110, gt=False)), sat.point(lut(60, gt=False)))
    else:
        hy0, hy1 = int(0.060 * H), int(0.102 * H)
        white = ImageChops.darker(mx.point(lut(180)), sat.point(lut(60, gt=False)))
    hx0, hx1 = int(0.15 * W), int(0.90 * W)
    rp = row_profile(white, (hx0, hy0, hx1, hy1))
    rows = [i for i, v in enumerate(rp) if v > 3]
    # 把连续的行分组：丢掉紧贴窗口顶部的一段（亮色主题里是状态栏药丸的底边），
    # 再取第一段 = 标题行；其后的段（如日期）不打码
    runs = []
    for i in rows:
        if runs and i - runs[-1][1] <= 2:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    if runs and runs[0][0] <= 2:
        runs = runs[1:]
    if runs:
        r0, r1 = runs[0]
        ryy0, ryy1 = hy0 + r0, hy0 + r1
        cp = col_profile(white, (hx0, ryy0, hx1, ryy1 + 1))
        cols = [i for i, v in enumerate(cp) if v > 2]
        if cols:
            box = (hx0 + cols[0] - 8, ryy0 - 6, hx0 + cols[-1] + 9, ryy1 + 7)
            jobs.append((box, "title"))
            print(f"  标题打码 box={box}")

    # ---------- 打码 ----------
    for box, kind in jobs:
        nv = {"avatar": 4, "text": 3, "title": 3}[kind]
        mosaic(im, box, nv)

    if debug:
        di = im.copy()
        d = ImageDraw.Draw(di)
        for box, kind in jobs:
            d.rectangle(box, outline=(255, 60, 60), width=3)
        dbg = os.path.join(outdir, os.path.splitext(os.path.basename(path))[0] + "_debug.png")
        di.save(dbg)
        print(f"  debug -> {dbg}")

    base = os.path.splitext(os.path.basename(path))[0]
    outpath = os.path.join(outdir, f"{base}{suffix}.png")
    im.save(outpath)
    print(f"  => 已输出 {outpath}  ({time.time()-t0:.1f}s)")
    return outpath


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("images", nargs="+")
    ap.add_argument("--outdir", default="out")
    ap.add_argument("--suffix", default="_masked")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    for p in args.images:
        process(p, args.outdir, args.suffix, args.debug)
