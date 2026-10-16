/* =====================================================================
 *  愚见者的故事书 · 自解压启动器（launcher）
 *
 *  打包脚本 tools\pack_release.ps1 会把这个启动器编译成 launcher_stub.exe，
 *  然后在它后面追加一段数据：
 *      [启动器本体][包裹数据][8 字节魔数 + 8 字节数据偏移 + 8 字节数据长度]
 *  运行成品 exe 时，它把包裹数据解开到
 *      %LOCALAPPDATA%\愚见者的故事书\
 *  然后启动里面的游戏本体。所以发给别人只需要给这一个 exe。
 *
 *  包裹数据格式（全部小端）：
 *      uint32  文件数
 *      每个文件：
 *          uint32  文件名长度（UTF-16 字节数）
 *          bytes   文件名（UTF-16LE，相对路径，例如 stories\雨夜末班车.txt）
 *          uint64  内容长度
 *          bytes   内容
 *
 *  编译：
 *      g++ -std=c++17 -mwindows -O2 -s -static -o launcher_stub.exe launcher.cpp
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

#include <string>
#include <vector>

using std::wstring;
using std::vector;

static const char kMagic[8] = {'F', 'O', 'O', 'L', 'B', 'O', 'X', '1'};
static const wchar_t* kAppName = L"愚见者的故事书";

struct Trailer {
    char magic[8];
    unsigned long long offset;
    unsigned long long size;
};

static void fail(const wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

static wstring selfPath() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return wstring(buf, n);
}

static wstring parentDir(const wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return (cut == wstring::npos) ? wstring() : path.substr(0, cut);
}

static wstring joinPath(const wstring& a, const wstring& b) {
    if (a.empty()) return b;
    return (a.back() == L'\\') ? a + b : a + L"\\" + b;
}

static bool fileExists(const wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 新版包里已经没有的文件（比如上一版自带的示例剧本）要清掉，
// 不然它们会一直留在解包目录里，游戏列表里也一直显示。
static bool payloadHas(const vector<wstring>& keep, const wstring& rel) {
    for (size_t i = 0; i < keep.size(); ++i)
        if (keep[i] == rel) return true;
    return false;
}

static void removeStaleFiles(const wstring& dir, const wstring& relPrefix,
                             const vector<wstring>& keep) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    vector<wstring> subdirs;
    do {
        const wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (name == L"_原图") continue;  // 转换前的原图是作者自己留的，别动
            subdirs.push_back(name);
            continue;
        }
        if (name == L"存档.txt" || name == L".package") continue;
        const wstring rel = relPrefix.empty() ? name : (relPrefix + L"\\" + name);
        if (!payloadHas(keep, rel)) DeleteFileW(joinPath(dir, name).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const wstring& sub : subdirs) {
        const wstring full = joinPath(dir, sub);
        removeStaleFiles(full, relPrefix.empty() ? sub : (relPrefix + L"\\" + sub), keep);
        RemoveDirectoryW(full.c_str());  // 空了才删得掉
    }
}

// 递归创建目录（路径里用 \ 分隔）
static void ensureDir(const wstring& path) {
    wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        const bool last = (i + 1 == path.size());
        if (path[i] == L'\\' || path[i] == L'/' || last) {
            if (cur.size() > 3 && GetFileAttributesW(cur.c_str()) == INVALID_FILE_ATTRIBUTES)
                CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
}

static bool readAt(HANDLE h, unsigned long long offset, void* buf, DWORD bytes) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h, buf, bytes, &got, nullptr) && got == bytes;
}

static bool writeFileBytes(const wstring& path, const BYTE* data, unsigned long long size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    unsigned long long done = 0;
    while (done < size) {
        const DWORD chunk = (DWORD)((size - done > 1u << 20) ? 1u << 20 : size - done);
        DWORD wrote = 0;
        if (!WriteFile(h, data + done, chunk, &wrote, nullptr) || wrote != chunk) {
            ok = false;
            break;
        }
        done += wrote;
    }
    CloseHandle(h);
    return ok;
}

static bool readTextFile(const wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[256] = {0};
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.assign(buf, got);
    return true;
}

static bool writeTextFile(const wstring& path, const std::string& text) {
    return writeFileBytes(path, (const BYTE*)text.data(), text.size());
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    const wstring self = selfPath();
    HANDLE h = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fail(L"打不开自己这个文件，可能被安全软件拦住了。");
        return 1;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(h, &fileSize) || fileSize.QuadPart < (LONGLONG)sizeof(Trailer)) {
        CloseHandle(h);
        fail(L"这个 exe 不完整，请重新下载。");
        return 1;
    }

    Trailer trailer{};
    if (!readAt(h, (unsigned long long)fileSize.QuadPart - sizeof(Trailer), &trailer,
                sizeof(Trailer)) ||
        memcmp(trailer.magic, kMagic, 8) != 0) {
        CloseHandle(h);
        fail(L"这个 exe 不是打包好的故事书（找不到内置数据）。");
        return 1;
    }
    if (trailer.size == 0 || trailer.size > (unsigned long long)512 * 1024 * 1024 ||
        trailer.offset + trailer.size > (unsigned long long)fileSize.QuadPart) {
        CloseHandle(h);
        fail(L"内置数据有问题，请重新下载。");
        return 1;
    }

    vector<BYTE> payload((size_t)trailer.size);
    if (!readAt(h, trailer.offset, payload.data(), (DWORD)payload.size())) {
        CloseHandle(h);
        fail(L"读取内置数据失败。");
        return 1;
    }
    CloseHandle(h);

    // 解包到 %LOCALAPPDATA%\愚见者的故事书
    wchar_t localApp[MAX_PATH * 2] = {0};
    wstring base = L".";
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH * 2) > 0) base = localApp;
    else {
        wchar_t tmp[MAX_PATH * 2] = {0};
        GetTempPathW(MAX_PATH * 2, tmp);
        base = tmp;
    }
    const wstring targetDir = joinPath(base, kAppName);
    ensureDir(targetDir);

    // 记录这次解的是哪一包，内容没变就不重复解压（这样用户改过的剧本不会被覆盖）
    const wstring marker = joinPath(targetDir, L".package");
    const wstring stamp = std::to_wstring(trailer.size) + L"-" + std::to_wstring(trailer.offset);
    std::string stampUtf8;
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, stamp.c_str(), (int)stamp.size(), nullptr,
                                          0, nullptr, nullptr);
        if (n > 0) {
            stampUtf8.resize(n);
            WideCharToMultiByte(CP_UTF8, 0, stamp.c_str(), (int)stamp.size(), &stampUtf8[0], n,
                                nullptr, nullptr);
        }
    }
    bool upToDate = false;
    {
        std::string old;
        if (readTextFile(marker, old)) upToDate = (old == stampUtf8);
    }

    const BYTE* p = payload.data();
    const BYTE* end = payload.data() + payload.size();
    auto need = [&](size_t n) { return (size_t)(end - p) >= n; };
    auto readU32 = [&]() {
        unsigned int v = 0;
        memcpy(&v, p, 4);
        p += 4;
        return v;
    };
    auto readU64 = [&]() {
        unsigned long long v = 0;
        memcpy(&v, p, 8);
        p += 8;
        return v;
    };

    if (!need(4)) {
        fail(L"内置数据损坏。");
        return 1;
    }
    const unsigned int count = readU32();
    wstring gameExe;
    vector<wstring> payloadPaths;  // 这一版包里到底有哪些文件
    int written = 0;
    bool allOk = true;

    for (unsigned int i = 0; i < count; ++i) {
        if (!need(4)) break;
        const unsigned int nameBytes = readU32();
        if (!need(nameBytes + 8)) break;
        vector<wchar_t> nameBuf(nameBytes / 2 + 1, L'\0');  // 拷贝一份，避免非对齐访问
        memcpy(nameBuf.data(), p, nameBytes);
        const wstring rel(nameBuf.data(), nameBytes / 2);
        payloadPaths.push_back(rel);
        p += nameBytes;
        const unsigned long long size = readU64();
        if (!need((size_t)size)) break;

        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, L".exe") == 0 && rel.find(L'\\') == wstring::npos)
            gameExe = rel;

        if (!upToDate) {
            const wstring full = joinPath(targetDir, rel);
            ensureDir(parentDir(full));
            // 个别文件可能被占用（比如游戏正开着），失败就记下来，下次再试
            if (writeFileBytes(full, p, size))
                ++written;
            else
                allOk = false;
        }
        p += (size_t)size;
    }

    if (gameExe.empty()) {
        fail(L"包里没找到游戏程序。");
        return 1;
    }
    const wstring gamePath = joinPath(targetDir, gameExe);
    if (!fileExists(gamePath)) {
        fail(L"解压失败，游戏程序没有落地。\n目标目录：" + targetDir);
        return 1;
    }
    // 全部文件都写成功了才记"已是最新"，否则下次运行会重来一遍
    if (!upToDate && allOk && written == (int)count) {
        // 换了新包：把上一版留下、这一版已经没有的剧本 / 图片清掉（存档和原图不动）
        removeStaleFiles(joinPath(targetDir, L"stories"), L"stories", payloadPaths);
        removeStaleFiles(joinPath(targetDir, L"images"), L"images", payloadPaths);
        writeTextFile(marker, stampUtf8);
    }

    // 启动游戏本体
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wstring cmd = L"\"" + gamePath + L"\"";
    if (!CreateProcessW(gamePath.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr,
                        targetDir.c_str(), &si, &pi)) {
        fail(L"启动游戏失败。\n路径：" + gamePath);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
