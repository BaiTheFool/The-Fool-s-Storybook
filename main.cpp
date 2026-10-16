/* =====================================================================
 *  愚 见 者 的 故 事 书   ｜   通用文字冒险播放器（Win32 + GDI+，单文件实现）
 *
 *  它不绑死任何题材：科幻、怪谈、日常、历史……都只是 stories 文件夹里的
 *  一个文本档案。剧情、分支、图片全部和代码分开，随时改、随时加，
 *  不用重新编译，也不用动这里一行代码。
 *
 *  目录约定（都相对于 exe 所在目录）：
 *      stories/              故事档案，一个档案 = 一个 .txt
 *      images/<档案名>/       该档案的图片，文件名用数字，如 1.png、2.png
 *
 *  剧本语法（详见 stories/_模板.txt，井号开头是注释）：
 *      @标题 雨夜末班车            档案名，显示在选档案界面
 *      @开始 站台                  从哪个节点开始（不写就用第一个节点）
 *      :: 节点名                   一个新节点 / 分支
 *      @场景 第一幕 ｜ 末班车       在剧情里插一行小标题
 *      @图片 1 小窗                显示 1.png，小窗播放
 *      @图片 1 全屏                整窗铺满，文字浮在图片上（带半透明底框）
 *      @图片 0                    关闭图片（图片会保留到下一次改动，可以当背景）
 *      > 选项文字 -> 目标节点        一个选项按钮
 *      -> 目标节点                 没有选项，点「继续」直接跳过去
 *      @结局 1 末班车 | 结语        结局
 *
 *  编译：
 *      g++ -std=c++17 -mwindows -O2 -o 愚见者的故事书.exe main.cpp -lgdi32 -luser32 -lgdiplus -lole32 -lshell32 -lmsimg32
 * ===================================================================== */

#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>  // IStream：把图片读进内存，避免锁住文件
#include <shellapi.h>

#include <gdiplus.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using std::map;
using std::vector;
using std::wstring;

/* ==================== 主题配色 ==================== */
namespace Theme {
const COLORREF BG_TOP = RGB(9, 12, 19);
const COLORREF BG_BOTTOM = RGB(19, 26, 38);
const COLORREF PANEL = RGB(21, 28, 40);
const COLORREF PANEL_EDGE = RGB(43, 57, 78);
const COLORREF CARD = RGB(24, 32, 45);
const COLORREF CARD_EDGE = RGB(56, 72, 96);
const COLORREF TEXT = RGB(230, 236, 246);
const COLORREF TEXT_DIM = RGB(156, 170, 192);
const COLORREF ACCENT = RGB(114, 226, 209);
const COLORREF AMBER = RGB(246, 197, 106);
const COLORREF QUOTE = RGB(212, 179, 120);
const COLORREF WARN = RGB(232, 150, 118);
const COLORREF BTN = RGB(26, 34, 48);
const COLORREF BTN_HOT = RGB(37, 50, 70);
const COLORREF BTN_DOWN = RGB(45, 62, 86);
const COLORREF BTN_EDGE = RGB(57, 73, 97);
const COLORREF BADGE = RGB(39, 52, 71);
const COLORREF RULE = RGB(56, 72, 95);
const COLORREF HINT = RGB(107, 122, 145);
const COLORREF SCRIM = RGB(5, 8, 13);
}  // namespace Theme

/* ==================== 基础工具 ==================== */

static int g_dpi = 96;
static inline int S(int v) { return MulDiv(v, g_dpi, 96); }

static HDC g_measureDC = nullptr;

static HFONT makeFont(int pt, int weight) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

static int measure(const wstring& s, HFONT f) {
    HGDIOBJ old = SelectObject(g_measureDC, f);
    SIZE sz{0, 0};
    GetTextExtentPoint32W(g_measureDC, s.c_str(), (int)s.size(), &sz);
    SelectObject(g_measureDC, old);
    return sz.cx;
}

static wstring trim(const wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (iswspace((wint_t)s[b]) != 0 || s[b] == 0xFEFF)) ++b;
    while (e > b && iswspace((wint_t)s[e - 1]) != 0) --e;
    return s.substr(b, e - b);
}

static bool startsWith(const wstring& s, const wstring& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static vector<wstring> splitLines(const wstring& text) {
    vector<wstring> out;
    wstring cur;
    for (wchar_t c : text) {
        if (c == L'\n') {
            if (!cur.empty() && cur.back() == L'\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        if (cur.back() == L'\r') cur.pop_back();
        out.push_back(cur);
    }
    return out;
}

static bool parseInt(const wstring& s, int& out) {
    const wstring t = trim(s);
    if (t.empty()) return false;
    for (wchar_t c : t)
        if (!iswdigit((wint_t)c)) return false;
    out = _wtoi(t.c_str());
    return true;
}

static wstring lower(wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower((wint_t)c);
    return s;
}

static bool fileExists(const wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool dirExists(const wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 递归创建目录
static void ensureDir(const wstring& path) {
    wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        const bool last = (i + 1 == path.size());
        if (path[i] == L'\\' || path[i] == L'/' || last) {
            if (cur.size() > 3 && !dirExists(cur)) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
}

static wstring exeDir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const wstring p(buf, n);
    const size_t cut = p.find_last_of(L"\\/");
    return (cut == wstring::npos) ? p : p.substr(0, cut);
}

static wstring joinPath(const wstring& a, const wstring& b) { return a + L"\\" + b; }

static wstring baseName(const wstring& path) {  // 去掉目录和扩展名
    const size_t cut = path.find_last_of(L"\\/");
    const wstring name = (cut == wstring::npos) ? path : path.substr(cut + 1);
    const size_t dot = name.find_last_of(L'.');
    return (dot == wstring::npos) ? name : name.substr(0, dot);
}

static wstring extName(const wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    const wstring name = (cut == wstring::npos) ? path : path.substr(cut + 1);
    const size_t dot = name.find_last_of(L'.');
    return (dot == wstring::npos) ? wstring() : lower(name.substr(dot));
}

/* ---------- 文本文件读写（UTF-8 / UTF-16 / 本地编码都能读） ---------- */

static bool readTextFile(const wstring& path, wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::string raw((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    const BOOL ok = raw.empty() ? TRUE : ReadFile(h, &raw[0], (DWORD)raw.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    raw.resize(read);

    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        out.assign((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
        return true;
    }

    size_t skip = 0;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF)
        skip = 3;

    const char* data = raw.data() + skip;
    const int bytes = (int)(raw.size() - skip);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, flags, data, bytes, nullptr, 0);
    if (n <= 0) {  // 不是合法 UTF-8，就按本地编码（简中系统为 GBK）再试
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, data, bytes, nullptr, 0);
    }
    if (n <= 0) return false;
    out.resize(n);
    MultiByteToWideChar(cp, flags, data, bytes, &out[0], n);
    return true;
}

static bool writeTextFile(const wstring& path, const wstring& text, bool withBom) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                      nullptr, nullptr);
    std::string bytes;
    if (withBom) bytes += "\xEF\xBB\xBF";
    if (n > 0) {
        std::string body((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &body[0], n, nullptr,
                            nullptr);
        bytes += body;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok =
        bytes.empty() ? TRUE : WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

/* ==================== 图片 ==================== */

static ULONG_PTR g_gdiplusToken = 0;
static map<wstring, Gdiplus::Image*> g_imgCache;  // 路径 -> 图片（nullptr = 加载失败）

static void clearImageCache() {
    for (auto& kv : g_imgCache)
        if (kv.second) delete kv.second;
    g_imgCache.clear();
}

static int getEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *clsid = info[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

// 先把图片读进内存再交给 GDI+，这样文件不会被程序锁住，随时能替换
static Gdiplus::Image* loadImage(const wstring& path) {
    auto it = g_imgCache.find(path);
    if (it != g_imgCache.end()) return it->second;

    Gdiplus::Image* img = nullptr;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024 * 1024) {
            vector<BYTE> data((size_t)size.QuadPart);
            DWORD read = 0;
            if (ReadFile(h, data.data(), (DWORD)data.size(), &read, nullptr) &&
                read == data.size()) {
                HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, data.size());
                if (mem) {
                    void* p = GlobalLock(mem);
                    if (p) {
                        memcpy(p, data.data(), data.size());
                        GlobalUnlock(mem);
                        IStream* stream = nullptr;
                        if (CreateStreamOnHGlobal(mem, TRUE, &stream) == S_OK) {
                            img = Gdiplus::Image::FromStream(stream);
                            if (img && img->GetLastStatus() != Gdiplus::Ok) {
                                delete img;
                                img = nullptr;
                            }
                            stream->Release();  // 图片自己持有流引用
                        } else {
                            GlobalFree(mem);
                        }
                    } else {
                        GlobalFree(mem);
                    }
                }
            }
        }
        CloseHandle(h);
    }
    g_imgCache[path] = img;
    return img;
}

// 一个档案的图片文件夹扫描结果：编号 -> 文件路径
struct FolderScan {
    map<int, wstring> byNo;
    int pngCount = 0;
    int otherCount = 0;
    bool scanned = false;
};
static map<wstring, FolderScan> g_folderScans;

static int extRank(const wstring& ext) {
    if (ext == L".png") return 0;
    if (ext == L".jpg" || ext == L".jpeg") return 1;
    if (ext == L".bmp") return 2;
    if (ext == L".gif") return 3;
    if (ext == L".tif" || ext == L".tiff") return 4;
    return 5;
}

// 只关心图片文件；文件夹里放个 说明.txt 之类不会被当成"格式不对的图片"
static bool isImageExt(const wstring& ext) {
    static const wchar_t* known[] = {L".png",  L".jpg",  L".jpeg", L".bmp",
                                     L".gif",  L".tif",  L".tiff", L".webp",
                                     L".heic", L".avif", L".svg"};
    for (const wchar_t* k : known)
        if (ext == k) return true;
    return false;
}

static const FolderScan& scanFolder(const wstring& folder) {
    auto it = g_folderScans.find(folder);
    if (it != g_folderScans.end() && it->second.scanned) return it->second;

    FolderScan scan;
    scan.scanned = true;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const wstring name = fd.cFileName;
            const wstring ext = extName(name);
            if (!isImageExt(ext)) continue;
            if (ext == L".png")
                ++scan.pngCount;
            else
                ++scan.otherCount;

            if (extRank(ext) >= 5) continue;  // 认不出格式的只统计，不参与编号
            int no = 0;
            if (!parseInt(baseName(name), no)) continue;  // 只认数字命名的文件
            const wstring full = folder + L"\\" + name;
            auto cur = scan.byNo.find(no);
            if (cur == scan.byNo.end() || extRank(ext) < extRank(extName(cur->second)))
                scan.byNo[no] = full;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_folderScans[folder] = scan;
    return g_folderScans[folder];
}

static wstring findImagePath(const wstring& folder, int no) {
    const FolderScan& scan = scanFolder(folder);
    auto it = scan.byNo.find(no);
    return (it == scan.byNo.end()) ? wstring() : it->second;
}

static void clearFolderCache() { g_folderScans.clear(); }

/* 把文件夹里不是 png 的图片自动转成 png（原图移到 _原图\，不删）。
   在每次扫描图片文件夹之前调用，所以玩家不需要做任何操作。返回转换张数。 */
static int autoConvertFolder(const wstring& folder) {
    if (!dirExists(folder)) return 0;

    vector<wstring> pending;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wstring name = fd.cFileName;
        const wstring ext = extName(name);
        if (ext.empty() || ext == L".png") continue;
        int no = 0;
        if (!parseInt(baseName(name), no)) continue;  // 非数字命名的不动它
        if (extRank(ext) >= 5) continue;              // 不认识的格式也不动它
        pending.push_back(folder + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (pending.empty()) return 0;

    CLSID pngClsid;
    if (getEncoderClsid(L"image/png", &pngClsid) < 0) return 0;

    int converted = 0;
    for (const wstring& src : pending) {
        const wstring stem = baseName(src);
        const wstring target = folder + L"\\" + stem + L".png";
        if (fileExists(target)) continue;  // 已经有同名 png 就不动

        g_imgCache.erase(src);  // 可能之前读过并缓存了失败结果
        Gdiplus::Image* img = loadImage(src);
        if (!img) continue;
        if (img->Save(target.c_str(), &pngClsid, nullptr) != Gdiplus::Ok) continue;

        const wstring backup = folder + L"\\_原图";
        ensureDir(backup);
        MoveFileW(src.c_str(), (backup + L"\\" + stem + extName(src)).c_str());
        ++converted;
    }
    if (converted > 0) g_folderScans.erase(folder);  // 让计数重新来一遍
    return converted;
}

// 等比缩放画进 box；cover = true 时铺满并裁掉多余部分（全屏用）
static void drawImageIn(HDC dc, const RECT& box, Gdiplus::Image* img, bool cover) {
    if (!img) return;
    const int bw = box.right - box.left, bh = box.bottom - box.top;
    if (bw <= 0 || bh <= 0) return;
    const double iw = (double)img->GetWidth(), ih = (double)img->GetHeight();
    if (iw <= 0 || ih <= 0) return;

    const double scale = cover ? (std::max)(bw / iw, bh / ih) : (std::min)(bw / iw, bh / ih);
    const double dw = iw * scale, dh = ih * scale;
    const double dx = box.left + (bw - dw) / 2.0, dy = box.top + (bh - dh) / 2.0;

    Gdiplus::Graphics g(dc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    if (cover) {
        Gdiplus::RectF clip((Gdiplus::REAL)box.left, (Gdiplus::REAL)box.top, (Gdiplus::REAL)bw,
                            (Gdiplus::REAL)bh);
        g.SetClip(clip);
    }
    g.DrawImage(img, Gdiplus::RectF((Gdiplus::REAL)dx, (Gdiplus::REAL)dy, (Gdiplus::REAL)dw,
                                    (Gdiplus::REAL)dh));
}

// GDI 本身没有 alpha，这里用 1x1 的 32 位 DIB + AlphaBlend 做半透明填充
static void blendFillRounded(HDC dc, const RECT& r, int radius, COLORREF color, BYTE alpha) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    HDC src = CreateCompatibleDC(dc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 1;
    bi.bmiHeader.biHeight = 1;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(src);
        return;
    }
    *(DWORD*)bits = (DWORD)GetBValue(color) | ((DWORD)GetGValue(color) << 8) |
                    ((DWORD)GetRValue(color) << 16) | 0xFF000000u;

    HGDIOBJ oldBmp = SelectObject(src, bmp);
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
    const int saved = SaveDC(dc);
    ExtSelectClipRgn(dc, rgn, RGN_AND);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, 0};
    AlphaBlend(dc, r.left, r.top, w, h, src, 0, 0, 1, 1, bf);
    RestoreDC(dc, saved);

    DeleteObject(rgn);
    SelectObject(src, oldBmp);
    DeleteObject(bmp);
    DeleteDC(src);
}

/* ==================== 剧本数据模型 ==================== */

/* ---------- 数值 / 条件 / 效果 ---------- */

struct VarDef {  // @数值 精神值 = 80
    wstring name;
    int initial = 0;
    wstring dice;  // 初值也可以掷出来：@数值 力量 = 3d6
};

struct Condition {  // 变量 与 某个数 的比较
    wstring var;
    int op = 0;  // 0 == / 1 != / 2 > / 3 >= / 4 < / 5 <=
    int value = 0;
    wstring rhsVar;  // 右边也可以写成另一个数值：骰点 <= 精神值
    bool contains = false;  // 数组包含：背包 含 面包
    wstring text;
};

struct Effect {  // 把变量设为 / 加上 / 减去
    wstring var;
    int kind = 0;  // 0 设为 / 1 加 / 2 减
    int value = 0;
    wstring dice;  // 用骰子代替固定值：@改变 精神值 -1d6
    wstring fromVar;  // 用另一个数值代替固定值：@改变 敌方血量 -伤害
    wstring text;
};

struct DiceRoll {  // @骰 骰点 = 1d100
    wstring var;
    wstring expr;
};

/* ---------- 数组（可以混着放数字和文字） ---------- */

struct ArrElem {
    bool numeric = false;  // 看起来像整数的就记成数字，方便取出来当数值用
    int num = 0;
    wstring text;  // 显示和比较都用这一份
};

struct ArrayDef {  // @数组 背包 = 面包, 手电筒
    wstring name;
    vector<ArrElem> initial;
};

struct StoryArray {  // 运行时的一个数组
    wstring name;
    vector<ArrElem> items;
};

struct ArrayOp {  // @加 / @删 / @删位 / @插 / @清空 / @数量 / @查找
    enum Kind { None, Add, Remove, RemoveAt, Insert, Clear, CountTo, FindTo } kind = None;
    wstring arr;
    wstring elem;
    int index = 0;
    wstring dest;  // 结果写到哪个数值里（数量 / 查找返回的下标）
};

/* ---------- 节点里的一条"非文字语句" ---------- */

struct Stmt {
    enum Kind { EffectK, RollK, ArrayK } kind = EffectK;
    vector<Condition> conds;  // 条件满足才执行这一条（@改变 ... | 命中 >= 40）
    vector<Effect> effects;  // EffectK：一条 @改变 可以写好几个
    DiceRoll roll;           // RollK
    ArrayOp op;              // ArrayK
};

/* ---------- 侧栏要展示的东西 ---------- */

struct DisplayItem {
    wstring name;      // 数值名，或者数组名
    bool bar = false;  // 柱状条（血条那种）
    bool list = false; // 数组列表
    wstring maxSource; // 条的最大值：数字，或者另一个数值名
};

struct Step {
    enum Kind { Text, Scene, Image } kind = Text;
    wstring text;     // Text / Scene 用
    int imageNo = 0;  // Image 用；0 = 关闭图片
    bool fullscreen = false;
};

struct JumpTarget {  // -> 目标节点 | 条件
    wstring target;
    vector<Condition> conds;
};

/* ---------- 一段流程：@行动 和 @回合 里装的东西 ---------- */

struct FlowBlock {
    vector<Stmt> stmts;        // 要执行的语句
    vector<Step> steps;        // 要显示的文字 / 图片
    vector<JumpTarget> jumps;  // 满足条件就离开这个节点
};

struct Choice {
    wstring text;
    wstring target;
    vector<Condition> conds;  // 全部满足才显示这个选项
    vector<Effect> effects;   // 选中后立刻生效
    FlowBlock feedback;       // @反馈：选中后先弹一页，点确认再去目标节点
};

struct ActionBlock {
    wstring label;
    vector<Condition> conds;  // 满足才显示这个按钮
    vector<Effect> effects;   // 点了立刻生效
    FlowBlock body;
    bool freeTurn = false;    // @不耗回合：跑完不推进回合
};

/* ---------- 信息展示窗（@信息）和功能按键（@按键） ---------- */

struct InfoBlock {
    wstring title;   // @信息 物品栏
    FlowBlock body;  // 只放文字，打开时用当前数值展开
};

struct FuncButton {
    wstring label;   // 按钮文字
    wstring target;  // 目标：信息窗标题，或者要跳过去的节点名
};

/* ---------- 画面卡（黑屏大字 / 全屏图 / 渐显 / 打字） ---------- */

struct Card {
    bool typing = false;   // 逐字打出来（不写就是一下全显示）
    int imageNo = 0;       // 0 = 没图；否则全屏显示 images/档案/N.png
    wstring target;        // 空 = 放完继续当前节点；否则跳到这个节点
    vector<wstring> lines; // 大字内容，一行一条
};

struct Node {
    wstring name;
    wstring summary;  // @摘要：流程图里显示的那句话
    vector<Step> steps;
    vector<Choice> choices;
    vector<JumpTarget> jumps;  // -> 直接跳转（可以带条件，按顺序取第一个满足的）
    vector<Stmt> stmts;           // @改变 / @骰 / 数组操作，按书写顺序执行
    vector<ActionBlock> actions;  // @行动：有它的节点就是"封装节点"（按钮不进流程图）
    FlowBlock round;              // @回合：每个回合自动跑的固定流程
    vector<DisplayItem> disps; // @显示：侧栏要展示哪些数值 / 数组
    vector<InfoBlock> infos;   // @信息：可以随时打开的信息窗
    vector<FuncButton> funcs;  // @按键：节点上方那排功能键
    vector<Card> cards;        // @画面：进节点时先放的画面卡
    bool isEnding = false;
    int endingNo = 0;
    wstring endingTitle;
    wstring endingQuote;
};

struct Archive {
    wstring path;
    wstring base;  // 文件名（不含扩展名），同时是 images 下的子文件夹名
    wstring title;
    wstring startNode;
    vector<Node> nodes;
    map<wstring, int> indexByName;
    int startIndex = 0;
    int endingCount = 0;
    vector<VarDef> vars;  // 本故事声明的数值
    vector<ArrayDef> arrays;  // 本故事声明的数组
    vector<InfoBlock> infos;  // 故事级信息窗（写在节点外面，所有节点都能用）
};

static const int kSummaryMax = 24;  // @摘要 建议字数上限（流程图卡片一行放得下）

static int nodeIndexOf(const Archive& ar, const wstring& name) {
    auto it = ar.indexByName.find(name);
    return (it == ar.indexByName.end()) ? -1 : it->second;
}

static wstring stripQuote(const wstring& s) {
    const wstring t = trim(s);
    if (t.size() >= 2 &&
        ((t.front() == L'"' && t.back() == L'"') || (t.front() == L'「' && t.back() == L'」')))
        return t.substr(1, t.size() - 2);
    return t;
}

// "-＞" 和 "→" 都当跳转箭头
static size_t findArrow(const wstring& s) {
    const size_t p = s.find(L"->");
    const size_t q = s.find(L"→");
    if (p == wstring::npos) return q;
    if (q == wstring::npos) return p;
    return (std::min)(p, q);
}

static size_t arrowLength(const wstring& s, size_t at) {
    return (s.compare(at, 2, L"->") == 0) ? 2 : 1;
}

/* ---------- 条件 / 效果的写法解析 ---------- */

// 带正负号的整数（"80" / "+10" / "-5"）
static bool parseIntLoose(const wstring& s, int& out) {
    const wstring t = trim(s);
    if (t.empty()) return false;
    size_t i = 0;
    if (t[0] == L'+' || t[0] == L'-') i = 1;
    if (i >= t.size()) return false;
    for (size_t k = i; k < t.size(); ++k)
        if (!iswdigit((wint_t)t[k])) return false;
    out = _wtoi(t.c_str());
    return true;
}

// 用逗号 / 分号把 "|" 后面的东西切成一条条子句
static void splitClauses(const wstring& text, vector<wstring>& out) {
    wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L'，' || c == L';' || c == L'；' || c == L'、') {
            const wstring t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur += c;
        }
    }
    const wstring t = trim(cur);
    if (!t.empty()) out.push_back(t);
}

// 按空格把一句话切成词（@数组 相关指令用）
static void splitTokens(const wstring& text, vector<wstring>& out) {
    wstring cur;
    for (wchar_t c : text) {
        if (iswspace((wint_t)c) || c == L'\u3000') {
            const wstring t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur += c;
        }
    }
    const wstring t = trim(cur);
    if (!t.empty()) out.push_back(t);
}

// 取第一个词，后面剩下的原样返回（数组元素里可以带空格）
static wstring restAfterFirstToken(const wstring& text, wstring& first) {
    size_t i = 0;
    while (i < text.size() && !iswspace((wint_t)text[i]) && text[i] != L'\u3000') ++i;
    first = text.substr(0, i);
    return trim(text.substr(i));
}

// 元素不能为空、不能带换行（其余符号存进存档时会转义）
static bool elemOk(const wstring& e) {
    return !e.empty() && e.find_first_of(L"\r\n") == wstring::npos;
}

// 存进存档时把 \ , | 转义，读回来再还原
static wstring escapeElem(const wstring& e) {
    wstring out;
    for (wchar_t c : e) {
        if (c == L'\\' || c == L',' || c == L'|') {
            out += L'\\';
            out += c;
        } else {
            out += c;
        }
    }
    return out;
}

static wstring unescapeElem(const wstring& e) {
    wstring out;
    for (size_t i = 0; i < e.size(); ++i) {
        if (e[i] == L'\\' && i + 1 < e.size()) {
            out += e[++i];
            continue;
        }
        out += e[i];
    }
    return out;
}

static vector<wstring> splitEscaped(const wstring& text) {
    vector<wstring> out;
    wstring cur;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\\' && i + 1 < text.size()) {
            cur += text[i];
            cur += text[++i];
            continue;
        }
        if (text[i] == L',') {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += text[i];
    }
    out.push_back(cur);
    return out;
}

// 指令行的行尾注释：写了 " # 说明" 或 " // 说明"（前面留空格）就会被忽略
// 正文行不处理，免得剧情里的 # 被吃掉
static wstring stripTrailingComment(const wstring& text) {
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != L' ' && text[i] != L'\t' && text[i] != L'\u3000') continue;
        if (text[i + 1] == L'#') return trim(text.substr(0, i));
        if (text[i + 1] == L'/' && i + 2 < text.size() && text[i + 2] == L'/')
            return trim(text.substr(0, i));
    }
    return text;
}

/* ---------- 骰子 ---------- */

static std::mt19937& diceRng() {
    static std::mt19937 gen((unsigned)(GetTickCount64() ^
                                       ((ULONGLONG)GetCurrentProcessId() << 20)));
    return gen;
}

struct DiceTerm {
    int count = 1;
    int sides = 6;
    int sign = 1;
};

struct DiceExpr {
    vector<DiceTerm> terms;
    vector<int> consts;
    bool ok = false;
};

// 支持：d100 / 1d100 / 2d6+1 / 2d6-1d4+3 / 3d6
static DiceExpr parseDice(const wstring& text) {
    DiceExpr ex;
    wstring t;
    for (wchar_t c : trim(text))
        if (!iswspace((wint_t)c)) t += c;
    if (t.empty()) return ex;

    int sign = 1;
    size_t i = 0;
    while (i < t.size()) {
        const wchar_t c = t[i];
        if (c == L'+') {
            sign = 1;
            ++i;
            continue;
        }
        if (c == L'-') {
            sign = -1;
            ++i;
            continue;
        }
        int num = 0;
        bool hasNum = false;
        while (i < t.size() && iswdigit((wint_t)t[i])) {
            num = num * 10 + (t[i] - L'0');
            hasNum = true;
            ++i;
        }
        if (i < t.size() && (t[i] == L'd' || t[i] == L'D')) {
            ++i;
            int sides = 0;
            while (i < t.size() && iswdigit((wint_t)t[i])) {
                sides = sides * 10 + (t[i] - L'0');
                ++i;
            }
            if (sides <= 0 || sides > 1000000) return ex;
            DiceTerm term;
            term.count = hasNum ? num : 1;
            term.sides = sides;
            term.sign = sign;
            if (term.count <= 0 || term.count > 1000) return ex;
            ex.terms.push_back(term);
        } else {
            if (!hasNum || num > 100000000) return ex;
            ex.consts.push_back(sign * num);
        }
        sign = 1;
    }
    ex.ok = !(ex.terms.empty() && ex.consts.empty());
    return ex;
}

static bool diceOk(const wstring& text) { return parseDice(text).ok; }

// 去掉最前面的正负号（效果里的加减号已经单独记了）
static wstring stripSign(const wstring& text) {
    wstring t = trim(text);
    if (!t.empty() && (t[0] == L'+' || t[0] == L'-')) t = trim(t.substr(1));
    return t;
}

static int rollDiceExpr(const DiceExpr& ex) {
    int total = 0;
    for (const DiceTerm& t : ex.terms) {
        std::uniform_int_distribution<int> d(1, t.sides);
        for (int i = 0; i < t.count; ++i) total += d(diceRng()) * t.sign;
    }
    for (int c : ex.consts) total += c;
    return total;
}

static int rollDiceText(const wstring& text) {
    const DiceExpr ex = parseDice(text);
    return ex.ok ? rollDiceExpr(ex) : 0;
}

// 一条子句要么是条件（有比较符），要么是效果（设 / 加 / 减）
static bool parseClause(const wstring& clause, Condition* cond, Effect* eff, wstring& err) {
    const wstring s = trim(clause);
    if (s.empty()) {
        err = L"有空的条件 / 效果";
        return false;
    }

    // 数组包含：背包 含 面包
    for (const wchar_t* kw : {L"含", L"包含"}) {
        const size_t at = s.find(kw);
        if (at == wstring::npos || at == 0) continue;
        const wstring arr = stripQuote(trim(s.substr(0, at)));
        const wstring elem = trim(s.substr(at + wstring(kw).size()));
        if (arr.empty() || elem.empty()) continue;
        Condition cc;
        cc.var = arr;
        cc.contains = true;
        cc.rhsVar = stripQuote(elem);
        cc.text = s;
        if (cond) *cond = cc;
        return true;
    }

    struct Cmp {
        const wchar_t* tok;
        size_t len;
        int op;
    };
    static const Cmp kCmp[] = {{L">=", 2, 3}, {L"<=", 2, 5}, {L"==", 2, 0},
                               {L"!=", 2, 1}, {L">", 1, 2},  {L"<", 1, 4}};
    for (const Cmp& c : kCmp) {
        const size_t at = s.find(c.tok);
        if (at == wstring::npos || at == 0) continue;
        const wstring var = stripQuote(trim(s.substr(0, at)));
        if (var.empty()) continue;
        const wstring rhs = trim(s.substr(at + c.len));
        int val = 0;
        Condition cc;
        cc.var = var;
        cc.op = c.op;
        cc.text = s;
        if (parseIntLoose(rhs, val)) {
            cc.value = val;
        } else if (!rhs.empty() && !diceOk(rhs)) {
            cc.rhsVar = stripQuote(rhs);  // 右边写成另一个数值：骰点 <= 精神值
        } else {
            continue;
        }
        if (cond) *cond = cc;
        return true;
    }

    struct Assign {
        const wchar_t* tok;
        size_t len;
        int kind;
    };
    static const Assign kAssign[] = {{L"+=", 2, 1}, {L"-=", 2, 2}, {L"=", 1, 0}};
    for (const Assign& a : kAssign) {
        const size_t at = s.find(a.tok);
        if (at == wstring::npos || at == 0) continue;
        int val = 0;
        const wstring var = stripQuote(trim(s.substr(0, at)));
        if (var.empty()) continue;
        const wstring rhs = trim(s.substr(at + a.len));
        Effect ee;
        ee.var = var;
        ee.kind = a.kind;
        ee.text = s;
        if (parseIntLoose(rhs, val)) {
            ee.value = val;
        } else if (diceOk(rhs)) {
            ee.dice = stripSign(rhs);  // 效果也能用骰子：精神值 -= 1d6
        } else if (!rhs.empty() && rhs.find_first_of(L"=<>!+-") == wstring::npos) {
            ee.fromVar = stripQuote(rhs);  // 效果也能用别的数值：敌方血量 -= 伤害
        } else {
            continue;
        }
        if (eff) *eff = ee;
        return true;
    }

    // 还有 "精神值 +10" / "精神值 -5" / "精神值 +1d6" 这种不带等号的写法
    for (size_t at = s.size(); at > 1; --at) {
        const wchar_t c = s[at - 1];
        if (c != L'+' && c != L'-') continue;
        int val = 0;
        const wstring var = stripQuote(trim(s.substr(0, at - 1)));
        if (var.empty()) continue;
        const wstring rhs = s.substr(at - 1);
        Effect ee;
        ee.var = var;
        ee.kind = (c == L'+') ? 1 : 2;
        ee.text = s;
        if (parseIntLoose(rhs, val)) {
            ee.value = (val < 0) ? -val : val;
        } else if (diceOk(rhs)) {
            ee.dice = stripSign(rhs);
        } else if (!rhs.empty() && rhs.find_first_of(L"=<>!") == wstring::npos) {
            ee.fromVar = stripQuote(stripSign(rhs));  // "-伤害" -> "伤害"
        } else {
            continue;
        }
        if (eff) *eff = ee;
        return true;
    }

    err = L"看不懂这段条件 / 效果：「" + s +
          L"」（可以写：精神值 +10　精神值 -5　精神值 = 30　精神值 >= 50）";
    return false;
}

