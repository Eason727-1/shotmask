// ============================================================
//  shotmask.cpp —— 聊天截图自动打码（C++ 版）
// ------------------------------------------------------------
//  功能：把聊天截图里的【头像】和【名字】打上马赛克，保留聊天正文。
//        全自动：不用框选、不用 OCR，靠“位置 + 颜色”判断。
//        逻辑与 Python 版（mask.py）完全一致，参数经过实测校准。
//
//  依赖：两个单文件库（放在与本文件相同的目录里）：
//      stb_image.h        —— 读 PNG/JPG   （github.com/nothings/stb）
//      stb_image_write.h  —— 写 PNG
//
//  编译（Linux / Mac / Windows 的 MinGW 都行）：
//      g++ shotmask.cpp -o shotmask -O2
//
//  运行：
//      ./shotmask 截图1.png [截图2.png ...]
//      输出：<原名>_masked.png
//
//  原理速览：
//      1) 测背景亮度 bgL（截图左边缘竖条的中位数，深色主题约 25）
//      2) 头像 = 左侧固定竖带里的大块“非背景”区域（宽松判定）
//      3) 名字 = 内容区里的“灰色小字行”（正文是白色，亮度更高）
//      4) 标题栏 = 顶部白色文字行（里面也常含名字）
//      5) 对以上区域涂马赛克
//
//  注意：当前参数按【深色主题】校准；亮色主题需要把
//        “比背景亮”的判定反过来（见 testPixel 处注释）。
// ============================================================

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using std::pair;
using std::string;
using std::vector;

// ------------------------------------------------
//  一张图片：像素按 RGB 顺序连续存放
//  px[3 * (y * W + x) + c]，c = 0/1/2 对应 R/G/B
//  例：取 (x, y) 的红色分量 = px[3 * (y*W + x) + 0]
// ------------------------------------------------
struct Image {
    int W = 0, H = 0;
    vector<unsigned char> px;

    // 取 (x,y) 像素的指针（内部使用，调用处保证不越界）
    unsigned char* at(int x, int y) { return &px[3 * (y * W + x)]; }
    const unsigned char* at(int x, int y) const { return &px[3 * (y * W + x)]; }
};

// ---- 小工具：某像素的最大通道（≈亮度）和饱和度（最大-最小通道） ----
// 为什么这样算？截图背景是深灰（R≈G≈B），文字是中性白/灰：看“最大通道”就够。
// 而彩色装饰（头像、表情）有三个通道差很大的特点 → 用“饱和度”区分。
inline int chMax(const unsigned char* p) { return std::max(p[0], std::max(p[1], p[2])); }
inline int chMin(const unsigned char* p) { return std::min(p[0], std::min(p[1], p[2])); }

// ------------------------------------------------
//  三种“这个像素算不算内容”的判定（深色主题）
//    P_STRICT : 严格，用于文字行分析
//    P_LOOSE  : 宽松，用于头像检测（暗色照片头像也能框住）
//    P_WHITE  : 白色文字，用于标题栏
//
//  ★ 换成亮色主题时：把 mx > bgL + X 改成 mx < bgL - X
//    （亮色背景是大白话：内容比背景【暗】，正好反过来）
// ------------------------------------------------
enum Pred { P_STRICT, P_LOOSE, P_WHITE };

bool testPixel(const Image& im, int x, int y, int bgL, Pred mode) {
    const unsigned char* p = im.at(x, y);
    int mx = chMax(p), mn = chMin(p), sat = mx - mn;
    if (mode == P_STRICT) return mx > bgL + 32 || sat > 30;
    if (mode == P_LOOSE)  return mx > bgL + 12 || sat > 15;
    /* P_WHITE */         return mx > 180 && sat < 60;
}

// ------------------------------------------------
//  行/列投影：把一块区域按行（或按列）“压扁”，
//  得到每行（列）里内容像素的占比，换算成 0~255 的整数。
//  例：某行返回值 255 表示整行都是内容，0 表示整行都是背景。
//  这是整个工具最核心的技巧 —— 把“看图”变成了“看一维数组”。
// ------------------------------------------------
vector<int> rowProfile(const Image& im, int x0, int y0, int x1, int y1,
                       int bgL, Pred mode) {
    vector<int> prof(y1 - y0, 0);
    for (int y = y0; y < y1; y++) {
        int cnt = 0;
        for (int x = x0; x < x1; x++)
            if (testPixel(im, x, y, bgL, mode)) cnt++;
        prof[y - y0] = (int)std::lround(255.0 * cnt / (x1 - x0));
    }
    return prof;
}

