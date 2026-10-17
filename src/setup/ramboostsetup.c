/* RAMBlockCache 安装器 V10.2 —— 唯一安装入口（BUG-002/009 整改 + BOOT_START 根因修复）
 * 动作：
 *   1) 清理旧版本残留（旧服务；旧非标准卷类 GUID {536670a4-...} 下的 UpperFilters）
 *   2) 复制驱动到 %SystemRoot%\system32\drivers
 *   3) 创建服务（KERNEL_DRIVER + BOOT_START + "System Bus Extender" 组）
 *      + 写 Parameters 默认值；旧服务存在则 ChangeServiceConfig 幂等升级
 *   4) 向标准卷类 {71a27cdd-...} 的 UpperFilters 读-改-写【追加】RAMBlockCache
 *      （保留 volsnap；已存在则幂等跳过）
 *   5) 提示重启一次生效；之后 ATTACH/DETACH 热开关免重启
 * 编译：cl /W4 /O2 ramboostsetup.c /link advapi32.lib wintrust.lib
 *        （wintrust.lib 供 [FIX A2] 签名预检；pragma comment 已冗余保险）
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <shellapi.h>
#include <wintrust.h>          /* [FIX A2] VerifyDriverSignature: WinVerifyTrust */
#include <softpub.h>           /* [FIX A2] WINTRUST_ACTION_GENERIC_VERIFY_V2 GUID */
#pragma comment(lib, "wintrust.lib")   /* [FIX A2] 自包含链接（编译行亦同步追加） */

/* IsUserAnAdmin 由 shell32.dll 导出（shlobj.h）；WIN32_LEAN_AND_MEAN 不拉 shlobj.h，
 * 这里显式给出原型，消除 C4013。 */
EXTERN_C BOOL WINAPI IsUserAnAdmin(void);

#define SVC_NAME      L"RAMBlockCache"
#define DRV_SYSNAME   L"RAMBlockCache.sys"
#define CLS_VOLUME    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{71a27cdd-812a-11d0-bec7-08002be2092f}"
#define CLS_BAD       L"SYSTEM\\CurrentControlSet\\Control\\Class\\{536670a4-367c-463f-8b14-fa40f6d6fc1f}"
#define PARM_KEY      L"SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Parameters"

static VOID Fail(LPCWSTR msg) { fwprintf(stderr, L"[X] %s (err=%lu)\n", msg, GetLastError()); }
static VOID Ok (LPCWSTR msg) { wprintf(L"[ok] %s\n", msg); }

/* ---- REG_MULTI_SZ 追加一项（幂等，不重复） ---- */
static LONG RegMultiSzAppend(LPCWSTR sub, LPCWSTR val, LPCWSTR entry)
{
    HKEY h; LONG r; DWORD type = 0, cb = 0, cap;
    WCHAR stackbuf[1024]; WCHAR *buf = stackbuf; DWORD i, n, len;

    r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, NULL, 0,
                        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &h, NULL);
    if (r) return r;
    cb = sizeof(stackbuf);
    r = RegQueryValueExW(h, val, NULL, &type, (LPBYTE)buf, &cb);
    if (r == ERROR_MORE_DATA) {
        /* [FIX R2-W11] 既有值超过栈缓冲：按真实尺寸堆分配重读——原代码把
         * MORE_DATA 当"空值"整值覆写，会删光 volsnap 等既有条目（比 C6 原病更糟） */
        buf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, cb);
        if (!buf) { RegCloseKey(h); return ERROR_NOT_ENOUGH_MEMORY; }
        r = RegQueryValueExW(h, val, NULL, &type, (LPBYTE)buf, &cb);
    }
    if (r == ERROR_FILE_NOT_FOUND) { buf[0] = 0; buf[1] = 0; cb = 0; }
    else if (r != ERROR_SUCCESS || type != REG_MULTI_SZ) {
        /* [FIX R2-W11] 既有值存在但非 MULTI_SZ：拒绝而非覆写（防整值毁坏） */
        if (buf != stackbuf) HeapFree(GetProcessHeap(), 0, buf);
        RegCloseKey(h);
        return (r == ERROR_SUCCESS) ? ERROR_INVALID_DATA : r;
    }
    cap = (buf == stackbuf) ? 1024 : cb / sizeof(WCHAR);

    /* 查重（以读得字符数为硬界，损坏值不越界） */
    n = cb / sizeof(WCHAR);
    for (i = 0; i < n && buf[i]; i += (DWORD)wcslen(buf + i) + 1)
        if (_wcsicmp(buf + i, entry) == 0) {
            if (buf != stackbuf) HeapFree(GetProcessHeap(), 0, buf);
            RegCloseKey(h); return ERROR_SUCCESS;
        }

    len = (DWORD)wcslen(entry) + 1;
    n = cb / sizeof(WCHAR);
    /* [FIX C6] RegQueryValueExW 返回的 cb 含结尾双 NUL：追加必须覆盖第一个终止 NUL，
     * 否则新条目落在终止符之后、枚举永远不可见（UpperFilters 追加静默失效） */
    if (n > 0) n -= 1;
    if (n + len + 1 > cap) {
        if (buf != stackbuf) HeapFree(GetProcessHeap(), 0, buf);
        RegCloseKey(h); return ERROR_MORE_DATA;
    }
    memcpy(buf + n, entry, len * sizeof(WCHAR));
    n += len; buf[n] = 0;                    /* 双零结尾 */

    r = RegSetValueExW(h, val, 0, REG_MULTI_SZ, (const BYTE*)buf,
                       (n + 1) * sizeof(WCHAR));
    if (buf != stackbuf) HeapFree(GetProcessHeap(), 0, buf);
    RegCloseKey(h);
    return r;
}