/* 解析剧本；返回 false 表示整个文件不可用 */
static bool parseScript(const wstring& text, Archive& ar, vector<wstring>& problems) {
    const vector<wstring> lines = splitLines(text);
    Node* cur = nullptr;
    int flowKind = 0;  // 0 = 节点正文 / 1 = 最后一个 @行动 / 2 = @回合
    InfoBlock* globalInfo = nullptr;  // 写在节点外面的 @信息（全篇通用）
    vector<Condition> stmtConds;  // 当前这条语句带的条件（写 @改变 ... | 条件 时用）
    auto problem = [&](size_t lineNo, const wstring& msg) {
        problems.push_back(L"第 " + std::to_wstring(lineNo) + L" 行：" + msg);
    };

    auto curFlow = [&]() -> FlowBlock* {
        if (!cur && flowKind == 3 && globalInfo) return &globalInfo->body;
        if (!cur) return nullptr;
        if (flowKind == 1 && !cur->actions.empty()) return &cur->actions.back().body;
        if (flowKind == 2) return &cur->round;
        if (flowKind == 3 && !cur->infos.empty()) return &cur->infos.back().body;
        if (flowKind == 4 && !cur->choices.empty()) return &cur->choices.back().feedback;
        return nullptr;
    };
    // 现在写的内容能不能落进某一段流程里（节点正文 / 行动 / 回合 / 故事级信息窗）
    auto haveFlow = [&]() { return cur != nullptr || (flowKind == 3 && globalInfo != nullptr); };
    auto pushStep = [&](const Step& s) {
        if (!haveFlow()) return;
        FlowBlock* fb = curFlow();
        if (fb) fb->steps.push_back(s);
        else if (cur) cur->steps.push_back(s);
    };
    auto pushStmt = [&](const Stmt& s) {
        if (!haveFlow()) return;
        Stmt item = s;
        item.conds.insert(item.conds.end(), stmtConds.begin(), stmtConds.end());
        FlowBlock* fb = curFlow();
        if (fb) fb->stmts.push_back(item);
        else if (cur) cur->stmts.push_back(item);
    };
    auto pushJump = [&](const JumpTarget& j) {
        if (!haveFlow()) return;
        FlowBlock* fb = curFlow();
        if (fb) fb->jumps.push_back(j);
        else if (cur) cur->jumps.push_back(j);
    };
    auto pushArrayOp = [&](size_t lineNo, const ArrayOp& op) {
        if (!cur) {
            problem(lineNo, L"数组指令得写在某个 :: 节点里面");
            return;
        }
        bool found = false;
        for (const ArrayDef& d : ar.arrays)
            if (d.name == op.arr) found = true;
        if (!found) {
            problem(lineNo, L"没有这个数组：「" + op.arr + L"」（先在节点外用 @数组 声明）");
            return;
        }
        Stmt st;
        st.kind = Stmt::ArrayK;
        st.op = op;
        pushStmt(st);
    };
    // 语句后面可以跟 "| 条件"：不满足就整条跳过（@改变 敌方血量 -伤害 | 命中 >= 40）
    auto splitStmtCond = [&](wstring& arg, size_t lineNo) {
        stmtConds.clear();
        const size_t bar = arg.find(L'|');
        const wstring head = trim(bar == wstring::npos ? arg : arg.substr(0, bar));
        if (bar == wstring::npos) return;
        vector<wstring> cls;
        splitClauses(arg.substr(bar + 1), cls);
        for (const wstring& cl : cls) {
            Condition c;
            Effect e;
            wstring err;
            if (!parseClause(cl, &c, &e, err)) {
                problem(lineNo, err);
                continue;
            }
            if (!c.var.empty())
                stmtConds.push_back(c);
            else
                problem(lineNo, L"语句后面的 | 只能写条件");
        }
        arg = head;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        wstring line = trim(lines[i]);
        const size_t lineNo = i + 1;
        if (line.empty() || line[0] == L'#' || startsWith(line, L"//")) continue;  // 整行注释
        // 指令行支持行尾注释（正文行不动）
        if (line[0] == L'@' || line[0] == L'>' || line[0] == L'\uFF1E' ||
            startsWith(line, L"::") || startsWith(line, L"->") || startsWith(line, L"→")) {
            line = stripTrailingComment(line);
            if (line.empty()) continue;
        }

        // ---------- 指令 ----------
        if (line[0] == L'@') {
            const wstring rest = trim(line.substr(1));
            const size_t sp = rest.find_first_of(L" \t\u3000");
            const wstring cmd = lower(sp == wstring::npos ? rest : rest.substr(0, sp));
            wstring arg = (sp == wstring::npos) ? wstring() : trim(rest.substr(sp + 1));
            stmtConds.clear();
            // 语句类指令：先把末尾的 "| 条件" 拆出来
            if (cmd == L"改变" || cmd == L"数值变化" || cmd == L"change" || cmd == L"骰" ||
                cmd == L"骰子" || cmd == L"roll" || cmd == L"随机" || cmd == L"加" ||
                cmd == L"追加" || cmd == L"增" || cmd == L"push" || cmd == L"删" ||
                cmd == L"移除" || cmd == L"remove" || cmd == L"删位" || cmd == L"移除位" ||
                cmd == L"removeat" || cmd == L"插" || cmd == L"插入" || cmd == L"insert" ||
                cmd == L"清空" || cmd == L"clear" || cmd == L"数量" || cmd == L"count" ||
                cmd == L"查找" || cmd == L"find") {
                splitStmtCond(arg, lineNo);
            }

            if (cmd == L"title" || cmd == L"标题") {
                ar.title = stripQuote(arg);
            } else if (cmd == L"start" || cmd == L"开始") {
                ar.startNode = stripQuote(arg);
            } else if (cmd == L"scene" || cmd == L"场景") {
                if (!haveFlow()) {
                    problem(lineNo, L"@场景 得写在某个 :: 节点里面");
                } else if (!arg.empty()) {
                    Step s;
                    s.kind = Step::Scene;
                    s.text = arg;
                    pushStep(s);
                }
            } else if (cmd == L"img" || cmd == L"image" || cmd == L"图片") {
                wstring a1 = arg, a2;
                const size_t sp2 = arg.find_first_of(L" \t\u3000");
                if (sp2 != wstring::npos) {
                    a1 = trim(arg.substr(0, sp2));
                    a2 = lower(trim(arg.substr(sp2 + 1)));
                }
                if (!cur) {
                    problem(lineNo, L"@图片 得写在某个 :: 节点里面");
                } else {
                    Step s;
                    s.kind = Step::Image;
                    int no = 0;
                    const wstring a1l = lower(a1);
                    if (a1l == L"none" || a1l == L"off" || a1 == L"无" || a1 == L"关闭" ||
                        (parseInt(a1, no) && no == 0)) {
                        s.imageNo = 0;
                    } else if (parseInt(a1, no) && no > 0) {
                        s.imageNo = no;
                    } else {
                        problem(lineNo, L"@图片 的编号只支持数字，比如：@图片 1 小窗");
                        continue;
                    }
                    s.fullscreen =
                        (a2 == L"full" || a2 == L"全屏" || a2 == L"整屏" || a2 == L"满屏");
                    pushStep(s);
                }
            } else if (cmd == L"end" || cmd == L"ending" || cmd == L"结局") {
                if (!cur) {
                    problem(lineNo, L"@结局 得写在某个 :: 节点里面");
                } else {
                    const size_t bar = arg.find(L'|');
                    const wstring head = trim(bar == wstring::npos ? arg : arg.substr(0, bar));
                    const wstring quote =
                        (bar == wstring::npos) ? wstring() : stripQuote(arg.substr(bar + 1));
                    int no = 0;
                    const size_t sp2 = head.find_first_of(L" \t\u3000");
                    const wstring first = (sp2 == wstring::npos) ? head : head.substr(0, sp2);
                    wstring name = (sp2 == wstring::npos) ? wstring() : trim(head.substr(sp2 + 1));
                    if (parseInt(first, no)) {
                        cur->endingNo = no;
                    } else {
                        name = head;  // 没写编号就把整串当名字
                        cur->endingNo = 0;
                    }
                    cur->isEnding = true;
                    cur->endingTitle = name;
                    cur->endingQuote = quote;
                }
            } else if (cmd == L"var" || cmd == L"数值" || cmd == L"变量") {
                // @数值 精神值 = 80
                const size_t eq = arg.find(L'=');
                const wstring name =
                    stripQuote(trim(eq == wstring::npos ? arg : arg.substr(0, eq)));
                const wstring valText = (eq == wstring::npos) ? wstring() : trim(arg.substr(eq + 1));
                int init = 0;
                VarDef vd;
                vd.name = name;
                bool ok = (!name.empty() && eq != wstring::npos);
                if (ok) {
                    if (parseIntLoose(valText, init)) {
                        vd.initial = init;
                    } else if (diceOk(valText)) {
                        vd.dice = stripSign(valText);  // 初值也能掷出来，比如 3d6
                    } else {
                        ok = false;
                    }
                }
                if (!ok) {
                    problem(lineNo, L"@数值 的写法是：@数值 精神值 = 80（初值也可以掷：3d6）");
                } else {
                    bool dup = false;
                    for (const VarDef& d : ar.vars)
                        if (d.name == name) dup = true;
                    if (dup)
                        problem(lineNo, L"数值「" + name + L"」声明了两次");
                    else
                        ar.vars.push_back(vd);
                }
            } else if (cmd == L"summary" || cmd == L"摘要") {
                if (!cur) {
                    problem(lineNo, L"@摘要 得写在某个 :: 节点里面");
                } else if (arg.empty()) {
                    problem(lineNo, L"@摘要 后面要写一句话");
                } else {
                    cur->summary = arg;
                }
            } else if (cmd == L"change" || cmd == L"改变" || cmd == L"数值变化") {
                // @改变 精神值 -5      /      @改变 血量 -伤害, 精神值 +1d6
                if (!cur) {
                    problem(lineNo, L"@改变 得写在某个 :: 节点里面");
                } else {
                    vector<wstring> cls;
                    splitClauses(arg, cls);
                    Stmt st;
                    st.kind = Stmt::EffectK;
                    for (const wstring& cl : cls) {
                        Effect e;
                        wstring err;
                        if (parseClause(cl, nullptr, &e, err))
                            st.effects.push_back(e);
                        else
                            problem(lineNo, err);
                    }
                    if (!st.effects.empty()) pushStmt(st);
                }
            } else if (cmd == L"骰" || cmd == L"骰子" || cmd == L"roll" || cmd == L"随机") {
                // @骰 骰点 = 1d100     /     @骰 力气 = 3d6, 敏捷 = 3d6
                if (!cur) {
                    problem(lineNo, L"@骰 得写在某个 :: 节点里面");
                } else {
                    vector<wstring> items;
                    splitClauses(arg, items);
                    if (items.empty())
                        problem(lineNo, L"@骰 的写法是：@骰 骰点 = 1d100");
                    for (const wstring& it : items) {
                        const size_t eq = it.find(L'=');
                        const wstring name =
                            stripQuote(trim(eq == wstring::npos ? it : it.substr(0, eq)));
                        const wstring expr =
                            (eq == wstring::npos) ? wstring() : trim(it.substr(eq + 1));
                        if (name.empty() || eq == wstring::npos || !diceOk(expr)) {
                            problem(lineNo,
                                    L"@骰 的写法是：@骰 骰点 = 1d100；一次掷多个写："
                                    L"@骰 力气 = 3d6, 敏捷 = 3d6");
                            continue;
                        }
                        DiceRoll dr;
                        dr.var = name;
                        dr.expr = stripSign(expr);
                        Stmt st;
                        st.kind = Stmt::RollK;
                        st.roll = dr;
                        pushStmt(st);
                    }
                }
            } else if (cmd == L"数组" || cmd == L"array") {
                // @数组 背包 = 面包, 手电筒      /      @数组 背包
                const size_t eq = arg.find(L'=');
                const wstring name = stripQuote(trim(eq == wstring::npos ? arg : arg.substr(0, eq)));
                if (name.empty()) {
                    problem(lineNo, L"@数组 的写法是：@数组 背包 = 面包, 手电筒");
                } else {
                    bool dup = false;
                    for (const ArrayDef& d : ar.arrays)
                        if (d.name == name) dup = true;
                    if (dup) {
                        problem(lineNo, L"数组「" + name + L"」声明了两次");
                    } else {
                        ArrayDef ad;
                        ad.name = name;
                        if (eq != wstring::npos) {
                            vector<wstring> items;
                            splitClauses(arg.substr(eq + 1), items);
                            for (const wstring& it : items) {
                                if (!elemOk(it)) {
                                    problem(lineNo,
                                            L"数组元素里不能有 , ， ; ； 、 | 这些符号：" + it);
                                    continue;
                                }
                                ArrElem e;
                                e.text = trim(it);
                                int v = 0;
                                if (parseIntLoose(e.text, v)) {
                                    e.numeric = true;
                                    e.num = v;
                                }
                                ad.initial.push_back(e);
                            }
                        }
                        ar.arrays.push_back(ad);
                    }
                }
            } else if (cmd == L"显示" || cmd == L"display") {
                // @显示 血量 条 100     /     @显示 背包 列表
                if (!cur) {
                    problem(lineNo, L"@显示 得写在某个 :: 节点里面");
                } else {
                    vector<wstring> tk;
                    splitTokens(arg, tk);
                    if (tk.empty()) {
                        problem(lineNo, L"@显示 的写法是：@显示 血量 条 100　或　@显示 背包 列表");
                    } else {
                        DisplayItem d;
                        d.name = stripQuote(tk[0]);
                        for (size_t t = 1; t < tk.size(); ++t) {
                            const wstring low = lower(tk[t]);
                            if (low == L"条" || low == L"bar") d.bar = true;
                            else if (low == L"列表" || low == L"list") d.list = true;
                            else d.maxSource = stripQuote(tk[t]);
                        }
                        cur->disps.push_back(d);
                    }
                }
            } else if (cmd == L"行动" || cmd == L"action") {
                // @行动 攻击      /      @行动 用面包 | 背包 含 面包
                if (!cur) {
                    problem(lineNo, L"@行动 得写在某个 :: 节点里面");
                } else {
                    const size_t bar = arg.find(L'|');
                    const wstring head = trim(bar == wstring::npos ? arg : arg.substr(0, bar));
                    const wstring tail = (bar == wstring::npos) ? wstring() : arg.substr(bar + 1);
                    ActionBlock ab;
                    ab.label = stripQuote(head);
                    if (ab.label.empty()) {
                        problem(lineNo, L"@行动 后面要写按钮文字，比如：@行动 攻击");
                    } else {
                        vector<wstring> cls;
                        splitClauses(tail, cls);
                        for (const wstring& cl : cls) {
                            Condition cond;
                            Effect eff;
                            wstring err;
                            if (!parseClause(cl, &cond, &eff, err)) {
                                problem(lineNo, err);
                                continue;
                            }
                            if (!cond.var.empty()) ab.conds.push_back(cond);
                            if (!eff.var.empty()) ab.effects.push_back(eff);
                        }
                        cur->actions.push_back(ab);
                        flowKind = 1;
                    }
                }
            } else if (cmd == L"回合" || cmd == L"round") {
                if (!cur) {
                    problem(lineNo, L"@回合 得写在某个 :: 节点里面");
                } else if (!cur->round.stmts.empty() || !cur->round.steps.empty() ||
                           !cur->round.jumps.empty()) {
                    problem(lineNo, L"一个节点只能写一段 @回合");
                } else {
                    flowKind = 2;
                }
            } else if (cmd == L"不耗回合" || cmd == L"free") {
                if (cur && flowKind == 1 && !cur->actions.empty())
                    cur->actions.back().freeTurn = true;
                else
                    problem(lineNo, L"@不耗回合 要写在某个 @行动 里面");
            } else if (cmd == L"反馈" || cmd == L"结果" || cmd == L"result") {
                // @反馈 你的学力提升了。     （写在 > 选项 后面，选中后先弹这一页）
                if (!cur || cur->choices.empty()) {
                    problem(lineNo, L"@反馈 要写在某个 > 选项 的后面");
                } else {
                    if (!arg.empty()) {  // 也可以直接写在 @反馈 这一行
                        Step s;
                        s.kind = Step::Text;
                        s.text = stripQuote(arg);
                        cur->choices.back().feedback.steps.push_back(s);
                    }
                    flowKind = 4;
                }
            } else if (cmd == L"信息" || cmd == L"info" || cmd == L"信息窗") {
                // @信息 物品栏      （后面写的内容就是打开时显示的东西）
                // 写在节点外面 = 故事级信息窗，所有节点都能用
                InfoBlock ib;
                ib.title = stripQuote(arg);
                if (ib.title.empty()) {
                    problem(lineNo, L"@信息 后面要写标题，比如：@信息 物品栏");
                } else if (!cur) {
                    ar.infos.push_back(ib);
                    globalInfo = &ar.infos.back();
                    flowKind = 3;
                } else {
                    cur->infos.push_back(ib);
                    flowKind = 3;
                }
            } else if (cmd == L"按键" || cmd == L"按钮" || cmd == L"key") {
                // @按键 背包 -> 物品栏     （打开信息窗）
                // @按键 地图 -> 车站       （跳到某个节点）
                if (!cur) {
                    problem(lineNo, L"@按键 得写在某个 :: 节点里面");
                    continue;
                }
                if (flowKind != 0) {
                    problem(lineNo, L"@按键 只能写在节点正文里");
                    continue;
                }
                const size_t ar = arg.find(L"->");
                FuncButton fb;
                fb.label = stripQuote(trim(ar == wstring::npos ? arg : arg.substr(0, ar)));
                fb.target = (ar == wstring::npos) ? wstring() : stripQuote(trim(arg.substr(ar + 2)));
                if (fb.label.empty() || fb.target.empty()) {
                    problem(lineNo, L"@按键 的写法是：@按键 背包 -> 物品栏（或 -> 节点名）");
                } else {
                    cur->funcs.push_back(fb);
                }
            } else if (cmd == L"画面" || cmd == L"标题卡" || cmd == L"card") {
                // @画面 打字 图片 3 -> 开场      （一个节点一张卡；多行就写多条 @画面）
                if (!cur) {
                    problem(lineNo, L"@画面 得写在某个 :: 节点里面");
                } else {
                    const size_t bar = arg.find(L"->");
                    const wstring head = trim(bar == wstring::npos ? arg : arg.substr(0, bar));
                    const wstring target =
                        (bar == wstring::npos) ? wstring() : stripQuote(trim(arg.substr(bar + 2)));
                    if (bar != wstring::npos && target.empty()) {
                        problem(lineNo, L"@画面 的 -> 后面要写目标节点");
                        continue;
                    }
                    if (cur->cards.empty()) cur->cards.push_back(Card());
                    Card& card = cur->cards.back();
                    if (!target.empty()) card.target = target;

                    vector<wstring> tk;
                    splitTokens(head, tk);
                    bool keywordForm = false;
                    for (size_t i = 0; i < tk.size(); ++i) {
                        const wstring low = lower(tk[i]);
                        if (low == L"打字" || low == L"typing" || low == L"逐字") {
                            card.typing = true;
                            keywordForm = true;
                        } else if (low == L"直接" || low == L"静默" || low == L"instant") {
                            card.typing = false;
                            keywordForm = true;
                        } else if ((low == L"图片" || low == L"图" || low == L"image") &&
                                   i + 1 < tk.size()) {
                            int no = 0;
                            if (parseInt(tk[++i], no) && no > 0) {
                                card.imageNo = no;
                                keywordForm = true;
                            } else {
                                problem(lineNo, L"@画面 图片 后面要写编号，比如：@画面 图片 3");
                            }
                        }
                    }
                    // 不是关键字写法，就把这行当成一行大字
                    if (!keywordForm && !head.empty()) card.lines.push_back(head);
                }
            } else if (cmd == L"加" || cmd == L"追加" || cmd == L"增" || cmd == L"push") {
                // @加 背包 面包        （加在末尾）
                wstring arrName;
                const wstring elem = restAfterFirstToken(arg, arrName);
                if (arrName.empty() || !elemOk(elem)) {
                    problem(lineNo, L"@加 的写法是：@加 背包 面包");
                } else {
                    ArrayOp op;
                    op.kind = ArrayOp::Add;
                    op.arr = arrName;
                    op.elem = elem;
                    pushArrayOp(lineNo, op);
                }
            } else if (cmd == L"删" || cmd == L"移除" || cmd == L"remove") {
                // @删 背包 面包        （删掉第一个一样的）
                wstring arrName;
                const wstring elem = restAfterFirstToken(arg, arrName);
                if (arrName.empty() || !elemOk(elem)) {
                    problem(lineNo, L"@删 的写法是：@删 背包 面包");
                } else {
                    ArrayOp op;
                    op.kind = ArrayOp::Remove;
                    op.arr = arrName;
                    op.elem = elem;
                    pushArrayOp(lineNo, op);
                }
            } else if (cmd == L"删位" || cmd == L"移除位" || cmd == L"removeat") {
                // @删位 背包 0         （按下标删，从 0 开始）
                vector<wstring> tk;
                splitTokens(arg, tk);
                int idx = -1;
                if (tk.size() < 2 || !parseIntLoose(tk[1], idx)) {
                    problem(lineNo, L"@删位 的写法是：@删位 背包 0（下标从 0 开始）");
                } else {
                    ArrayOp op;
                    op.kind = ArrayOp::RemoveAt;
                    op.arr = tk[0];
                    op.index = idx;
                    pushArrayOp(lineNo, op);
                }
            } else if (cmd == L"插" || cmd == L"插入" || cmd == L"insert") {
                // @插 背包 0 面包      （插到指定下标处）
                wstring arrName, idxTok;
                const wstring afterName = restAfterFirstToken(arg, arrName);
                int idx = -1;
                const wstring elem = restAfterFirstToken(afterName, idxTok);
                if (arrName.empty() || !parseIntLoose(idxTok, idx) || !elemOk(elem)) {
                    problem(lineNo, L"@插 的写法是：@插 背包 0 面包");
                } else {
                    ArrayOp op;
                    op.kind = ArrayOp::Insert;
                    op.arr = arrName;
                    op.index = idx;
                    op.elem = elem;
                    pushArrayOp(lineNo, op);
                }
            } else if (cmd == L"清空" || cmd == L"clear") {
                vector<wstring> tk;
                splitTokens(arg, tk);
                if (tk.empty()) {
                    problem(lineNo, L"@清空 的写法是：@清空 背包");
                } else {
                    ArrayOp op;
                    op.kind = ArrayOp::Clear;
                    op.arr = tk[0];
                    pushArrayOp(lineNo, op);
                }
            } else if (cmd == L"数量" || cmd == L"count") {
                // @数量 背包 -> 件数
                vector<wstring> tk;
                splitTokens(arg, tk);
                if (tk.size() < 3 || tk[1] != L"->") {
                    problem(lineNo, L"@数量 的写法是：@数量 背包 -> 件数");
                } else {
                    ArrayOp op;
                    op.kind = ArrayOp::CountTo;
                    op.arr = tk[0];
                    op.dest = stripQuote(tk[2]);
                    pushArrayOp(lineNo, op);
                }
            } else if (cmd == L"查找" || cmd == L"find") {
                // @查找 背包 面包 -> 位置      （找到写下标，找不到写 -1）
                wstring arrName;
                const wstring body = restAfterFirstToken(arg, arrName);
                const size_t ar = body.find(L"->");
                const wstring elem = trim(ar == wstring::npos ? body : body.substr(0, ar));
                const wstring dest = trim(ar == wstring::npos ? wstring() : body.substr(ar + 2));
                if (arrName.empty() || !elemOk(elem) || dest.empty()) {
                    problem(lineNo, L"@查找 的写法是：@查找 背包 面包 -> 位置");
                } else {
                    ArrayOp op;
                    op.kind = ArrayOp::FindTo;
                    op.arr = arrName;
                    op.elem = elem;
                    op.dest = stripQuote(dest);
                    pushArrayOp(lineNo, op);
                }
            } else {
                problem(lineNo, L"看不懂的指令 @" + cmd);
            }
            continue;
        }

        // ---------- 节点 ----------
        if (startsWith(line, L"::")) {
            const wstring name = trim(line.substr(2));
            if (name.empty()) {
                problem(lineNo, L":: 后面要写节点名");
                cur = nullptr;
                continue;
            }
            Node n;
            n.name = name;
            ar.nodes.push_back(n);
            cur = &ar.nodes.back();
            flowKind = 0;
            globalInfo = nullptr;
            continue;
        }

        // ---------- 直接跳转 ----------
        if (startsWith(line, L"->") || startsWith(line, L"→")) {
            if (!cur) {
                problem(lineNo, L"-> 得写在某个 :: 节点里面");
                continue;
            }
            const wstring rest = trim(line.substr(startsWith(line, L"->") ? 2 : 1));
            const size_t bar = rest.find(L'|');
            JumpTarget jt;
            jt.target = stripQuote(trim(bar == wstring::npos ? rest : rest.substr(0, bar)));
            if (bar != wstring::npos) {
                vector<wstring> clauses;
                splitClauses(rest.substr(bar + 1), clauses);
                for (const wstring& cl : clauses) {
                    Condition cond;
                    Effect eff;
                    wstring err;
                    if (!parseClause(cl, &cond, &eff, err)) {
                        problem(lineNo, err);
                    } else if (cond.var.empty()) {
                        problem(lineNo, L"-> 后面只能写条件（比如：精神值 >= 50），不能写数值修改");
                    } else {
                        jt.conds.push_back(cond);
                    }
                }
            }
            if (jt.target.empty())
                problem(lineNo, L"-> 后面要写目标节点");
            else
                pushJump(jt);
            continue;
        }

        // ---------- 选项 ----------
        if (line[0] == L'>' || line[0] == L'\uFF1E') {
            flowKind = 0;  // 选项永远属于节点正文（@反馈 这类块到这一行就结束）
            if (!cur) {
                problem(lineNo, L"> 选项得写在某个 :: 节点里面");
                continue;
            }
            const wstring body = trim(line.substr(1));
            const size_t bar = body.find(L'|');
            const wstring head = (bar == wstring::npos) ? body : body.substr(0, bar);
            const wstring tail = (bar == wstring::npos) ? wstring() : body.substr(bar + 1);
            const size_t arrow = findArrow(head);
            Choice c;
            if (arrow == wstring::npos) {
                c.text = stripQuote(head);
                problem(lineNo, L"选项「" + c.text + L"」没写 -> 目标节点");
            } else {
                c.text = stripQuote(trim(head.substr(0, arrow)));
                c.target = stripQuote(trim(head.substr(arrow + arrowLength(head, arrow))));
                if (c.target.empty())
                    problem(lineNo, L"选项「" + c.text + L"」的箭头后面是空的");
            }
            if (!tail.empty()) {
                vector<wstring> clauses;
                splitClauses(tail, clauses);
                for (const wstring& cl : clauses) {
                    Condition cond;
                    Effect eff;
                    wstring err;
                    if (!parseClause(cl, &cond, &eff, err)) {
                        problem(lineNo, err);
                        continue;
                    }
                    if (!cond.var.empty()) c.conds.push_back(cond);  // 满不满足才显示
                    if (!eff.var.empty()) c.effects.push_back(eff);   // 选中后生效
                }
            }
            cur->choices.push_back(c);
            continue;
        }

        // ---------- 正文 ----------
        if (!haveFlow()) {
            problem(lineNo, L"这行文字不在任何节点里（先用 :: 节点名 开一个节点）");
            continue;
        }
        if (line == L"---") continue;
        Step s;
        s.kind = Step::Text;
        s.text = line;
        pushStep(s);
    }

    if (ar.nodes.empty()) {
        problems.push_back(L"档案里一个 :: 节点都没有");
        return false;
    }

    for (size_t i = 0; i < ar.nodes.size(); ++i) {
        const wstring& name = ar.nodes[i].name;
        if (ar.indexByName.count(name))
            problems.push_back(L"节点名重复：「" + name + L"」（后面那个会被忽略）");
        else
            ar.indexByName[name] = (int)i;
    }

    if (!ar.startNode.empty()) {
        ar.startIndex = nodeIndexOf(ar, ar.startNode);
        if (ar.startIndex < 0) {
            problems.push_back(L"@开始 指向的节点「" + ar.startNode + L"」不存在，已改用第一个节点");
            ar.startIndex = 0;
        }
    }

    // 条件 / 效果里用到的数值必须声明过；忘了声明的自动补一个 0，并统一提醒一次
    // （@骰 自己就会声明那个数值，不算"忘了声明"）
    std::set<wstring> autoVars;
    auto touchVar = [&](const wstring& name) {
        if (name.empty()) return;
        for (const VarDef& d : ar.vars)
            if (d.name == name) return;
        ar.vars.push_back({name, 0});
        autoVars.insert(name);
    };
    auto touchEffects = [&](const vector<Effect>& es) {
        for (const Effect& e : es) {
            touchVar(e.var);
            touchVar(e.fromVar);
        }
    };
    auto touchStmts = [&](const vector<Stmt>& list) {
        for (const Stmt& st : list) {
            if (st.kind == Stmt::EffectK)
                touchEffects(st.effects);
            else if (st.kind == Stmt::RollK)
                touchVar(st.roll.var);
            else if (st.kind == Stmt::ArrayK)
                touchVar(st.op.dest);
        }
    };
    auto touchConds = [&](const vector<Condition>& cs) {
        for (const Condition& cd : cs) {
            if (cd.contains) continue;  // "背包 含 面包" 里的两个名字是数组和元素，不是数值
            touchVar(cd.var);
            touchVar(cd.rhsVar);
        }
    };
    auto touchJumps = [&](const vector<JumpTarget>& js) {
        for (const JumpTarget& j : js) touchConds(j.conds);
    };
    // @骰 自己就会声明那个数值，不算"忘了声明"
    for (const Node& n : ar.nodes) {
        vector<const vector<Stmt>*> lists = {&n.stmts, &n.round.stmts};
        for (const ActionBlock& ab : n.actions) lists.push_back(&ab.body.stmts);
        for (const vector<Stmt>* list : lists)
            for (const Stmt& st : *list)
                if (st.kind == Stmt::RollK && !st.roll.var.empty())
                    touchVar(st.roll.var);
    }
    for (const Node& n : ar.nodes) {
        touchStmts(n.stmts);
        touchStmts(n.round.stmts);
        touchJumps(n.jumps);
        touchJumps(n.round.jumps);
        for (const Choice& c : n.choices) {
            touchConds(c.conds);
            touchEffects(c.effects);
        }
        for (const ActionBlock& ab : n.actions) {
            touchConds(ab.conds);
            touchEffects(ab.effects);
            touchStmts(ab.body.stmts);
            touchJumps(ab.body.jumps);
        }
    }
    if (!autoVars.empty()) {
        wstring list;
        for (const wstring& v : autoVars) {
            if (!list.empty()) list += L"、";
            list += v;
        }
        problems.push_back(L"这些数值没有用 @数值 声明，已按初始值 0 处理：" + list);
    }

    // 正文里 {数值名} 引用的名字必须存在
    std::set<wstring> unknownRefs;
    auto scanRefs = [&](const wstring& text) {
        for (size_t i = 0; i < text.size();) {
            const size_t open = text.find(L'{', i);
            if (open == wstring::npos) break;
            const size_t close = text.find(L'}', open + 1);
            if (close == wstring::npos) break;
            const wstring name = trim(text.substr(open + 1, close - open - 1));
            if (!name.empty()) {
                // {背包[0]} 这种，只检查中括号前面那个名字
                const size_t br = name.find(L'[');
                const wstring base = (br == wstring::npos) ? name : trim(name.substr(0, br));
                bool found = false;
                for (const VarDef& d : ar.vars)
                    if (d.name == base) found = true;
                for (const ArrayDef& d : ar.arrays)
                    if (d.name == base) found = true;
                if (!found) unknownRefs.insert(base);
            }
            i = close + 1;
        }
    };
    for (const Node& n : ar.nodes) {
        for (const Step& st : n.steps)
            if (st.kind != Step::Image) scanRefs(st.text);
        scanRefs(n.endingQuote);
    }
    if (!unknownRefs.empty()) {
        wstring list;
        for (const wstring& v : unknownRefs) {
            if (!list.empty()) list += L"、";
            list += v;
        }
        problems.push_back(L"正文里写了 {" + list + L"}，但没有这些数值（会原样显示）");
    }

    int noSummary = 0;
    for (const Node& n : ar.nodes) {
        if (n.summary.empty()) ++noSummary;
        // @显示 里写的名字得存在，条状要用数值，列表要用数组
        auto hasVar = [&](const wstring& name) {
            for (const VarDef& d : ar.vars)
                if (d.name == name) return true;
            return false;
        };
        auto hasArray = [&](const wstring& name) {
            for (const ArrayDef& d : ar.arrays)
                if (d.name == name) return true;
            return false;
        };
        for (const DisplayItem& d : n.disps) {
            if (d.list && !hasArray(d.name))
                problems.push_back(L"节点「" + n.name + L"」的 @显示 " + d.name +
                                   L" 列表：没有这个数组");
            else if (!d.list && !hasVar(d.name))
                problems.push_back(L"节点「" + n.name + L"」的 @显示 " + d.name +
                                   L"：没有这个数值（@显示 的名字要在 @数值 里声明过）");
            if (d.bar && !d.maxSource.empty()) {
                int tmp = 0;
                if (!parseIntLoose(d.maxSource, tmp) && !hasVar(d.maxSource))
                    problems.push_back(L"节点「" + n.name + L"」的 @显示 " + d.name + L" 条 " +
                                       d.maxSource + L"：最大值既不是数字也不是已声明的数值");
            }
        }
        // 封装节点：@行动 是它的按钮，节点自己的选项不会显示
        if (!n.actions.empty() && !n.choices.empty())
            problems.push_back(L"节点「" + n.name +
                               L"」同时有 @行动 和 > 选项：封装节点里只有 @行动 会显示，选项被忽略了");
        for (const ActionBlock& ab : n.actions) {
            if (ab.freeTurn && n.round.stmts.empty() && n.round.steps.empty())
                problems.push_back(L"节点「" + n.name + L"」的 @行动 " + ab.label +
                                   L" 写了 @不耗回合，但这个节点没有 @回合 流程");
        }
        // 含 后面那个名字得是数组
        for (const FuncButton& fb : n.funcs) {  // @按键 的目标要能对上
            bool isInfo = false;
            for (const InfoBlock& ib : n.infos)
                if (ib.title == fb.target) isInfo = true;
            for (const InfoBlock& ib : ar.infos)
                if (ib.title == fb.target) isInfo = true;
            const bool isNode = nodeIndexOf(ar, fb.target) >= 0;
            if (!isInfo && !isNode)
                problems.push_back(L"节点「" + n.name + L"」的 @按键 " + fb.label + L" 指向「" +
                                   fb.target + L"」：既不是本节点的信息窗，也不是节点名");
        }
        for (const InfoBlock& ib : n.infos) {
            if (!ib.body.stmts.empty() || !ib.body.jumps.empty())
                problems.push_back(L"节点「" + n.name + L"」的信息窗「" + ib.title +
                                   L"」里不要写 @改变 / @骰 / 数组操作 / ->，它只用来展示");
        }

        auto checkContains = [&](const vector<Condition>& cs) {
            for (const Condition& c : cs)
                if (c.contains && !hasArray(c.var))
                    problems.push_back(L"节点「" + n.name + L"」的条件「" + c.text +
                                       L"」里的「" + c.var + L"」不是数组");
        };
        for (const Choice& c : n.choices) checkContains(c.conds);
        for (const JumpTarget& j : n.jumps) checkContains(j.conds);
        for (const JumpTarget& j : n.round.jumps) checkContains(j.conds);
        for (const ActionBlock& ab : n.actions) {
            checkContains(ab.conds);
            for (const JumpTarget& j : ab.body.jumps) checkContains(j.conds);
        }
        for (const Choice& c : n.choices)
            if (!c.target.empty() && nodeIndexOf(ar, c.target) < 0)
                problems.push_back(L"节点「" + n.name + L"」的选项「" + c.text +
                                   L"」指向不存在的节点「" + c.target + L"」");
        for (const JumpTarget& j : n.jumps)
            if (nodeIndexOf(ar, j.target) < 0)
                problems.push_back(L"节点「" + n.name + L"」的 -> 指向不存在的节点「" + j.target +
                                   L"」");
        // @画面 -> 目标 也是这个节点的出口，所以算不算"卡住"要看它
        bool hasCardExit = false;
        for (const Card& cd : n.cards) {
            if (cd.target.empty()) continue;
            hasCardExit = true;
            if (nodeIndexOf(ar, cd.target) < 0)
                problems.push_back(L"节点「" + n.name + L"」的 @画面 -> 指向不存在的节点「" +
                                   cd.target + L"」");
        }
        if (!n.isEnding && n.actions.empty() && n.choices.empty() && n.jumps.empty() &&
            !hasCardExit)
            problems.push_back(L"节点「" + n.name +
                               L"」既没有选项也没有 ->，玩家走到这里会卡住（现在会显示结束按钮）");
        bool hasFallback = false;
        for (const JumpTarget& j : n.jumps)
            if (j.conds.empty()) hasFallback = true;
        if (!n.jumps.empty() && !hasFallback)
            problems.push_back(L"节点「" + n.name +
                               L"」的 -> 全带条件，建议最后补一条不带条件的兜底 ->，免得全部不满足");
        if ((int)n.summary.size() > kSummaryMax)
            problems.push_back(L"节点「" + n.name + L"」的 @摘要 有 " +
                               std::to_wstring(n.summary.size()) + L" 个字，超过 " +
                               std::to_wstring(kSummaryMax) + L" 字了（流程图里会截断）");
    }
    if (noSummary > 0)
        problems.push_back(L"有 " + std::to_wstring(noSummary) +
                           L" 个节点没写 @摘要，流程图里会用节点名代替（建议补上）");

    for (const Node& n : ar.nodes)
        if (n.isEnding) ar.endingCount = (std::max)(ar.endingCount, n.endingNo);
    if (ar.endingCount == 0)
        for (const Node& n : ar.nodes)
            if (n.isEnding) ++ar.endingCount;

    return true;
}