vector<int> colProfile(const Image& im, int x0, int y0, int x1, int y1,
                       int bgL, Pred mode) {
    vector<int> prof(x1 - x0, 0);
    for (int x = x0; x < x1; x++) {
        int cnt = 0;
        for (int y = y0; y < y1; y++)
            if (testPixel(im, x, y, bgL, mode)) cnt++;
        prof[x - x0] = (int)std::lround(255.0 * cnt / (y1 - y0));
    }
    return prof;
}

// ------------------------------------------------
//  在“一维投影数组”里找出所有“连续大于阈值”的区间
//  例：数组里第 100~160 个值都 > 76 → 得到一个区间 {100, 160}
//  返回：{[起点, 终点], ...}（下标都是数组内下标）
// ------------------------------------------------
vector<pair<int, int>> runsOf(const vector<int>& v, int thr) {
    vector<pair<int, int>> out;
    int n = (int)v.size();
    for (int i = 0; i < n; i++) {
        if (v[i] > thr) {
            int j = i;
            while (j + 1 < n && v[j + 1] > thr) j++;
            out.push_back({i, j});
            i = j;              // 跳过这一整段，从区间末尾继续
        }
    }
    return out;
}

// ------------------------------------------------
//  统计一块区域里“内容像素”的平均亮度 / 平均饱和度
//  用途：区分“灰色小字”（名字，亮度低）和“白色正文”（亮度高）。
//  返回 false 表示这块区域里没有任何内容像素。
// ------------------------------------------------
bool meanBrightSat(const Image& im, int x0, int y0, int x1, int y1, int bgL,
                   double& outB, double& outS) {
    long long n = 0;
    double sb = 0, ss = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const unsigned char* p = im.at(x, y);
            if (testPixel(im, x, y, bgL, P_STRICT)) {
                n++;
                sb += chMax(p);
                ss += chMax(p) - chMin(p);
            }
        }
    if (n == 0) return false;
    outB = sb / n;      // 平均亮度
    outS = ss / n;      // 平均饱和度
    return true;
}

// ------------------------------------------------
//  一块区域的中位数亮度 —— 用来估计“背景亮度 bgL”
//  取中位数而不是平均值：个别亮像素（文字等）不会干扰结果
// ------------------------------------------------
int medianBrightness(const Image& im, int x0, int y0, int x1, int y1) {
    int hist[256] = {0};        // 直方图：hist[v] = 亮度 v 出现了几次
    long long tot = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            hist[chMax(im.at(x, y))]++;
            tot++;
        }
    long long acc = 0;
    for (int v = 0; v < 256; v++) {
        acc += hist[v];
        if (acc * 2 >= tot) return v;   // 累计到一半 → 中位数
    }
    return 0;
}

// ------------------------------------------------
//  打马赛克：把矩形区域竖着切成 nv 块，
//  每块取“平均颜色”再整块填平 → 细节全消失
//  （nv 越小块越大、效果越狠：头像用 4，文字用 3）
// ------------------------------------------------
void mosaic(Image& im, int x0, int y0, int x1, int y1, int nv) {
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(im.W, x1); y1 = std::min(im.H, y1);
    if (x1 - x0 < 4 || y1 - y0 < 4) return;
    int block = std::max(6, (int)std::lround((double)(y1 - y0) / nv));
    for (int by = y0; by < y1; by += block) {
        int ey = std::min(by + block, y1);
        for (int bx = x0; bx < x1; bx += block) {
            int ex = std::min(bx + block, x1);
            // 1) 算这一小块的平均颜色
            long long sr = 0, sg = 0, sb = 0;
            int n = 0;
            for (int y = by; y < ey; y++)
                for (int x = bx; x < ex; x++) {
                    const unsigned char* p = im.at(x, y);
                    sr += p[0]; sg += p[1]; sb += p[2];
                    n++;
                }
            unsigned char r = (unsigned char)(sr / n);
            unsigned char g = (unsigned char)(sg / n);
            unsigned char b = (unsigned char)(sb / n);
            // 2) 用平均色把这一小块填平
            for (int y = by; y < ey; y++)
                for (int x = bx; x < ex; x++) {
                    unsigned char* p = im.at(x, y);
                    p[0] = r; p[1] = g; p[2] = b;
                }
        }
    }
}

// 矩形范围：x1 / y1 是“右下开区间”（和 STL 的习惯一致）
struct Box { int x0, y0, x1, y1; };