/* ---- REG_MULTI_SZ 摘除一项（保留其余） ---- */
static LONG RegMultiSzRemove(LPCWSTR sub, LPCWSTR val, LPCWSTR entry)
{
    HKEY h; LONG r; DWORD type, cb = 0; DWORD rd;
    static WCHAR buf[2048], out[2048];
    DWORD i, n = 0;

    r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0,
                      KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &h);
    if (r) return r;
    /* [FIX M21] 两段式查询：先问长度，超缓冲按失败处理（原 MORE_DATA 被当成功，残留未清理）
     * [S-建议] 类型不符与超缓冲分码：误报 MORE_DATA 会误导调用方重试更大缓冲 */
    r = RegQueryValueExW(h, val, NULL, &type, NULL, &cb);
    if (r == ERROR_FILE_NOT_FOUND) { RegCloseKey(h); return ERROR_SUCCESS; }
    if (r != ERROR_SUCCESS) { RegCloseKey(h); return r; }
    if (type != REG_MULTI_SZ) { RegCloseKey(h); return ERROR_INVALID_DATA; }
    if (cb > sizeof(buf)) { RegCloseKey(h); return ERROR_MORE_DATA; }
    rd = sizeof(buf);
    r = RegQueryValueExW(h, val, NULL, &type, (LPBYTE)buf, &rd);
    if (r != ERROR_SUCCESS || type != REG_MULTI_SZ) { RegCloseKey(h); return (r == ERROR_SUCCESS) ? ERROR_INVALID_DATA : r; }

    for (i = 0; buf[i]; i += (DWORD)wcslen(buf + i) + 1) {
        if (_wcsicmp(buf + i, entry) == 0) continue;
        DWORD l = (DWORD)wcslen(buf + i) + 1;
        if (n + l >= 2047) { RegCloseKey(h); return ERROR_MORE_DATA; }  /* [FIX M21] 超长不静默截断 */
        memcpy(out + n, buf + i, l * sizeof(WCHAR)); n += l;
    }
    out[n] = 0;
    r = RegSetValueExW(h, val, 0, REG_MULTI_SZ, (const BYTE*)out, (n + 1) * sizeof(WCHAR));
    RegCloseKey(h);
    return r;
}

