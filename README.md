# shotmask — 聊天截图自动打码工具

把聊天截图里的 **头像** 和 **名字（昵称）** 自动打上马赛克，**保留聊天正文**。

全自动：无需 OCR、无需手动框选，靠“位置 + 颜色”启发式判断。

## 效果

| 自动打码 | 原样保留 |
|----------|----------|
| ✅ 头像 | ✅ 聊天正文 |
| ✅ 名字标签 | ✅ 时间戳 |
| ✅ 标题栏（常含名字） | ✅ 日期 |
| ✅ 引用行“某某: …”里的名字 | |

> 参数按**深色主题**截图实测校准；亮色主题用 `python/analyze.py` 校准后同样可用。

## 目录结构

- **`python/`** —— 主力版本，基于 [Pillow](https://python-pillow.org)
  - `mask.py`：自动打码
  - `analyze.py`：校准诊断（打印背景亮度、每行文字亮度等关键数值）
- **`cpp/`** —— 纯 C++ 实现（逻辑与 Python 版一致），附带
  [stb](https://github.com/nothings/stb) 单文件图像库

## 使用

### Python 版

```sh
pip install pillow
python python/mask.py 截图.png             # 输出 out/截图_masked.png
python python/mask.py 图1.png 图2.png       # 支持多张
```

### C++ 版

```sh
cd cpp
g++ shotmask.cpp -o shotmask -O2
./shotmask 截图.png                        # 输出 截图_masked.png
```

## 原理

不做 OCR，核心是三步启发式 + 一个投影技巧：

1. 测背景亮度 `bgL`（取截图左边缘竖条的中位数）
2. **头像** = 左侧固定竖带里的大块“非背景”区域
   （行投影找上/下边界 → 列投影找左/右边界）
3. **名字** = 内容区里的“灰色小字行”
   （实测：名字亮度 ≈120，正文 ≈178，用平均亮度/饱和度区分，黑白分明）
4. **标题栏** = 顶部导航区里的白色文字行
5. 对以上区域涂马赛克

其中最核心的技巧是 **行/列投影**：把二维图像压成一维数组后，
“找头像”“找文字行”就变成了简单的“找连续区间”。

## 校准（换主题 / 换 App）

```sh
python python/analyze.py 新截图.png
```

根据输出调整 `mask.py` 中的阈值（亮色主题需把亮度判定取反），
再跑一遍确认即可。详见脚本内注释。

## 说明

- 参数在 1206×2622 的深色主题聊天截图（QQ）上实测校准
- 附带的 `stb_image.h` / `stb_image_write.h` 来自
  [nothings/stb](https://github.com/nothings/stb)（public domain / MIT，
  文件头内有完整许可声明）