static bool loadArchiveFile(const wstring& path, Archive& ar, vector<wstring>& problems) {
    wstring text;
    if (!readTextFile(path, text)) {
        problems.push_back(L"读不到文件：" + path);
        return false;
    }
    ar = Archive();
    ar.path = path;
    ar.base = baseName(path);
    if (!parseScript(text, ar, problems)) return false;
    if (ar.title.empty()) ar.title = ar.base;
    return true;
}

static const wchar_t* kTemplateScript = LR"SCN(
# ============================================================
#  新档案模板：复制这个文件、改个名字，就能开始写自己的故事。
#  （文件名以下划线开头，不会出现在游戏的档案列表里）
#
#  ── 基础写法 ──────────────────────────────────────────────
#     @标题 我的故事                档案名
#     @开始 开场                    从哪个节点开始
#     :: 节点名                     一个节点
#     @场景 第一章 ｜ 名字           插一行小标题
#     @图片 1 小窗 / @图片 1 全屏     放图（images/档案名/1.png）
#     @图片 0                      收掉图片
#     > 选项文字 -> 目标节点         一个选项
#     -> 目标节点                   点「继续」直接跳过去
#     @结局 1 结局名 | 收尾的话       结局
#
#  ── 数值：想在界面上显示，就在节点里 @显示 ──────────────────────
#     @数值 精神值 = 60             声明一个数值（写一次就够）
#     @改变 精神值 -5               进入这个节点时生效
#     > 选项 -> 目标 | 精神值 +10     选中后加 10
#     > 选项 -> 目标 | 精神值 -5      选中后减 5
#     > 选项 -> 目标 | 精神值 = 30    选中后直接设成 30
#     > 选项 -> 目标 | 精神值 >= 50   条件不满足就不显示这个选项
#     > 选项 -> 目标 | 精神值 >= 50, 精神值 -5     条件 + 效果一起写
#     -> 目标节点 | 精神值 >= 50      条件跳转：从上往下，第一条满足的生效
#     比较符： >=  <=  >  <  ==  !=     多条用逗号或分号隔开
#
#  ── @摘要：流程图卡片上显示的那句话（建议 24 字以内）────────
#     每个节点都写一句，比如： @摘要 走廊尽头那扇门
#
#  ── 骰子 / 检定（跑团风）──────────────────────────────────
#     @数值 力量 = 3d6              初值也能掷出来（建角色属性很方便）
#     @骰 骰点 = 1d100              进这个节点时掷骰，结果写进数值
#     @骰 力气 = 3d6, 敏捷 = 3d6     一句话掷多个
#     @骰 伤害 = 2d6+1              多个骰求和再加常数
#     @改变 精神值 -1d6             效果里也能用骰子（随机伤害）
#     > 硬闯 -> 门后 | 骰点 <= 60    检定：掷骰和固定值比
#     > 硬闯 -> 门后 | 骰点 <= 力量  检定：掷骰和另一个数值比
#     正文里写 {骰点}，显示时替换成当前值
#
#  ── 循环 ──────────────────────────────────────────────────
#     节点可以指回前面的节点，数值会继承下来，用变量当计数器就能做
#     "同样一段重复几遍"的剧情（详细说明见 README）
#
#  ── 封装节点（回合制战斗那种）──────────────────────────────
#     在节点里写 @行动，按钮点了就在本节点内执行，不会在流程图上多出分支；
#     @回合 的内容每个回合自动跑（敌人反击之类）；@显示 决定侧栏露什么；
#     -> 目标 | 条件 是这个节点的出口。完整能跑的例子见 README「封装节点」一节。
#
#  ── 数组 / 信息窗 / 功能按键 ────────────────────────────────
#     @数组 背包 = 面包, 3          数组可混放文字和数字
#     @加 / @删 / @删位 / @插 / @清空 / @数量 背包 -> 件数 / @查找 背包 面包 -> 位置
#     条件写「背包 含 面包」；正文写 {背包} 或 {背包[0]}
#     @信息 物品栏                  信息窗（写在节点外=全篇通用）
#     @按键 我的背包 -> 物品栏       节点上方的小按键：开信息窗
#     @按键 去商店 -> 商店           按键也可以直接跳节点
#
#  ── 选项反馈 / 注释 ────────────────────────────────────────
#     > 认真学习 -> 教室 | 学力 +1
#     @反馈 你认真听了一节课，学力涨到了 {学力}。   ← 选中后先弹这一页，点确认再走
#     # 井号开头的整行是注释；指令后面留空格写 # 说明 是行尾注释（正文里不算注释）
#
#  ── 画面卡（黑屏大字 / 全屏图 / 渐显）──────────────────────
#     @画面 打字                 开一张画面卡（打字=逐字浮现；写「直接」就一下全显示）
#     @画面 十月三日             后面每条 @画面 都是大字的一行，字号按字数自动定
#     @画面 七日轮回
#     @画面 -> 开场             放完跳到「开场」；不写 -> 就放完继续本节点
#     @画面 图片 3              改用 images/档案名/3.png 全屏铺满（图上也带底部提示）
#     @画面 图片 3 -> 走廊       图片卡放完跳转
#     只写 @画面 -> 目标 就行，不用再补一条 ->（卡片写了 -> 就一定跳走，本节点正文不再显示）
#     一个节点一张卡（要连续两张就拆成两个节点用 -> 串起来）；
#     黑屏淡入、底部自动显示 [点击屏幕或按任意键继续]，任意键先补完打字再翻页；
#     大字里也能用 {数值} / {数组}。
#
#  写完点右下角「流程图 Tab」，就能看到自己走出来的路线图：
#  同一个节点被正常剧情走进过好几次时，卡片会叠起来（右下角 ×N），鼠标停上去微微散开，
#  点一下就把每一次进入摊成一排（第几次 / 从哪来 / 当时的数值），选一张就回到那一次的状态，
#  Esc 退回流程图。这些自动记录，剧本里不用写；每个节点最多留最近 12 次。
# ============================================================

@标题 我的新故事
@开始 开场

@数值 精神值 = 50

:: 开场
@摘要 开场：站在门口
# @图片 1 小窗        ← 想配图就把这行前面的 # 去掉
这里写开场白，一行就是一段，太长了会自动折行。
> 选项一 -> 节点甲 | 精神值 +10
> 选项二 -> 节点乙 | 精神值 -10

:: 节点甲
@摘要 选了选项一之后
@场景 第一章 ｜ 名字
走到这里说明玩家选了选项一，精神值也涨过了。
# @骰 骰点 = 1d100              ← 想加随机因素就把这几行的 # 去掉
# > 硬闯进去 -> 结尾 | 骰点 <= 60
-> 结尾 | 精神值 >= 55
-> 节点乙

:: 节点乙
@摘要 选了选项二之后
玩家选了选项二。没有选项也没有 -> 的节点会自动显示
「再玩一次 / 选择其他档案 / 退出游戏」。

:: 结尾
@摘要 最后一个节点
想写的都写完之后，用 @结局 收尾。
@结局 1 结局名字 | 这里写一句收尾的话。
)SCN";

/* ==================== 全局状态 ==================== */

static wstring g_root, g_storiesDir, g_imagesDir;
static wstring g_version;  // 打包时写进 exe 旁边的 版本.txt，用来在界面上显示版本号

struct ArchiveEntry {
    wstring path, base, title;
    int problemCount = 0;
    int pngCount = 0;
    int otherCount = 0;
    bool lastPlayed = false;   // 上次在玩的就是它
    wstring lastNode;          // 上次停在哪个节点
    int visitedNodes = 0;      // 已经走过多少个节点
};
static vector<ArchiveEntry> g_archives;

enum class Screen { Archives, Story, Flowchart, Settings, About };

/* ---------- 窗口大小预设 + 设置（存在 exe 旁边的 设置.txt） ---------- */
struct SizePreset {
    const wchar_t* label;
    int w, h;    // 物理像素；w = 0 表示全屏（最大化）
    bool full;
};
static const SizePreset kSizePresets[] = {
    {L"全屏（占满屏幕，默认）", 0, 0, true},
    {L"1920 × 1080（16:9）", 1920, 1080, false},
    {L"1600 × 900（16:9）", 1600, 900, false},
    {L"1440 × 810（16:9）", 1440, 810, false},
    {L"1280 × 720（16:9）", 1280, 720, false},
    {L"1600 × 1000（16:10）", 1600, 1000, false},
    {L"1280 × 800（16:10）", 1280, 800, false},
    {L"1024 × 576（16:9 小窗）", 1024, 576, false},
};
static const int kSizeCount = (int)(sizeof(kSizePresets) / sizeof(kSizePresets[0]));
static int g_sizeIndex = 0;   // 0 = 全屏
static int g_speedIndex = 1;  // 打字速度：0 慢 / 1 正常 / 2 快 / 3 瞬间
static wstring g_settingsPath;
static Screen g_settingsFrom = Screen::Archives;
static int g_dispDpi = 96;    // 系统本身的显示缩放

static int typingCps() {  // 每秒浮现多少字
    switch (g_speedIndex) {
        case 0: return 45;
        case 2: return 140;
        case 3: return 100000;
        default: return 80;
    }
}

static void saveSettings() {
    if (g_settingsPath.empty()) return;
    wstring out = L"# 愚见者的故事书 · 设置（用记事本改也一样有效）\r\n";
    out += L"# size：窗口大小，对应设置界面里那一列的第几个（0 = 全屏）\r\n";
    out += L"# speed：打字速度  0 慢 / 1 正常 / 2 快 / 3 直接全显示\r\n";
    out += L"size " + std::to_wstring(g_sizeIndex) + L"\r\n";
    out += L"speed " + std::to_wstring(g_speedIndex) + L"\r\n";
    writeTextFile(g_settingsPath, out, true);
}

static void loadSettings() {
    if (g_settingsPath.empty()) return;
    wstring text;
    if (!readTextFile(g_settingsPath, text)) return;
    for (const wstring& raw : splitLines(text)) {
        const wstring line = trim(raw);
        if (line.empty() || line[0] == L'#') continue;
        if (startsWith(line, L"size ")) {
            int v = 0;
            if (parseInt(trim(line.substr(5)), v) && v >= 0 && v < kSizeCount) g_sizeIndex = v;
        } else if (startsWith(line, L"speed ")) {
            int v = 0;
            if (parseInt(trim(line.substr(6)), v) && v >= 0 && v <= 3) g_speedIndex = v;
        }
    }
}
static Screen g_screen = Screen::Archives;

/* ---------- 数值（运行时的值）、存档、流程图 ---------- */

struct VarVal {
    wstring name;
    int value = 0;
};

// 一次"从正常流程走进某个节点"的记录：流程图上的叠卡和展开面板都靠它
struct NodeEntry {
    int order = 0;        // 这是第几次进入这个节点（跨存档累计）
    wstring from;         // 这次是从哪个节点走进来的（开局 / 没有前置就是空的）
    vector<VarVal> vars;  // 走进来那一刻的全部数值
};
static const int kMaxNodeEntries = 12;  // 每个节点最多留这么多次进入记录（界面也摊不下更多）

struct StoryProgress {  // 一个故事的游玩进度（写进存档）
    wstring base;
    std::set<int> endings;                    // 解锁过的结局编号
    wstring currentNode;                      // 停在哪
    vector<VarVal> currentVars;               // 离开时的数值
    vector<StoryArray> arrays;                // 离开时各个数组里的内容
    std::set<wstring> visited;                // 走过的节点
    map<wstring, vector<VarVal>> nodeVars;    // 每个节点上次到达时的数值
    map<wstring, vector<NodeEntry>> entries;   // 每个节点每一次进入的记录（按先后）
    std::set<wstring> edges;                  // 走过的边，存 "起点\n终点"
    bool played = false;                      // 打过这个故事的标志
};

static vector<VarVal> g_vars;                        // 当前故事的数值
static vector<StoryArray> g_arrays;                  // 当前故事的数组
static map<wstring, ULONGLONG> g_varFlash;           // 刚变过的数值 -> 高亮到什么时候
static map<wstring, StoryProgress> g_progress;       // 所有故事的进度
static wstring g_lastStory;                          // 上次在玩哪个故事（文件名）
static wstring g_savePath;                           // 存档文件路径

/* 流程图（静态图 + 显示用状态） */
struct GraphEdge {
    int from = -1;
    int to = -1;
    wstring label;      // 选项文字（直接跳转是空的）
    bool dynamic = false;  // 带条件
};
static vector<GraphEdge> g_graphEdges;
static vector<int> g_graphDepth;  // 每个节点在第几层（BFS）

struct FlowCard {
    RECT rc{};
    int node = -1;
    bool ghost = false;  // 还没解锁的"接下来"
    wstring ghostName;
    wstring ghostLabel;
    int entries = 0;     // 这个节点被正常流程进入过几次（>=2 就画成叠卡）
};
static vector<FlowCard> g_flowCards;
static vector<FlowCard> g_flowPlan;  // 画布坐标下的卡片
static map<int, float> g_flowSpreadAnim;  // 卡片下标 -> 叠卡展开动画进度 0..1
static void scanArchives();
static void layoutSettings();
static void layoutAbout();

/* 展开面板：把一个节点每一次进入的记录摊开在屏幕上 */
static bool g_fanOpen = false;
static int g_fanNode = -1;
static wstring g_fanTitle;
static vector<NodeEntry> g_fanEntries;  // 打开时拷一份，跳转途中不怕被改
static vector<RECT> g_fanRects;         // 每张记录卡的矩形（屏幕坐标）
static RECT g_fanBox{};                 // 整个面板区域
static int g_fanHot = -1;
static void drawFlowFan(HDC dc);
static void layoutFlowFan();
static void closeFlowFan();
static int g_flowScrollX = 0, g_flowScrollY = 0;
static int g_flowCanvasW = 0, g_flowCanvasH = 0;
static RECT g_flowViewport{};

static Archive g_ar;
static int g_node = 0;
static vector<wstring> g_problems;
static wstring g_status;
static std::set<int> g_unlocked;

enum class LS { Scene, Body, Quote, Rule, EndingTitle };
struct DLine {
    wstring text;
    LS style = LS::Body;
    int offset = 0;
    int len = 0;
};
struct ImgMark {
    int at = 0;
    int no = 0;
    bool full = false;
};

static vector<DLine> g_lines;
static vector<ImgMark> g_marks;
static vector<Step> g_shownSteps;  // 面板上现在显示的是哪一段文字（节点正文 / 行动 / 回合）
static int g_totalChars = 0;
static bool g_typing = false;
static ULONGLONG g_typeStart = 0;
static int g_imgNo = 0;      // 当前显示的图片编号，0 = 不显示
static bool g_imgFull = false;

struct UiButton {
    wstring label;
    int kind = 0;  // 0 选项 / 1 继续 / 2 重开 / 3 档案列表 / 4 退出
    int arg = 0;
};
static vector<UiButton> g_uiButtons;

enum Action {
    A_None = 0,
    A_Restart,
    A_List,
    A_Flow,
    A_Back,
    A_Settings,
    A_About,
    A_CopyDoc,
    A_OpenStories,
    A_OpenImages,
    A_Quit
};
struct ToolBtn {
    RECT rc{};
    Action action = A_None;
    wstring label;
};
static vector<ToolBtn> g_tools;

/* 字体 */
static HFONT g_fTitle = nullptr, g_fScene = nullptr, g_fBody = nullptr, g_fQuote = nullptr,
             g_fEnding = nullptr, g_fButton = nullptr, g_fBadge = nullptr, g_fSmall = nullptr,
             g_fCardTitle = nullptr;

// 字号跟着界面缩放走，所以换窗口大小时要重做一遍
static void createFonts() {
    if (g_fTitle) DeleteObject(g_fTitle);
    if (g_fScene) DeleteObject(g_fScene);
    if (g_fBody) DeleteObject(g_fBody);
    if (g_fQuote) DeleteObject(g_fQuote);
    if (g_fEnding) DeleteObject(g_fEnding);
    if (g_fButton) DeleteObject(g_fButton);
    if (g_fBadge) DeleteObject(g_fBadge);
    if (g_fSmall) DeleteObject(g_fSmall);
    if (g_fCardTitle) DeleteObject(g_fCardTitle);
    g_fTitle = makeFont(23, FW_BOLD);
    g_fScene = makeFont(13, FW_BOLD);
    g_fBody = makeFont(12, FW_NORMAL);
    g_fQuote = makeFont(12, FW_NORMAL);
    g_fEnding = makeFont(17, FW_BOLD);
    g_fButton = makeFont(12, FW_NORMAL);
    g_fBadge = makeFont(10, FW_BOLD);
    g_fSmall = makeFont(9, FW_NORMAL);
    g_fCardTitle = makeFont(13, FW_BOLD);
}

/* ---------- 设置页（窗口大小 / 打字速度） ---------- */
static bool g_ddOpen = false;        // 窗口大小那个下拉展开着
static RECT g_ddButton{};            // 下拉按钮
static vector<RECT> g_ddItems;       // 展开后每个选项的矩形
static RECT g_setSizeRow{};          // 「窗口大小」这一行
static RECT g_setSpeedRow{};         // 「打字速度」这一行
static vector<RECT> g_speedPills;    // 慢 / 正常 / 快 / 瞬间
static const wchar_t* kSpeedNames[4] = {L"慢", L"正常", L"快", L"瞬间"};
/* 画刷 / 画笔 */
static HBRUSH g_brPanel = nullptr, g_brCard = nullptr, g_brBtn = nullptr, g_brBtnHot = nullptr,
               g_brBtnDown = nullptr, g_brBadge = nullptr;
static HBRUSH g_brScrollTrack = nullptr, g_brScrollThumb = nullptr, g_brScrollThumbHot = nullptr;
static HBRUSH g_brHpBar = nullptr, g_brHpLow = nullptr;
static HPEN g_penPanel = nullptr, g_penCard = nullptr, g_penBtn = nullptr, g_penBtnHot = nullptr,
             g_penRule = nullptr;

/* 布局 */
static HWND g_hwnd = nullptr;
static int g_cw = 0, g_ch = 0;
static HDC g_bgDC = nullptr;
static HBITMAP g_bgBmp = nullptr;
static int g_bgW = -1, g_bgH = -1;
static RECT g_rcHeader{}, g_rcContent{}, g_rcPanel{}, g_rcImage{}, g_rcHint{};
static RECT g_rcStats{};  // 侧栏数值面板（血条那一块）
static RECT g_rcFuncs{};           // 功能按键条
static RECT g_rcStatus{};          // 提示行（档案问题之类）
static vector<RECT> g_funcRects;   // 每个功能按键的位置
static bool g_infoOpen = false;    // 信息窗开着吗
static InfoBlock g_infoCurrent;    // 当前打开的那个信息窗（拷贝一份，避免下标失效）
static int g_infoScroll = 0;
static vector<wstring> g_infoLines;  // 信息窗里排好版的文字
static bool g_inFeedback = false;  // 正在看选项反馈（当成一个临时节点显示）
static int g_feedbackNext = -1;    // 点「确认」之后去哪个节点
static vector<Card> g_cardQueue;   // 画面卡：这次要依次放的卡
static int g_cardAt = -1;          // 正在放第几张（-1 = 没在放）
static ULONGLONG g_cardStart = 0;
static bool g_cardSkipType = false;
static const int kTimerCard = 4;
static const int kCardFadeMs = 420;  // 渐显时长
static const int kCardCps = 38;      // 打字速度
static int cardTypedChars();
static bool cardTypingDone();
static int g_buttonsTop = 0;
static vector<RECT> g_btnRect;
static vector<RECT> g_archiveCards;
static int g_archiveScroll = 0;
static int g_textScroll = 0;      // 剧情面板的滚动偏移（像素）
static bool g_autoScroll = true;  // 打字时自动向下跟随
static bool g_dragThumb = false;  // 正在拖动滚动条滑块
static int g_dragGrab = 0;        // 按下时抓在滑块的哪个位置
static int g_dragFlow = 0;        // 流程图滚动条拖动中：0 无 / 1 纵向 / 2 横向
static int g_dragFlowGrab = 0;

/* 鼠标状态 */
enum class HotKind { None, Choice, Archive, ArchiveDel, Tool, Scrollbar, FlowCard, FlowBar, Func,
                     TextWide, SettingBtn, SettingItem, AboutBtn };
static HotKind g_hotKind = HotKind::None;
static int g_hotIndex = -1;
static int g_downKind = -1;  // 0 按钮 / 1 档案卡 / 2 工具
static int g_downIndex = -1;
static bool g_trackingLeave = false;

static const UINT_PTR kTimerTyping = 1;

static wstring currentImageFolder() { return joinPath(g_imagesDir, g_ar.base); }

static wstring expandVars(const wstring& text);  // 前置声明（数组元素写入时就要结算）

/* ==================== 数值运行时 ==================== */

static const UINT_PTR kTimerUi = 2;  // 数值高亮之类的界面刷新
static const UINT_PTR kTimerFlowAnim = 3;  // 流程图叠卡展开 / 收起的动画
static const UINT_PTR kTimerMarquee = 5;   // 选项文字太长时的横向轮播
static int g_marqueeMs = 0;                // 轮播已经走了多少毫秒
static bool g_marqueeOn = false;           // 有没有哪条选项需要轮播
static RECT g_rcBtnsCol{};                 // 选项按钮占的那一列（右边可能让给图片）

static bool varValue(const wstring& name, int& out) {
    for (const VarVal& v : g_vars)
        if (v.name == name) {
            out = v.value;
            return true;
        }
    return false;
}

static void varSetRaw(const wstring& name, int value) {
    for (VarVal& v : g_vars)
        if (v.name == name) {
            v.value = value;
            return;
        }
}

static void startVarFlashTimer() {
    if (g_hwnd) SetTimer(g_hwnd, kTimerUi, 200, nullptr);
}

static StoryArray* findArray(const wstring& name);
static bool arrHas(const StoryArray* a, const wstring& elem);

static bool evalCond(const Condition& c) {
    if (c.contains) return arrHas(findArray(c.var), c.rhsVar);  // 背包 含 面包
    int v = 0;
    if (!varValue(c.var, v)) return true;  // 没声明的数值不拦路
    int rhs = c.value;
    if (!c.rhsVar.empty()) {
        int rv = 0;
        if (!varValue(c.rhsVar, rv)) return true;
        rhs = rv;
    }
    switch (c.op) {
        case 0: return v == rhs;
        case 1: return v != rhs;
        case 2: return v > rhs;
        case 3: return v >= rhs;
        case 4: return v < rhs;
        case 5: return v <= rhs;
    }
    return true;
}

static bool evalConds(const vector<Condition>& cs) {
    for (const Condition& c : cs)
        if (!evalCond(c)) return false;
    return true;
}

static void applyEffects(const vector<Effect>& es) {
    bool changed = false;
    for (const Effect& e : es) {
        int v = 0;
        if (!varValue(e.var, v)) continue;
        int amount = e.value;
        if (!e.dice.empty())
            amount = rollDiceText(e.dice);
        else if (!e.fromVar.empty())
            varValue(e.fromVar, amount);  // 用另一个数值当量（比如"伤害"）
        const int nv = (e.kind == 1) ? v + amount : (e.kind == 2 ? v - amount : amount);
        if (nv == v) continue;
        varSetRaw(e.var, nv);
        g_varFlash[e.var] = GetTickCount64() + 1500;  // 变过的数值会亮一下
        changed = true;
    }
    if (changed) startVarFlashTimer();
}

static vector<VarVal> freshVars() {
    vector<VarVal> out;
    for (const VarDef& d : g_ar.vars)
        out.push_back({d.name, d.dice.empty() ? d.initial : rollDiceText(d.dice)});
    return out;
}

/* ==================== 数组运行时 ==================== */

static StoryArray* findArray(const wstring& name) {
    for (StoryArray& a : g_arrays)
        if (a.name == name) return &a;
    return nullptr;
}

static ArrElem makeArrElem(const wstring& raw) {
    ArrElem e;
    e.text = trim(raw);
    int v = 0;
    if (parseIntLoose(e.text, v)) {
        e.numeric = true;
        e.num = v;
    }
    return e;
}

static bool arrHas(const StoryArray* a, const wstring& elem) {
    if (!a) return false;
    const wstring key = trim(elem);
    for (const ArrElem& e : a->items)
        if (e.text == key) return true;
    return false;
}

static void execArrayOp(const ArrayOp& op) {
    StoryArray* a = findArray(op.arr);
    if (!a) return;
    switch (op.kind) {
        case ArrayOp::Add:
            a->items.push_back(makeArrElem(expandVars(op.elem)));  // {伤害} 在这里就结算好
            break;
        case ArrayOp::Remove: {
            const wstring key = trim(expandVars(op.elem));
            for (auto it = a->items.begin(); it != a->items.end(); ++it)
                if (it->text == key) {
                    a->items.erase(it);
                    break;
                }
            break;
        }
        case ArrayOp::RemoveAt:
            if (op.index >= 0 && op.index < (int)a->items.size())
                a->items.erase(a->items.begin() + op.index);
            break;
        case ArrayOp::Insert: {
            const int at = (std::max)(0, (std::min)(op.index, (int)a->items.size()));
            a->items.insert(a->items.begin() + at, makeArrElem(expandVars(op.elem)));
            break;
        }
        case ArrayOp::Clear:
            a->items.clear();
            break;
        case ArrayOp::CountTo:
            if (!op.dest.empty()) varSetRaw(op.dest, (int)a->items.size());
            break;
        case ArrayOp::FindTo: {
            if (op.dest.empty()) break;
            const wstring key = trim(expandVars(op.elem));
            int found = -1;
            for (size_t i = 0; i < a->items.size(); ++i)
                if (a->items[i].text == key) {
                    found = (int)i;
                    break;
                }
            varSetRaw(op.dest, found);
            break;
        }
        default:
            break;
    }
}

