/* RAMBlockCache 静默安装引擎 V10.2（由 dist\RAMBlockCache-Setup.exe 内嵌调用，
 * 亦可独立运行；[FIX N-b] 去"唯一安装入口"表述——统一口径见 README"安装"节：
 * 普通用户=GUI 安装器（本引擎为其内核阶段）；开发者/脚本=deploy.ps1；
 * install.ps1 / ramboostsetup.ps1=legacy 备选。BUG-002/009 整改 + BOOT_START 根因修复）
 * 动作：
 *   1) 清理旧版本残留（旧服务；旧非标准卷类 GUID {536670a4-...} 下的 UpperFilters）
 *   2) 复制驱动到 %SystemRoot%\system32\drivers
 *   3) 创建服务（KERNEL_DRIVER + BOOT_START + "System Bus Extender" 组）
 *      + 写 Parameters 默认值；旧服务存在则 ChangeServiceConfig 幂等升级
 *   4) 向标准卷类 {71a27cdd-...} 的 UpperFilters 读-改-写【追加】RAMBlockCache
 *      （保留 volsnap；已存在则幂等跳过）
 *   5) 提示重启一次生效；之后 ATTACH/DETACH 热开关免重启
 * [U1-2/U0-1] 命令行（纯增量；无参数时行为与历史一致）：
 *   /S                       静默安装（输出写入 %TEMP%\RAMBlockCache-Setup.log）
 *   /TestMode                测试模式：DEMAND_START + TestMode=1 + 不写 UpperFilters
 *                            （零重启 dev loop；驱动运行时挂栈路径的安装面）
 *   /AutoAttach=0|1          覆盖 AutoAttach 默认（历史默认 1）
 *   /DefaultMode=WriteThrough|WriteBack  覆盖写模式默认（历史默认 WriteThrough）
 *   未知参数一律拒绝（fail-closed）。
 * 编译：cl /W4 /O2 /utf-8 ramboostsetup.c /link advapi32.lib wintrust.lib shell32.lib user32.lib
 *        （wintrust.lib 供 [FIX A2] 签名预检；pragma comment 已冗余保险；
 *          /utf-8 防中文注释 C4819；shell32/user32 供 IsUserAnAdmin 与退出码路径）
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
    WCHAR stackbuf[1024]; WCHAR *buf = stackbuf; DWORD i, n, len, heapCap = 0;

    len = (DWORD)wcslen(entry) + 1;   /* [FIX U0-9] 提前计算：堆分配需预留追加余量 */

    r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, NULL, 0,
                        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &h, NULL);
    if (r) return r;
    cb = sizeof(stackbuf);
    r = RegQueryValueExW(h, val, NULL, &type, (LPBYTE)buf, &cb);
    if (r == ERROR_MORE_DATA) {
        /* [FIX R2-W11] 既有值超过栈缓冲：按真实尺寸堆分配重读——原代码把
         * MORE_DATA 当"空值"整值覆写，会删光 volsnap 等既有条目（比 C6 原病更糟）
         * [FIX U0-9] 分配量加追加余量：原精确分配使 cap==现有字符数，任何追加
         * 都恒 ERROR_MORE_DATA（>2KB 既有 UpperFilters 的追加路径整体失效）。 */
        DWORD alloc = cb + len * sizeof(WCHAR);
        buf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, alloc);
        if (!buf) { RegCloseKey(h); return ERROR_NOT_ENOUGH_MEMORY; }
        heapCap = alloc / sizeof(WCHAR);
        r = RegQueryValueExW(h, val, NULL, &type, (LPBYTE)buf, &cb);
        if (r == ERROR_MORE_DATA) {
            /* 值在两次查询间变大：保守拒绝（不截断不覆写） */
            HeapFree(GetProcessHeap(), 0, buf);
            RegCloseKey(h); return ERROR_MORE_DATA;
        }
    }
    if (r == ERROR_FILE_NOT_FOUND) { buf[0] = 0; buf[1] = 0; cb = 0; }
    else if (r != ERROR_SUCCESS || type != REG_MULTI_SZ) {
        /* [FIX R2-W11] 既有值存在但非 MULTI_SZ：拒绝而非覆写（防整值毁坏） */
        if (buf != stackbuf) HeapFree(GetProcessHeap(), 0, buf);
        RegCloseKey(h);
        return (r == ERROR_SUCCESS) ? ERROR_INVALID_DATA : r;
    }
    cap = (buf == stackbuf) ? 1024 : heapCap;

    /* 查重（以读得字符数为硬界，损坏值不越界） */
    n = cb / sizeof(WCHAR);
    for (i = 0; i < n && buf[i]; i += (DWORD)wcslen(buf + i) + 1)
        if (_wcsicmp(buf + i, entry) == 0) {
            if (buf != stackbuf) HeapFree(GetProcessHeap(), 0, buf);
            RegCloseKey(h); return ERROR_SUCCESS;
        }

    /* len 已在函数头计算（[FIX U0-9] 堆分配预留追加余量需要） */
    n = cb / sizeof(WCHAR);
    /* [FIX C6] RegQueryValueExW 返回的 cb 含结尾双 NUL：追加必须覆盖第一个终止 NUL，
     * 否则新条目落在终止符之后、枚举永远不可见（UpperFilters 追加静默失效）
     * [FIX #18] 已存在的空 MULTI_SZ（cb=4, buf="\0\0"）：n-=1 会留下一个前导
     * 空串（\0entry\0\0）——PnP 枚举到空前缀，行为未定义。空值按 n=0 处理。 */
    if (n > 0 && buf[0] == 0) { n = 0; }
    else if (n > 0) { n -= 1; }
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