static void DeleteServiceIfExists(void)
{
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    SC_HANDLE s;
    if (!m) return;
    s = OpenServiceW(m, SVC_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (s) {
        SERVICE_STATUS st;
        int wait;
        ControlService(s, SERVICE_CONTROL_STOP, &st);   /* 触发干净卸载/落盘 */
        /* [FIX W20] 等到 SERVICE_STOPPED（上限 ~15s）再 DeleteService，
         * 否则重装时 ERROR_SERVICE_MARKED_FOR_DELETE 中断 */
        for (wait = 0; wait < 30; wait++) {
            if (!QueryServiceStatus(s, &st)) break;
            if (st.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(500);
        }
        DeleteService(s);
        CloseServiceHandle(s);
        wprintf(L"[ok] old service removed\n");
    }
    CloseServiceHandle(m);
}

/* ---- [FIX A2] 签名预检：未签名/签名链无效的驱动绝不进入引导路径 ----
 * 0x7B 事故根因（2026-10-17 incident-0x7b-bootloop）：未签名 sys 被注册为
 * BOOT_START 并写入卷类 UpperFilters -> Windows Code Integrity 在引导路径
 * 拒载 -> 引导卷栈初始化失败 -> 开机循环。本检查在任何变更（复制/建服务/
 * 写 UpperFilters）之前执行，未签名即拒绝安装。
 * WinVerifyTrust(WINTRUST_ACTION_GENERIC_VERIFY_V2) 校验 Authenticode 签名
 * 链有效性：链到本机任一信任根即通过（RAMBoostFS 测试根、未来 WHQL/EV 均
 * 可），未签名/断链被拒。注意本检查管"有有效签名"；自签测试证书场景下
 * 内核能否加载还取决于 testsigning 是否开启——那由调用方的 WARN 提示高亮。 */
static BOOL VerifyDriverSignature(void)
{
    WCHAR src[MAX_PATH];
    WINTRUST_FILE_INFO fi;
    WINTRUST_DATA     wd;
    GUID              action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG              st;

    /* 源文件 = 安装器 exe 同目录的 RAMBlockCache.sys（与 CopyDriverFile 同源） */
    GetModuleFileNameW(NULL, src, MAX_PATH);
    { WCHAR *sl = wcsrchr(src, L'\\'); if (sl) *sl = 0; }
    wcscat_s(src, MAX_PATH, L"\\RAMBlockCache.sys");

    ZeroMemory(&fi, sizeof(fi));
    fi.cbStruct      = sizeof(fi);
    fi.pcwszFilePath = src;
    fi.hFile         = NULL;
    fi.pgKnownSubject = NULL;

    ZeroMemory(&wd, sizeof(wd));
    wd.cbStruct      = sizeof(wd);
    wd.dwUIChoice    = WTD_UI_NONE;        /* 无任何交互（安装器静默运行） */
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;  /* 测试证书无 CRL，不查吊销 */
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile         = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags   = 0;

    st = WinVerifyTrust(NULL, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;   /* 释放 provider 状态 */
    WinVerifyTrust(NULL, &action, &wd);

    if (st == ERROR_SUCCESS) { Ok(L"driver signature chain: VALID"); return TRUE; }

    fwprintf(stderr, L"[X] driver signature check FAILED (WinVerifyTrust=0x%08lX)\n", st);
    fwprintf(stderr, L"    An unsigned or untrusted driver MUST NOT be installed as\n");
    fwprintf(stderr, L"    BOOT_START + UpperFilters: Windows Code Integrity will\n");
    fwprintf(stderr, L"    refuse to load it at boot and the machine will 0x7B loop.\n");
    fwprintf(stderr, L"    Sign the driver first (see README signing section).\n");
    return FALSE;
}

static BOOL CopyDriverFile(void)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH];
    GetModuleFileNameW(NULL, src, MAX_PATH);
    WCHAR *sl = wcsrchr(src, L'\\'); if (sl) *sl = 0;
    wcscat_s(src, MAX_PATH, L"\\RAMBlockCache.sys");
    GetSystemDirectoryW(dst, MAX_PATH);
    wcscat_s(dst, MAX_PATH, L"\\drivers\\");
    wcscat_s(dst, MAX_PATH, DRV_SYSNAME);
    if (!CopyFileW(src, dst, FALSE)) { Fail(L"copy driver (place RAMBlockCache.sys beside me)"); return FALSE; }
    Ok(L"driver copied");
    return TRUE;
}

static void SetDefaults(void)
{
    HKEY h; LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, PARM_KEY, 0, NULL, 0,
                                     KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &h, NULL);
    if (r) return;
    struct { LPCWSTR n; DWORD v; } t[] = {
        { L"CacheSizeMB", 0 }, { L"WriteBack", 0 }, { L"SystemVolumeCached", 0 },
        { L"FlushIntervalMs", 60000 }, { L"DirtyHighPct", 60 }, { L"DirtyLowPct", 40 },
        /* [BUG3] 开机自动挂载数据卷（旧审计"服务创建后写 AutoAttach=1"意图丢失项：
         * 缺省时每次开机冷缓存，用户实测"挂载后与裸盘跑分一致"）。
         * 驱动侧自动路径已排除系统卷/引导关键卷；改 0 可恢复全手动。 */
        { L"AutoAttach", 1 },
    };
    for (DWORD i = 0; i < 7; i++)
        RegSetValueExW(h, t[i].n, 0, REG_DWORD, (const BYTE*)&t[i].v, sizeof(DWORD));
    RegCloseKey(h);
}