static vector<StoryArray> freshArrays() {
    vector<StoryArray> out;
    for (const ArrayDef& d : g_ar.arrays) out.push_back({d.name, d.initial});
    return out;
}

/* ==================== 节点里的语句 ==================== */

static void applyRoll(const DiceRoll& r) {
    int old = 0;
    if (!varValue(r.var, old)) return;
    varSetRaw(r.var, rollDiceText(r.expr));
    g_varFlash[r.var] = GetTickCount64() + 1800;
    startVarFlashTimer();
}

// 按书写顺序执行：改数值 / 掷骰 / 动数组
static void execStmts(const vector<Stmt>& list) {
    for (const Stmt& st : list) {
        if (!evalConds(st.conds)) continue;  // 带条件的语句，不满足就跳过
        if (st.kind == Stmt::EffectK)
            applyEffects(st.effects);
        else if (st.kind == Stmt::RollK)
            applyRoll(st.roll);
        else if (st.kind == Stmt::ArrayK)
            execArrayOp(st.op);
    }
}

/* 当前节点现在能走哪几条路（按当前数值判断） */
struct LiveEdge {
    wstring label;
    wstring target;
    bool isJump = false;
};

static vector<LiveEdge> liveEdgesFromCurrent() {
    vector<LiveEdge> out;
    if (g_ar.nodes.empty() || g_node < 0 || g_node >= (int)g_ar.nodes.size()) return out;
    const Node& n = g_ar.nodes[g_node];
    for (const Choice& c : n.choices)
        if (evalConds(c.conds)) out.push_back({c.text, c.target, false});
    if (n.choices.empty()) {
        for (const JumpTarget& j : n.jumps)
            if (evalConds(j.conds)) {
                out.push_back({L"继续", j.target, true});
                break;
            }
    }
    return out;
}

/* ==================== 存档 ==================== */

static wstring varsToString(const vector<VarVal>& vs) {
    wstring s;
    for (const VarVal& v : vs) {
        if (!s.empty()) s += L",";
        s += v.name + L"=" + std::to_wstring(v.value);
    }
    return s;
}

static void varsFromString(const wstring& text, vector<VarVal>& out) {
    vector<wstring> items;
    splitClauses(text, items);
    for (const wstring& it : items) {
        const size_t eq = it.find(L'=');
        if (eq == wstring::npos || eq == 0) continue;
        VarVal v;
        v.name = trim(it.substr(0, eq));
        v.value = _wtoi(trim(it.substr(eq + 1)).c_str());
        if (!v.name.empty()) out.push_back(v);
    }
}

// 用 | 把一行切开（节点名 / 故事名里不要写 |）
static vector<wstring> splitFields(const wstring& line) {
    vector<wstring> out;
    wstring cur;
    for (wchar_t c : line) {
        if (c == L'|') {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trim(cur));
    return out;
}

static void loadSave() {
    g_progress.clear();
    g_lastStory.clear();
    wstring text;
    if (!readTextFile(g_savePath, text)) return;

    for (const wstring& raw : splitLines(text)) {
        const wstring line = trim(raw);
        if (line.empty() || line[0] == L'#') continue;
        const vector<wstring> f = splitFields(line);
        if (f.empty()) continue;

        if (startsWith(f[0], L"last ")) {
            g_lastStory = trim(f[0].substr(5));
        } else if (startsWith(f[0], L"story ") && f.size() >= 1) {
            const wstring base = trim(f[0].substr(6));
            if (base.empty()) continue;
            StoryProgress& p = g_progress[base];
            p.base = base;
            p.played = true;
            for (size_t i = 1; i < f.size(); ++i) {
                if (startsWith(f[i], L"endings ")) {
                    vector<wstring> nums;
                    splitClauses(f[i].substr(8), nums);
                    for (const wstring& n : nums) {
                        int v = 0;
                        if (parseInt(n, v)) p.endings.insert(v);
                    }
                } else if (startsWith(f[i], L"current ")) {
                    p.currentNode = trim(f[i].substr(8));
                } else if (startsWith(f[i], L"vars ")) {
                    varsFromString(f[i].substr(5), p.currentVars);
                }
            }
        } else if (startsWith(f[0], L"node ") && f.size() >= 2) {
            const wstring base = trim(f[0].substr(5));
            if (base.empty()) continue;
            StoryProgress& p = g_progress[base];
            p.base = base;
            p.visited.insert(f[1]);
            if (f.size() >= 3 && startsWith(f[2], L"vars ")) {
                vector<VarVal> vs;
                varsFromString(f[2].substr(5), vs);
                p.nodeVars[f[1]] = vs;
            }
        } else if (startsWith(f[0], L"enter ") && f.size() >= 2) {
            // enter <故事> | <节点> | n 3 | from 上个节点 | vars 天数=2,精神=61
            const wstring base = trim(f[0].substr(6));
            if (base.empty()) continue;
            StoryProgress& p = g_progress[base];
            p.base = base;
            NodeEntry en;
            bool hasOrder = false;
            for (size_t i = 2; i < f.size(); ++i) {
                if (startsWith(f[i], L"n ")) {
                    int v = 0;
                    if (parseInt(trim(f[i].substr(2)), v)) hasOrder = true;
                    en.order = v;
                } else if (startsWith(f[i], L"from ")) {
                    en.from = trim(f[i].substr(5));
                } else if (startsWith(f[i], L"vars ")) {
                    varsFromString(f[i].substr(5), en.vars);
                }
            }
            vector<NodeEntry>& list = p.entries[f[1]];
            if (!hasOrder) en.order = list.empty() ? 1 : (list.back().order + 1);
            list.push_back(en);
        } else if (startsWith(f[0], L"edge ") && f.size() >= 2) {
        } else if (startsWith(f[0], L"edge ") && f.size() >= 2) {
            const wstring base = trim(f[0].substr(5));
            if (base.empty()) continue;
            StoryProgress& p = g_progress[base];
            p.base = base;
            const size_t ar = f[1].find(L" -> ");
            if (ar != wstring::npos)
                p.edges.insert(trim(f[1].substr(0, ar)) + L"\n" + trim(f[1].substr(ar + 4)));
        } else if (startsWith(f[0], L"arr ") && f.size() >= 2) {
            const wstring base = trim(f[0].substr(4));
            if (base.empty()) continue;
            StoryArray a;
            a.name = f[1];
            if (f.size() >= 3 && !f[2].empty()) {
                for (const wstring& raw : splitEscaped(f[2])) {
                    const wstring it = trim(unescapeElem(raw));
                    if (it.empty()) continue;
                    ArrElem e;
                    e.text = it;
                    int v = 0;
                    if (parseIntLoose(e.text, v)) {
                        e.numeric = true;
                        e.num = v;
                    }
                    a.items.push_back(e);
                }
            }
            g_progress[base].arrays.push_back(a);
        }
    }
}

static void saveGame() {
    if (g_savePath.empty()) return;
    // 把当前正在玩的故事的运行时状态并进进度里
    if (!g_ar.base.empty() && !g_ar.nodes.empty()) {
        StoryProgress& cur = g_progress[g_ar.base];
        cur.base = g_ar.base;
        cur.played = true;
        for (int e : g_unlocked) cur.endings.insert(e);
        cur.arrays = g_arrays;  // 数组的当前内容
    }
    wstring out = L"# 愚见者的故事书 · 存档\r\n";
    out += L"# 程序每走一步都会自动更新这里；删掉这个文件就等于全部从头再来。\r\n";
    out += L"save 1\r\n";
    if (!g_lastStory.empty()) out += L"last " + g_lastStory + L"\r\n";

    for (const auto& kv : g_progress) {
        const StoryProgress& p = kv.second;
        wstring e;
        for (int n : p.endings) {
            if (!e.empty()) e += L",";
            e += std::to_wstring(n);
        }
        wstring line = L"story " + p.base + L" | endings " + (e.empty() ? L"-" : e);
        if (!p.currentNode.empty()) line += L" | current " + p.currentNode;
        if (!p.currentVars.empty()) line += L" | vars " + varsToString(p.currentVars);
        out += line + L"\r\n";

        for (const wstring& node : p.visited) {
            wstring nl = L"node " + p.base + L" | " + node;
            auto it = p.nodeVars.find(node);
            if (it != p.nodeVars.end() && !it->second.empty())
                nl += L" | vars " + varsToString(it->second);
            out += nl + L"\r\n";
        }
        for (const wstring& ed : p.edges) {
            const size_t cut = ed.find(L'\n');
            if (cut == wstring::npos) continue;
            out += L"edge " + p.base + L" | " + ed.substr(0, cut) + L" -> " + ed.substr(cut + 1) +
                   L"\r\n";
        }
        for (const auto& kv : p.entries) {
            for (const NodeEntry& en : kv.second) {
                wstring line = L"enter " + p.base + L" | " + kv.first + L" | n " +
                               std::to_wstring(en.order);
                if (!en.from.empty()) line += L" | from " + en.from;
                if (!en.vars.empty()) line += L" | vars " + varsToString(en.vars);
                out += line + L"\r\n";
            }
        }
        for (const StoryArray& a : p.arrays) {
            wstring items;
            for (const ArrElem& e : a.items) {
                if (!items.empty()) items += L",";
                items += escapeElem(e.text);
            }
            out += L"arr " + p.base + L" | " + a.name + L" | " + items + L"\r\n";
        }
    }
    writeTextFile(g_savePath, out, true);
}

static StoryProgress& progressNow() {
    StoryProgress& p = g_progress[g_ar.base];
    p.base = g_ar.base;
    p.played = true;
    return p;
}

/* ==================== 流程图数据 ==================== */

// 按剧本生成整张图（含没走过的边），再 BFS 算出每个节点在第几层
static void buildGraph() {
    g_graphEdges.clear();
    g_graphDepth.assign(g_ar.nodes.size(), -1);

    for (size_t i = 0; i < g_ar.nodes.size(); ++i) {
        for (const Choice& c : g_ar.nodes[i].choices) {
            const int t = nodeIndexOf(g_ar, c.target);
            if (t >= 0) g_graphEdges.push_back({(int)i, t, c.text, !c.conds.empty()});
        }
        for (const JumpTarget& j : g_ar.nodes[i].jumps) {
            const int t = nodeIndexOf(g_ar, j.target);
            if (t >= 0) g_graphEdges.push_back({(int)i, t, L"", !j.conds.empty()});
        }
    }

    if (g_ar.startIndex < 0 || g_ar.startIndex >= (int)g_ar.nodes.size()) return;
    vector<int> queue_;
    g_graphDepth[g_ar.startIndex] = 0;
    queue_.push_back(g_ar.startIndex);
    for (size_t head = 0; head < queue_.size(); ++head) {
        const int u = queue_[head];
        for (const GraphEdge& e : g_graphEdges)
            if (e.from == u && g_graphDepth[e.to] < 0) {
                g_graphDepth[e.to] = g_graphDepth[u] + 1;
                queue_.push_back(e.to);
            }
    }
}

/* ==================== 折行 ==================== */

static bool isWordChar(wchar_t c) {
    return c < 128 && (iswalnum((wint_t)c) != 0 || c == L'\'' || c == L'-');
}

static void wrapInto(const wstring& text, HFONT f, int maxWidth, vector<wstring>& out) {
    wstring line;
    for (wchar_t c : text) {
        if (!line.empty() && measure(line + c, f) > maxWidth) {
            size_t cut = line.size();
            if (isWordChar(c) && isWordChar(line.back())) {
                size_t k = line.size();
                while (k > 0 && isWordChar(line[k - 1])) --k;
                if (k > 0) cut = k;  // 整个单词挪到下一行
            }
            out.push_back(line.substr(0, cut));
            line = line.substr(cut);
        }
        line += c;
    }
    if (!line.empty()) out.push_back(line);
}

static HFONT fontOf(LS s) {
    switch (s) {
        case LS::Scene: return g_fScene;
        case LS::Quote: return g_fQuote;
        case LS::EndingTitle: return g_fEnding;
        default: return g_fBody;
    }
}

static COLORREF colorOf(LS s) {
    switch (s) {
        case LS::Scene: return Theme::ACCENT;
        case LS::Quote: return Theme::QUOTE;
        case LS::EndingTitle: return Theme::AMBER;
        default: return Theme::TEXT;
    }
}

static int lineHeightOf(LS s) {
    switch (s) {
        case LS::Scene: return S(28);
        case LS::EndingTitle: return S(36);
        case LS::Rule: return S(12);
        default: return S(26);
    }
}

static int gapBetween(bool hasPrev, LS prev, LS cur) {
    if (!hasPrev) return 0;
    if (cur == LS::Rule || prev == LS::Rule) return S(20);
    if (prev == LS::Body && cur == LS::Body) return 0;
    return S(13);
}

// 正文里可以写 {骰点} / {精神值}，显示时替换成当前的值
static wstring expandVars(const wstring& text) {
    if (text.find(L'{') == wstring::npos) return text;
    wstring out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] != L'{') {
            out += text[i++];
            continue;
        }
        const size_t close = text.find(L'}', i + 1);
        if (close == wstring::npos) {
            out += text.substr(i);
            break;
        }
        const wstring inside = trim(text.substr(i + 1, close - i - 1));
        bool done = false;
        const size_t br = inside.find(L'[');
        if (br != wstring::npos && !inside.empty() && inside.back() == L']') {
            // {背包[0]}：取数组里的第几个
            const wstring arrName = trim(inside.substr(0, br));
            const wstring idxText = trim(inside.substr(br + 1, inside.size() - br - 2));
            int idx = 0;
            const StoryArray* a = findArray(arrName);
            if (a && parseIntLoose(idxText, idx) && idx >= 0 && idx < (int)a->items.size()) {
                out += a->items[idx].text;
                done = true;
            }
        } else if (!inside.empty()) {
            int v = 0;
            if (varValue(inside, v)) {
                out += std::to_wstring(v);
                done = true;
            } else if (const StoryArray* a = findArray(inside)) {
                // {背包}：把整个数组列出来
                for (size_t k = 0; k < a->items.size(); ++k) {
                    if (k) out += L"、";
                    out += a->items[k].text;
                }
                done = true;
            }
        }
        if (!done) out += text.substr(i, close - i + 1);  // 没这个数值就原样留着
        i = close + 1;
    }
    return out;
}

static void buildLinesFrom(const vector<Step>& steps, const Node& n) {
    g_lines.clear();
    g_marks.clear();
    const int padX = S(30);
    const int maxW = (std::max)(S(120), (int)(g_rcPanel.right - g_rcPanel.left) - padX * 2);
    int cursor = 0;

    for (const Step& st : steps) {
        if (st.kind == Step::Image) {
            ImgMark m;
            m.at = cursor;
            m.no = st.imageNo;
            m.full = st.fullscreen;
            g_marks.push_back(m);
            continue;
        }
        vector<wstring> ls;
        const LS style = (st.kind == Step::Scene) ? LS::Scene : LS::Body;
        wrapInto(expandVars(st.text), fontOf(style), maxW, ls);
        for (const wstring& l : ls) g_lines.push_back({l, style, 0, 0});
    }

    if (n.isEnding) {
        g_lines.push_back({L"", LS::Rule, 0, 0});
        wstring title = L"【 结 局 ";
        title += (n.endingNo > 0) ? std::to_wstring(n.endingNo) : L"?";
        title += L" / " + std::to_wstring(g_ar.endingCount) + L" 】 " + n.endingTitle;
        g_lines.push_back({title, LS::EndingTitle, 0, 0});
        if (!n.endingQuote.empty()) {
            vector<wstring> qs;
            wrapInto(expandVars(n.endingQuote), g_fQuote, maxW, qs);
            for (const wstring& l : qs) g_lines.push_back({l, LS::Quote, 0, 0});
        }
    }

    g_totalChars = 0;
    for (DLine& dl : g_lines) {
        dl.offset = g_totalChars;
        dl.len = (int)dl.text.size();
        g_totalChars += dl.len;
    }
}

// 面板上现在显示的是哪一段文字（节点正文 / 某个行动 / 行动+回合）
static void buildCurrentLines() {
    if (g_ar.nodes.empty()) return;
    buildLinesFrom(g_shownSteps, g_ar.nodes[g_node]);
}

static int contentHeight() {
    int h = 0;
    bool hasPrev = false;
    LS prevStyle = LS::Body;
    for (const DLine& dl : g_lines) {
        h += gapBetween(hasPrev, prevStyle, dl.style) + lineHeightOf(dl.style);
        hasPrev = true;
        prevStyle = dl.style;
    }
    return h;
}

static int revealedChars() {
    if (!g_typing) return g_totalChars;
    // 文本很长时自动加快，免得等太久（最多 8 秒左右浮现完）
    int cps = typingCps();  // 设置页里能调
    if (g_totalChars > cps * 8) cps = g_totalChars / 8;
    const ULONGLONG ms = GetTickCount64() - g_typeStart;
    const int n = (int)(ms * cps / 1000);
    return (std::min)(n, g_totalChars);
}

// 按当前进度更新图片状态：图片会一直保留，直到剧本里出现下一个 @图片
static void applyImageMarkers(int revealed) {
    for (const ImgMark& m : g_marks)
        if (m.at <= revealed) {
            g_imgNo = m.no;
            g_imgFull = m.full;
        }
}

/* ---------- 剧情文字滚动 ---------- */

static int storyPadY() { return S(22); }
// 正文框顶部给「展开 / 收起」按钮留的一条带子，正文从它下面开始
static int storyTopBand() { return S(34); }
static int storyPadTop() { return storyPadY() + storyTopBand(); }

static int storyViewportHeight() {
    return (std::max)(0, (int)(g_rcPanel.bottom - g_rcPanel.top) - storyPadTop() - storyPadY());
}

static int maxTextScroll() {
    return (std::max)(0, contentHeight() - storyViewportHeight());
}

// 已经浮现出来的文字有多高（用来决定自动滚到哪儿）
static int revealedTextBottom() {
    const int revealed = revealedChars();
    int h = 0;
    bool hasPrev = false;
    LS prevStyle = LS::Body;
    for (const DLine& dl : g_lines) {
        if (dl.len > 0 && revealed <= dl.offset) break;  // 这行还没开始显示
        h += gapBetween(hasPrev, prevStyle, dl.style) + lineHeightOf(dl.style);
        hasPrev = true;
        prevStyle = dl.style;
        if (dl.len > 0 && revealed < dl.offset + dl.len) break;  // 正显示到一半
    }
    return h;
}

// 打字时自动向下滚动；没在打字就只把偏移夹回合法范围
static void updateAutoScroll() {
    const int maxScroll = maxTextScroll();
    if (!g_typing || !g_autoScroll) {
        g_textScroll = (std::max)(0, (std::min)(g_textScroll, maxScroll));
        return;
    }
    const int viewport = storyViewportHeight();
    const int bottom = revealedTextBottom();
    g_textScroll = (bottom > viewport) ? (std::min)(bottom - viewport, maxScroll) : 0;
}

struct ScrollInfo {
    bool visible = false;
    int maxScroll = 0;
    RECT track{}, thumb{};
};

static ScrollInfo scrollInfo() {
    ScrollInfo si;
    si.maxScroll = maxTextScroll();
    if (si.maxScroll <= 0) return si;

    const int padY = storyPadY();
    si.track = {g_rcPanel.right - S(22), g_rcPanel.top + padY, g_rcPanel.right - S(9),
                g_rcPanel.bottom - padY};
    const int trackH = (int)(si.track.bottom - si.track.top);
    if (trackH <= 0) return si;

    const int viewport = storyViewportHeight();
    const int content = (std::max)(1, contentHeight());
    int thumbH = (std::max)(S(36), trackH * viewport / content);
    thumbH = (std::min)(thumbH, trackH);
    const int travel = trackH - thumbH;
    const int thumbTop = si.track.top + travel * g_textScroll / (std::max)(1, si.maxScroll);
    si.thumb = {si.track.left, thumbTop, si.track.right, thumbTop + thumbH};
    si.visible = true;
    return si;
}

static void scrollTextBy(int delta) {
    g_autoScroll = false;
    g_textScroll = (std::max)(0, (std::min)(g_textScroll + delta, maxTextScroll()));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
/* ==================== 布局 ==================== */

static void rebuildBackground() {
    if (g_bgDC && g_bgW == g_cw && g_bgH == g_ch) return;
    if (g_bgDC) {
        DeleteDC(g_bgDC);
        g_bgDC = nullptr;
    }
    if (g_bgBmp) {
        DeleteObject(g_bgBmp);
        g_bgBmp = nullptr;
    }
    g_bgW = g_bgH = -1;
    if (g_cw <= 0 || g_ch <= 0) return;

    g_bgDC = CreateCompatibleDC(nullptr);
    g_bgBmp = CreateCompatibleBitmap(g_bgDC, g_cw, g_ch);
    HGDIOBJ old = SelectObject(g_bgDC, g_bgBmp);
    const int steps = (std::max)(1, g_ch - 1);
    for (int y = 0; y < g_ch; ++y) {
        const double t = (double)y / steps;
        auto mix = [&](int a, int b) { return (int)(a + (b - a) * t + 0.5); };
        const COLORREF c = RGB(mix(GetRValue(Theme::BG_TOP), GetRValue(Theme::BG_BOTTOM)),
                               mix(GetGValue(Theme::BG_TOP), GetGValue(Theme::BG_BOTTOM)),
                               mix(GetBValue(Theme::BG_TOP), GetBValue(Theme::BG_BOTTOM)));
        HBRUSH br = CreateSolidBrush(c);
        RECT r{0, y, g_cw, y + 1};
        FillRect(g_bgDC, &r, br);
        DeleteObject(br);
    }
    SelectObject(g_bgDC, old);
    g_bgW = g_cw;
    g_bgH = g_ch;
}

static int buttonsAreaHeight(int count) {
    if (count <= 0) count = 1;
    const int slotH = (count <= 3) ? S(40) : (count <= 5 ? S(32) : S(27));
    const int gap = (count <= 3) ? S(10) : S(8);
    return count * slotH + (count - 1) * gap;
}

static void layoutButtons() {
    g_btnRect.clear();
    const int n = (int)g_uiButtons.size();
    g_marqueeOn = false;
    if (n <= 0) {
        if (g_hwnd) KillTimer(g_hwnd, kTimerMarquee);
        return;
    }
    const int margin = S(26);
    const int gap = (n <= 3) ? S(10) : S(8);
    const int slotH = (n <= 3) ? S(40) : (n <= 5 ? S(32) : S(27));
    // 选项只占一列（图片可以在它右边），不再从头横跨到窗口右边
    const int left = (g_rcBtnsCol.right > g_rcBtnsCol.left) ? (int)g_rcBtnsCol.left : margin;
    const int right = (g_rcBtnsCol.right > g_rcBtnsCol.left) ? (int)g_rcBtnsCol.right
                                                             : (int)g_cw - margin;
    for (int i = 0; i < n; ++i) {
        const int top = g_buttonsTop + i * (slotH + gap);
        g_btnRect.push_back({left, top, right, top + slotH});
        const int textW = right - S(64) - (left + S(14) + S(28) + S(14));
        if (i < (int)g_uiButtons.size() && measure(g_uiButtons[i].label, g_fButton) > textW)
            g_marqueeOn = true;  // 装不下 → 让它横向轮播
    }
    if (g_hwnd) {
        if (g_marqueeOn) SetTimer(g_hwnd, kTimerMarquee, 40, nullptr);
        else KillTimer(g_hwnd, kTimerMarquee);
    }
}

static void layoutTools() {
    g_tools.clear();
    vector<std::pair<Action, wstring>> items;
    if (g_screen == Screen::Story) {
        items = {{A_Flow, L"流程图 Tab"},
                 {A_Restart, L"重开"},
                 {A_Settings, L"设置"},
                 {A_List, L"档案列表"}};
    } else if (g_screen == Screen::Flowchart) {
        items = {{A_Back, L"返回故事 Tab"}, {A_Settings, L"设置"}, {A_List, L"档案列表"}};
    } else if (g_screen == Screen::Settings) {
        items = {{A_Back, L"返回"}};
    } else if (g_screen == Screen::About) {
        items = {{A_Back, L"返回"}};
    } else {
        items = {{A_About, L"关于本游戏 & 剧本添加引导"},
                 {A_Settings, L"设置"},
                 {A_Quit, L"退出"}};
    }
    const int h = S(22), pad = S(9);
    int total = 0;
    for (auto& it : items) total += measure(it.second, g_fSmall) + pad * 2 + S(4);

    const int y = (g_rcHint.top + g_rcHint.bottom - h) / 2;
    int x = (std::max)(g_rcHint.left, g_rcHint.right - total);
    for (auto& it : items) {
        const int w = measure(it.second, g_fSmall) + pad * 2;
        ToolBtn t;
        t.rc = {x, y, x + w, y + h};
        t.action = it.first;
        t.label = it.second;
        g_tools.push_back(t);
        x += w + S(4);
    }
}

static wstring statusLine();  // 前置声明（下面用得到）

static void layoutArchiveCards() {
    g_archiveCards.clear();
    const int padX = S(22), cardH = S(66), gap = S(12);
    const int x0 = g_rcPanel.left + padX;
    const int w = (g_rcPanel.right - g_rcPanel.left) - padX * 2;
    int y = g_rcPanel.top + S(20) - g_archiveScroll;
    for (size_t i = 0; i < g_archives.size(); ++i) {
        g_archiveCards.push_back({x0, y, x0 + w, y + cardH});
        y += cardH + gap;
    }
}

static int archiveListContentHeight() {
    const int cardH = S(66), gap = S(12);
    return (int)g_archives.size() * (cardH + gap) + S(20) * 2 - gap;
}

/* ==================== 侧栏数值面板 ==================== */

static const vector<DisplayItem>& currentDisps() {
    static const vector<DisplayItem> empty;
    if (g_ar.nodes.empty() || g_node < 0 || g_node >= (int)g_ar.nodes.size()) return empty;
    return g_ar.nodes[g_node].disps;
}

static const vector<FuncButton>& currentFuncs() {
    static const vector<FuncButton> empty;
    if (g_ar.nodes.empty() || g_node < 0 || g_node >= (int)g_ar.nodes.size()) return empty;
    return g_ar.nodes[g_node].funcs;
}

static const vector<InfoBlock>& currentInfos() {
    static const vector<InfoBlock> empty;
    if (g_ar.nodes.empty() || g_node < 0 || g_node >= (int)g_ar.nodes.size()) return empty;
    return g_ar.nodes[g_node].infos;
}

// 信息窗的位置：屏幕正中一块
static RECT infoRect() {
    const int w = (std::min)(S(640), (int)g_cw - S(120));
    const int h = (std::min)(S(430), (int)g_ch - S(160));
    return {(g_cw - w) / 2, (g_ch - h) / 2, (g_cw + w) / 2, (g_ch + h) / 2};
}

static int statsItemHeight(const DisplayItem& d) { return d.bar ? S(30) : S(24); }

// 正文框展开 / 收起：展开时正文框顶到窗顶，能看到更多字
static bool g_textWide = false;
static RECT textWideRect() {  // 按钮就挂在正文框的右上角（偏上的位置）
    const int w = S(104), h = S(26);  // 正文框上方的正中间
    const int cx = (int)((g_rcPanel.left + g_rcPanel.right) / 2);
    return {cx - w / 2, g_rcPanel.top + S(5), cx + w / 2, g_rcPanel.top + S(5) + h};
}

static int statsPanelHeight() {
    const vector<DisplayItem>& d = currentDisps();
    if (d.empty()) return 0;
    int h = S(10) * 2;
    for (size_t i = 0; i < d.size(); ++i) {
        h += statsItemHeight(d[i]);
        if (i + 1 < d.size()) h += S(6);
    }
    return h;
}


/* ==================== 流程图排版 ==================== */

static const StoryProgress* progressFor(const wstring& base) {
    auto it = g_progress.find(base);
    return (it == g_progress.end()) ? nullptr : &it->second;
}

static bool nodeVisited(const wstring& name) {
    const StoryProgress* p = progressFor(g_ar.base);
    return p && p->visited.count(name) > 0;
}

// 某个节点的全部进入记录（没记过就返回 nullptr）
static const vector<NodeEntry>* entriesFor(const wstring& node) {
    const StoryProgress* p = progressFor(g_ar.base);
    if (!p) return nullptr;
    auto it = p->entries.find(node);
    if (it == p->entries.end() || it->second.empty()) return nullptr;
    return &it->second;
}

static int entryCountFor(const wstring& node) {
    const vector<NodeEntry>* l = entriesFor(node);
    return l ? (int)l->size() : 0;
}

static bool edgeTraversed(const wstring& from, const wstring& to) {
    const StoryProgress* p = progressFor(g_ar.base);
    return p && p->edges.count(from + L"\n" + to) > 0;
}

static std::set<wstring> g_liveTargets;  // 当前节点此刻能去的节点

static int flowViewportW() { return (int)(g_flowViewport.right - g_flowViewport.left); }
static int flowViewportH() { return (int)(g_flowViewport.bottom - g_flowViewport.top); }
static int flowMaxScrollX() {
    return (std::max)(0, g_flowCanvasW - flowViewportW() + S(24));
}
static int flowMaxScrollY() {
    return (std::max)(0, g_flowCanvasH - flowViewportH() + S(16));
}

// 把要展示的节点排成"一层一行"的图（画布坐标），并记下当前能走的路
static void buildFlowPlan() {
    g_flowPlan.clear();
    g_flowCanvasW = g_flowCanvasH = 0;
    g_liveTargets.clear();
    if (g_ar.nodes.empty()) return;

    const vector<LiveEdge> live = liveEdgesFromCurrent();
    for (const LiveEdge& le : live) g_liveTargets.insert(le.target);

    map<int, vector<FlowCard>> rows;
    for (size_t i = 0; i < g_ar.nodes.size(); ++i) {
        if (!nodeVisited(g_ar.nodes[i].name)) continue;
        int d = (i < g_graphDepth.size()) ? g_graphDepth[i] : -1;
        if (d < 0) d = 0;
        FlowCard c;
        c.node = (int)i;
        c.entries = entryCountFor(g_ar.nodes[i].name);
        rows[d].push_back(c);
    }

    // 当前节点"接下来"指向的、还没解锁的节点：画成虚线幽灵卡
    std::set<wstring> ghostDone;
    for (const LiveEdge& le : live) {
        if (le.target.empty() || nodeVisited(le.target) || ghostDone.count(le.target)) continue;
        const int t = nodeIndexOf(g_ar, le.target);
        if (t < 0) continue;
        ghostDone.insert(le.target);
        int d = (t < (int)g_graphDepth.size() && g_graphDepth[t] >= 0) ? g_graphDepth[t] : 0;
        FlowCard c;
        c.ghost = true;
        c.ghostName = le.target;
        c.ghostLabel = le.label;
        rows[d].push_back(c);
    }

    const int cw = S(200), chh = S(88), gapX = S(26), gapY = S(50);
    int y = 0, maxW = 0;
    for (auto& kv : rows) {
        int x = 0;
        for (FlowCard& c : kv.second) {
            c.rc = {x, y, x + cw, y + chh};
            x += cw + gapX;
        }
        if (x > 0) maxW = (std::max)(maxW, x - gapX);
        y += chh + gapY;
    }
    g_flowCanvasW = maxW;
    g_flowCanvasH = (y > 0) ? y - gapY : 0;

    for (auto& kv : rows)
        for (const FlowCard& c : kv.second) g_flowPlan.push_back(c);
}

// 画布坐标 -> 屏幕坐标（按当前滚动位置）
static void layoutFlowCards() {
    g_flowCards.clear();
    const int vw = flowViewportW();
    const int padX = (g_flowCanvasW < vw) ? (vw - g_flowCanvasW) / 2 : S(10);
    const int padY = S(8);
    for (const FlowCard& c : g_flowPlan) {
        FlowCard s = c;
        const int x = (int)c.rc.left + padX - g_flowScrollX;
        const int y = (int)c.rc.top + padY - g_flowScrollY;
        const int w = (int)(c.rc.right - c.rc.left), h = (int)(c.rc.bottom - c.rc.top);
        s.rc = {g_flowViewport.left + x, g_flowViewport.top + y, g_flowViewport.left + x + w,
                g_flowViewport.top + y + h};
        g_flowCards.push_back(s);
    }
}

static const FlowCard* flowCardForNode(int node) {
    for (const FlowCard& c : g_flowCards)
        if (!c.ghost && c.node == node) return &c;
    return nullptr;
}

static const FlowCard* flowCardForGhost(const wstring& name) {
    for (const FlowCard& c : g_flowCards)
        if (c.ghost && c.ghostName == name) return &c;
    return nullptr;
}

static void computeLayout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_cw = rc.right;
    g_ch = rc.bottom;

    const int margin = S(26), headerH = S(76), hintH = S(26);
    g_rcHeader = {margin, margin, g_cw - margin, margin + headerH};
    g_rcHint = {margin, g_ch - margin - hintH, g_cw - margin, g_ch - margin};

    const int statusH = statusLine().empty() ? 0 : S(24);
    g_rcStatus = {margin + S(9), g_rcHeader.bottom + S(2), g_cw - margin,
                  g_rcHeader.bottom + S(2) + (statusH > 0 ? S(20) : 0)};
    int contentTop = g_rcHeader.bottom + S(12) + statusH;

    // 功能按键条（@按键 写了的节点才有）
    g_funcRects.clear();
    g_rcFuncs = {};
    if (g_screen == Screen::Story) {
        // @按键 现在排在左侧栏里（下面统一排），这里只清一下
        g_funcRects.clear();
        g_rcFuncs = {};
    }

    if (g_screen == Screen::Story) {
        const int n = (int)g_uiButtons.size();
        g_buttonsTop = g_rcHint.top - S(14) - buttonsAreaHeight(n);
        g_rcContent = {margin, contentTop, g_cw - margin, g_buttonsTop - S(16)};

        g_rcImage = {};
        g_rcStats = {};
        g_rcFuncs = {};
        g_rcBtnsCol = {};
        g_funcRects.clear();
        const bool showStats = !g_ar.nodes.empty() && g_node >= 0 &&
                               g_node < (int)g_ar.nodes.size() &&
                               !g_ar.nodes[g_node].disps.empty();
        const vector<FuncButton>& funcs = currentFuncs();
        const int areaTop = (int)g_rcContent.top;
        const int areaBottom = (int)g_rcContent.bottom;
        const int areaH = areaBottom - areaTop;
        const int contentW = (int)g_rcContent.right - margin;

        /* ---- 下半区：选项靠左（变窄），右边一块放信息（数值 / 数组 / @按键）---- */
        const int optW = (std::min)(S(460), (int)((double)contentW * 0.30));
        const int optH = buttonsAreaHeight(n);
        int infoH = showStats ? statsPanelHeight() : 0;
        const int funcRowH = funcs.empty() ? 0 : S(32);
        if (funcRowH > 0) infoH += funcRowH + S(10);
        int lowerH = (std::max)(optH, infoH);
        lowerH = (std::min)(lowerH, (std::max)(S(90), areaH * 42 / 100));
        const int lowerTop = areaBottom - lowerH;

        g_rcBtnsCol = {margin, lowerTop, margin + optW, lowerTop + optH};
        g_buttonsTop = lowerTop;

        const int infoLeft = margin + optW + S(20);
        if (infoLeft + S(160) < (int)g_cw - margin) {  // 右边放得下才放
            int y = lowerTop;
            if (funcRowH > 0) {  // @按键 横着排一行
                int x = infoLeft;
                for (size_t fi = 0; fi < funcs.size(); ++fi) {
                    const int w = measure(funcs[fi].label, g_fSmall) + S(26);
                    if (x + w > (int)g_cw - margin) break;
                    g_funcRects.push_back({x, y, x + w, y + funcRowH});
                    x += w + S(8);
                }
                if (!g_funcRects.empty()) {
                    g_rcFuncs = {infoLeft, y, g_funcRects.back().right, y + funcRowH};
                    y += funcRowH + S(10);
                }
            }
            if (showStats) {
                const int sw = (std::min)(S(560), (int)g_cw - margin - infoLeft);
                const int sh = (std::min)(statsPanelHeight(), areaBottom - y);
                g_rcStats = {infoLeft, y, infoLeft + sw, y + sh};
            }
        }

        /* ---- 上半区：正文在左，图片在右（按原图比例，占满能占的地方）---- */
        const int upperTop = areaTop;
        const int upperBottom = lowerTop - S(16);
        const int upperH = upperBottom - upperTop;
        const int imgColW = (std::min)(S(820), (int)((double)contentW * 0.40));
        const bool smallImg = (g_imgNo > 0 && !g_imgFull) && !g_textWide && upperH >= S(180) &&
                              contentW - imgColW - S(20) >= S(420);
        if (smallImg) {
            const int textRight = (int)g_cw - margin - imgColW - S(20);
            g_rcPanel = {margin, upperTop, textRight, upperBottom};
            // 画框就按图片自己的比例算，不留黑边
            Gdiplus::Image* img = loadImage(findImagePath(currentImageFolder(), g_imgNo));
            double ratio = 16.0 / 9.0;
            if (img && img->GetWidth() > 0 && img->GetHeight() > 0)
                ratio = (double)img->GetWidth() / (double)img->GetHeight();
            const int colLeft = textRight + S(20);
            const int colW = (int)g_cw - margin - colLeft;
            int w = colW, h = (int)((double)colW / ratio);
            if (h > upperH) {
                h = upperH;
                w = (int)((double)upperH * ratio);
            }
            const int cx = colLeft + (colW - w) / 2;
            g_rcImage = {cx, upperTop + (upperH - h) / 2, cx + w, upperTop + (upperH + h) / 2};
        } else {
            g_rcPanel = {margin, upperTop, (int)g_cw - margin, upperBottom};
        }
        if (g_textWide) {  // 展开：正文吃掉图片那一列，顶到窗口最上面；下半区的信息栏照旧
            g_rcImage = {};
            g_rcPanel = {margin, S(12), (int)g_cw - margin, upperBottom};
        }
        g_rcContent = {margin, areaTop, (int)g_rcContent.right, areaBottom};
        layoutButtons();
    } else if (g_screen == Screen::Flowchart) {
        g_buttonsTop = 0;
        g_rcImage = {};
        g_rcContent = {margin, contentTop, g_cw - margin, g_rcHint.top - S(16)};
        g_rcPanel = g_rcContent;
        g_flowViewport = {g_rcContent.left + S(12), g_rcContent.top + S(12),
                          g_rcContent.right - S(12) - S(18), g_rcContent.bottom - S(12) - S(16)};
        buildFlowPlan();
        g_flowScrollX = (std::max)(0, (std::min)(g_flowScrollX, flowMaxScrollX()));
        g_flowScrollY = (std::max)(0, (std::min)(g_flowScrollY, flowMaxScrollY()));
        layoutFlowCards();
    } else if (g_screen == Screen::Settings) {
        g_buttonsTop = 0;
        g_rcImage = {};
        g_rcContent = {margin, contentTop, g_cw - margin, g_rcHint.top - S(16)};
        g_rcPanel = g_rcContent;
        layoutSettings();
    } else if (g_screen == Screen::About) {
        g_buttonsTop = 0;
        g_rcImage = {};
        g_rcContent = {margin, contentTop, g_cw - margin, g_rcHint.top - S(16)};
        g_rcPanel = g_rcContent;
        layoutAbout();
    } else {
        g_buttonsTop = 0;
        g_rcImage = {};
        g_rcContent = {margin, contentTop, g_cw - margin, g_rcHint.top - S(16)};
        g_rcPanel = {margin, contentTop, g_cw - margin, g_rcHint.top - S(16)};
    }

    rebuildBackground();
    layoutTools();
    if (g_screen == Screen::Archives) {
        const int inner = (g_rcPanel.bottom - g_rcPanel.top) - S(20) * 2;
        const int maxScroll = (std::max)(0, archiveListContentHeight() - S(20) * 2 - inner);
        g_archiveScroll = (std::max)(0, (std::min)(g_archiveScroll, maxScroll));
        layoutArchiveCards();
    }
    if (g_fanOpen) layoutFlowFan();  // 展开面板跟着窗口大小重排
}