/* [U1-2/U0-1] 安装选项（命令行覆盖；无参数时保持历史默认值逐字节不变）。
 * /S              静默：输出重定向 %TEMP%\RAMBlockCache-Setup.log，屏幕零输出
 * /TestMode       测试模式：DEMAND_START 服务 + 写 TestMode=1 + 不写卷类 UpperFilters
 *                  （零重启 dev loop 的安装面；运行时挂栈路径由驱动 TestMode 门放行）
 * /AutoAttach=0|1         覆盖默认 AutoAttach（历史默认 1）
 * /DefaultMode=WriteThrough|WriteBack  覆盖默认写模式（历史默认 WriteThrough=直写） */
typedef struct _RB_INSTALL_OPTS {
    BOOL   Silent;
    BOOL   TestMode;
    DWORD  AutoAttach;
    DWORD  WriteBack;
} RB_INSTALL_OPTS;

/* 参数解析：未知参数/非法值一律拒绝（fail-closed：不静默忽略拼写错误，
 * 批量静默安装时一个拼错的开关必须失败可见，而不是装出一个非预期默认态）。
 * 例外：以 .exe 结尾的参数视为【启动器自引用】跳过——UAC 提权重启 /
 * runas / 部分沙箱包装器会把目标 exe 路径当作首个参数传入（本机沙箱实测）。
 * 真实开关一律 '/' 前缀且不会以 .exe 结尾，该豁免不削弱拼写错拼的拒绝。 */
static BOOL RbParseArgs(int argc, LPWSTR *argv, RB_INSTALL_OPTS *o)
{
    int i;
    for (i = 1; i < argc; i++) {
        LPCWSTR a = argv[i];
        size_t alen = wcslen(a);
        if (alen > 4 && _wcsicmp(a + alen - 4, L".exe") == 0) continue;   /* 启动器自引用 */
        if (_wcsicmp(a, L"/S") == 0) {
            o->Silent = TRUE;
        } else if (_wcsicmp(a, L"/TestMode") == 0) {
            o->TestMode = TRUE;
        } else if (_wcsnicmp(a, L"/AutoAttach=", 12) == 0) {
            LPCWSTR v = a + 12;
            if (wcscmp(v, L"0") == 0) o->AutoAttach = 0;
            else if (wcscmp(v, L"1") == 0) o->AutoAttach = 1;
            else return FALSE;
        } else if (_wcsnicmp(a, L"/DefaultMode=", 13) == 0) {
            LPCWSTR v = a + 13;
            if (_wcsicmp(v, L"WriteThrough") == 0) o->WriteBack = 0;
            else if (_wcsicmp(v, L"WriteBack") == 0) o->WriteBack = 1;
            else return FALSE;
        } else {
            return FALSE;
        }
    }
    return TRUE;
}

/* [FIX #17] SetDefaults 的注册表写失败码（ERROR_SUCCESS=无失败）：wmain 侧
 * 记一行日志——配置缺省写失败不阻断驱动安装主体，但绝不静默。 */
static LONG g_lastRegFail = ERROR_SUCCESS;

