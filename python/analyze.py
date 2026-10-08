#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze.py — shotmask 校准助手（只读诊断，不改图）
用法: python3 analyze.py 图片1 [图片2 ...]

对任意聊天截图打印关键特征，用于给 mask.py 设定/校准阈值：
  - 背景亮度 bgL（左/右/中三处采样）+ 主题猜测（深色/亮色）
  - 头像行段候选（y 范围 + 宽度）
  - 每行文字的【亮度 / 饱和 / 高度】—— 用来找“名字 vs 正文”的分界阈值
注意：这里的“内容判定”用“与背景亮度差 > 12”兼顾深浅两种主题，
      数值与 mask.py 的规则略有出入属正常，作用是帮助选阈值。
"""
import sys
import os
from PIL import Image, ImageOps, ImageChops


def lut(f):
    return [255 if f(i) else 0 for i in range(256)]


def med_of(img, box):
    """区域中位数（img 为 L 模式）"""
    h = img.crop(box).histogram()
    tot = sum(h)
    acc = 0
    for v, c in enumerate(h):
        acc += c
        if acc >= tot * 0.5:
            return v
    return 0


def rowprof(m, box):
    x0, y0, x1, y1 = box
    return list(m.crop(box).resize((1, y1 - y0), Image.BOX).getdata())


def colprof(m, box):
    x0, y0, x1, y1 = box
    return list(m.crop(box).resize((x1 - x0, 1), Image.BOX).getdata())


def runs_of(vals, thr):
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


def mean_bs(mx, sat, cm, box):
    mreg = cm.crop(box)
    ma = mreg.resize((1, 1), Image.BOX).getdata()[0]
    if ma == 0:
        return None
    b = ImageChops.multiply(mx.crop(box), mreg).resize((1, 1), Image.BOX).getdata()[0]
    s = ImageChops.multiply(sat.crop(box), mreg).resize((1, 1), Image.BOX).getdata()[0]
    return b * 255.0 / ma, s * 255.0 / ma


for path in sys.argv[1:]:
    im = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    W, H = im.size
    R, G, B = im.split()
    mx = ImageChops.lighter(ImageChops.lighter(R, G), B)
    mn = ImageChops.darker(ImageChops.darker(R, G), B)
    sat = ImageChops.difference(mx, mn)

    s1 = med_of(mx, (0, int(0.20 * H), max(4, int(0.015 * W)), int(0.80 * H)))
    s2 = med_of(mx, (int(0.992 * W), int(0.20 * H), W, int(0.80 * H)))
    s3 = med_of(mx, (int(0.30 * W), int(0.55 * H), int(0.70 * W), int(0.60 * H)))
    bgL = s1
    theme = "亮色(light)" if bgL > 128 else "深色(dark)"
    print(f"\n=== {os.path.basename(path)}  {W}x{H}")
    print(f"  背景采样: 左={s1} 右={s2} 中={s3} → bgL={bgL}  主题猜测: {theme}")

    content = ImageChops.lighter(
        mx.point(lut(lambda v: abs(v - bgL) > 12)),
        sat.point(lut(lambda v: v > 15)))

    # ---- 头像候选 ----
    sx0, sx1 = int(0.030 * W), int(0.175 * W)
    y0s, y1s = int(0.055 * H), int(0.985 * H)
    prof = rowprof(content, (sx0, y0s, sx1, y1s))
    print("  头像行段候选:")
    for (a, b) in runs_of(prof, 76):
        h = b - a + 1
        if not (int(0.020 * H) <= h <= int(0.13 * H)):
            continue
        yy0, yy1 = a + y0s, b + y0s
        cols = colprof(content, (sx0, yy0, sx1, yy1 + 1))
        idx = [i for i, v in enumerate(cols) if v > 127]
        if not idx:
            continue
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
        print(f"    y=({yy0},{yy1}) h={h}  x=({sx0+b0},{sx0+b1+1}) w={b1-b0+1}")

    # ---- 文字行统计 ----
    tx0, tx1, ty0 = int(0.155 * W), int(0.80 * W), int(0.110 * H)
    tp = rowprof(content, (tx0, ty0, tx1, H))
    print("  文字行统计 (深色主题参考: 名字≈120 / 正文≈178):")
    for (a, b) in runs_of(tp, 3):
        h = b - a + 1
        if h < int(0.008 * H):
            continue
        yy0, yy1 = ty0 + a, ty0 + b
        box = (tx0, yy0, tx1, yy1 + 1)
        r = mean_bs(mx, sat, content, box)
        if not r:
            continue
        mb, ms = r
        cols = colprof(content, box)
        idx = [i for i, v in enumerate(cols) if v > 13]
        cx0, cx1 = tx0 + idx[0], tx0 + idx[-1] + 1
        print(f"    y=({yy0},{yy1}) h={h} x=({cx0},{cx1}) 亮度={mb:.0f} 饱和={ms:.0f}")