// 排一次完整的版：先算面板宽度 → 折行 → 应用图片指令 → 再算一次
static void relayout() {
    computeLayout();
    if (g_screen == Screen::Story && !g_ar.nodes.empty()) {
        buildCurrentLines();
        applyImageMarkers(revealedChars());
        computeLayout();
        updateAutoScroll();
    }
}

// 窗口尺寸变了：按"设计稿 940 × 760"重算界面缩放（字号跟着走），排版才始终装得下
static void applyUiScale(int cw, int ch) {
    if (cw <= 0 || ch <= 0) return;
    int d = g_dispDpi;
    const int byW = (int)((double)cw * 96.0 / 940.0);
    const int byH = (int)((double)ch * 96.0 / 760.0);
    if (byW < d) d = byW;
    if (byH < d) d = byH;
    if (d < 72) d = 72;
    if (d == g_dpi) return;
    g_dpi = d;
    createFonts();
    g_flowSpreadAnim.clear();
}

static wstring statusLine() {
    vector<wstring> parts;
    if (!g_status.empty()) parts.push_back(g_status);
    if (g_screen == Screen::Story && !g_problems.empty()) {
        wstring s = L"⚠ " + g_problems[0];
        if (g_problems.size() > 1)
            s += L"（共 " + std::to_wstring(g_problems.size()) + L" 处，详见 " + g_ar.base +
                 L".issues.log）";
        parts.push_back(s);
    }
    wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += L"　｜　";
        out += parts[i];
    }
    return out;
}

static bool statusLineIsWarning() {
    return g_screen == Screen::Story && !g_problems.empty();
}

/* ==================== 绘制 ==================== */

struct BrushPenGuard {
    HDC dc;
    HGDIOBJ oldBrush, oldPen;
    BrushPenGuard(HDC d, HBRUSH b, HPEN p) : dc(d) {
        oldBrush = SelectObject(dc, b);
        oldPen = SelectObject(dc, p);
    }
    ~BrushPenGuard() {
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
    }
};