// ------------------------------------------------
//  找头像：
//  1) 在左侧竖带 [0.03W, 0.175W] 上做行投影（宽松判定）
//  2) 高度在 [0.02H, 0.13H] 之间的行段 = 疑似头像的行范围
//  3) 对行段再做列投影，取“最宽的连续列块” = 头像的宽度
//     （这一步能排除掉头像右边同一行的名字文字）
//  4) 宽度需在 [0.045W, 0.22W] 之间才算数（标准约 108px @1206）
// ------------------------------------------------
vector<Box> findAvatars(const Image& im, int bgL) {
    int W = im.W, H = im.H;
    int sx0 = (int)(0.030 * W), sx1 = (int)(0.175 * W);   // 头像竖带
    int y0s = (int)(0.055 * H), y1s = (int)(0.985 * H);   // 跳过顶部状态栏
    vector<int> prof = rowProfile(im, sx0, y0s, sx1, y1s, bgL, P_LOOSE);

    vector<Box> avatars;
    for (auto& r : runsOf(prof, 76)) {          // 行内容占比 > 0.30
        int a = r.first, b = r.second;
        int h = b - a + 1;
        if (h < (int)(0.020 * H) || h > (int)(0.13 * H)) continue;  // 高度不像头像
        // 向上下扩展“弱边缘”，避免截掉照片的暗边
        while (a > 0 && prof[a - 1] > 30) a--;
        while (b + 1 < (int)prof.size() && prof[b + 1] > 30) b++;
        int yy0 = a + y0s, yy1 = b + y0s;

        // 列投影 → 找最宽连续列块
        vector<int> cols = colProfile(im, sx0, yy0, sx1, yy1 + 1, bgL, P_LOOSE);
        vector<int> idx;
        for (int i = 0; i < (int)cols.size(); i++)
            if (cols[i] > 127) idx.push_back(i);       // 列内容占比 > 0.5
        if (idx.empty()) continue;

        vector<pair<int, int>> blocks;                 // 间隔 ≤3 的列并成一块
        int cur0 = idx[0], prev = idx[0];
        for (size_t k = 1; k < idx.size(); k++) {
            if (idx[k] - prev <= 3) prev = idx[k];
            else { blocks.push_back({cur0, prev}); cur0 = prev = idx[k]; }
        }
        blocks.push_back({cur0, prev});

        pair<int, int> best = blocks[0];               // 取最宽的一块
        for (auto& bl : blocks)
            if (bl.second - bl.first > best.second - best.first) best = bl;

        int cx0 = sx0 + best.first, cx1 = sx0 + best.second + 1;
        int wd = cx1 - cx0;
        if (wd < (int)(0.045 * W) || wd > (int)(0.22 * W)) continue;  // 宽度不像头像
        avatars.push_back({cx0, yy0, cx1, yy1});
        printf("  头像 y=(%d,%d) h=%d  x=(%d,%d) w=%d\n",
               yy0, yy1, yy1 - yy0 + 1, cx0, cx1, wd);
    }
    return avatars;
}

// ------------------------------------------------
//  找“名字 / 引用行”：
//  在内容区（x 从“头像右侧”开始）做行投影（严格判定），
//  每段文字行算一下平均亮度：
//    ★ 关键判据：亮度 < 160 且饱和度 < 45 → 灰色小字 = 名字/引用，打码
//      实测参考：名字亮度 ≈ 120（灰），正文 ≈ 178（白）→ 不会误伤正文
// ------------------------------------------------
vector<Box> findGrayLines(const Image& im, int bgL, int tx0) {
    int W = im.W, H = im.H;
    int tx1 = (int)(0.80 * W);          // 右边 20% 是时间戳，不参与
    int ty0 = (int)(0.110 * H);         // 跳过顶部导航区
    vector<int> prof = rowProfile(im, tx0, ty0, tx1, H, bgL, P_STRICT);

    vector<Box> out;
    for (auto& r : runsOf(prof, 3)) {   // 行里只要有 1% 文字就算一行
        int a = r.first, b = r.second;
        int h = b - a + 1;
        if (h < (int)(0.008 * H)) continue;      // 太薄 → 可能只是分隔线
        int yy0 = ty0 + a, yy1 = ty0 + b;

        double mb = 0, ms = 0;
        if (!meanBrightSat(im, tx0, yy0, tx1, yy1 + 1, bgL, mb, ms)) continue;

        // 这一行文字的水平范围（列投影）
        vector<int> cols = colProfile(im, tx0, yy0, tx1, yy1 + 1, bgL, P_STRICT);
        vector<int> idx;
        for (int i = 0; i < (int)cols.size(); i++)
            if (cols[i] > 13) idx.push_back(i);
        if (idx.empty()) continue;
        int cx0 = tx0 + idx.front(), cx1 = tx0 + idx.back() + 1;

        bool isGray = (mb < 160 && ms < 45);
        printf("  文字行 y=(%d,%d) h=%d 亮度=%.0f 饱和=%.0f -> %s\n",
               yy0, yy1, h, mb, ms, isGray ? "打码" : "跳过");
        if (isGray)
            out.push_back({cx0 - 8, yy0 - 6, cx1 + 8, yy1 + 6});   // 稍微外扩一点更保险
    }
    return out;
}