/* [U1-2] 默认值可被 /AutoAttach= 与 /DefaultMode= 覆盖（无参数时即下表历史值）。 */
static void SetDefaults(DWORD autoAttach, DWORD writeBack)
{
    HKEY h; LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, PARM_KEY, 0, NULL, 0,
                                     KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &h, NULL);
    if (r) return;
    /* [BUG3] 开机自动挂载数据卷（旧审计"服务创建后写 AutoAttach=1"意图丢失项：
     * 缺省时每次开机冷缓存，用户实测"挂载后与裸盘跑分一致"）。
     * 驱动侧自动路径已排除系统卷/引导关键卷；改 0 可恢复全手动。
     * [FIX #17] 三处整改：
     *  ① 循环上界改 ARRAYSIZE(t)（原硬编码 7——增删默认项即静默错位/漏写）；
     *  ② RegSetValueExW 返回值累积检查（原全忽略——写失败无声）；
     *  ③ 仅写"不存在"的键（原无条件覆写——用户调过的 CacheSizeMB/
     *     FlushIntervalMs/DirtyHighPct 等一次重装全丢）。安装语义=补缺省，
     *     不是重置配置；要重置请删注册表值后重装。 */
    struct { LPCWSTR n; DWORD v; } t[] = {
        { L"CacheSizeMB", 0 }, { L"WriteBack", writeBack }, { L"SystemVolumeCached", 0 },
        { L"FlushIntervalMs", 60000 }, { L"DirtyHighPct", 60 }, { L"DirtyLowPct", 40 },
        { L"AutoAttach", autoAttach },
    };
    LONG fr = ERROR_SUCCESS;
    for (DWORD i = 0; i < ARRAYSIZE(t); i++) {
        DWORD exist = 0, cb = sizeof(exist), type = 0;
        if (RegQueryValueExW(h, t[i].n, NULL, &type, (LPBYTE)&exist, &cb) == ERROR_SUCCESS)
            continue;                       /* 已存在（用户配置/上次安装）：不覆写 */
        LONG r = RegSetValueExW(h, t[i].n, 0, REG_DWORD, (const BYTE*)&t[i].v, sizeof(DWORD));
        if (r != ERROR_SUCCESS) fr = r;
    }
    RegCloseKey(h);
    /* 返回值上报：wmain 侧记日志（配置写失败不影响驱动安装主体，但必须可见） */
    if (fr != ERROR_SUCCESS) g_lastRegFail = fr;
}