static void drawRoundedRect(HDC dc, const RECT& r, int radius, HBRUSH brush, HPEN pen) {
    BrushPenGuard guard(dc, brush, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
}

static void drawRoundedRectOutline(HDC dc, const RECT& r, int radius, HPEN pen) {
    BrushPenGuard guard(dc, (HBRUSH)GetStockObject(NULL_BRUSH), pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
}

static void drawHeader(HDC dc) {
    RECT t1 = g_rcHeader;
    t1.left += S(8);
    t1.bottom = t1.top + S(40);
    t1.right -= S(8);
    SelectObject(dc, g_fTitle);
    SetTextColor(dc, Theme::TEXT);
    const wstring title =
        (g_screen != Screen::Archives && !g_ar.title.empty()) ? g_ar.title
                                                              : wstring(L"愚 见 者 的 故 事 书");
    DrawTextW(dc, title.c_str(), -1, &t1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT t2 = g_rcHeader;
    t2.left += S(9);
    t2.top = t1.bottom - S(6);
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, Theme::TEXT_DIM);
    wstring sub;
    if (g_screen == Screen::Story) {
        sub = g_ar.base + L".txt";
        if (g_ar.endingCount > 0)
            sub += L"　｜　已解锁结局 " + std::to_wstring(g_unlocked.size()) + L" / " +
                   std::to_wstring(g_ar.endingCount);
    } else if (g_screen == Screen::Flowchart) {
        int visited = 0;
        for (const Node& n : g_ar.nodes)
            if (nodeVisited(n.name)) ++visited;
        sub = L"流程图 ｜ 已解锁节点 " + std::to_wstring(visited) + L" / " +
              std::to_wstring(g_ar.nodes.size()) + L"　｜　点已经走过的节点可以回溯";
    } else {
        sub = L"选择故事档案（共 " + std::to_wstring(g_archives.size()) +
              L" 个） ｜ 剧本放在 stories 文件夹";
        if (!g_version.empty()) sub += L" ｜ " + g_version;
    }
    DrawTextW(dc, sub.c_str(), -1, &t2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void drawStatusLine(HDC dc) {
    const wstring s = statusLine();
    if (s.empty()) return;
    RECT r = g_rcStatus;
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, statusLineIsWarning() ? Theme::WARN : Theme::ACCENT);
    DrawTextW(dc, s.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void drawStoryText(HDC dc) {
    const int padX = S(30), padY = storyPadY();
    const int revealed = revealedChars();

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, g_rcPanel.left + padX, g_rcPanel.top + storyPadTop(),
                      g_rcPanel.right - padX, g_rcPanel.bottom - padY);

    int y = g_rcPanel.top + storyPadTop() - g_textScroll;
    const int panelInner = storyViewportHeight();
    const int textH = contentHeight();
    // 展开时不居中：整块贴着顶部排，遇到"差一点放不下"就整体上移，别切掉半行
    if (!g_textWide && textH < panelInner) y += (panelInner - textH) / 2;
    const int innerBottom = g_rcPanel.bottom - padY;
    if (y + textH > innerBottom) y = innerBottom - textH;

    bool hasPrev = false, blocked = false;
    LS prevStyle = LS::Body;
    for (const DLine& dl : g_lines) {
        const int lh = lineHeightOf(dl.style);
        y += gapBetween(hasPrev, prevStyle, dl.style);
        hasPrev = true;
        prevStyle = dl.style;

        if (!blocked) {
            const int shown = revealed - dl.offset;
            const bool reached = (dl.len == 0) ? (shown >= 0) : (shown > 0);
            if (reached) {
                if (dl.style == LS::Rule) {
                    HPEN oldPen = (HPEN)SelectObject(dc, g_penRule);
                    const int mid = y + lh / 2;
                    MoveToEx(dc, g_rcPanel.left + padX, mid, nullptr);
                    LineTo(dc, g_rcPanel.right - padX, mid);
                    SelectObject(dc, oldPen);
                } else {
                    const wstring part =
                        (shown >= dl.len) ? dl.text : dl.text.substr(0, (size_t)shown);
                    HFONT oldFont = (HFONT)SelectObject(dc, fontOf(dl.style));
                    SetTextColor(dc, colorOf(dl.style));
                    TEXTMETRICW tm{};
                    GetTextMetricsW(dc, &tm);
                    TextOutW(dc, g_rcPanel.left + padX, y + (lh - tm.tmHeight) / 2, part.c_str(),
                             (int)part.size());
                    SelectObject(dc, oldFont);
                }
            } else {
                blocked = true;  // 后面的内容还没浮现
            }
        }
        y += lh;
    }
    RestoreDC(dc, saved);
}

static void drawMissingImage(HDC dc, const RECT& box, int no) {
    wstring tip = L"（图片缺失）";
    RECT t = box;
    t.bottom = (box.top + box.bottom) / 2 - S(2);
    SelectObject(dc, g_fButton);
    SetTextColor(dc, Theme::WARN);
    DrawTextW(dc, tip.c_str(), -1, &t, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);

    wstring path = std::to_wstring(no) + L".png";
    RECT t2 = box;
    t2.top = (box.top + box.bottom) / 2 + S(4);
    t2.left += S(20);
    t2.right -= S(20);
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, Theme::TEXT_DIM);
    DrawTextW(dc, path.c_str(), -1, &t2, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void drawImageCard(HDC dc) {
    if (g_rcImage.right <= g_rcImage.left) return;
    drawRoundedRect(dc, g_rcImage, S(16), g_brCard, g_penCard);

    const int inset = S(4);
    const RECT box{g_rcImage.left + inset, g_rcImage.top + inset, g_rcImage.right - inset,
                   g_rcImage.bottom - inset};
    Gdiplus::Image* img = loadImage(findImagePath(currentImageFolder(), g_imgNo));
    if (!img) {
        drawMissingImage(dc, box, g_imgNo);
        return;
    }
    const int saved = SaveDC(dc);
    HRGN rgn = CreateRoundRectRgn(box.left, box.top, box.right + 1, box.bottom + 1, S(14), S(14));
    ExtSelectClipRgn(dc, rgn, RGN_AND);
    drawImageIn(dc, box, img, false);
    RestoreDC(dc, saved);
    DeleteObject(rgn);
}

static void drawButtons(HDC dc, bool fullscreen) {
    for (size_t i = 0; i < g_btnRect.size() && i < g_uiButtons.size(); ++i) {
        const RECT base = g_btnRect[i];
        const bool hot = (g_hotKind == HotKind::Choice && g_hotIndex == (int)i);
        const bool down = (g_downKind == 0 && g_downIndex == (int)i);
        RECT r = base;
        if (down) OffsetRect(&r, 0, S(1));

        if (fullscreen) {
            const BYTE alpha = down ? 235 : (hot ? 225 : 205);
            blendFillRounded(dc, r, S(14), down ? Theme::BTN_DOWN : (hot ? Theme::BTN_HOT : Theme::BTN),
                             alpha);
            drawRoundedRectOutline(dc, r, S(14), hot ? g_penBtnHot : g_penBtn);
        } else {
            drawRoundedRect(dc, r, S(14), down ? g_brBtnDown : (hot ? g_brBtnHot : g_brBtn),
                            hot ? g_penBtnHot : g_penBtn);
        }

        const int badgeSize = (std::min)(S(28), (int)(r.bottom - r.top) - S(6));
        RECT badge{r.left + S(14), r.top + ((r.bottom - r.top) - badgeSize) / 2,
                   r.left + S(14) + badgeSize, r.top + ((r.bottom - r.top) - badgeSize) / 2 + badgeSize};
        if (fullscreen)
            blendFillRounded(dc, badge, S(14), Theme::BADGE, 210);
        else
            drawRoundedRect(dc, badge, S(14), g_brBadge, hot ? g_penBtnHot : g_penBtn);

        const wstring num = std::to_wstring(i + 1);
        SelectObject(dc, g_fBadge);
        SetTextColor(dc, hot ? Theme::ACCENT : Theme::TEXT_DIM);
        DrawTextW(dc, num.c_str(), -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT text{badge.right + S(14), r.top, r.right - S(40), r.bottom};
        SelectObject(dc, g_fButton);
        SetTextColor(dc, hot ? Theme::TEXT : RGB(214, 223, 236));
        const wstring& label = g_uiButtons[i].label;
        const int availW = (int)(text.right - text.left);
        const int needW = measure(label, g_fButton);
        if (needW <= availW) {
            DrawTextW(dc, label.c_str(), -1, &text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            // 装不下：来回横着走一圈（两边各停一下），保证整句都看得到
            const int over = needW - availW;
            const int cycle = 1600 + over * 18 + 900;  // 停 1.6s + 走完 + 停 0.9s
            const int phase = g_marqueeMs % (cycle * 2);
            int off = 0;
            if (phase < 1600) off = 0;
            else if (phase < 1600 + over * 18)
                off = (phase - 1600) / 18;
            else if (phase < cycle) off = over;
            else {
                const int p2 = phase - cycle;
                if (p2 < 900) off = over;
                else if (p2 < 900 + over * 18) off = over - (p2 - 900) / 18;
                else off = 0;
            }
            const int saved = SaveDC(dc);
            IntersectClipRect(dc, text.left, text.top, text.right, text.bottom);
            RECT t2 = text;
            t2.left -= off;
            t2.right = t2.left + needW + S(8);
            DrawTextW(dc, label.c_str(), -1, &t2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RestoreDC(dc, saved);
        }

        if (hot) {
            RECT arrow{r.right - S(34), r.top, r.right - S(16), r.bottom};
            SetTextColor(dc, Theme::ACCENT);
            DrawTextW(dc, L"\u25B8", -1, &arrow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

static void drawFooter(HDC dc) {
    SelectObject(dc, g_fSmall);

    RECT h = g_rcHint;
    if (!g_tools.empty()) h.right = g_tools.front().rc.left - S(10);
    SetTextColor(dc, Theme::HINT);
    const wstring hint = (g_screen == Screen::Story)
                             ? L"数字键选择 ｜ 空格跳过打字 / 翻页 ｜ Tab 流程图 ｜ Esc 退出"
                         : (g_screen == Screen::Flowchart)
                             ? L"滚轮上下 · Shift+滚轮左右 ｜ 点走过的节点回溯 ｜ Tab 返回"
                             : L"点一个故事开始 ｜ 鼠标滚轮翻页";
    DrawTextW(dc, hint.c_str(), -1, &h, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    for (size_t i = 0; i < g_tools.size(); ++i) {
        const bool hot = (g_hotKind == HotKind::Tool && g_hotIndex == (int)i);
        if (hot) blendFillRounded(dc, g_tools[i].rc, S(6), Theme::BTN_HOT, 235);
        RECT r = g_tools[i].rc;
        SetTextColor(dc, hot ? Theme::ACCENT : Theme::HINT);
        DrawTextW(dc, g_tools[i].label.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// 档案卡右下角的「删存档」按钮
static RECT delSaveRect(const RECT& card) {
    return {card.right - S(102), card.bottom - S(30), card.right - S(16), card.bottom - S(8)};
}

// 删掉某个档案的全部存档（先弹窗确认）
static void deleteArchiveSave(size_t i) {
    if (i >= g_archives.size()) return;
    const wstring base = g_archives[i].base;
    const wstring title = g_archives[i].title;
    const wstring msg = L"要删掉《" + title +
                        L"》的全部存档吗？\r\n\r\n"
                        L"结局收集、流程图进度、每次进入节点的数值、当前停在哪，都会被清空，"
                        L"而且找不回来。\r\n\r\n"
                        L"故事本身（stories\\" +
                        base + L".txt）和图片不受影响。";
    if (MessageBoxW(g_hwnd, msg.c_str(), L"删除存档", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;

    g_progress.erase(base);
    if (g_lastStory == base) g_lastStory.clear();
    if (g_ar.base == base) {  // 还在内存里的话一起清掉，免得下次自动存档又写回去
        g_ar = Archive();
        g_node = -1;
        g_uiButtons.clear();
        g_problems.clear();
        g_flowPlan.clear();
        g_flowCards.clear();
    }
    saveGame();
    scanArchives();
    g_status = L"已删除《" + title + L"》的存档";
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void drawArchiveCard(HDC dc, const RECT& r, const ArchiveEntry& e, bool hot, bool delHot,
                            bool hasSave) {
    drawRoundedRect(dc, r, S(14), hot ? g_brBtnHot : g_brCard, hot ? g_penBtnHot : g_penCard);

    RECT t{r.left + S(20), r.top + S(10), r.right - S(170), r.top + S(38)};
    SelectObject(dc, g_fCardTitle);
    SetTextColor(dc, Theme::TEXT);
    DrawTextW(dc, e.title.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    wstring sub = e.base + L".txt";
    const int images = e.pngCount + e.otherCount;
    if (images > 0) {
        sub += L"　｜　图片 " + std::to_wstring(images) + L" 张";
        if (e.otherCount > 0)
            sub += L"（其中 " + std::to_wstring(e.otherCount) + L" 张不是 png）";
    } else {
        sub += L"　｜　暂无图片";
    }
    RECT s{r.left + S(20), t.bottom - S(6), r.right - S(170), r.bottom - S(8)};
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, e.otherCount > 0 ? Theme::WARN : Theme::TEXT_DIM);
    DrawTextW(dc, sub.c_str(), -1, &s, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 有「删存档」按钮时，右边那行提示往上让一点，别压在一起
    RECT tail{r.right - S(170), r.top, r.right - S(18), r.bottom - (hasSave ? S(30) : 0)};
    if (e.problemCount > 0) {
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, Theme::WARN);
        DrawTextW(dc, (L"⚠ " + std::to_wstring(e.problemCount) + L" 处问题").c_str(), -1, &tail,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (e.lastPlayed) {  // 上次就在玩这个：提示可以从哪儿继续
        RECT t2 = tail;
        t2.bottom = (r.top + r.bottom) / 2 + S(1);
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, Theme::ACCENT);
        DrawTextW(dc, L"▶ 接着上次玩", -1, &t2,
                  DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT t3 = tail;
        t3.top = t2.bottom;
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, Theme::TEXT_DIM);
        DrawTextW(dc, e.lastNode.c_str(), -1, &t3,
                  DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (e.visitedNodes > 0) {
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, Theme::TEXT_DIM);
        DrawTextW(dc, (L"已走过 " + std::to_wstring(e.visitedNodes) + L" 个节点").c_str(), -1,
                  &tail, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (hot) {
        SelectObject(dc, g_fButton);
        SetTextColor(dc, Theme::ACCENT);
        DrawTextW(dc, L"开始 ▸", -1, &tail, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    if (hasSave) {  // 有存档才给这个按钮
        RECT dr = delSaveRect(r);
        drawRoundedRect(dc, dr, S(9), delHot ? g_brBtnHot : g_brBadge,
                        delHot ? g_penBtnHot : g_penBtn);
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, delHot ? Theme::WARN : Theme::HINT);
        DrawTextW(dc, L"删存档", -1, &dr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// 右侧滚动条：只有文字装不下的时候才出现
static void drawScrollBar(HDC dc) {
    const ScrollInfo si = scrollInfo();
    if (!si.visible) return;
    HPEN noPen = (HPEN)GetStockObject(NULL_PEN);
    drawRoundedRect(dc, si.track, S(7), g_brScrollTrack, noPen);
    const bool hot = g_dragThumb || g_hotKind == HotKind::Scrollbar;
    drawRoundedRect(dc, si.thumb, S(7), hot ? g_brScrollThumbHot : g_brScrollThumb, noPen);
}

// 侧栏：@显示 写了的数值 / 数组才画在这里（条状、列表、普通数值）
static void drawStatsPanel(HDC dc) {
    const vector<DisplayItem>& items = currentDisps();
    if (items.empty() || g_rcStats.right <= g_rcStats.left) return;

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, g_rcStats.left, g_rcStats.top, g_rcStats.right, g_rcStats.bottom);
    drawRoundedRect(dc, g_rcStats, S(14), g_brCard, g_penCard);
    HPEN noPen = (HPEN)GetStockObject(NULL_PEN);

    const int pad = S(14);
    int y = (int)g_rcStats.top + S(10);
    for (const DisplayItem& it : items) {
        const int ih = statsItemHeight(it);
        RECT row{g_rcStats.left + pad, y, g_rcStats.right - pad, y + ih};

        if (it.list) {  // 数组：名字：元素、元素
            wstring txt = it.name + L"：";
            const StoryArray* a = findArray(it.name);
            if (a) {
                if (a->items.empty())
                    txt += L"（空）";
                for (size_t i = 0; i < a->items.size(); ++i) {
                    if (i) txt += L"、";
                    txt += expandVars(a->items[i].text);
                }
            } else {
                txt += L"（没有这个数组）";
            }
            SelectObject(dc, g_fSmall);
            SetTextColor(dc, a ? Theme::TEXT : Theme::WARN);
            DrawTextW(dc, txt.c_str(), -1, &row,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += ih + S(6);
            continue;
        }

        int v = 0;
        const bool has = varValue(it.name, v);
        int maxV = 100;
        if (!it.maxSource.empty()) {
            int m = 0;
            if (parseIntLoose(it.maxSource, m))
                maxV = m;
            else if (varValue(it.maxSource, m))
                maxV = m;
            if (maxV <= 0) maxV = 100;
        }

        SelectObject(dc, g_fSmall);
        SetTextColor(dc, Theme::TEXT_DIM);
        wstring vtxt = has ? std::to_wstring(v) : wstring(L"?");
        if (it.bar) vtxt += L" / " + std::to_wstring(maxV);

        const int valW = S(70);
        RECT nameR = row;
        const int nameW = (std::min)(pad + measure(it.name, g_fSmall) + S(8),
                                     (int)(row.right - row.left) - valW - S(40));
        nameR.right = (int)nameR.left + (std::max)(S(30), nameW);
        DrawTextW(dc, it.name.c_str(), -1, &nameR,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT valR = row;
        valR.left = (int)row.right - valW;
        SelectObject(dc, g_fSmall);
        // 刚变过的数值亮一下
        bool flash = false;
        auto fit = g_varFlash.find(it.name);
        if (fit != g_varFlash.end() && GetTickCount64() < fit->second) flash = true;
        if (flash) {
            RECT glow = row;
            InflateRect(&glow, S(4), S(2));
            blendFillRounded(dc, glow, S(8), Theme::ACCENT, 55);
        }
        SetTextColor(dc, has ? (flash ? Theme::ACCENT : Theme::TEXT) : Theme::WARN);
        DrawTextW(dc, vtxt.c_str(), -1, &valR,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (it.bar) {  // 名字和数值中间夹一条
            const int bh = S(12);
            RECT slot{(int)nameR.right + S(6), (int)row.top + (ih - bh) / 2, (int)valR.left - S(6),
                      (int)row.top + (ih - bh) / 2 + bh};
            if (slot.right - slot.left >= S(20)) {
                drawRoundedRect(dc, slot, S(6), g_brScrollTrack, noPen);
                if (has && maxV > 0 && v > 0) {
                    const int clamped = (std::min)(v, maxV);
                    const int fillW = (int)((double)(slot.right - slot.left) * clamped / maxV);
                    if (fillW > 0) {
                        RECT fill = slot;
                        fill.right = (std::min)((LONG)(slot.left + fillW), slot.right);
                        if (fill.right - fill.left < S(6)) fill.right = fill.left + S(6);
                        const double ratio = (double)clamped / (double)maxV;
                        drawRoundedRect(dc, fill, S(6), ratio <= 0.3 ? g_brHpLow : g_brHpBar, noPen);
                    }
                }
            }
        }
        y += ih + S(6);
    }
    RestoreDC(dc, saved);
}

/* ---------- 流程图绘制 ---------- */

struct BarGeom {
    bool visible = false;
    RECT track{}, thumb{};
};

static BarGeom makeVBarGeom(const RECT& track, int scroll, int maxScroll, int viewport,
                            int content) {
    BarGeom g;
    const int trackLen = (int)(track.bottom - track.top);
    if (maxScroll <= 0 || trackLen <= 0) return g;
    g.visible = true;
    g.track = track;
    int thumbLen = trackLen * viewport / (std::max)(1, content);
    thumbLen = (std::max)(S(32), (std::min)(thumbLen, trackLen));
    const int travel = trackLen - thumbLen;
    const int top = (int)track.top + travel * scroll / (std::max)(1, maxScroll);
    g.thumb = {track.left, top, track.right, top + thumbLen};
    return g;
}

static BarGeom makeHBarGeom(const RECT& track, int scroll, int maxScroll, int viewport,
                            int content) {
    BarGeom g;
    const int trackLen = (int)(track.right - track.left);
    if (maxScroll <= 0 || trackLen <= 0) return g;
    g.visible = true;
    g.track = track;
    int thumbLen = trackLen * viewport / (std::max)(1, content);
    thumbLen = (std::max)(S(32), (std::min)(thumbLen, trackLen));
    const int travel = trackLen - thumbLen;
    const int left = (int)track.left + travel * scroll / (std::max)(1, maxScroll);
    g.thumb = {left, track.top, left + thumbLen, track.bottom};
    return g;
}

static BarGeom flowVBar() {
    const RECT track{g_flowViewport.right + S(2), g_flowViewport.top, g_flowViewport.right + S(12),
                     g_flowViewport.bottom};
    return makeVBarGeom(track, g_flowScrollY, flowMaxScrollY(), flowViewportH(),
                        (std::max)(1, g_flowCanvasH));
}

static BarGeom flowHBar() {
    const RECT track{g_flowViewport.left, g_flowViewport.bottom + S(4), g_flowViewport.right,
                     g_flowViewport.bottom + S(14)};
    return makeHBarGeom(track, g_flowScrollX, flowMaxScrollX(), flowViewportW(),
                        (std::max)(1, g_flowCanvasW));
}

static void drawBar(HDC dc, const BarGeom& g, bool hot) {
    if (!g.visible) return;
    HPEN noPen = (HPEN)GetStockObject(NULL_PEN);
    drawRoundedRect(dc, g.track, S(6), g_brScrollTrack, noPen);
    drawRoundedRect(dc, g.thumb, S(6), hot ? g_brScrollThumbHot : g_brScrollThumb, noPen);
}

// back = 这条边是"往回走"（循环），绕到卡片右侧上去，免得和正向边重叠成一条线
static void drawFlowEdge(HDC dc, const RECT& a, const RECT& b, bool live, bool traversed,
                         bool back, bool selfLoop) {
    const COLORREF col = live ? Theme::ACCENT : (traversed ? RGB(80, 106, 140) : Theme::RULE);
    HPEN pen = CreatePen(live ? PS_DOT : PS_SOLID, live ? 1 : (traversed ? 2 : 1), col);
    HGDIOBJ old = SelectObject(dc, pen);

    POINT tri[3];
    if (back || selfLoop) {
        const int ay = selfLoop ? (int)a.top + S(22) : (int)((a.top + a.bottom) / 2);
        const int by = selfLoop ? (int)a.bottom - S(22) : (int)((b.top + b.bottom) / 2);
        const int sideX = (std::max)((int)a.right, (int)b.right) + S(26);
        MoveToEx(dc, (int)a.right, ay, nullptr);
        LineTo(dc, sideX, ay);
        LineTo(dc, sideX, by);
        LineTo(dc, (int)b.right, by);
        tri[0] = {(int)b.right, by};
        tri[1] = {(int)b.right + S(8), by - S(5)};
        tri[2] = {(int)b.right + S(8), by + S(5)};
    } else {
        const int ax = (int)((a.left + a.right) / 2), ay = (int)a.bottom;
        const int bx = (int)((b.left + b.right) / 2), by = (int)b.top;
        const int midY = (ay + by) / 2;
        MoveToEx(dc, ax, ay, nullptr);
        LineTo(dc, ax, midY);
        LineTo(dc, bx, midY);
        LineTo(dc, bx, by);
        tri[0] = {bx, by};
        tri[1] = {bx - S(5), by - S(8)};
        tri[2] = {bx + S(5), by - S(8)};
    }
    SelectObject(dc, old);
    DeleteObject(pen);

    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, tri, 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(br);
}

static void drawFlowCard(HDC dc, const FlowCard& c, bool hot, int stackSpread) {
    const int pad = S(12);

    if (c.ghost) {  // 还没解锁的"接下来"
        blendFillRounded(dc, c.rc, S(14), Theme::PANEL, 170);
        HPEN pen = CreatePen(PS_DOT, 1, Theme::RULE);
        drawRoundedRectOutline(dc, c.rc, S(14), pen);
        DeleteObject(pen);
        RECT t1 = c.rc;
        t1.left += pad;
        t1.right -= pad;
        t1.bottom = (int)((c.rc.top + c.rc.bottom) / 2);
        SelectObject(dc, g_fBody);
        SetTextColor(dc, Theme::TEXT_DIM);
        DrawTextW(dc, L"？ 未解锁", -1, &t1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        RECT t2 = c.rc;
        t2.left += pad;
        t2.right -= pad;
        t2.top = t1.bottom + S(4);
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, Theme::HINT);
        DrawTextW(dc, c.ghostLabel.c_str(), -1, &t2,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    const Node& n = g_ar.nodes[c.node];
    const bool current = (c.node == g_node);

    // 被正常流程走进过好几次的节点：后面垫几张，画成"叠在一起"的样子
    const bool stacked = (c.entries >= 2);
    if (stacked) {
        const int layers = (std::min)(3, c.entries);  // 视觉上最多再垫 2 张
        for (int k = layers - 1; k >= 1; --k) {
            const int off = stackSpread * k / layers;
            RECT sr = c.rc;
            OffsetRect(&sr, off, off * 3 / 4);
            blendFillRounded(dc, sr, S(14), Theme::CARD, 170);
            drawRoundedRectOutline(dc, sr, S(14), g_penCard);
        }
    }
    if (current) {  // 当前节点先铺一层冷光
        RECT glow = c.rc;
        InflateRect(&glow, S(4), S(4));
        blendFillRounded(dc, glow, S(18), Theme::ACCENT, 45);
    }
    drawRoundedRect(dc, c.rc, S(14), hot ? g_brBtnHot : g_brCard,
                    (current || hot) ? g_penBtnHot : g_penCard);

    RECT nameR{c.rc.left + pad, c.rc.top + S(8), c.rc.right - pad, c.rc.top + S(26)};
    if (n.isEnding) nameR.right -= S(52);
    RECT sumR{c.rc.left + pad, nameR.bottom, c.rc.right - pad, c.rc.bottom - S(26)};
    RECT varR{c.rc.left + pad, c.rc.bottom - S(26), c.rc.right - pad, c.rc.bottom - S(8)};
    if (stacked) varR.right -= S(46);  // 给右下角的"×N"让位

    SelectObject(dc, g_fSmall);
    SetTextColor(dc, current ? Theme::ACCENT : Theme::HINT);
    const wstring nameText = (current ? L"▸ " : L"") + n.name;
    DrawTextW(dc, nameText.c_str(), -1, &nameR, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(dc, g_fBody);
    SetTextColor(dc, Theme::TEXT);
    const wstring body = n.summary.empty() ? n.name : n.summary;
    DrawTextW(dc, body.c_str(), -1, &sumR, DT_LEFT | DT_TOP | DT_WORDBREAK);

    wstring varText;  // 上次到这个节点时的数值
    const StoryProgress* p = progressFor(g_ar.base);
    if (p) {
        auto it = p->nodeVars.find(n.name);
        if (it != p->nodeVars.end()) {
            for (const VarVal& v : it->second) {
                if (!varText.empty()) varText += L" · ";
                varText += v.name + L" " + std::to_wstring(v.value);
            }
        }
    }
    if (varText.empty()) varText = L"—";
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, Theme::TEXT_DIM);
    DrawTextW(dc, varText.c_str(), -1, &varR, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (n.isEnding) {
        RECT badge{c.rc.right - S(58), c.rc.top + S(7), c.rc.right - S(8), c.rc.top + S(25)};
        drawRoundedRect(dc, badge, S(8), g_brBadge, g_penBtn);
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, Theme::AMBER);
        const wstring txt =
            (n.endingNo > 0) ? (L"结局 " + std::to_wstring(n.endingNo)) : wstring(L"结局");
        DrawTextW(dc, txt.c_str(), -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (stacked) {  // 进入次数
        RECT badge{c.rc.right - S(48), c.rc.bottom - S(26), c.rc.right - S(8), c.rc.bottom - S(8)};
        drawRoundedRect(dc, badge, S(8), g_brBadge, g_penBtn);
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, Theme::ACCENT);
        DrawTextW(dc, (L"×" + std::to_wstring(c.entries)).c_str(), -1, &badge,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// 节点上方的功能按键（@按键）
static void drawFuncRow(HDC dc) {
    const vector<FuncButton>& funcs = currentFuncs();
    for (size_t i = 0; i < g_funcRects.size() && i < funcs.size(); ++i) {
        const bool hot = (g_hotKind == HotKind::Func && g_hotIndex == (int)i);
        drawRoundedRect(dc, g_funcRects[i], S(10), hot ? g_brBtnHot : g_brBtn,
                        hot ? g_penBtnHot : g_penBtn);
        RECT r = g_funcRects[i];
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, hot ? Theme::ACCENT : Theme::TEXT_DIM);
        DrawTextW(dc, funcs[i].label.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// 信息展示窗（@信息 + @按键）

static void drawInfoWindow(HDC dc) {
    if (!g_infoOpen) return;
    const InfoBlock& ib = g_infoCurrent;
    blendFillRounded(dc, {0, 0, g_cw, g_ch}, 0, Theme::SCRIM, 165);  // 背景压暗

    const RECT r = infoRect();
    drawRoundedRect(dc, r, S(16), g_brPanel, g_penCard);

    RECT title{r.left + S(24), r.top + S(16), r.right - S(24), r.top + S(54)};
    SelectObject(dc, g_fCardTitle);
    SetTextColor(dc, Theme::ACCENT);
    DrawTextW(dc, ib.title.c_str(), -1, &title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const int lineTop = (int)title.bottom + S(6);
    const int lineBottom = (int)r.bottom - S(40);
    HPEN oldPen = (HPEN)SelectObject(dc, g_penRule);
    MoveToEx(dc, r.left + S(24), lineTop, nullptr);
    LineTo(dc, r.right - S(24), lineTop);
    SelectObject(dc, oldPen);

    const int lineH = S(26);
    const int viewH = lineBottom - lineTop - S(10);
    const int maxScroll = (std::max)(0, (int)g_infoLines.size() * lineH - viewH);
    g_infoScroll = (std::max)(0, (std::min)(g_infoScroll, maxScroll));

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, r.left + S(24), lineTop + S(6), r.right - S(24), lineBottom);
    SelectObject(dc, g_fBody);
    SetTextColor(dc, Theme::TEXT);
    int y = lineTop + S(6) - g_infoScroll;
    for (const wstring& l : g_infoLines) {
        TextOutW(dc, r.left + S(24), y, l.c_str(), (int)l.size());
        y += lineH;
    }
    RestoreDC(dc, saved);

    RECT hint{r.left + S(24), r.bottom - S(36), r.right - S(24), r.bottom - S(10)};
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, Theme::HINT);
    DrawTextW(dc, maxScroll > 0 ? L"滚轮翻页 ｜ 点一下 / Esc 关闭" : L"点一下 / Esc 关闭", -1, &hint,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void paintFlowchart(HDC dc) {
    if (g_bgDC) BitBlt(dc, 0, 0, g_cw, g_ch, g_bgDC, 0, 0, SRCCOPY);
    drawHeader(dc);
    drawStatusLine(dc);
    drawRoundedRect(dc, g_rcPanel, S(18), g_brPanel, g_penPanel);

    if (g_ar.nodes.empty()) {
        SelectObject(dc, g_fBody);
        SetTextColor(dc, Theme::TEXT_DIM);
        RECT t = g_rcPanel;
        DrawTextW(dc, L"这个档案里还没有节点。", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        drawFooter(dc);
        return;
    }

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, g_flowViewport.left, g_flowViewport.top, g_flowViewport.right,
                      g_flowViewport.bottom);

    for (const GraphEdge& e : g_graphEdges) {  // 先画线，再画卡片
        const FlowCard* ca = flowCardForNode(e.from);
        if (!ca) continue;
        const wstring toName = g_ar.nodes[e.to].name;
        const bool tr = edgeTraversed(g_ar.nodes[e.from].name, toName);
        const bool live = (e.from == g_node) && g_liveTargets.count(toName) > 0;
        if (!tr && !live) continue;
        const FlowCard* cb = flowCardForNode(e.to);
        if (!cb) cb = flowCardForGhost(toName);
        if (!cb) continue;
        const int dFrom = (e.from < (int)g_graphDepth.size()) ? g_graphDepth[e.from] : 0;
        const int dTo = (e.to < (int)g_graphDepth.size()) ? g_graphDepth[e.to] : 0;
        const bool selfLoop = (e.from == e.to);
        const bool back = !selfLoop && (dTo <= dFrom);
        drawFlowEdge(dc, ca->rc, cb->rc, live, tr, back, selfLoop);
    }

    for (size_t i = 0; i < g_flowCards.size(); ++i) {
        const bool hot = (g_hotKind == HotKind::FlowCard && g_hotIndex == (int)i);
        int spread = 0;
        if (g_flowCards[i].entries >= 2) {
            const auto it = g_flowSpreadAnim.find((int)i);
            const float t = (it == g_flowSpreadAnim.end()) ? 0.f : it->second;
            spread = S(6) + (int)(S(13) * t);  // 鼠标停上去：叠在一起的微微散开
        }
        drawFlowCard(dc, g_flowCards[i], hot, spread);
    }
    RestoreDC(dc, saved);

    drawBar(dc, flowVBar(), g_hotKind == HotKind::FlowBar && g_hotIndex == 1);
    drawBar(dc, flowHBar(), g_hotKind == HotKind::FlowBar && g_hotIndex == 2);
    drawFooter(dc);
    drawFlowFan(dc);  // 展开面板（开着才画）
}

static void paintStory(HDC dc) {
    if (g_cardAt >= 0 && g_cardAt < (int)g_cardQueue.size()) {  // 正在放画面卡
        const Card& c = g_cardQueue[g_cardAt];
        const RECT all{0, 0, g_cw, g_ch};
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &all, black);
        DeleteObject(black);
        if (c.imageNo > 0) {
            Gdiplus::Image* img = loadImage(findImagePath(currentImageFolder(), c.imageNo));
            if (img) drawImageIn(dc, all, img, true);
        }

        const int revealed = cardTypedChars();
        const int nLines = (std::max)(1, (int)c.lines.size());
        int maxChars = 1;
        for (const wstring& l : c.lines) maxChars = (std::max)(maxChars, (int)l.size());
        // 字号：字少时按"标题"的比例（不超过窗口高度的 12%），字多时自动缩小
        const int sizeW = (int)((double)g_cw * 0.66 / (double)maxChars);
        const int sizeH = (int)((double)(g_ch * 45 / 100) / (double)nLines / 1.35);
        const int sizeCap = g_ch * 12 / 100;
        const int fs = (std::max)(S(16), (std::min)((std::min)(sizeW, sizeH), sizeCap));
        HFONT f = CreateFontW(-fs, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(242, 246, 252));
        const int lineH = (int)(fs * 1.35);
        int y = (g_ch - lineH * nLines) / 2;
        int done = 0;
        for (const wstring& l : c.lines) {
            const wstring shown = expandVars(l);
            wstring part = shown;
            const int remain = revealed - done;
            if (c.typing && !g_cardSkipType && remain < (int)shown.size())
                part = shown.substr(0, (size_t)(std::max)(0, remain));
            RECT r{0, y, g_cw, y + lineH};
            DrawTextW(dc, part.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            done += (int)shown.size();
            y += lineH;
        }
        SelectObject(dc, oldF);
        DeleteObject(f);

        if (cardTypingDone()) {
            RECT hint{0, g_ch - S(96), g_cw, g_ch - S(48)};
            SelectObject(dc, g_fBody);
            SetTextColor(dc, RGB(188, 196, 210));
            DrawTextW(dc, L"[ 点击屏幕或按任意键继续 ]", -1, &hint,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        const ULONGLONG el = GetTickCount64() - g_cardStart;  // 渐显
        if (el < (ULONGLONG)kCardFadeMs) {
            const int alpha = 255 - (int)(el * 255 / kCardFadeMs);
            if (alpha > 0) blendFillRounded(dc, all, 0, RGB(0, 0, 0), (BYTE)alpha);
        }
        return;
    }
    const bool full = (g_imgFull && g_imgNo > 0);
    const RECT all{0, 0, g_cw, g_ch};
    const bool hasImage = full && findImagePath(currentImageFolder(), g_imgNo).size() > 0;

    if (hasImage) {
        drawImageIn(dc, all, loadImage(findImagePath(currentImageFolder(), g_imgNo)), true);
        blendFillRounded(dc, all, 0, Theme::SCRIM, 60);                    // 整体压暗一点
        blendFillRounded(dc, {0, 0, g_cw, S(130)}, 0, Theme::SCRIM, 110);  // 顶部更暗，标题清楚
        blendFillRounded(dc, {0, g_ch - S(96), g_cw, g_ch}, 0, Theme::SCRIM, 120);
    } else if (g_bgDC) {
        BitBlt(dc, 0, 0, g_cw, g_ch, g_bgDC, 0, 0, SRCCOPY);
    }

    drawHeader(dc);
    drawStatusLine(dc);
    drawStatsPanel(dc);
    drawImageCard(dc);

    if (full) {
        blendFillRounded(dc, g_rcPanel, S(18), Theme::PANEL, 208);  // 文字底框
        drawRoundedRectOutline(dc, g_rcPanel, S(18), g_penPanel);
    } else {
        drawRoundedRect(dc, g_rcPanel, S(18), g_brPanel, g_penPanel);
    }
    drawStoryText(dc);
    drawScrollBar(dc);
    {  // 正文框右上角的「展开 / 收起」
        RECT br = textWideRect();
        const bool hot = (g_hotKind == HotKind::TextWide);
        drawRoundedRect(dc, br, S(9), hot ? g_brBtnHot : g_brBadge, hot ? g_penBtnHot : g_penBtn);
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, hot ? Theme::ACCENT : Theme::TEXT_DIM);
        DrawTextW(dc, g_textWide ? L"▼ 收起" : L"▲ 展开", -1, &br,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    drawButtons(dc, full);
    drawFuncRow(dc);

    drawInfoWindow(dc);
    drawFooter(dc);
}

static void paintArchives(HDC dc) {
    if (g_bgDC) BitBlt(dc, 0, 0, g_cw, g_ch, g_bgDC, 0, 0, SRCCOPY);
    drawHeader(dc);
    drawStatusLine(dc);
    drawRoundedRect(dc, g_rcPanel, S(18), g_brPanel, g_penPanel);

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, g_rcPanel.left + S(6), g_rcPanel.top + S(6), g_rcPanel.right - S(6),
                      g_rcPanel.bottom - S(6));
    if (g_archives.empty()) {
        SelectObject(dc, g_fBody);
        SetTextColor(dc, Theme::TEXT_DIM);
        RECT t = g_rcPanel;
        t.top += S(40);
        DrawTextW(dc,
                  L"还没有故事。\r\n\r\n把剧本 .txt 放进 stories 文件夹，"
                  L"然后重新打开程序就能看到。",
                  -1, &t, DT_CENTER | DT_TOP | DT_WORDBREAK);
    } else {
        for (size_t i = 0; i < g_archiveCards.size(); ++i)
            drawArchiveCard(dc, g_archiveCards[i], g_archives[i],
                            g_hotKind == HotKind::Archive && g_hotIndex == (int)i,
                            g_hotKind == HotKind::ArchiveDel && g_hotIndex == (int)i,
                            progressFor(g_archives[i].base) != nullptr);
    }
    RestoreDC(dc, saved);
    drawFooter(dc);
}

/* ==================== 设置页 ==================== */

/* ==================== 关于 / 剧本添加引导 ==================== */

enum class DocStyle { H1, H2, H3, Body, Bullet, Code, Quote, Rule, Table };
struct DocLine {
    DocStyle style = DocStyle::Body;
    wstring text;
};
static vector<DocLine> g_docLines;      // README 解析出来的行
static vector<DocLine> g_docDrawn;      // 按当前宽度折好行、可以照着画的行
static int g_docContentH = 0;           // 折行之后总高度
static int g_docScroll = 0;
static int g_docTop = 0, g_docBottom = 0, g_docLeft = 0, g_docRight = 0;
static RECT g_aboutBtnCopy{}, g_aboutBtnStories{}, g_aboutBtnImages{}, g_aboutBtnBack{};
static wstring g_docPayload;            // 点「复制全文」时进剪贴板的整段文字
static wstring g_docTitle;

static HFONT docFont(DocStyle s) {
    switch (s) {
        case DocStyle::H1: return g_fTitle;
        case DocStyle::H2: return g_fCardTitle;
        case DocStyle::H3: return g_fScene;
        case DocStyle::Code: return g_fSmall;
        case DocStyle::Table: return g_fSmall;
        case DocStyle::Quote: return g_fQuote;
        default: return g_fBody;
    }
}
static int docLineH(DocStyle s) {
    switch (s) {
        case DocStyle::H1: return S(44);
        case DocStyle::H2: return S(32);
        case DocStyle::H3: return S(28);
        case DocStyle::Code: return S(22);
        case DocStyle::Table: return S(24);
        case DocStyle::Quote: return S(26);
        case DocStyle::Rule: return S(16);
        default: return S(27);
    }
}

// 把 README 那样的 Markdown 拆成一行行"能画"的东西（够用的子集：标题 / 列表 / 引用 / 代码块 / 表格 / 分隔线）
static wstring stripInlineMark(const wstring& s) {  // 去掉 **加粗** 和 `代码` 的记号，留下文字
    wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'`') continue;
        if (c == L'*' && i + 1 < s.size() && s[i + 1] == L'*') {
            ++i;
            continue;
        }
        out += c;
    }
    return out;
}

static void parseMarkdownDoc(const wstring& text) {
    g_docLines.clear();
    bool inCode = false;
    for (const wstring& rawLine : splitLines(text)) {
        wstring line = rawLine;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
        const wstring t = trim(line);
        if (startsWith(t, L"```")) {  // 代码块围栏本身不显示
            inCode = !inCode;
            continue;
        }
        if (inCode) {
            g_docLines.push_back({DocStyle::Code, line});
            continue;
        }
        if (t.empty()) {
            if (!g_docLines.empty() && !g_docLines.back().text.empty())
                g_docLines.push_back({DocStyle::Body, L""});
            continue;
        }
        if (t == L"---" || t == L"***" || t == L"___") {
            g_docLines.push_back({DocStyle::Rule, L""});
            continue;
        }
        if (startsWith(t, L"####")) {
            g_docLines.push_back({DocStyle::H3, stripInlineMark(trim(t.substr(4)))});
            continue;
        }
        if (startsWith(t, L"###")) {
            g_docLines.push_back({DocStyle::H3, stripInlineMark(trim(t.substr(3)))});
            continue;
        }
        if (startsWith(t, L"##")) {
            g_docLines.push_back({DocStyle::H2, stripInlineMark(trim(t.substr(2)))});
            continue;
        }
        if (startsWith(t, L"#")) {
            g_docLines.push_back({DocStyle::H1, stripInlineMark(trim(t.substr(1)))});
            continue;
        }
        if (startsWith(t, L">")) {
            g_docLines.push_back({DocStyle::Quote, L"▏ " + stripInlineMark(trim(t.substr(1)))});
            continue;
        }
        if (startsWith(t, L"- ") || startsWith(t, L"* ") || startsWith(t, L"+ ")) {
            g_docLines.push_back({DocStyle::Bullet, L"・" + stripInlineMark(trim(t.substr(2)))});
            continue;
        }
        if (t.front() == L'|') {  // 表格：把竖线换成分隔符，够读就行
            vector<wstring> cells;
            wstring cur;
            for (size_t i = 1; i < t.size(); ++i) {
                if (t[i] == L'|') {
                    cells.push_back(trim(cur));
                    cur.clear();
                } else {
                    cur += t[i];
                }
            }
            cells.push_back(trim(cur));
            wstring joined, raw;
            for (size_t i = 0; i < cells.size(); ++i) {
                if (cells[i].empty()) continue;
                if (!joined.empty()) joined += L"　｜　";
                joined += cells[i];
                raw += cells[i];
            }
            bool dashRow = !raw.empty();
            for (wchar_t ch : raw)
                if (ch != L'-' && ch != L':' && ch != L' ') dashRow = false;
            if (dashRow) continue;  // 表格的对齐分隔行不用画
            g_docLines.push_back({DocStyle::Table, stripInlineMark(joined)});
            continue;
        }
        g_docLines.push_back({DocStyle::Body, stripInlineMark(line)});
    }
    // 去掉多余的连续空行
    vector<DocLine> cleaned;
    for (size_t i = 0; i < g_docLines.size(); ++i) {
        if (g_docLines[i].style == DocStyle::Body && g_docLines[i].text.empty() &&
            !cleaned.empty() && cleaned.back().style == DocStyle::Body && cleaned.back().text.empty())
            continue;
        cleaned.push_back(g_docLines[i]);
    }
    g_docLines.swap(cleaned);
}

// 按面板宽度把每行折好（画的时候照着画就行，滚动条也靠它算总高）
static void wrapDocLines(int width) {
    g_docDrawn.clear();
    g_docContentH = 0;
    if (width <= S(40)) return;
    const int indent = S(16);
    for (const DocLine& dl : g_docLines) {
        const int ind = (dl.style == DocStyle::Bullet || dl.style == DocStyle::Code ||
                         dl.style == DocStyle::Quote)
                            ? indent
                            : 0;
        const int avail = width - ind;
        if (dl.style == DocStyle::Rule) {
            g_docDrawn.push_back(dl);
            g_docContentH += docLineH(DocStyle::Rule);
            continue;
        }
        if (dl.text.empty()) {
            g_docDrawn.push_back({DocStyle::Body, L""});
            g_docContentH += S(14);
            continue;
        }
        HFONT f = docFont(dl.style);
        wstring cur;
        for (size_t i = 0; i < dl.text.size(); ++i) {
            const wstring next = cur + dl.text[i];
            if (!cur.empty() && measure(next, f) > avail) {
                g_docDrawn.push_back({dl.style, cur});
                g_docContentH += docLineH(dl.style);
                cur = wstring(1, dl.text[i]);
            } else {
                cur = next;
            }
        }
        g_docDrawn.push_back({dl.style, cur});
        g_docContentH += docLineH(dl.style);
    }
    g_docContentH += S(16);
}

static void copyTextToClipboard(const wstring& text) {
    if (!g_hwnd || !OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

static void openFolderInExplorer(const wstring& path) {
    ensureDir(path);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void loadAboutDoc() {
    wstring text;
    if (!readTextFile(joinPath(g_root, L"README.md"), text) || trim(text).empty()) {
        text = L"# 愚见者的故事书\r\n\r\n"
               L"没找到 README.md（它应该和 exe 放在一起）。\r\n\r\n"
               L"## 剧本怎么写\r\n"
               L"- 剧本是 stories 文件夹里的 .txt，一个文件一个故事；\r\n"
               L"- 图片放 images\\<档案名>，文件名用数字，剧本里写 @图片 1 小窗；\r\n"
               L"- 改完在游戏里按 F5 重载，不用重开。\r\n";
    }
    g_docTitle = L"关于本游戏 & 剧本添加引导";
    parseMarkdownDoc(text);
    const wstring storiesDir = g_storiesDir;
    const wstring imagesDir = g_imagesDir;
    g_docPayload = L"下面这份是《愚见者的故事书》的完整说明（读它就能照着写剧本、加图片）。\r\n" +
                   wstring(L"剧本目录：") + storiesDir + L"\r\n" + wstring(L"图片目录：") + imagesDir +
                   L"\r\n\r\n---- 下面是 README 原文 ----\r\n\r\n" + text;
    g_docScroll = 0;
}

static int docMaxScroll() {
    return (std::max)(0, g_docContentH - (g_docBottom - g_docTop));
}

static void layoutAbout() {
    const int margin = S(26);
    g_rcPanel = {margin, g_rcHeader.bottom + S(12), (int)g_cw - margin, (int)g_rcHint.top - S(16)};
    const int padX = S(28);
    const int btnH = S(30), gap = S(8);
    const int y = g_rcPanel.top + S(74);
    int x = g_rcPanel.left + padX;
    g_aboutBtnCopy = {x, y, x + measure(L"复制全文", g_fSmall) + S(30), y + btnH};
    x = g_aboutBtnCopy.right + gap;
    g_aboutBtnStories = {x, y, x + measure(L"打开剧本文件夹", g_fSmall) + S(30), y + btnH};
    x = g_aboutBtnStories.right + gap;
    g_aboutBtnImages = {x, y, x + measure(L"打开图片文件夹", g_fSmall) + S(30), y + btnH};
    g_aboutBtnBack = {g_rcPanel.right - padX - measure(L"返回", g_fSmall) - S(30), y,
                      g_rcPanel.right - padX, y + btnH};

    g_docLeft = g_rcPanel.left + padX;
    g_docRight = g_rcPanel.right - padX;
    g_docTop = g_aboutBtnCopy.bottom + S(14);
    g_docBottom = g_rcPanel.bottom - S(14);
    wrapDocLines(g_docRight - g_docLeft);
    g_docScroll = (std::max)(0, (std::min)(g_docScroll, docMaxScroll()));
}

static void paintAbout(HDC dc) {
    if (g_bgDC) BitBlt(dc, 0, 0, g_cw, g_ch, g_bgDC, 0, 0, SRCCOPY);
    drawHeader(dc);
    drawStatusLine(dc);
    drawRoundedRect(dc, g_rcPanel, S(18), g_brPanel, g_penPanel);

    RECT title{g_rcPanel.left + S(28), g_rcPanel.top + S(14), g_rcPanel.right - S(28),
               g_rcPanel.top + S(52)};
    SelectObject(dc, g_fCardTitle);
    SetTextColor(dc, Theme::ACCENT);
    DrawTextW(dc, g_docTitle.c_str(), -1, &title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT hint{g_rcPanel.left + S(28), title.bottom, g_rcPanel.right - S(28), title.bottom + S(24)};
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, Theme::HINT);
    DrawTextW(dc, L"把下面这份说明整段复制给 AI（Codex / ChatGPT 都行），就能让它照着帮你写新剧本、配图片素材。",
              -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    auto drawBtn = [&](const RECT& r, const wstring& label, bool hot) {
        drawRoundedRect(dc, r, S(9), hot ? g_brBtnHot : g_brBtn, hot ? g_penBtnHot : g_penBtn);
        RECT t = r;
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, hot ? Theme::ACCENT : Theme::TEXT);
        DrawTextW(dc, label.c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    drawBtn(g_aboutBtnCopy, L"复制全文", g_hotKind == HotKind::AboutBtn && g_hotIndex == 0);
    drawBtn(g_aboutBtnStories, L"打开剧本文件夹", g_hotKind == HotKind::AboutBtn && g_hotIndex == 1);
    drawBtn(g_aboutBtnImages, L"打开图片文件夹", g_hotKind == HotKind::AboutBtn && g_hotIndex == 2);
    drawBtn(g_aboutBtnBack, L"返回", g_hotKind == HotKind::AboutBtn && g_hotIndex == 3);

    // README 正文
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, g_docLeft, g_docTop, g_docRight, g_docBottom);
    int y = g_docTop - g_docScroll;
    for (const DocLine& dl : g_docDrawn) {
        const int h = docLineH(dl.style);
        if (y + h >= g_docTop && y <= g_docBottom) {
            if (dl.style == DocStyle::Rule) {
                HPEN old = (HPEN)SelectObject(dc, g_penRule);
                MoveToEx(dc, g_docLeft, y + h / 2, nullptr);
                LineTo(dc, g_docRight, y + h / 2);
                SelectObject(dc, old);
            } else if (!dl.text.empty()) {
                const int ind = (dl.style == DocStyle::Bullet || dl.style == DocStyle::Code ||
                                 dl.style == DocStyle::Quote)
                                    ? S(16)
                                    : 0;
                RECT r{g_docLeft + ind, y, g_docRight, y + h};
                SelectObject(dc, docFont(dl.style));
                switch (dl.style) {
                    case DocStyle::H1: SetTextColor(dc, Theme::ACCENT); break;
                    case DocStyle::H2: SetTextColor(dc, Theme::ACCENT); break;
                    case DocStyle::H3: SetTextColor(dc, Theme::TEXT); break;
                    case DocStyle::Code: SetTextColor(dc, Theme::QUOTE); break;
                    case DocStyle::Quote: SetTextColor(dc, Theme::QUOTE); break;
                    case DocStyle::Table: SetTextColor(dc, Theme::TEXT_DIM); break;
                    case DocStyle::Bullet: SetTextColor(dc, Theme::TEXT_DIM); break;
                    default: SetTextColor(dc, Theme::TEXT); break;
                }
                DrawTextW(dc, dl.text.c_str(), -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
        }
        y += h;
        if (y > g_docBottom) break;
    }
    RestoreDC(dc, saved);

    // 右边的滚动条
    const int viewH = g_docBottom - g_docTop;
    if (g_docContentH > viewH && viewH > 0) {
        const int trackH = viewH;
        const int thumbH = (std::max)(S(30), trackH * viewH / g_docContentH);
        const int travel = trackH - thumbH;
        const int top = g_docTop + (docMaxScroll() > 0 ? travel * g_docScroll / docMaxScroll() : 0);
        RECT track{(int)g_rcPanel.right - S(12), g_docTop, (int)g_rcPanel.right - S(6), g_docBottom};
        RECT thumb{track.left, top, track.right, top + thumbH};
        HPEN noPen = (HPEN)GetStockObject(NULL_PEN);
        drawRoundedRect(dc, track, S(3), g_brScrollTrack, noPen);
        drawRoundedRect(dc, thumb, S(3), g_brScrollThumb, noPen);
    }

    drawFooter(dc);
}

static void applySizePreset(int idx) {
    if (idx < 0 || idx >= kSizeCount) return;
    g_sizeIndex = idx;
    const SizePreset& sp = kSizePresets[idx];
    if (sp.full) {
        ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
        saveSettings();
        return;
    }
    RECT rc{0, 0, sp.w, sp.h};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT wa{0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - ww) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    ShowWindow(g_hwnd, SW_RESTORE);
    SetWindowPos(g_hwnd, nullptr, x, y, ww, wh, SWP_NOZORDER);
    saveSettings();
}

// 设置页的位置：中间一块面板，一行窗口大小（下拉）、一行打字速度（四个小按钮）
static void layoutSettings() {
    g_ddItems.clear();
    g_speedPills.clear();
    g_ddButton = {};
    const int w = (std::min)(S(680), (int)g_cw - S(80));
    const int h = (std::min)(S(400), (int)g_ch - S(140));
    const int left = (g_cw - w) / 2, top = (g_ch - h) / 2;
    g_rcPanel = {left, top, left + w, top + h};

    const int rowH = S(34), padX = S(26), labelW = S(120);
    g_setSizeRow = {left + padX, top + S(74), g_rcPanel.right - padX, top + S(74) + rowH};
    g_ddButton = {g_setSizeRow.left + labelW, g_setSizeRow.top, g_setSizeRow.right,
                  g_setSizeRow.top + rowH};

    g_setSpeedRow = {g_setSizeRow.left, g_setSizeRow.bottom + S(26), g_setSizeRow.right,
                     g_setSizeRow.bottom + S(26) + rowH};
    const int gap = S(8), pillW =
        (int)(g_setSpeedRow.right - g_setSpeedRow.left - labelW - gap * 3) / 4;
    int x = g_setSpeedRow.left + labelW;
    for (int i = 0; i < 4; ++i) {
        g_speedPills.push_back({x, g_setSpeedRow.top, x + pillW, g_setSpeedRow.top + rowH});
        x += pillW + gap;
    }

    if (g_ddOpen) {  // 展开的选项列表，盖在下面几行上面
        const int ih = S(30);
        int y = g_ddButton.bottom + S(4);
        for (int i = 0; i < kSizeCount; ++i) {
            g_ddItems.push_back({g_ddButton.left, y, g_ddButton.right, y + ih});
            y += ih;
        }
    }
}

static void paintSettings(HDC dc) {
    if (g_bgDC) BitBlt(dc, 0, 0, g_cw, g_ch, g_bgDC, 0, 0, SRCCOPY);
    drawHeader(dc);
    drawStatusLine(dc);
    drawRoundedRect(dc, g_rcPanel, S(18), g_brPanel, g_penPanel);

    RECT title{g_rcPanel.left + S(26), g_rcPanel.top + S(14), g_rcPanel.right - S(26),
               g_rcPanel.top + S(54)};
    SelectObject(dc, g_fCardTitle);
    SetTextColor(dc, Theme::ACCENT);
    DrawTextW(dc, L"设置", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 窗口大小
    RECT lab = g_setSizeRow;
    lab.right = g_ddButton.left - S(10);
    SelectObject(dc, g_fBody);
    SetTextColor(dc, Theme::TEXT_DIM);
    DrawTextW(dc, L"窗口大小", -1, &lab, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const bool bhot = (g_hotKind == HotKind::SettingBtn && g_hotIndex == 0) || g_ddOpen;
    drawRoundedRect(dc, g_ddButton, S(9), bhot ? g_brBtnHot : g_brBtn,
                    bhot ? g_penBtnHot : g_penBtn);
    RECT bl = g_ddButton;
    bl.right -= S(30);
    bl.left += S(14);
    SelectObject(dc, g_fBody);
    SetTextColor(dc, Theme::TEXT);
    DrawTextW(dc, kSizePresets[g_sizeIndex].label, -1, &bl,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT ar = g_ddButton;
    ar.left = ar.right - S(30);
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, Theme::ACCENT);
    DrawTextW(dc, g_ddOpen ? L"▲" : L"▼", -1, &ar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 打字速度
    RECT slab = g_setSpeedRow;
    if (!g_speedPills.empty()) slab.right = g_speedPills.front().left - S(10);
    SelectObject(dc, g_fBody);
    SetTextColor(dc, Theme::TEXT_DIM);
    DrawTextW(dc, L"打字速度", -1, &slab, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (size_t i = 0; i < g_speedPills.size(); ++i) {
        const bool sel = ((int)i == g_speedIndex);
        const bool hot = (g_hotKind == HotKind::SettingBtn && g_hotIndex == (int)i + 1);
        drawRoundedRect(dc, g_speedPills[i], S(9), (sel || hot) ? g_brBtnHot : g_brBtn,
                        (sel || hot) ? g_penBtnHot : g_penBtn);
        RECT r = g_speedPills[i];
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, sel ? Theme::ACCENT : Theme::TEXT_DIM);
        DrawTextW(dc, kSpeedNames[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT hint{g_rcPanel.left + S(26), g_rcPanel.bottom - S(78), g_rcPanel.right - S(26),
              g_rcPanel.bottom - S(16)};
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, Theme::HINT);
    DrawTextW(dc,
              L"窗口大小随时能换（全屏会立刻占满屏幕）；打字速度写「瞬间」就是一句话直接全显示。\r\n"
              L"选择会记在 exe 旁边的 设置.txt 里，关掉再开还是这个设置。",
              -1, &hint, DT_LEFT | DT_TOP | DT_WORDBREAK);

    // 下拉列表
    if (g_ddOpen && !g_ddItems.empty()) {
        RECT box{g_ddItems.front().left - S(6), g_ddItems.front().top - S(6),
                 g_ddItems.back().right + S(6), g_ddItems.back().bottom + S(6)};
        blendFillRounded(dc, box, S(12), Theme::SCRIM, 240);
        drawRoundedRectOutline(dc, box, S(12), g_penCard);
        for (size_t i = 0; i < g_ddItems.size(); ++i) {
            const bool sel = ((int)i == g_sizeIndex);
            const bool hot = (g_hotKind == HotKind::SettingItem && g_hotIndex == (int)i);
            if (hot) blendFillRounded(dc, g_ddItems[i], S(8), Theme::BTN_HOT, 235);
            RECT r = g_ddItems[i];
            r.left += S(12);
            SelectObject(dc, g_fSmall);
            SetTextColor(dc, sel ? Theme::ACCENT : Theme::TEXT);
            DrawTextW(dc, kSizePresets[i].label, -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    drawFooter(dc);
}

static void paintAll(HDC dc) {
    SetBkMode(dc, TRANSPARENT);
    if (g_screen == Screen::Story)
        paintStory(dc);
    else if (g_screen == Screen::Flowchart)
        paintFlowchart(dc);
    else if (g_screen == Screen::Settings)
        paintSettings(dc);
    else if (g_screen == Screen::About)
        paintAbout(dc);
    else
        paintArchives(dc);
}

/* ==================== 流程与动作 ==================== */

static void writeIssueLog() {
    const wstring log = joinPath(g_storiesDir, g_ar.base + L".issues.log");
    if (g_problems.empty()) {
        DeleteFileW(log.c_str());
        return;
    }
    wstring text = g_ar.base + L".txt 的问题清单（改完剧本重载一下就会重新生成）：\r\n\r\n";
    for (size_t i = 0; i < g_problems.size(); ++i)
        text += std::to_wstring(i + 1) + L". " + g_problems[i] + L"\r\n";
    writeTextFile(log, text, true);
}

// 打开 / 关闭信息展示窗
static void startNode(int idx, bool recordEdge, bool applyEnter);

static void openInfoBlock(const InfoBlock& ib) {
    g_infoOpen = true;
    g_infoCurrent = ib;
    g_infoScroll = 0;
    g_infoLines.clear();
    const RECT r = infoRect();
    const int maxW = (int)(r.right - r.left) - S(48);
    for (const Step& st : g_infoCurrent.body.steps) {
        if (st.kind == Step::Image) continue;
        vector<wstring> ls;
        wrapInto(expandVars(st.text), g_fBody, maxW, ls);
        for (const wstring& l : ls) g_infoLines.push_back(l);
    }
    if (g_infoLines.empty()) g_infoLines.push_back(L"（这里还没有内容）");
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void closeInfo() {
    if (!g_infoOpen) return;
    g_infoOpen = false;
    g_infoScroll = 0;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 选项反馈：走正常节点的显示方式（同一个文字面板 + 一个「确认」按钮）
static void buildUiButtons();

static void enterFeedback(const wstring& title, const vector<Step>& steps, int nextNode) {
    g_inFeedback = true;
    g_feedbackNext = nextNode;
    g_shownSteps.clear();
    Step head;  // 先把选项本身当小标题显示
    head.kind = Step::Scene;
    head.text = title;
    g_shownSteps.push_back(head);
    for (const Step& s : steps) g_shownSteps.push_back(s);

    g_typing = false;
    buildUiButtons();   // 这时候只有一个「确认」
    computeLayout();
    buildCurrentLines();
    applyImageMarkers(0);
    computeLayout();
    g_textScroll = 0;
    g_autoScroll = true;
    g_typing = true;
    g_typeStart = GetTickCount64();
    SetTimer(g_hwnd, kTimerTyping, 16, nullptr);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void leaveFeedback() {
    if (!g_inFeedback) return;
    g_inFeedback = false;
    const int t = g_feedbackNext;
    g_feedbackNext = -1;
    startNode(t >= 0 ? t : g_ar.startIndex, true, true);
}

/* ---------- 画面卡播放 ---------- */

static int cardTypedChars() {
    if (g_cardAt < 0 || g_cardAt >= (int)g_cardQueue.size()) return 0;
    const Card& c = g_cardQueue[g_cardAt];
    int total = 0;
    for (const wstring& l : c.lines) total += (int)l.size();
    if (!c.typing || g_cardSkipType) return total;
    const ULONGLONG el = GetTickCount64() - g_cardStart;
    const ULONGLONG after = (el > (ULONGLONG)kCardFadeMs) ? el - kCardFadeMs : 0;
    return (std::min)(total, (int)(after * kCardCps / 1000));
}

static bool cardTypingDone() {
    if (g_cardAt < 0 || g_cardAt >= (int)g_cardQueue.size()) return true;
    const Card& c = g_cardQueue[g_cardAt];
    if (!c.typing || g_cardSkipType) return true;
    int total = 0;
    for (const wstring& l : c.lines) total += (int)l.size();
    return cardTypedChars() >= total;
}

static void resumeNodeAfterCards() {
    const Node& n = g_ar.nodes[g_node];
    // 正文是空的、又只有一条不带条件的 ->：卡片放完直接跳，别再多一页空白让人点确认
    if (!n.isEnding && n.steps.empty() && n.choices.empty() && n.actions.empty() &&
        n.jumps.size() == 1 && n.jumps[0].conds.empty()) {
        const int t = nodeIndexOf(g_ar, n.jumps[0].target);
        if (t >= 0) {
            startNode(t, true, true);
            return;
        }
    }
    g_shownSteps = n.steps;
    buildUiButtons();
    computeLayout();
    buildCurrentLines();
    applyImageMarkers(0);
    computeLayout();
    g_textScroll = 0;
    g_autoScroll = true;
    g_typing = true;
    g_typeStart = GetTickCount64();
    SetTimer(g_hwnd, kTimerTyping, 16, nullptr);
    saveGame();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void showCard(int i) {
    g_cardAt = i;
    g_cardStart = GetTickCount64();
    g_cardSkipType = false;
    SetTimer(g_hwnd, kTimerCard, 16, nullptr);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 点一下 / 按任意键：先补完打字，再翻下一张；放完按剧本决定去哪
static void cardInput() {
    if (g_cardAt < 0) return;
    if (!cardTypingDone()) {
        g_cardSkipType = true;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    const wstring target = g_cardQueue[g_cardAt].target;
    ++g_cardAt;
    g_cardSkipType = false;
    g_cardStart = GetTickCount64();
    if (target.empty() && g_cardAt < (int)g_cardQueue.size()) {
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    g_cardAt = -1;
    KillTimer(g_hwnd, kTimerCard);
    if (!target.empty()) {
        const int t = nodeIndexOf(g_ar, target);
        startNode(t >= 0 ? t : g_ar.startIndex, true, true);
        return;
    }
    resumeNodeAfterCards();  // 没写 -> 只是放了一张画面，继续本节点
}

// 按了一个功能键：先看本节点的信息窗，再看故事级信息窗，最后当作节点名跳过去
static void activateFunc(int i) {
    const vector<FuncButton>& fs = currentFuncs();
    if (i < 0 || i >= (int)fs.size()) return;
    const FuncButton fb = fs[i];
    const vector<InfoBlock>& infos = currentInfos();
    for (size_t k = 0; k < infos.size(); ++k)
        if (infos[k].title == fb.target) {
            openInfoBlock(infos[k]);
            return;
        }
    for (size_t k = 0; k < g_ar.infos.size(); ++k)
        if (g_ar.infos[k].title == fb.target) {
            openInfoBlock(g_ar.infos[k]);
            return;
        }
    const int t = nodeIndexOf(g_ar, fb.target);
    if (t >= 0) {
        startNode(t, true, true);
        return;
    }
    g_status = L"按键「" + fb.label + L"」找不到目标：" + fb.target;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void buildUiButtons() {
    g_uiButtons.clear();
    if (g_ar.nodes.empty()) return;
    if (g_inFeedback) {  // 反馈页：只有一个确认
        g_uiButtons.push_back({L"确认", 6, 0});
        return;
    }
    const Node& n = g_ar.nodes[g_node];

    // 封装节点（写了 @行动）：按钮就是节点内的行动，点了不会离开这个节点
    if (!n.actions.empty()) {
        int shown = 0;
        for (size_t i = 0; i < n.actions.size(); ++i) {
            if (!evalConds(n.actions[i].conds)) continue;  // 条件不满足的按钮不显示
            g_uiButtons.push_back({n.actions[i].label, 5, (int)i});
            if (++shown >= 8) break;
        }
        if (shown > 0) return;
        g_uiButtons.push_back({L"再玩一次（回到开头）", 2, 0});
        g_uiButtons.push_back({L"选择其他档案", 3, 0});
        g_uiButtons.push_back({L"退出游戏", 4, 0});
        return;
    }

    bool anyChoice = false;
    for (size_t i = 0; i < n.choices.size() && i < 8; ++i) {
        if (!evalConds(n.choices[i].conds)) continue;  // 条件不满足的选项不显示
        g_uiButtons.push_back({n.choices[i].text, 0, (int)i});
        anyChoice = true;
    }
    if (anyChoice) return;

    if (!n.isEnding) {  // 没有可选的选项，就看直接跳转
        for (const JumpTarget& j : n.jumps)
            if (evalConds(j.conds)) {
                g_uiButtons.push_back({L"继续 →", 1, 0});
                return;
            }
    }
    // 结局，或者所有出口的条件都不满足：给结束按钮，别把玩家卡死
    g_uiButtons.push_back({L"再玩一次（回到开头）", 2, 0});
    g_uiButtons.push_back({L"选择其他档案", 3, 0});
    g_uiButtons.push_back({L"退出游戏", 4, 0});
}

// recordEdge: 这次移动算不算"走过了这条边"；applyEnter: 要不要触发节点的 @改变
static void startNode(int idx, bool recordEdge, bool applyEnter);

// 依次试一组出口条件，命中就跳过去（返回 true 表示已经离开了这个节点）
static bool tryLeaveVia(const vector<JumpTarget>& jumps) {
    for (const JumpTarget& j : jumps) {
        if (!evalConds(j.conds)) continue;
        const int t = nodeIndexOf(g_ar, j.target);
        startNode(t >= 0 ? t : g_ar.startIndex, true, true);
        return true;
    }
    return false;
}

// 点了一个行动按钮：在本节点里跑完它，再回到本节点（不进流程图）
static void runActionBlock(int index) {
    if (g_ar.nodes.empty() || g_node < 0 || g_node >= (int)g_ar.nodes.size()) return;
    const Node& n = g_ar.nodes[g_node];
    if (index < 0 || index >= (int)n.actions.size()) return;
    const ActionBlock ab = n.actions[index];

    applyEffects(ab.effects);
    execStmts(ab.body.stmts);
    if (tryLeaveVia(ab.body.jumps)) return;  // 这一下直接结束战斗之类

    vector<Step> next;
    for (const Step& s : ab.body.steps) next.push_back(s);
    if (!ab.freeTurn) {
        execStmts(n.round.stmts);  // 回合流程：敌人反击之类
        for (const Step& s : n.round.steps) next.push_back(s);
        if (tryLeaveVia(n.round.jumps)) return;
    }
    if (tryLeaveVia(n.jumps)) return;  // 节点自己的出口条件（比如血空了）

    // 行动 / 回合里要是没写文字，就保留面板上原来那段，别变成空白
    bool hasText = false;
    for (const Step& s : next)
        if (s.kind != Step::Image) hasText = true;
    if (hasText) g_shownSteps = next;

    // 封装节点没换节点，但数值变了：把当前状态写回进度，存档才是最新的
    {
        StoryProgress& p = progressNow();
        p.currentNode = n.name;
        p.currentVars = g_vars;
    }

    buildUiButtons();
    computeLayout();
    buildCurrentLines();
    applyImageMarkers(0);
    computeLayout();
    g_textScroll = 0;
    g_autoScroll = true;
    g_typing = true;
    g_typeStart = GetTickCount64();
    SetTimer(g_hwnd, kTimerTyping, 16, nullptr);
    saveGame();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void startNode(int idx, bool recordEdge, bool applyEnter) {
    if (g_ar.nodes.empty()) return;
    if (idx < 0 || idx >= (int)g_ar.nodes.size()) idx = g_ar.startIndex;
    const wstring fromName = (recordEdge && g_node >= 0 && g_node < (int)g_ar.nodes.size())
                                 ? g_ar.nodes[g_node].name
                                 : wstring();
    g_node = idx;
    const Node& n = g_ar.nodes[idx];

    if (applyEnter) execStmts(n.stmts);  // @改变 / @骰 / 数组操作，真的走进来才跑
    if (n.isEnding && n.endingNo > 0) g_unlocked.insert(n.endingNo);

    // 记进度：走过的节点 / 走过的边 / 每个节点上次到达时的数值
    StoryProgress& p = progressNow();
    if (!fromName.empty() && fromName != n.name) p.edges.insert(fromName + L"\n" + n.name);
    p.visited.insert(n.name);
    p.nodeVars[n.name] = g_vars;
    // 进入记录：只有"正常顺着剧情走进来"才记一笔（从流程图回溯进来不算）
    if (recordEdge) {
        NodeEntry en;
        en.from = fromName;
        en.vars = g_vars;
        vector<NodeEntry>& list = p.entries[n.name];
        en.order = list.empty() ? 1 : (list.back().order + 1);
        list.push_back(en);
        if ((int)list.size() > kMaxNodeEntries)
            list.erase(list.begin(), list.begin() + ((int)list.size() - kMaxNodeEntries));
    }
    p.currentNode = n.name;
    p.currentVars = g_vars;
    g_lastStory = g_ar.base;

    g_typing = false;
    g_shownSteps = n.steps;  // 面板先显示节点正文

    // 画面卡：先依次放卡片，放完再回到这个节点的正文
    if (!n.cards.empty()) {
        g_cardQueue = n.cards;
        showCard(0);
        return;
    }

    // 封装节点：刚进来就先看看出口条件是不是已经满足了
    if (!n.actions.empty() && tryLeaveVia(n.jumps)) return;

    buildUiButtons();
    computeLayout();            // 先拿到面板宽度
    buildCurrentLines();        // 按宽度折行
    applyImageMarkers(0);       // 应用节点开头就生效的图片指令
    computeLayout();            // 小窗 / 全屏可能变了，再排一次

    g_textScroll = 0;           // 新节点从顶部开始
    g_infoOpen = false;         // 换节点时关掉信息窗
    g_inFeedback = false;      // 反馈页状态清掉
    g_autoScroll = true;
    g_dragThumb = false;
    g_typing = true;
    g_typeStart = GetTickCount64();
    SetTimer(g_hwnd, kTimerTyping, 16, nullptr);
    saveGame();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void skipTyping() {
    if (!g_typing) return;
    g_typing = false;
    KillTimer(g_hwnd, kTimerTyping);
    relayout();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 没在打字时，空格 / 点击画面 = 往下翻一页
static void pageDownIfNeeded() {
    const int viewport = storyViewportHeight();
    if (viewport <= 0) return;
    if (g_textScroll < maxTextScroll()) scrollTextBy(viewport - S(30));
}

static void scanArchives() {
    g_archives.clear();
    int converted = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_storiesDir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const wstring name = fd.cFileName;
            if (name.empty() || name[0] == L'_' || name[0] == L'.') continue;
            const wstring ext = extName(name);
            if (ext != L".txt" && ext != L".scn") continue;

            ArchiveEntry e;
            e.path = joinPath(g_storiesDir, name);
            e.base = baseName(name);
            Archive ar;
            vector<wstring> problems;
            if (loadArchiveFile(e.path, ar, problems))
                e.title = ar.title;
            else
                e.title = e.base + L"（读不出来）";
            e.problemCount = (int)problems.size();

            const wstring imgFolder = joinPath(g_imagesDir, e.base);
            converted += autoConvertFolder(imgFolder);  // 顺手把 jpg/bmp/gif 转成 png
            const FolderScan& fs = scanFolder(imgFolder);
            e.pngCount = fs.pngCount;
            e.otherCount = fs.otherCount;

            const StoryProgress* sp = progressFor(e.base);
            if (sp) {
                e.lastPlayed = (!g_lastStory.empty() && e.base == g_lastStory);
                e.lastNode = sp->currentNode;
                e.visitedNodes = (int)sp->visited.size();
            }
            g_archives.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(g_archives.begin(), g_archives.end(),
              [](const ArchiveEntry& a, const ArchiveEntry& b) { return a.base < b.base; });
    if (converted > 0)
        g_status = L"已自动把 " + std::to_wstring(converted) +
                   L" 张图片转成 png（原图移到 _原图\\ 文件夹）";
}

static void bootstrapApp() {
    ensureDir(g_storiesDir);
    ensureDir(g_imagesDir);

    const wstring tpl = joinPath(g_storiesDir, L"_模板.txt");
    if (!fileExists(tpl)) writeTextFile(tpl, kTemplateScript, true);
}

static void openArchive(size_t idx) {
    if (idx >= g_archives.size()) return;
    Archive ar;
    vector<wstring> problems;
    if (!loadArchiveFile(g_archives[idx].path, ar, problems)) {
        g_status = problems.empty() ? L"打不开这个档案" : problems[0];
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    g_ar = ar;
    g_problems = problems;
    g_status.clear();
    g_unlocked.clear();
    g_vars = freshVars();
    g_arrays = freshArrays();
    g_varFlash.clear();
    g_imgNo = 0;
    g_imgFull = false;
    g_flowScrollX = g_flowScrollY = 0;
    clearImageCache();
    clearFolderCache();
    writeIssueLog();
    buildGraph();

    // 存档里如果有这个故事的进度，就接着上次的位置和数值继续
    const StoryProgress* p = progressFor(g_ar.base);
    if (p) {
        for (int e : p->endings) g_unlocked.insert(e);
        for (const StoryArray& a : p->arrays) {  // 存档里的数组覆盖初值
            StoryArray* dst = findArray(a.name);
            if (dst) dst->items = a.items;
        }
    }

    int start = g_ar.startIndex;
    bool resuming = false;
    if (p && !p->currentNode.empty()) {
        const int t = nodeIndexOf(g_ar, p->currentNode);
        if (t >= 0) {
            start = t;
            resuming = true;
            if (!p->currentVars.empty()) g_vars = p->currentVars;
        }
    }

    g_screen = Screen::Story;
    g_node = -1;
    startNode(start, false, !resuming);
}

static void goArchives() {
    saveGame();
    g_screen = Screen::Archives;
    g_status.clear();
    g_archiveScroll = 0;
    clearImageCache();
    clearFolderCache();
    scanArchives();
    relayout();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void restartArchive() {
    if (g_screen != Screen::Story) return;
    g_imgNo = 0;
    g_imgFull = false;
    g_vars = freshVars();  // 数值回到初始值（流程图里走过的记录保留）
    g_arrays = freshArrays();
    g_varFlash.clear();
    startNode(g_ar.startIndex, false, true);
}

static void reloadArchive() {
    if (g_screen == Screen::Archives) return;
    const wstring keep = g_ar.nodes.empty() ? wstring() : g_ar.nodes[g_node].name;
    Archive ar;
    vector<wstring> problems;
    if (!loadArchiveFile(g_ar.path, ar, problems)) {
        g_status = problems.empty() ? L"重载失败" : problems[0];
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    const vector<VarVal> keepVars = g_vars;  // 同名数值保留当前的值
    g_ar = ar;
    g_problems = problems;
    g_status.clear();
    g_vars = freshVars();
    for (const VarVal& old : keepVars)
        for (VarVal& v : g_vars)
            if (v.name == old.name) v.value = old.value;
    const int converted = autoConvertFolder(currentImageFolder());  // 顺手把新丢进来的 jpg 转成 png
    if (converted > 0)
        g_status = L"已自动把 " + std::to_wstring(converted) +
                   L" 张图片转成 png（原图移到 _原图\\ 文件夹）";
    clearImageCache();
    clearFolderCache();
    writeIssueLog();
    buildGraph();
    const int idx = keep.empty() ? -1 : nodeIndexOf(g_ar, keep);
    startNode(idx >= 0 ? idx : g_ar.startIndex, false, false);
}

// 从流程图点一个走过的节点回溯过去（数值也回到上次到那里时的那一份）
static void jumpToNode(int idx) {
    if (idx < 0 || idx >= (int)g_ar.nodes.size()) return;
    const Node& n = g_ar.nodes[idx];
    const StoryProgress* p = progressFor(g_ar.base);
    if (p) {
        auto it = p->nodeVars.find(n.name);
        if (it != p->nodeVars.end()) {
            for (const VarVal& snap : it->second)
                for (VarVal& v : g_vars)
                    if (v.name == snap.name) v.value = snap.value;
        }
    }
    g_varFlash.clear();
    g_imgNo = 0;
    g_imgFull = false;
    g_screen = Screen::Story;
    g_node = -1;  // 回溯不算"走过一条边"
    startNode(idx, false, false);
}

/* ---------- 展开面板：一个节点的每一次进入记录 ---------- */

static void closeFlowFan() {
    g_fanOpen = false;
    g_fanNode = -1;
    g_fanHot = -1;
    g_fanEntries.clear();
    g_fanRects.clear();
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void openFlowFan(int nodeIdx) {
    if (nodeIdx < 0 || nodeIdx >= (int)g_ar.nodes.size()) return;
    const wstring name = g_ar.nodes[nodeIdx].name;
    const vector<NodeEntry>* list = entriesFor(name);
    if (!list || list->size() < 2) return;  // 只进去过一次就不用摊开了

    g_fanOpen = true;
    g_fanNode = nodeIdx;
    g_fanTitle = name;
    g_fanEntries = *list;  // 拷一份，跳转途中不怕被改
    g_fanHot = -1;
    layoutFlowFan();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 把这些记录卡排到屏幕上：一行放不下就换行，整块居中
static void layoutFlowFan() {
    g_fanRects.clear();
    if (!g_fanOpen || g_fanEntries.empty()) return;

    const int margin = S(40);
    const int gapX = S(22), gapY = S(26);
    const int top = S(190);  // 卡片从这儿开始（标题在它上面）
    const int n = (int)g_fanEntries.size();

    // 先按宽度定几列，再按剩下的高度分给每一行，卡片跟着高矮自适应
    int cardW = S(196);
    const int itemW = cardW;
    const int availW = (std::max)(S(200), g_cw - margin * 2);
    int cols = (std::max)(1, (availW + gapX) / (itemW + gapX));
    cols = (std::min)(cols, n);
    const int rows = (n + cols - 1) / cols;

    const int availH = (std::max)(S(200), g_ch - top - S(20));
    int perRow = (availH - (rows - 1) * gapY) / rows;
    int capH = (std::max)(S(38), perRow * 34 / 100);  // 卡片下方的小字说明
    if (capH > S(96)) capH = S(96);
    int cardH = (std::max)(S(56), perRow - capH);
    if (cardH > S(150)) cardH = S(150);  // 别撑得太夸张
    const int itemH = cardH + capH;

    const int blockW = cols * itemW + (cols - 1) * gapX;
    const int blockH = rows * itemH + (rows - 1) * gapY;
    const int x0 = (g_cw - blockW) / 2;
    const int y0 = (std::max)(top, (g_ch - blockH) / 2);

    g_fanBox = {x0 - S(18), S(104), x0 + blockW + S(18), y0 + blockH + S(14)};

    for (int i = 0; i < n; ++i) {
        const int c = i % cols, rr = i / cols;
        const int x = x0 + c * (itemW + gapX);
        const int y = y0 + rr * (itemH + gapY);
        g_fanRects.push_back({x, y, x + cardW, y + cardH});
    }
}

// 一条记录卡的小字："第 3 次・从「走廊」来" + 变量（放不下就用 …）
static wstring entryCaptionLine(const Node& n, const NodeEntry& en, int width, HFONT font) {
    wstring head = L"第 " + std::to_wstring(en.order) + L" 次";
    head += en.from.empty() ? wstring(L"（开局）") : (L"・从「" + en.from + L"」来");

    // 变量只展示"这个节点本来就会显示在界面上的那些"，多了就截掉
    vector<wstring> shown;
    for (const DisplayItem& d : n.disps) {
        bool isArray = false;
        for (const StoryArray& a : g_arrays)
            if (a.name == d.name) isArray = true;
        wstring t = d.name;
        if (!isArray) {
            for (const VarVal& v : en.vars)
                if (v.name == d.name) t += L" " + std::to_wstring(v.value);
        }
        shown.push_back(t);
    }
    if (shown.empty())  // 节点没写 @显示：把记下来的数值都摆出来（也照样截断）
        for (const VarVal& v : en.vars) shown.push_back(v.name + L" " + std::to_wstring(v.value));

    wstring varsText;
    for (size_t i = 0; i < shown.size(); ++i) {
        const wstring next = varsText.empty() ? shown[i] : (varsText + L" · " + shown[i]);
        if (measure(next, font) > width) {
            varsText += L" …";
            break;
        }
        varsText = next;
    }
    if (varsText.empty()) return head;
    return head + L"\r\n" + varsText;
}

static void drawFlowFan(HDC dc) {
    if (!g_fanOpen) return;
    if (g_fanNode < 0 || g_fanNode >= (int)g_ar.nodes.size()) return;
    const Node& n = g_ar.nodes[g_fanNode];

    // 盖住整个界面：黑色半透明，挡住流程图上的任何交互
    blendFillRounded(dc, {0, 0, g_cw, g_ch}, 0, Theme::SCRIM, 222);

    RECT title{g_fanBox.left, S(108), g_fanBox.right, S(136)};
    SelectObject(dc, g_fCardTitle);
    SetTextColor(dc, Theme::ACCENT);
    DrawTextW(dc, (L"「" + g_fanTitle + L"」进来过 " + std::to_wstring(g_fanEntries.size()) +
                   L" 次").c_str(),
              -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT hint{g_fanBox.left, S(138), g_fanBox.right, S(160)};
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, Theme::HINT);
    DrawTextW(dc, L"点一张回到那一刻的状态 ｜ Esc 返回流程图", -1, &hint,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const wstring body = n.summary.empty() ? n.name : n.summary;
    for (size_t i = 0; i < g_fanRects.size() && i < g_fanEntries.size(); ++i) {
        const RECT& r = g_fanRects[i];
        const bool hot = ((int)i == g_fanHot);
        const NodeEntry& en = g_fanEntries[i];
        const bool latest = (i + 1 == g_fanEntries.size());

        drawRoundedRect(dc, r, S(14), hot ? g_brBtnHot : g_brCard, hot ? g_penBtnHot : g_penCard);

        RECT no{r.left + S(12), r.top + S(8), r.right - S(12), r.top + S(26)};
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, latest ? Theme::ACCENT : Theme::HINT);
        DrawTextW(dc, (L"第 " + std::to_wstring(en.order) + L" 次进入").c_str(), -1, &no,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT sum{r.left + S(12), no.bottom + S(2), r.right - S(12), r.bottom - S(10)};
        SelectObject(dc, g_fBody);
        SetTextColor(dc, Theme::TEXT);
        DrawTextW(dc, body.c_str(), -1, &sum, DT_LEFT | DT_TOP | DT_WORDBREAK);

        RECT cap{r.left, r.bottom + S(6), r.right, r.bottom + S(60)};
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, hot ? Theme::ACCENT : Theme::TEXT_DIM);
        const wstring text = entryCaptionLine(n, en, (int)(r.right - r.left), g_fSmall);
        DrawTextW(dc, text.c_str(), -1, &cap, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    }
}

// 回到"第 n 次进入那个节点"时的状态
static void jumpToEntry(int nodeIdx, int entryIdx) {
    if (nodeIdx < 0 || nodeIdx >= (int)g_ar.nodes.size()) return;
    const Node& n = g_ar.nodes[nodeIdx];
    const vector<NodeEntry>* list = entriesFor(n.name);
    vector<VarVal> snap;
    if (list && entryIdx >= 0 && entryIdx < (int)list->size()) snap = (*list)[entryIdx].vars;
    closeFlowFan();

    for (const VarVal& s : snap)
        for (VarVal& v : g_vars)
            if (v.name == s.name) v.value = s.value;
    g_varFlash.clear();
    g_imgNo = 0;
    g_imgFull = false;
    g_screen = Screen::Story;
    g_node = -1;  // 回溯不算"走过一条边"，也不再记一次进入
    startNode(nodeIdx, false, false);
}

static void activateButton(int i) {
    if (i < 0 || i >= (int)g_uiButtons.size()) return;
    const UiButton b = g_uiButtons[i];
    if (b.kind == 0) {
        const Node& n = g_ar.nodes[g_node];
        if (b.arg < (int)n.choices.size()) {
            const Choice& c = n.choices[b.arg];
            if (!evalConds(c.conds)) return;  // 数值已经变了，这条选不了
            applyEffects(c.effects);          // 先结算选项的数值变化
            const int t = nodeIndexOf(g_ar, c.target);
            const int target = (t >= 0) ? t : g_ar.startIndex;

            // 只有作者写了 @反馈 才先走一页反馈；不写就直接进下一个节点
            if (!c.feedback.steps.empty()) {
                enterFeedback(c.text, c.feedback.steps, target);
                return;
            }
            startNode(target, true, true);
        }
    } else if (b.kind == 1) {
        for (const JumpTarget& j : g_ar.nodes[g_node].jumps) {
            if (!evalConds(j.conds)) continue;
            const int t = nodeIndexOf(g_ar, j.target);
            startNode(t >= 0 ? t : g_ar.startIndex, true, true);
            break;
        }
    } else if (b.kind == 2) {
        restartArchive();
    } else if (b.kind == 3) {
        goArchives();
    } else if (b.kind == 4) {
        DestroyWindow(g_hwnd);
    } else if (b.kind == 5) {
        runActionBlock(b.arg);  // 封装节点里的行动按钮
    } else if (b.kind == 6) {
        leaveFeedback();  // 反馈页的「确认」
    }
}

static void runAction(Action a) {
    switch (a) {
        case A_Restart: restartArchive(); break;
        case A_List: goArchives(); break;
        case A_Flow:
            if (!g_ar.nodes.empty()) {
                g_screen = Screen::Flowchart;
                g_flowScrollX = 0;
                g_flowScrollY = 0;
                relayout();
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
            break;
        case A_Back:
            if (g_screen == Screen::About) {
                g_screen = Screen::Archives;
                g_docScroll = 0;
                relayout();
                InvalidateRect(g_hwnd, nullptr, FALSE);
            } else if (g_screen == Screen::Settings) {
                g_ddOpen = false;
                g_screen = (g_settingsFrom == Screen::Settings) ? Screen::Archives : g_settingsFrom;
                relayout();
                InvalidateRect(g_hwnd, nullptr, FALSE);
            } else if (g_screen == Screen::Flowchart) {
                g_screen = Screen::Story;
                relayout();
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
            break;
        case A_Settings:
            if (g_screen != Screen::Settings) g_settingsFrom = g_screen;
            g_screen = Screen::Settings;
            g_ddOpen = false;
            relayout();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case A_About:
            loadAboutDoc();
            g_screen = Screen::About;
            relayout();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case A_CopyDoc:
            copyTextToClipboard(g_docPayload);
            g_status = L"整份说明已复制到剪贴板：粘给 AI（Codex / ChatGPT）就能让它照着帮你写剧本、配图";
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case A_OpenStories:
            openFolderInExplorer(g_storiesDir);
            break;
        case A_OpenImages:
            openFolderInExplorer(g_imagesDir);
            break;
        case A_Quit: DestroyWindow(g_hwnd); break;
        default: break;
    }
}

static void updateHover(POINT p) {
    g_hotKind = HotKind::None;
    g_hotIndex = -1;
    if (g_infoOpen) return;  // 信息窗打开时，其它地方都不响应
    if (g_fanOpen) {  // 展开面板开着：只认面板里那几张记录卡
        const int prev = g_fanHot;
        g_fanHot = -1;
        for (size_t i = 0; i < g_fanRects.size(); ++i)
            if (PtInRect(&g_fanRects[i], p)) {
                g_fanHot = (int)i;
                break;
            }
        if (prev != g_fanHot && g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (g_screen == Screen::Story) {
        {
            RECT br = textWideRect();
            if (PtInRect(&br, p)) {
                g_hotKind = HotKind::TextWide;
                g_hotIndex = 0;
                return;
            }
        }
        for (size_t i = 0; i < g_btnRect.size(); ++i)
            if (PtInRect(&g_btnRect[i], p)) {
                g_hotKind = HotKind::Choice;
                g_hotIndex = (int)i;
                return;
            }
        for (size_t i = 0; i < g_funcRects.size(); ++i)
            if (PtInRect(&g_funcRects[i], p)) {
                g_hotKind = HotKind::Func;
                g_hotIndex = (int)i;
                return;
            }
    } else if (g_screen == Screen::Settings) {
        if (g_ddOpen) {  // 下拉展开时只认那份列表
            for (size_t i = 0; i < g_ddItems.size(); ++i)
                if (PtInRect(&g_ddItems[i], p)) {
                    g_hotKind = HotKind::SettingItem;
                    g_hotIndex = (int)i;
                    return;
                }
            return;
        }
        if (PtInRect(&g_ddButton, p)) {
            g_hotKind = HotKind::SettingBtn;
            g_hotIndex = 0;
            return;
        }
        for (size_t i = 0; i < g_speedPills.size(); ++i)
            if (PtInRect(&g_speedPills[i], p)) {
                g_hotKind = HotKind::SettingBtn;
                g_hotIndex = (int)i + 1;
                return;
            }
    } else if (g_screen == Screen::About) {
        const RECT btns[4] = {g_aboutBtnCopy, g_aboutBtnStories, g_aboutBtnImages, g_aboutBtnBack};
        for (int i = 0; i < 4; ++i)
            if (PtInRect(&btns[i], p)) {
                g_hotKind = HotKind::AboutBtn;
                g_hotIndex = i;
                return;
            }
    } else if (g_screen == Screen::Archives && PtInRect(&g_rcPanel, p)) {
        for (size_t i = 0; i < g_archiveCards.size() && i < g_archives.size(); ++i) {
            const RECT dr = delSaveRect(g_archiveCards[i]);
            if (progressFor(g_archives[i].base) && PtInRect(&dr, p)) {
                g_hotKind = HotKind::ArchiveDel;
                g_hotIndex = (int)i;
                return;
            }
        }
        for (size_t i = 0; i < g_archiveCards.size(); ++i)
            if (PtInRect(&g_archiveCards[i], p)) {
                g_hotKind = HotKind::Archive;
                g_hotIndex = (int)i;
                return;
            }
    }
    if (g_screen == Screen::Story) {
        const ScrollInfo si = scrollInfo();
        if (si.visible && PtInRect(&si.track, p)) {
            g_hotKind = HotKind::Scrollbar;
            g_hotIndex = 0;
            return;
        }
    } else if (g_screen == Screen::Flowchart) {
        const BarGeom vb = flowVBar();
        if (vb.visible && PtInRect(&vb.track, p)) {
            g_hotKind = HotKind::FlowBar;
            g_hotIndex = 1;
            return;
        }
        const BarGeom hb = flowHBar();
        if (hb.visible && PtInRect(&hb.track, p)) {
            g_hotKind = HotKind::FlowBar;
            g_hotIndex = 2;
            return;
        }
        if (PtInRect(&g_flowViewport, p))
            for (size_t i = 0; i < g_flowCards.size(); ++i)
                if (!g_flowCards[i].ghost && PtInRect(&g_flowCards[i].rc, p)) {
                    const bool multi = (g_flowCards[i].entries >= 2);
                    // 当前节点：只进来过一次的话，点它也没意义
                    if (g_flowCards[i].node == g_node && !multi) continue;
                    g_hotKind = HotKind::FlowCard;
                    g_hotIndex = (int)i;
                    // 多次进来过的节点：停上去就把叠着的几张微微散开
                    if (multi) {
                        g_flowSpreadAnim[(int)i];  // 建个键，动画从当前值开始
                        SetTimer(g_hwnd, kTimerFlowAnim, 16, nullptr);
                    }
                    return;
                }
    }
    for (size_t i = 0; i < g_tools.size(); ++i)
        if (PtInRect(&g_tools[i].rc, p)) {
            g_hotKind = HotKind::Tool;
            g_hotIndex = (int)i;
            return;
        }
}

static LPCWSTR cursorIdFor(POINT p) {
    updateHover(p);
    if (g_infoOpen) return IDC_HAND;
    if (g_hotKind == HotKind::Scrollbar) return IDC_SIZENS;
    if (g_hotKind == HotKind::FlowBar) return (g_hotIndex == 2) ? IDC_SIZEWE : IDC_SIZENS;
    return (g_hotKind != HotKind::None) ? IDC_HAND : IDC_ARROW;
}

/* ==================== 窗口过程 ==================== */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            g_measureDC = CreateCompatibleDC(nullptr);

            g_fTitle = makeFont(23, FW_BOLD);
            g_fScene = makeFont(13, FW_BOLD);
            g_fBody = makeFont(12, FW_NORMAL);
            g_fQuote = makeFont(12, FW_NORMAL);
            g_fEnding = makeFont(17, FW_BOLD);
            g_fButton = makeFont(12, FW_NORMAL);
            g_fBadge = makeFont(10, FW_BOLD);
            g_fSmall = makeFont(9, FW_NORMAL);
            g_fCardTitle = makeFont(13, FW_BOLD);

            g_brPanel = CreateSolidBrush(Theme::PANEL);
            g_brCard = CreateSolidBrush(Theme::CARD);
            g_brBtn = CreateSolidBrush(Theme::BTN);
            g_brBtnHot = CreateSolidBrush(Theme::BTN_HOT);
            g_brBtnDown = CreateSolidBrush(Theme::BTN_DOWN);
            g_brBadge = CreateSolidBrush(Theme::BADGE);
            g_brScrollTrack = CreateSolidBrush(RGB(15, 20, 29));
            g_brScrollThumb = CreateSolidBrush(RGB(58, 76, 100));
            g_brScrollThumbHot = CreateSolidBrush(Theme::ACCENT);
            g_brHpBar = CreateSolidBrush(RGB(74, 176, 164));
            g_brHpLow = CreateSolidBrush(RGB(196, 96, 88));
            g_penPanel = CreatePen(PS_SOLID, 1, Theme::PANEL_EDGE);
            g_penCard = CreatePen(PS_SOLID, 1, Theme::CARD_EDGE);
            g_penBtn = CreatePen(PS_SOLID, 1, Theme::BTN_EDGE);
            g_penBtnHot = CreatePen(PS_SOLID, 1, Theme::ACCENT);
            g_penRule = CreatePen(PS_SOLID, 1, Theme::RULE);

            g_root = exeDir();
            g_storiesDir = joinPath(g_root, L"stories");
            g_imagesDir = joinPath(g_root, L"images");

            // 打包时会在 exe 旁边放一个 版本.txt，第一行是版本号
            wstring verText;
            if (readTextFile(joinPath(g_root, L"版本.txt"), verText)) {
                const size_t cut = verText.find_first_of(L"\r\n");
                g_version = trim(cut == wstring::npos ? verText : verText.substr(0, cut));
            }
            g_savePath = joinPath(g_root, L"存档.txt");
            loadSave();
            bootstrapApp();
            scanArchives();
            computeLayout();
            return 0;
        }

        case WM_SIZE:
            if (g_measureDC) {
                applyUiScale(LOWORD(lp), HIWORD(lp));
                relayout();
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (g_cw > 0 && g_ch > 0) {
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, g_cw, g_ch);
                HGDIOBJ old = SelectObject(mem, bmp);
                paintAll(mem);
                BitBlt(dc, 0, 0, g_cw, g_ch, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TIMER:
            if (wp == kTimerCard) {  // 画面卡：渐显 / 打字期间重画
                if (g_cardAt < 0) {
                    KillTimer(hwnd, kTimerCard);
                } else {
                    InvalidateRect(hwnd, nullptr, FALSE);
                    if (GetTickCount64() - g_cardStart > (ULONGLONG)kCardFadeMs && cardTypingDone())
                        KillTimer(hwnd, kTimerCard);
                }
            }
            if (wp == kTimerTyping) {
                const int revealed = revealedChars();
                const bool done = revealed >= g_totalChars;
                const int prevNo = g_imgNo;
                const bool prevFull = g_imgFull;
                applyImageMarkers(revealed);
                if (done) {
                    g_typing = false;
                    KillTimer(hwnd, kTimerTyping);
                }
                updateAutoScroll();  // 文字超过一屏时自动往下滚
                if (prevNo != g_imgNo || prevFull != g_imgFull) relayout();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (wp == kTimerUi) {  // 数值高亮到点了就取消
                const ULONGLONG now = GetTickCount64();
                bool active = false;
                for (auto it = g_varFlash.begin(); it != g_varFlash.end();) {
                    if (now >= it->second)
                        it = g_varFlash.erase(it);
                    else {
                        active = true;
                        ++it;
                    }
                }
                if (!active) KillTimer(hwnd, kTimerUi);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (wp == kTimerFlowAnim) {  // 叠卡展开 / 收起
                bool busy = false;
                for (auto& kv : g_flowSpreadAnim) {
                    const bool want =
                        (g_hotKind == HotKind::FlowCard && kv.first == g_hotIndex &&
                         kv.first >= 0 && kv.first < (int)g_flowCards.size() &&
                         g_flowCards[kv.first].entries >= 2);
                    const float target = want ? 1.f : 0.f;
                    if (kv.second < target) {
                        kv.second = (std::min)(target, kv.second + 0.20f);
                        busy = true;
                    } else if (kv.second > target) {
                        kv.second = (std::max)(target, kv.second - 0.20f);
                        busy = true;
                    }
                }
                if (!busy) KillTimer(hwnd, kTimerFlowAnim);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (wp == kTimerMarquee) {  // 选项文字横向轮播
                if (!g_marqueeOn) {
                    KillTimer(hwnd, kTimerMarquee);
                } else {
                    g_marqueeMs += 40;
                    if (g_marqueeMs > 100000000) g_marqueeMs = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;

        case WM_MOUSEMOVE: {
            const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!g_trackingLeave) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                g_trackingLeave = true;
            }
            if (g_dragThumb) {  // 拖动滚动条
                const ScrollInfo si = scrollInfo();
                const int travel =
                    (int)(si.track.bottom - si.track.top) - (int)(si.thumb.bottom - si.thumb.top);
                if (travel > 0) {
                    int top = p.y - g_dragGrab - (int)si.track.top;
                    top = (std::max)(0, (std::min)(top, travel));
                    g_textScroll = top * si.maxScroll / travel;
                    g_autoScroll = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (g_dragFlow) {  // 拖动流程图的滚动条
                if (g_dragFlow == 1) {
                    const BarGeom vb = flowVBar();
                    const int travel = (int)(vb.track.bottom - vb.track.top) -
                                       (int)(vb.thumb.bottom - vb.thumb.top);
                    if (travel > 0) {
                        int top = p.y - g_dragFlowGrab - (int)vb.track.top;
                        top = (std::max)(0, (std::min)(top, travel));
                        g_flowScrollY = top * flowMaxScrollY() / travel;
                        computeLayout();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                } else {
                    const BarGeom hb = flowHBar();
                    const int travel = (int)(hb.track.right - hb.track.left) -
                                       (int)(hb.thumb.right - hb.thumb.left);
                    if (travel > 0) {
                        int left = p.x - g_dragFlowGrab - (int)hb.track.left;
                        left = (std::max)(0, (std::min)(left, travel));
                        g_flowScrollX = left * flowMaxScrollX() / travel;
                        computeLayout();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                return 0;
            }
            updateHover(p);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSELEAVE:
            g_trackingLeave = false;
            g_hotKind = HotKind::None;
            g_hotIndex = -1;
            g_downKind = g_downIndex = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (g_cardAt >= 0) {  // 画面卡：点一下继续
                cardInput();
                return 0;
            }
            if (g_infoOpen) {  // 信息窗开着：点一下关闭
                closeInfo();
                return 0;
            }
            if (g_fanOpen) {  // 展开面板开着：只有记录卡能点
                updateHover(p);
                g_downKind = 6;
                g_downIndex = g_fanHot;
                return 0;
            }
            updateHover(p);
            g_downKind = -1;
            g_downIndex = -1;
            if (g_hotKind == HotKind::Scrollbar) {
                const ScrollInfo si = scrollInfo();
                if (PtInRect(&si.thumb, p)) {
                    g_dragThumb = true;
                    g_dragGrab = p.y - (int)si.thumb.top;
                    SetCapture(hwnd);
                } else {
                    const int page = storyViewportHeight() - S(30);
                    scrollTextBy(p.y < (int)si.thumb.top ? -page : page);
                }
            } else if (g_hotKind == HotKind::FlowBar) {
                const BarGeom vb = flowVBar();
                const BarGeom hb = flowHBar();
                if (g_hotIndex == 1 && vb.visible) {
                    if (PtInRect(&vb.thumb, p)) {
                        g_dragFlow = 1;
                        g_dragFlowGrab = p.y - (int)vb.thumb.top;
                        SetCapture(hwnd);
                    } else {
                        const int page = p.y < (int)vb.thumb.top ? -(flowViewportH() - S(30))
                                                                 : (flowViewportH() - S(30));
                        g_flowScrollY = (std::max)(0, (std::min)(g_flowScrollY + page,
                                                                  flowMaxScrollY()));
                        computeLayout();
                    }
                } else if (g_hotIndex == 2 && hb.visible) {
                    if (PtInRect(&hb.thumb, p)) {
                        g_dragFlow = 2;
                        g_dragFlowGrab = p.x - (int)hb.thumb.left;
                        SetCapture(hwnd);
                    } else {
                        const int page = p.x < (int)hb.thumb.left ? -(flowViewportW() - S(30))
                                                                   : (flowViewportW() - S(30));
                        g_flowScrollX = (std::max)(0, (std::min)(g_flowScrollX + page,
                                                                  flowMaxScrollX()));
                        computeLayout();
                    }
                }
            } else if (g_hotKind == HotKind::FlowCard) {
                g_downKind = 3;
                g_downIndex = g_hotIndex;
            } else if (g_hotKind == HotKind::Func) {
                g_downKind = 4;
                g_downIndex = g_hotIndex;
            } else if (g_hotKind == HotKind::Choice) {
                g_downKind = 0;
                g_downIndex = g_hotIndex;
            } else if (g_hotKind == HotKind::TextWide) {
                g_downKind = 7;
                g_downIndex = 0;
            } else if (g_hotKind == HotKind::SettingBtn) {
                g_downKind = 8;
                g_downIndex = g_hotIndex;
            } else if (g_hotKind == HotKind::SettingItem) {
                g_downKind = 9;
                g_downIndex = g_hotIndex;
            } else if (g_hotKind == HotKind::AboutBtn) {
                g_downKind = 10;
                g_downIndex = g_hotIndex;
            } else if (g_hotKind == HotKind::Archive) {
                g_downKind = 1;
                g_downIndex = g_hotIndex;
            } else if (g_hotKind == HotKind::ArchiveDel) {
                g_downKind = 5;
                g_downIndex = g_hotIndex;
            } else if (g_hotKind == HotKind::Tool) {
                g_downKind = 2;
                g_downIndex = g_hotIndex;
            } else {
                if (g_typing)
                    skipTyping();
                else
                    pageDownIfNeeded();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g_dragThumb) {
                g_dragThumb = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_dragFlow) {
                g_dragFlow = 0;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            updateHover(p);
            const int kind = g_downKind, index = g_downIndex;
            g_downKind = g_downIndex = -1;

            if (kind == 6) {  // 展开面板里点了一张记录卡
                if (index >= 0 && index == g_fanHot && g_fanNode >= 0) jumpToEntry(g_fanNode, index);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (kind == 7 && g_hotKind == HotKind::TextWide) {  // 正文框展开 / 收起
                g_textWide = !g_textWide;
                g_autoScroll = true;
                relayout();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (kind == 8 && g_hotKind == HotKind::SettingBtn) {  // 设置页上的按钮
                if (index == 0) {
                    g_ddOpen = !g_ddOpen;
                    relayout();
                } else if (index >= 1 && index <= 4) {
                    g_speedIndex = index - 1;
                    saveSettings();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (kind == 9 && g_hotKind == HotKind::SettingItem) {  // 选了窗口大小
                if (index >= 0 && index < kSizeCount) applySizePreset(index);
                g_ddOpen = false;
                relayout();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (kind == 10 && g_hotKind == HotKind::AboutBtn) {  // 引导页上的按钮
                switch (index) {
                    case 0: runAction(A_CopyDoc); break;
                    case 1: runAction(A_OpenStories); break;
                    case 2: runAction(A_OpenImages); break;
                    case 3: runAction(A_Back); break;
                    default: break;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (kind == 0 && g_hotKind == HotKind::Choice && g_hotIndex == index) {
                activateButton(index);
            } else if (kind == 1 && g_hotKind == HotKind::Archive && g_hotIndex == index) {
                openArchive((size_t)index);
            } else if (kind == 5 && g_hotKind == HotKind::ArchiveDel && g_hotIndex == index) {
                deleteArchiveSave((size_t)index);
            } else if (kind == 2 && g_hotKind == HotKind::Tool && g_hotIndex == index) {
                if (index >= 0 && index < (int)g_tools.size()) runAction(g_tools[index].action);
            } else if (kind == 3 && g_hotKind == HotKind::FlowCard && g_hotIndex == index &&
                       index >= 0 && index < (int)g_flowCards.size()) {
                // 点流程图上的节点回溯过去；进过好几次的节点先把记录摊开
                if (g_flowCards[index].entries >= 2)
                    openFlowFan(g_flowCards[index].node);
                else
                    jumpToNode(g_flowCards[index].node);
            } else if (kind == 4 && g_hotKind == HotKind::Func && g_hotIndex == index) {
                activateFunc(index);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (g_fanOpen) return 0;  // 展开面板开着时不滚流程图
            if (g_screen == Screen::About) {  // 引导页里滚 README
                g_docScroll = (std::max)(0, (std::min)(g_docScroll - delta * S(30) * 3,
                                                       docMaxScroll()));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_infoOpen) {  // 信息窗里滚轮翻页
                g_infoScroll = (std::max)(0, g_infoScroll - delta * S(26) * 3);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_screen == Screen::Archives && !g_archives.empty()) {
                g_archiveScroll -= delta * S(70);
                computeLayout();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (g_screen == Screen::Story && maxTextScroll() > 0) {
                scrollTextBy(-delta * S(26) * 3);
            } else if (g_screen == Screen::Flowchart) {
                if ((GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0) {
                    g_flowScrollX = (std::max)(
                        0, (std::min)(g_flowScrollX - delta * S(60), flowMaxScrollX()));
                } else {
                    g_flowScrollY = (std::max)(
                        0, (std::min)(g_flowScrollY - delta * S(60), flowMaxScrollY()));
                }
                computeLayout();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (g_fanOpen) {  // 展开面板：Esc 退回流程图，其它键都不响应
                if (wp == VK_ESCAPE) closeFlowFan();
                return 0;
            }
            if (g_screen == Screen::Settings) {  // 设置页：Esc 先收下拉，再返回
                if (wp == VK_ESCAPE || wp == VK_TAB) {
                    if (g_ddOpen) {
                        g_ddOpen = false;
                        relayout();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    } else {
                        runAction(A_Back);
                    }
                }
                return 0;
            }
            if (g_screen == Screen::About) {  // 引导页：Esc 返回，翻页键滚 README
                if (wp == VK_ESCAPE || wp == VK_TAB) {
                    runAction(A_Back);
                } else if (wp == VK_PRIOR || wp == VK_UP) {
                    g_docScroll = (std::max)(0, g_docScroll - (g_docBottom - g_docTop - S(30)));
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (wp == VK_NEXT || wp == VK_DOWN) {
                    g_docScroll = (std::min)(docMaxScroll(),
                                             g_docScroll + (g_docBottom - g_docTop - S(30)));
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (wp == VK_HOME) {
                    g_docScroll = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (wp == VK_END) {
                    g_docScroll = docMaxScroll();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (g_cardAt >= 0) {  // 画面卡：按任意键继续
                cardInput();
                return 0;
            }
            if (g_infoOpen) {  // 信息窗开着：Esc / 空格 / 回车都关掉
                if (wp == VK_ESCAPE || wp == VK_SPACE || wp == VK_RETURN || wp == VK_TAB)
                    closeInfo();
                return 0;
            }
            if (wp == VK_TAB) {  // Tab 在"故事 / 流程图"之间来回切
                if (g_screen == Screen::Story)
                    runAction(A_Flow);
                else if (g_screen == Screen::Flowchart)
                    runAction(A_Back);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (g_screen == Screen::Flowchart) {
                    runAction(A_Back);
                    return 0;
                }
                if (g_screen == Screen::Settings) {
                    runAction(A_Back);
                    return 0;
                }
                DestroyWindow(hwnd);
                return 0;
            }
            if (wp == VK_F2) {  // 顺手给个设置快捷键
                runAction(g_screen == Screen::Settings ? A_Back : A_Settings);
                return 0;
            }
            if (g_screen == Screen::Story) {
                if (wp >= '1' && wp <= '9') {
                    const int idx = (int)(wp - '1');
                    // 屏幕上有几个按钮，数字键就选第几个（选项 / 继续 / 结束按钮都算）
                    if (idx < (int)g_uiButtons.size()) activateButton(idx);
                } else if (wp == VK_SPACE || wp == VK_RETURN) {
                    if (g_inFeedback && !g_typing && wp == VK_RETURN)
                        leaveFeedback();  // 反馈页打完字后，回车等于按确认
                    else if (g_typing)
                        skipTyping();
                    else
                        pageDownIfNeeded();
                } else if (wp == VK_F5) {
                    reloadArchive();
                } else if (wp == VK_PRIOR) {
                    scrollTextBy(-(storyViewportHeight() - S(30)));
                } else if (wp == VK_NEXT) {
                    scrollTextBy(storyViewportHeight() - S(30));
                } else if (wp == VK_HOME) {
                    scrollTextBy(-maxTextScroll());
                } else if (wp == VK_END) {
                    scrollTextBy(maxTextScroll());
                } else if (wp == VK_UP) {
                    scrollTextBy(-S(52));
                } else if (wp == VK_DOWN) {
                    scrollTextBy(S(52));
                }
            } else if (g_screen == Screen::Archives && wp >= '1' && wp <= '9') {
                const int idx = (int)(wp - '1');
                if (idx < (int)g_archives.size()) openArchive((size_t)idx);
            } else if (g_screen == Screen::Flowchart) {
                int dx = 0, dy = 0;
                if (wp == VK_LEFT)
                    dx = -S(60);
                else if (wp == VK_RIGHT)
                    dx = S(60);
                else if (wp == VK_UP)
                    dy = -S(60);
                else if (wp == VK_DOWN)
                    dy = S(60);
                else if (wp == VK_PRIOR)
                    dy = -(flowViewportH() - S(30));
                else if (wp == VK_NEXT)
                    dy = flowViewportH() - S(30);
                else if (wp == VK_HOME) {
                    g_flowScrollX = g_flowScrollY = 0;
                    computeLayout();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                } else if (wp == VK_F5) {
                    reloadArchive();
                    return 0;
                }
                if (dx != 0 || dy != 0) {
                    g_flowScrollX =
                        (std::max)(0, (std::min)(g_flowScrollX + dx, flowMaxScrollX()));
                    g_flowScrollY =
                        (std::max)(0, (std::min)(g_flowScrollY + dy, flowMaxScrollY()));
                    computeLayout();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                SetCursor(LoadCursorW(nullptr, cursorIdFor(p)));
                return TRUE;
            }
            break;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerTyping);
            KillTimer(hwnd, kTimerUi);
            saveGame();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================== 入口 ==================== */

static void decideClientSize(int& cw, int& ch) {
    g_dpi = g_dispDpi;

    RECT wa{0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    cw = S(940);
    ch = S(760);
    while ((cw > (wa.right - wa.left) || ch > (wa.bottom - wa.top)) && g_dpi > 72) {
        g_dpi -= 4;
        cw = S(940);
        ch = S(760);
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI * SetDpiAwareFn)(void);
        if (auto fn = (SetDpiAwareFn)GetProcAddress(user32, "SetProcessDPIAware")) fn();
    }

    Gdiplus::GdiplusStartupInput gdiInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiInput, nullptr) != Gdiplus::Ok) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FoolStorybookWnd";
    if (!RegisterClassExW(&wc)) return 1;

    // 先读设置（窗口大小 / 打字速度），再决定窗口开成多大
    {
        HDC screen = GetDC(nullptr);
        g_dispDpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        if (g_dispDpi < 96) g_dispDpi = 96;
    }
    g_root = exeDir();
    g_settingsPath = joinPath(g_root, L"设置.txt");
    loadSettings();

    int cw = 0, ch = 0;
    decideClientSize(cw, ch);

    RECT rc{0, 0, cw, ch};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    RECT wa{0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = wa.left + ((wa.right - wa.left) - winW) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - winH) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"愚见者的故事书", style, x, y,
                                winW, winH, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    if (!g_version.empty())  // 打包版会在 exe 旁边带一个 版本.txt
        SetWindowTextW(hwnd, (L"愚见者的故事书 " + g_version).c_str());

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    applySizePreset(g_sizeIndex);  // 按设置里的窗口大小开局（默认全屏）

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    clearImageCache();
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}