// ------------------------------------------------
//  找标题栏：顶部导航区里的白色文字行（标题常含名字）
//  白色判定：亮度 > 180 且饱和度 < 60
// ------------------------------------------------
bool findTitle(const Image& im, int bgL, Box& out) {
    int W = im.W, H = im.H;
    int hx0 = (int)(0.15 * W), hx1 = (int)(0.90 * W);
    int hy0 = (int)(0.060 * H), hy1 = (int)(0.102 * H);
    vector<int> rp = rowProfile(im, hx0, hy0, hx1, hy1, bgL, P_WHITE);
    vector<int> cp = colProfile(im, hx0, hy0, hx1, hy1, bgL, P_WHITE);

    int ry0 = -1, ry1 = -1, cx0 = -1, cx1 = -1;
    for (int i = 0; i < (int)rp.size(); i++)
        if (rp[i] > 3) { if (ry0 < 0) ry0 = i; ry1 = i; }
    for (int i = 0; i < (int)cp.size(); i++)
        if (cp[i] > 2) { if (cx0 < 0) cx0 = i; cx1 = i; }
    if (ry0 < 0 || cx0 < 0) return false;

    out = {hx0 + cx0 - 8, hy0 + ry0 - 6, hx0 + cx1 + 9, hy0 + ry1 + 7};
    return true;
}

// ------------------------------------------------
//  处理一张图：读入 → 检测 → 打码 → 保存
// ------------------------------------------------
bool process(const char* path) {
    // 1) 读图（强制读成 3 通道 RGB，忽略透明通道）
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path, &w, &h, &n, 3);
    if (!data) { printf("读取失败: %s\n", path); return false; }

    Image im;
    im.W = w; im.H = h;
    im.px.assign(data, data + (size_t)w * h * 3);   // 复制到 vector 里
    stbi_image_free(data);

    int W = im.W, H = im.H;

    // 2) 背景亮度：左边缘竖条（x < 0.015W）的中位数
    int bgL = medianBrightness(im, 0, (int)(0.20 * H),
                               std::max(4, (int)(0.015 * W)), (int)(0.80 * H));
    printf("\n=== %s  %dx%d  bgL=%d\n", path, W, H, bgL);

    // 3) 找头像
    vector<Box> avatars = findAvatars(im, bgL);

    // 4) 找名字：文字区从“头像的右边缘”开始，避免和头像重叠
    int avX1 = 0;
    for (auto& b : avatars) avX1 = std::max(avX1, b.x1);
    int tx0 = (int)(0.155 * W);
    if (avX1 > 0)
        tx0 = std::min(std::max(tx0, avX1 + 2), (int)(0.175 * W));
    vector<Box> names = findGrayLines(im, bgL, tx0);

    // 5) 找标题
    Box title;
    bool hasTitle = findTitle(im, bgL, title);

    // 6) 统一打马赛克（头像切 4 块 / 文字切 3 块，块越少越狠）
    for (auto& b : avatars)
        mosaic(im, b.x0 - 3, b.y0 - 3, b.x1 + 3, b.y1 + 3, 4);
    for (auto& b : names)
        mosaic(im, b.x0, b.y0, b.x1, b.y1, 3);
    if (hasTitle) {
        mosaic(im, title.x0, title.y0, title.x1, title.y1, 3);
        printf("  标题打码 (%d,%d)-(%d,%d)\n", title.x0, title.y0, title.x1, title.y1);
    }

    // 7) 保存为 <原名>_masked.png
    string out = path;
    size_t dot = out.rfind('.');
    if (dot != string::npos) out = out.substr(0, dot);
    out += "_masked.png";
    stbi_write_png(out.c_str(), W, H, 3, im.px.data(), W * 3);
    printf("  => 已输出 %s\n", out.c_str());
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: %s 图片1 [图片2 ...]\n", argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; i++) process(argv[i]);
    return 0;
}