int wmain(int argc, LPWSTR *argv)
{
    SC_HANDLE m, s;
    RB_INSTALL_OPTS opt = { FALSE, FALSE, 1, 0 };
    DWORD startType = SERVICE_BOOT_START;
    LPCWSTR svcGroup = L"System Bus Extender";
    WCHAR logPath[MAX_PATH];

    /* [U1-2] 参数解析先行于一切（含管理员检查）：拼错开关在提权前就失败可见 */
    if (!RbParseArgs(argc, argv, &opt)) {
        fwprintf(stderr, L"[X] bad argument. Usage:\n");
        fwprintf(stderr, L"    ramboostsetup.exe [/S] [/TestMode] [/AutoAttach=0|1] [/DefaultMode=WriteThrough|WriteBack]\n");
        return 1;
    }

    /* [U1-2] 静默模式：stdout/stderr 追加重定向到 %TEMP%\RAMBlockCache-Setup.log
     * （无人值守批次安装的归因面；既有全部打印语句零改动流入日志）。 */
    if (opt.Silent) {
        FILE *lf = NULL, *le = NULL;
        logPath[0] = L'\0';
        GetTempPathW(MAX_PATH, logPath);
        wcscat_s(logPath, MAX_PATH, L"RAMBlockCache-Setup.log");
        if (_wfreopen_s(&lf, logPath, L"a", stdout) == 0 && lf) {
            setvbuf(stdout, NULL, _IOLBF, 0);
            _wfreopen_s(&le, logPath, L"a", stderr);
        }
    }

    /* [U0-1] 测试模式：DEMAND_START + 无加载组（boot 组仅 BOOT_START 有意义） */
    if (opt.TestMode) { startType = SERVICE_DEMAND_START; svcGroup = NULL; }

    if (!IsUserAnAdmin()) { fwprintf(stderr, L"[X] run as Administrator\n"); return 1; }

    wprintf(L"== RAMBlockCache installer V10.2 ==\n");

    /* [FIX A2 + FIX 严重-1 2026-10-18] 签名预检：必须先于一切变更。
     * 原位置在"旧残留清理"之后——未签名 sys 喂进来时旧安装已被先行拆除
     * （DeleteServiceIfExists + 两处 RegMultiSzRemove），返回 1 虽不砖机
     * （摘过滤器是安全方向）但"失败零副作用"不成立：安装失败且原功能消失。
     * 现移到最前：验签不过，机器保持原状。 */
    if (!VerifyDriverSignature()) return 1;
    wprintf(L"[i] if this is a self-signed test cert, make sure testsigning is ON\n");
    wprintf(L"    (bcdedit /enum {current} -^> look for testsigning Yes),\n");
    wprintf(L"    otherwise the kernel will still refuse to load it at boot.\n");

    /* 1) 旧残留清理 */
    DeleteServiceIfExists();
    RegMultiSzRemove(CLS_BAD,   L"UpperFilters", L"RAMBlockCache");  /* 旧非标准 GUID（BUG-009） */
    RegMultiSzRemove(CLS_VOLUME,L"UpperFilters", L"RAMBlockCache");  /* 幂等重装 */
    Ok(L"stale state cleaned");

    /* 2) 驱动文件 */
    if (!CopyDriverFile()) return 1;

    /* 3) 服务（V10.2：BOOT_START + System Bus Extender 组；旧服务幂等升级。
     *      [U0-1] TestMode：DEMAND_START + 无组 = 不碰任何启动路径） */
    m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!m) { Fail(L"open SCM"); return 1; }
    {
        WCHAR bin[MAX_PATH];
        GetSystemDirectoryW(bin, MAX_PATH);
        wcscat_s(bin, MAX_PATH, L"\\drivers\\");
        wcscat_s(bin, MAX_PATH, DRV_SYSNAME);
        s = CreateServiceW(m, SVC_NAME, SVC_NAME, SERVICE_ALL_ACCESS,
                           SERVICE_KERNEL_DRIVER, startType,
                           SERVICE_ERROR_NORMAL, bin, svcGroup,
                           NULL, NULL, NULL, NULL);
        if (!s && GetLastError() == ERROR_SERVICE_EXISTS)
            s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
        if (s) {
            /* 幂等升级：旧 demand-start 服务同步为 boot-start / 组 / error-normal
             * （TestMode 反向同步为 demand-start / 无组） */
            if (!ChangeServiceConfigW(s,
                                      SERVICE_KERNEL_DRIVER,
                                      startType,
                                      SERVICE_ERROR_NORMAL,
                                      NULL,                      /* binPath 保持不变 */
                                      svcGroup,
                                      NULL, NULL, NULL, NULL, NULL)) {
                Fail(L"ChangeServiceConfig (start-type sync)");
            }
        }
        if (!s) { Fail(L"create service"); CloseServiceHandle(m); return 1; }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(m);
    SetDefaults(opt.AutoAttach, opt.WriteBack);
    /* [FIX #17] 配置缺省写失败可见化（原 RegSetValueExW 返回值全忽略） */
    if (g_lastRegFail != ERROR_SUCCESS) {
        WCHAR b[128];
        wsprintfW(b, L"config defaults partially failed (last error %lu) - defaults not fully written",
                  g_lastRegFail);
        Ok(b);   /* 用 Ok 通道保证输出不丢；不 Fail——驱动服务已装好，缺省可补 */
    }
    Ok(opt.TestMode ? L"service created (DEMAND_START, no boot group)"
                    : L"service created (BOOT_START, group=System Bus Extender)");

    if (opt.TestMode) {
        /* [U0-1] 测试模式注册表开关：驱动 RbAttachVolumeRuntime 的总门（默认关）。
         * 同时【不写】卷类 UpperFilters——测试模式彻底断 0x7B 路径，
         * 过滤设备只由运行时挂栈按需创建（sc start 不为既有卷触发 AddDevice）。 */
        HKEY h;
        DWORD one = 1;
        LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, PARM_KEY, 0, NULL, 0,
                                 KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &h, NULL);
        if (!r) {
            RegSetValueExW(h, L"TestMode", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
            RegCloseKey(h);
        }
        Ok(L"TestMode=1 (runtime attach armed; UpperFilters untouched)");
        wprintf(L"\n>>> DONE (TestMode). NO REBOOT NEEDED.\n");
        wprintf(L">>> Then:  sc start RAMBlockCache   /   ramboostctl attach <X:>\n");
        wprintf(L">>> Uninstall = sc stop (clean unload, dirty flushed). Production mode:\n");
        wprintf(L">>> re-run this installer WITHOUT /TestMode (one reboot then).\n");
        return 0;
    }

    /* 4) 标准卷类 UpperFilters 追加（volsnap 保留在原位） */
    /* [FIX U0-6] 生产模式显式清 TestMode=0：原实现只在 /TestMode 时写 1、从不清零——
     * 测试机（/TestMode）复装生产包后注册表残留 TestMode=1，把"生产不可达"的
     * 运行时挂栈路径在生产环境重新打开。写 0 而非删值（防旧值类型陷阱）。 */
    {
        HKEY h; DWORD zero = 0;
        LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, PARM_KEY, 0, NULL, 0,
                                 KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &h, NULL);
        if (!r) {
            RegSetValueExW(h, L"TestMode", 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
            RegCloseKey(h);
        }
    }
    if (RegMultiSzAppend(CLS_VOLUME, L"UpperFilters", L"RAMBlockCache"))
        { Fail(L"append Volume UpperFilters"); return 1; }
    Ok(L"Volume UpperFilters appended (volsnap kept)");

    wprintf(L"\n>>> DONE. Reboot ONCE to bring the filter into every volume stack\n");
    wprintf(L">>> (first mount needs one reboot; after that attach/detach is hot,\n");
    wprintf(L">>>  no reboot). Then run: ramboostctl volumes  /  ramboostctl attach <X:>\n");
    return 0;
}