int wmain(void)
{
    SC_HANDLE m, s;

    if (!IsUserAnAdmin()) { fwprintf(stderr, L"[X] run as Administrator\n"); return 1; }

    wprintf(L"== RAMBlockCache installer V10.2 ==\n");

    /* 1) 旧残留清理 */
    DeleteServiceIfExists();
    RegMultiSzRemove(CLS_BAD,   L"UpperFilters", L"RAMBlockCache");  /* 旧非标准 GUID（BUG-009） */
    RegMultiSzRemove(CLS_VOLUME,L"UpperFilters", L"RAMBlockCache");  /* 幂等重装 */
    Ok(L"stale state cleaned");

    /* [FIX A2] 签名预检：先于一切变更（复制/建服务/写 UpperFilters 之前）。
     * 未签名/签名链无效 -> 拒绝安装（0x7B 事故防线） */
    if (!VerifyDriverSignature()) return 1;
    wprintf(L"[i] if this is a self-signed test cert, make sure testsigning is ON\n");
    wprintf(L"    (bcdedit /enum {current} -^> look for testsigning Yes),\n");
    wprintf(L"    otherwise the kernel will still refuse to load it at boot.\n");

    /* 2) 驱动文件 */
    if (!CopyDriverFile()) return 1;

    /* 3) 服务（V10.2：BOOT_START + System Bus Extender 组；旧服务幂等升级） */
    m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!m) { Fail(L"open SCM"); return 1; }
    {
        WCHAR bin[MAX_PATH];
        GetSystemDirectoryW(bin, MAX_PATH);
        wcscat_s(bin, MAX_PATH, L"\\drivers\\");
        wcscat_s(bin, MAX_PATH, DRV_SYSNAME);
        s = CreateServiceW(m, SVC_NAME, SVC_NAME, SERVICE_ALL_ACCESS,
                           SERVICE_KERNEL_DRIVER, SERVICE_BOOT_START,
                           SERVICE_ERROR_NORMAL, bin, L"System Bus Extender",
                           NULL, NULL, NULL, NULL);
        if (!s && GetLastError() == ERROR_SERVICE_EXISTS)
            s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
        if (s) {
            /* 幂等升级：旧 demand-start 服务同步为 boot-start / 组 / error-normal */
            if (!ChangeServiceConfigW(s,
                                      SERVICE_KERNEL_DRIVER,
                                      SERVICE_BOOT_START,
                                      SERVICE_ERROR_NORMAL,
                                      NULL,                      /* binPath 保持不变 */
                                      L"System Bus Extender",
                                      NULL, NULL, NULL, NULL, NULL)) {
                Fail(L"ChangeServiceConfig (boot-start upgrade)");
            }
        }
        if (!s) { Fail(L"create service"); CloseServiceHandle(m); return 1; }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(m);
    SetDefaults();
    Ok(L"service created (BOOT_START, group=System Bus Extender)");

    /* 4) 标准卷类 UpperFilters 追加（volsnap 保留在原位） */
    if (RegMultiSzAppend(CLS_VOLUME, L"UpperFilters", L"RAMBlockCache"))
        { Fail(L"append Volume UpperFilters"); return 1; }
    Ok(L"Volume UpperFilters appended (volsnap kept)");

    wprintf(L"\n>>> DONE. Reboot ONCE to bring the filter into every volume stack\n");
    wprintf(L">>> (first mount needs one reboot; after that attach/detach is hot,\n");
    wprintf(L">>>  no reboot). Then run: ramboostctl volumes  /  ramboostctl attach <X:>\n");
    return 0;
}
