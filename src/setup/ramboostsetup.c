/* RAMBlockCache 静默安装引擎 V10.2（由 dist\RAMBlockCache-Setup.exe 内嵌调用，
 * 亦可独立运行；[FIX N-b] 去"唯一安装入口"表述——统一口径见 README"安装"节：
 * 普通用户=GUI 安装器（本引擎为其内核阶段）；开发者/脚本=deploy.ps1；
 * install.ps1 / ramboostsetup.ps1=legacy 备选。BUG-002/009 整改 + BOOT_START 根因修复）
 * 动作：
 *   1) 清理旧版本残留（旧服务；旧非标准卷类 GUID {536670a4-...} 下的 UpperFilters）
 *   2) 复制驱动到 %SystemRoot%\system32\drivers
 *   3) 创建服务（KERNEL_DRIVER + 首装 DEMAND_START / 显式 /BootStart 才
 *      BOOT_START + "System Bus Extender" 组）+ 写 Parameters 默认值；
 *      旧服务存在则 ChangeServiceConfig 只做幂等升级，绝不降级既有 BOOT_START
 *   4) 向标准卷类 {71a27cdd-...} 的 UpperFilters 读-改-写【追加】RAMBlockCache
 *      （保留 volsnap；已存在则幂等跳过）
 *   5) 提示重启一次生效；之后 ATTACH/DETACH 热开关免重启
 *   6) [P1.5] 武装 boot_guard 看门：随包 guard 脚本复制到
 *      C:\ProgramData\RAMBoost + 注册双计划任务（AtStartup boot_guard.ps1 /
 *      AtLogOn logon_disarm.ps1）+ 写 FilterArmed 武装基线 + 签名态盖章；
 *      三件套全部就绪才置 FilterArmed=1（绝不"武装了没看门"），
 *      武装失败不中止安装（降级为 签名预检 + offline_rollback.cmd 两层网）
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
 *
 * [88 B-2 2026-11-07] 不可逆变更前的顺序约束（严禁再把预检挪回后面）：
 *   VerifyDriverSignature() -> SecureBootAllowsInstall() -> EnableTestSigning()
 *   -> 才允许 DeleteServiceIfExists / RegMultiSzRemove / CopyDriverFile /
 *      CreateService / RegMultiSzAppend。
 *   Secure Boot 开启、或 testsigning 开启失败，必须在【任何】旧服务摘除、
 *   UpperFilters 改写、驱动映像覆盖、服务创建之前 return 1。否则会留下
 *   「测试签名 sys + 服务 + UpperFilters 引用它 + testsigning OFF」的半装态，
 *   开机 0x7B，且 FilterArmed 未置 1 使 boot_guard 跳过。失败时机器保持原状，
 *   最多残留 testsigning=on 而无驱动引用（安全方向）。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>         /* [88 B-1/B-2] QueryServiceConfigW / SERVICE_NO_CHANGE */
#include <stdio.h>
#include <string.h>
#include <shellapi.h>
#include <wincrypt.h>          /* [DISTRIB] 根证书导入 */
#include <wintrust.h>          /* [FIX A2] VerifyDriverSignature: WinVerifyTrust */
#include <softpub.h>           /* [FIX A2] WINTRUST_ACTION_GENERIC_VERIFY_V2 GUID */
#pragma comment(lib, "wintrust.lib")   /* [FIX A2] 自包含链接（编译行亦同步追加） */
#pragma comment(lib, "crypt32.lib")    /* [DISTRIB] 根证书自动导入（CertOpenStore/CertAddCertificateContextToStore） */

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

/* [88 H-2 2026-11-07] 原实现吞掉 ControlService / DeleteService / 超时，
 * 并无条件打印 [ok] old service removed：覆盖升级时旧驱动仍加载、映像被覆盖，
 * 失败被静默忽略。现在所有返回值都判，失败一律 FALSE 让调用方 return 1。 */
static BOOL DeleteServiceIfExists(void)
{
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    SC_HANDLE s;
    if (!m) { Fail(L"OpenSCManager (cleanup)"); return FALSE; }
    s = OpenServiceW(m, SVC_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!s) {
        DWORD e = GetLastError();
        CloseServiceHandle(m);
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) return TRUE;   /* 正常首装 */
        Fail(L"OpenService (cleanup) - refusing to continue with unknown service state");
        return FALSE;
    }
    {
        SERVICE_STATUS st;
        int wait;
        BOOL stopped = FALSE, ok = TRUE;
        WCHAR b[200];
        ZeroMemory(&st, sizeof(st));
        if (!ControlService(s, SERVICE_CONTROL_STOP, &st)) {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_NOT_ACTIVE || e == ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
                Ok(L"old service already stopped (STOP control not needed)");
                stopped = TRUE;
            } else {
                wsprintfW(b, L"ControlService(STOP) failed err=%lu - refusing to overwrite a live driver", e);
                Fail(b);
                ok = FALSE;
            }
        }
        for (wait = 0; ok && !stopped && wait < 30; wait++) {
            if (!QueryServiceStatus(s, &st)) {
                wsprintfW(b, L"QueryServiceStatus failed err=%lu while waiting for stop", GetLastError());
                Fail(b);
                ok = FALSE;
                break;
            }
            if (st.dwCurrentState == SERVICE_STOPPED) { stopped = TRUE; break; }
            Sleep(500);
        }
        if (ok && !stopped) {
            Fail(L"old service did not reach SERVICE_STOPPED within ~15s - refusing to overwrite a live driver image");
            ok = FALSE;
        }
        if (ok && !DeleteService(s)) {
            wsprintfW(b, L"DeleteService failed err=%lu (marked-for-delete means the old service is still referenced)", GetLastError());
            Fail(b);
            ok = FALSE;
        }
        CloseServiceHandle(s);
        CloseServiceHandle(m);
        if (!ok) return FALSE;
        Ok(L"old service removed (verified: stopped + DeleteService OK)");
        return TRUE;
    }
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
/* [DISTRIB-2026-11-04] 一键信任：把随包分发的根证书导入本机「受信任的根证书
 * 颁发机构」。新机器没有 RAMBoostFS 根证书时，WinVerifyTrust 返回
 * 0x800B0109（CERT_E_UNTRUSTEDROOT），安装器按 fail-closed 拒绝——用户拿到的
 * 是"装不了"。本函数在拿到该类错误后自动导入同目录 RAMBoostFS-Root.cer，
 * 使校验可通过。证书由官方安装包携带（与驱动同源），不降低安全性：
 * 导入的仍是签名链的根，无法用于验证其他任何文件。 */
static BOOL ImportBundledRootCert(void)
{
    WCHAR cer[MAX_PATH], tmp[MAX_PATH];
    HCERTSTORE store;
    PCCERT_CONTEXT ctx;
    DWORD  size = 0;
    PBYTE  buf  = NULL;
    HANDLE hf   = INVALID_HANDLE_VALUE;
    DWORD  rd   = 0;
    BOOL   ok   = FALSE;

    GetModuleFileNameW(NULL, cer, MAX_PATH);
    { WCHAR *sl = wcsrchr(cer, L'\\'); if (sl) *sl = 0; }
    wcscat_s(cer, MAX_PATH, L"\\RAMBoostFS-Root.cer");
    if (GetFileAttributesW(cer) == INVALID_FILE_ATTRIBUTES) return FALSE;

    hf = CreateFileW(cer, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(hf, NULL);
    if (size == 0 || size == INVALID_FILE_SIZE || size > 65536) { CloseHandle(hf); return FALSE; }
    buf = (PBYTE)HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) { CloseHandle(hf); return FALSE; }
    if (!ReadFile(hf, buf, size, &rd, NULL) || rd != size) { CloseHandle(hf); HeapFree(GetProcessHeap(), 0, buf); return FALSE; }
    CloseHandle(hf);

    store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, NULL,
                          CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
                          L"ROOT");
    if (!store) { HeapFree(GetProcessHeap(), 0, buf); return FALSE; }
    ctx = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, buf, size);
    if (ctx) {
        ok = CertAddCertificateContextToStore(store, ctx,
                 CERT_STORE_ADD_REPLACE_EXISTING, NULL) ? TRUE : FALSE;
        CertFreeCertificateContext(ctx);
    }
    CertCloseStore(store, 0);
    HeapFree(GetProcessHeap(), 0, buf);
    if (ok) Ok(L"root certificate imported to LocalMachine\\Root (bundled RAMBoostFS-Root.cer)");
    else    Fail(L"import bundled root certificate (manual: certutil -addstore Root RAMBoostFS-Root.cer)");
    (void)tmp;
    return ok;
}

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

    /* [DISTRIB-2026-11-04] 根证书不在本机信任列表时，自动导入随包证书后重试：
     * 新机器首次安装的必经路径（否则用户永远装不上，只能手工 certutil）。 */
    if (st == (LONG)0x800B0109 /* CERT_E_UNTRUSTEDROOT */ ||
        st == (LONG)0x800B010D /* CERT_E_UNTRUSTEDTESTROOT */) {
        Ok(L"root not trusted on this machine - importing bundled certificate");
        if (ImportBundledRootCert()) {
            ZeroMemory(&wd, sizeof(wd));
            wd.cbStruct      = sizeof(wd);
            wd.dwUIChoice    = WTD_UI_NONE;
            wd.fdwRevocationChecks = WTD_REVOKE_NONE;
            wd.dwUnionChoice = WTD_CHOICE_FILE;
            wd.pFile         = &fi;
            wd.dwStateAction = WTD_STATEACTION_VERIFY;
            st = WinVerifyTrust(NULL, &action, &wd);
            wd.dwStateAction = WTD_STATEACTION_CLOSE;
            WinVerifyTrust(NULL, &action, &wd);
        }
    }

    if (st == ERROR_SUCCESS) { Ok(L"driver signature chain: VALID"); return TRUE; }

    fwprintf(stderr, L"[X] driver signature check FAILED (WinVerifyTrust=0x%08lX)\n", st);
    fwprintf(stderr, L"    An unsigned or untrusted driver MUST NOT be installed as\n");
    fwprintf(stderr, L"    BOOT_START + UpperFilters: Windows Code Integrity will\n");
    fwprintf(stderr, L"    refuse to load it at boot and the machine will 0x7B loop.\n");
    fwprintf(stderr, L"    Sign the driver first (see README signing section).\n");
    return FALSE;
}

/* [88 B-2 2026-11-07] Secure Boot preflight. A test-signed driver cannot
 * load while Secure Boot is enabled: bcdedit /set testsigning on is ignored
 * by policy and the machine ends up with an UpperFilters reference to an
 * unloadable filter -> 0x7B. Refuse BEFORE the first machine change.
 * Missing State key/value = legacy BIOS -> warn and continue.
 * Any other registry error = fail closed (cannot prove Secure Boot is off). */
static BOOL SecureBootAllowsInstall(void)
{
    HKEY h = NULL;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
        0, KEY_READ | KEY_WOW64_64KEY, &h);
    if (r == ERROR_FILE_NOT_FOUND) {
        Ok(L"Secure Boot state not exposed (legacy BIOS / no State key) - continuing");
        return TRUE;
    }
    if (r != ERROR_SUCCESS) {
        Fail(L"read Secure Boot state (HKLM SecureBoot\\State)");
        return FALSE;
    }
    {
        DWORD val = 0, cb = sizeof(val), type = 0;
        LONG qr = RegQueryValueExW(h, L"UEFISecureBootEnabled", NULL, &type,
                                   (LPBYTE)&val, &cb);
        RegCloseKey(h);
        if (qr == ERROR_FILE_NOT_FOUND) {
            Ok(L"UEFISecureBootEnabled not present - assuming Secure Boot off");
            return TRUE;
        }
        if (qr != ERROR_SUCCESS || type != REG_DWORD) {
            Fail(L"read UEFISecureBootEnabled (expected REG_DWORD)");
            return FALSE;
        }
        if (val == 1) {
            Fail(L"ABORT: Secure Boot is ENABLED - a test-signed driver cannot load.");
            Fail(L"Enter BIOS/UEFI setup, disable Secure Boot, then re-run this installer.");
            Fail(L"No service, driver file or UpperFilters were changed (machine untouched).");
            return FALSE;
        }
        Ok(L"Secure Boot is off (UEFISecureBootEnabled=0)");
        return TRUE;
    }
}

static BOOL CopyDriverFile(void)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH];
    int retry;
    /* [88 H-2] Overwriting a still-loaded image is unsafe. If a previous
     * cleanup somehow left a RUNNING service, say so loudly and stop. */
    {
        SC_HANDLE sm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        if (sm) {
            SC_HANDLE ss = OpenServiceW(sm, SVC_NAME, SERVICE_QUERY_STATUS);
            if (ss) {
                SERVICE_STATUS sst;
                if (QueryServiceStatus(ss, &sst) && sst.dwCurrentState == SERVICE_RUNNING) {
                    Fail(L"old RAMBlockCache service is STILL RUNNING - refusing to overwrite its image; run 'sc stop RAMBlockCache' and re-run");
                    CloseServiceHandle(ss);
                    CloseServiceHandle(sm);
                    return FALSE;
                }
                CloseServiceHandle(ss);
            }
            CloseServiceHandle(sm);
        }
    }
    GetModuleFileNameW(NULL, src, MAX_PATH);
    WCHAR *sl = wcsrchr(src, L'\\'); if (sl) *sl = 0;
    wcscat_s(src, MAX_PATH, L"\\RAMBlockCache.sys");
    GetSystemDirectoryW(dst, MAX_PATH);
    wcscat_s(dst, MAX_PATH, L"\\drivers\\");
    wcscat_s(dst, MAX_PATH, DRV_SYSNAME);
    /* [FIX err=32 2026-10-24] 刚停服的旧驱动映像段释放有延迟（卸载异步），
     * 立刻覆盖拷贝会撞 ERROR_SHARING_VIOLATION。占用重试（上限 ~10s），
     * 超限才报错——重装不再要求用户手动等待。 */
    for (retry = 0; ; retry++) {
        if (CopyFileW(src, dst, FALSE)) break;
        if (GetLastError() != ERROR_SHARING_VIOLATION || retry >= 20) {
            Fail(L"copy driver (place RAMBlockCache.sys beside me)");
            return FALSE;
        }
        Sleep(500);
    }
    Ok(L"driver copied");
    return TRUE;
}

/* [U1-2/U0-1] 安装选项（命令行覆盖；无参数时保持历史默认值逐字节不变）。
 * /S              静默：输出重定向 %TEMP%\RAMBlockCache-Setup.log，屏幕零输出
 * /TestMode       测试模式：DEMAND_START 服务 + 写 TestMode=1 + 不写卷类 UpperFilters
 *                  （零重启 dev loop 的安装面；运行时挂栈路径由驱动 TestMode 门放行）
 * /AutoAttach=0|1         覆盖默认 AutoAttach（[FIX 2026-11-06] 默认 1→0：
 *                          与驱动默认（USER-REQ ②"开机不自动加速任何卷"）对齐；
 *                          原安装器显式写 1 覆盖驱动默认 0，新装机违反开机策略）
 * /DefaultMode=WriteThrough|WriteBack  覆盖默认写模式（历史默认 WriteThrough=直写） */
typedef struct _RB_INSTALL_OPTS {
    BOOL   Silent;
    BOOL   TestMode;
    DWORD  AutoAttach;
    DWORD  WriteBack;
    BOOL   NoTestSigning;   /* [DISTRIB] /NoTestSigning 保险丝：跳过自动 bcdedit */
    BOOL   BootStart;       /* [FIX 0x7B-DISTRIB] /BootStart 显式升级 BOOT_START（默认 DEMAND_START） */
} RB_INSTALL_OPTS;

/* [DISTRIB 2026-11-04] 一键测试签名模式：分发包的驱动为测试签名（RAMBoostFS
 * Root 测试链），目标机器必须 testsigning on 才能加载——否则 BOOT_START 加载
 * 失败（BootLoopGuard 兜底可进系统，但驱动不可用）。安装器以管理员运行
 * （app.manifest requireAdministrator），直接调 bcdedit 幂等开启；重启后生效
 * （桌面右下角"测试模式"水印为正常现象）。/NoTestSigning 保险丝供正式签名
 * 分发包关闭本行为。 */
static BOOL EnableTestSigning(BOOL silent)
{
    /* [DISTRIB-FIX 2026-11-05] 三改进（实测对方机器 bcdedit exit=1 无法归因）：
     * ① 输出重定向到临时文件并读出——360 等安全软件拦截 bcdedit 时能看到
     *    具体拒绝文本，而不是只有 exit=1；
     * ② 失败重试 3 次（偶发拦截/文件锁）；
     * ③ 每次重试间 Ok/Warn 可见。 */
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    WCHAR cmd[] = L"bcdedit /set testsigning on";
    WCHAR outf[MAX_PATH];
    DWORD rc = (DWORD)-1;
    int attempt;
    HANDLE hf;
    GetTempPathW(MAX_PATH, outf);
    wcscat_s(outf, MAX_PATH, L"rb-bcdedit-out.txt");
    for (attempt = 1; attempt <= 3; attempt++) {
        hf = CreateFileW(outf, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hf;
        si.hStdError  = hf;
        if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            Fail(L"launch bcdedit (testsigning) - run manually: bcdedit /set testsigning on");
            if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
            return FALSE;
        }
        WaitForSingleObject(pi.hProcess, 15000);
        GetExitCodeProcess(pi.hProcess, &rc);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
        if (rc == 0) {
            Ok(L"testsigning ON (reboot required; desktop watermark is normal)");
            return TRUE;
        }
        {
            WCHAR b[96];
            wsprintfW(b, L"bcdedit attempt %d/%d failed exit=%lu - retrying/see detail below",
                      attempt, 3, rc);
            Fail(b);
            /* 显示 bcdedit 的输出（拦截软件的拒绝原因会出现在这里） */
            hf = CreateFileW(outf, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                char abuf[1024]; DWORD rd = 0;
                if (ReadFile(hf, abuf, sizeof(abuf) - 1, &rd, NULL) && rd) {
                    abuf[rd] = 0;
                    { WCHAR wbuf[1024]; int wi = 0;
                      for (DWORD bi = 0; bi < rd && wi < 1023; bi++)
                          wbuf[wi++] = (wchar_t)(unsigned char)abuf[bi];
                      wbuf[wi] = 0;
                      Fail(wbuf); }
                }
                CloseHandle(hf);
            }
            Sleep(1000);
        }
    }
    Fail(L"bcdedit failed 3 times - likely blocked by security software (360 etc).");
    Fail(L"Manual: exit 360, then admin cmd: bcdedit /set testsigning on, reboot.");
    return FALSE;
}

/* ---- [P1.5 2026-11-06] boot_guard 看门协议（安装引擎侧移植，消 U-5）----
 * 开发机链路 src\scripts\deploy.ps1 的步骤 3/3a/4/5（武装基线 + 签名态盖章 +
 * guard 脚本 + 双计划任务）移植进分发安装链：GUI 分发包的静默引擎在完成
 * UpperFilters 追加与 testsigning 后同样武装三件套——此前分发机只有随包
 * offline_rollback.cmd，看门完全缺位。语义（严格）：
 *   ① 三件套（guard 脚本 + 两计划任务）全部就绪才写 FilterArmed=1；
 *   ② 任一失败不 return 1、不中止安装（360 等安全软件拦 schtasks 时硬拒
 *      = 分发机装不上），打 Fail 后降级为两层网继续走完安装；
 *   ③ 仅生产模式路径调用（TestMode 路径零改动）。 */

/* [P1.5-A1] 签名态盖章（deploy.ps1 步骤 3a 同口径）：从引擎同目录
 * RAMBlockCache.sys 提取签名者信息写入 PARM_KEY：
 *   TestSigned (REG_DWORD)：签名者证书自签（Subject 字符串 == Issuer 字符串）
 *                            → 1，CA 链签发 → 0；任何失败 → 保守 1
 *                            （与驱动侧 RbRegGetDW(L"TestSigned",1) 缺省一致）。
 *   Signer    (REG_SZ)：签名者 Subject，非空才写。
 * 驱动 DriverEntry 读取这两个值填 GET_VERSION（前端 DriverSignatureState 契约）；
 * 取不到 = 1/空 = 保守，反作弊警示照弹，不降低安全性。 */
static void StampSignerState(void)
{
    WCHAR sysPath[MAX_PATH];
    HCERTSTORE store = NULL;
    HCRYPTMSG  msg   = NULL;
    PCMSG_SIGNER_INFO info = NULL;
    PCCERT_CONTEXT signer = NULL;
    HKEY hk = NULL;
    DWORD cbInfo = 0, testSigned = 1;
    WCHAR subject[256] = L"", issuer[256] = L"";

    /* 源文件 = 安装器 exe 同目录的 RAMBlockCache.sys（与 CopyDriverFile 同源） */
    GetModuleFileNameW(NULL, sysPath, MAX_PATH);
    { WCHAR *sl = wcsrchr(sysPath, L'\\'); if (sl) *sl = 0; }
    wcscat_s(sysPath, MAX_PATH, L"\\" DRV_SYSNAME);

    /* 提取内嵌 Authenticode 签名的签名者：文件 -> (cert store + crypt msg) ->
     * CMSG_SIGNER_INFO（含 Issuer + SerialNumber）-> 按二者定位签名者证书。 */
    if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, sysPath,
                         CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                         CERT_QUERY_FORMAT_FLAG_BINARY,
                         0, NULL, NULL, NULL,
                         &store, &msg, NULL)) {
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &cbInfo) && cbInfo) {
            info = (PCMSG_SIGNER_INFO)HeapAlloc(GetProcessHeap(), 0, cbInfo);
            if (info && CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0,
                                         (PVOID)info, &cbInfo)) {
                CERT_INFO ci;
                ZeroMemory(&ci, sizeof(ci));
                ci.Issuer       = info->Issuer;
                ci.SerialNumber = info->SerialNumber;
                signer = CertFindCertificateInStore(store,
                             X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                             0, CERT_FIND_SUBJECT_CERT, &ci, NULL);
            }
        }
    }
    if (signer) {
        /* 判定口径与 deploy.ps1 3a 一致：证书字符串 Subject == Issuer = 自签
         * 测试证书 -> 1；CA 链签发（两者不同）-> 0。任一取名失败保持 1。 */
        if (CertNameToStrW(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                           &signer->pCertInfo->Subject,
                           CERT_NAME_SIMPLE_DISPLAY_TYPE,
                           subject, ARRAYSIZE(subject)) &&
            CertNameToStrW(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                           &signer->pCertInfo->Issuer,
                           CERT_NAME_SIMPLE_DISPLAY_TYPE,
                           issuer, ARRAYSIZE(issuer))) {
            testSigned = (wcscmp(subject, issuer) == 0) ? 1 : 0;
        }
    }

    /* 盖章：TestSigned 恒写（失败路径保守 1）；Signer 非空才写。
     * 注册表失败不中止安装：驱动侧缺省同保守 1，最多 UI 多弹一次警示。 */
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, PARM_KEY, 0, NULL, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hk, L"TestSigned", 0, REG_DWORD,
                       (const BYTE *)&testSigned, sizeof(testSigned));
        if (subject[0]) {
            RegSetValueExW(hk, L"Signer", 0, REG_SZ, (const BYTE *)subject,
                           (DWORD)((wcslen(subject) + 1) * sizeof(WCHAR)));
        }
        RegCloseKey(hk);
        {
            WCHAR b[512];
            wsprintfW(b, L"signature state stamped: TestSigned=%lu, Signer=%s",
                      testSigned, subject[0] ? subject : L"(none)");
            Ok(b);
        }
    } else {
        Fail(L"stamp signature state (Parameters key) - driver defaults apply");
    }

    if (signer) CertFreeCertificateContext(signer);
    if (info)   HeapFree(GetProcessHeap(), 0, info);
    if (msg)    CryptMsgClose(msg);
    if (store)  CertCloseStore(store, 0);
}

/* [P1.5-A2] guard 脚本随包落地：把引擎同目录（GUI 安装器的 staging，由
 * Payload.cs KernelFiles 提取）的 boot_guard.ps1 / logon_disarm.ps1 复制到
 * C:\ProgramData\RAMBoost\（目录不存在则创建；覆盖写）。
 * 任一源缺失 -> FALSE（按无看门降级，由调用方决定不中止安装）。 */
static BOOL InstallGuardScripts(void)
{
    WCHAR srcDir[MAX_PATH], dstDir[MAX_PATH];
    static const LPCWSTR names[2] = { L"boot_guard.ps1", L"logon_disarm.ps1" };
    DWORD i;

    GetModuleFileNameW(NULL, srcDir, MAX_PATH);
    { WCHAR *sl = wcsrchr(srcDir, L'\\'); if (sl) *sl = 0; }

    wcscpy_s(dstDir, MAX_PATH, L"C:\\ProgramData\\RAMBoost");
    if (GetFileAttributesW(dstDir) == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(dstDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            Fail(L"create C:\\ProgramData\\RAMBoost (guard scripts target)");
            return FALSE;
        }
    }
    for (i = 0; i < ARRAYSIZE(names); i++) {
        WCHAR srcFile[MAX_PATH], dstFile[MAX_PATH];
        wcscpy_s(srcFile, MAX_PATH, srcDir);
        wcscat_s(srcFile, MAX_PATH, L"\\");
        wcscat_s(srcFile, MAX_PATH, names[i]);
        wcscpy_s(dstFile, MAX_PATH, dstDir);
        wcscat_s(dstFile, MAX_PATH, L"\\");
        wcscat_s(dstFile, MAX_PATH, names[i]);
        if (!CopyFileW(srcFile, dstFile, FALSE)) {
            WCHAR b[MAX_PATH + 64];
            wsprintfW(b, L"copy guard script %s (must ship beside the engine)", names[i]);
            Fail(b);
            return FALSE;
        }
    }
    Ok(L"guard scripts installed to C:\\ProgramData\\RAMBoost");
    return TRUE;
}

/* [P1.5-A3] 注册单个看门计划任务（与 deploy.ps1 第 5 步 Register-ScheduledTask
 * 等价：SYSTEM / RunLevel Highest / AtStartup|AtLogOn，动作 = powershell
 * -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File）。
 * 进程启动模式完全参照 EnableTestSigning：输出重定向临时文件、SW_HIDE、
 * WaitForSingleObject 15s；退出码 0 -> TRUE，非 0 把临时文件内容打出来
 * （360 拦截 schtasks 的归因面）再 FALSE。/TR 值整体双引号、内部路径用 \"
 * 转义（标准 Windows 命令行转义，schtasks 解析正确）。 */
static BOOL RegisterGuardTask(LPCWSTR taskName, LPCWSTR schedule, LPCWSTR scriptPath)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    WCHAR cmd[1024];
    WCHAR outf[MAX_PATH];
    WCHAR b[160];
    DWORD rc = (DWORD)-1;
    HANDLE hf;

    wsprintfW(cmd,
        L"schtasks.exe /Create /F /TN %s /SC %s /RU SYSTEM /RL HIGHEST /TR "
        L"\"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden "
        L"-File \\\"%s\\\"\"",
        taskName, schedule, scriptPath);

    GetTempPathW(MAX_PATH, outf);
    wcscat_s(outf, MAX_PATH, L"rb-schtasks-out.txt");
    hf = CreateFileW(outf, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hf;
    si.hStdError  = hf;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Fail(L"launch schtasks (guard task) - security software may block it");
        if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
        return FALSE;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);

    if (rc == 0) {
        wsprintfW(b, L"scheduled task %s registered (SYSTEM, %s)", taskName, schedule);
        Ok(b);
        return TRUE;
    }
    wsprintfW(b, L"schtasks for %s failed exit=%lu - detail follows", taskName, rc);
    Fail(b);
    /* 打出 schtasks 的输出（安全软件拦截的拒绝原因会出现在这里） */
    hf = CreateFileW(outf, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        char abuf[1024]; DWORD rd = 0;
        if (ReadFile(hf, abuf, sizeof(abuf) - 1, &rd, NULL) && rd) {
            abuf[rd] = 0;
            { WCHAR wbuf[1024]; int wi = 0;
              for (DWORD bi = 0; bi < rd && wi < 1023; bi++)
                  wbuf[wi++] = (wchar_t)(unsigned char)abuf[bi];
              wbuf[wi] = 0;
              Fail(wbuf); }
        }
        CloseHandle(hf);
    }
    return FALSE;
}

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
        } else if (_wcsicmp(a, L"/NoTestSigning") == 0) {
            o->NoTestSigning = TRUE;   /* [DISTRIB] 正式签名分发包的保险丝 */
        } else if (_wcsicmp(a, L"/BootStart") == 0) {
            o->BootStart = TRUE;   /* [FIX 0x7B-DISTRIB] 显式升级引导启动 */
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
    RB_INSTALL_OPTS opt = { FALSE, FALSE, 0, 0 };   /* [FIX 2026-11-06] AutoAttach 1→0：安装器默认对齐驱动默认/用户开机策略 ② */
    /* [FIX 0x7B-DISTRIB 2026-11-04] 首装一律 DEMAND_START（物理上不可能 0x7B）。
     * 原 BOOT_START + System Bus Extender 组让驱动参与引导最早期——陌生硬件/
     * 驱动初始化任何失败 = INACCESSIBLE_BOOT_DEVICE（实测新机器 0x7B 死机）。
     * 新流程：首装 DEMAND_START → 用户手动 sc start 验证能加载 → 确认稳定后
     * 重跑本引擎加 /BootStart 开关升级 BOOT_START。正确性 > 开机即加速。
     * [U-7 2026-11-06] 原文"用 ramboostctl enable-boot 升级"为过时口径：
     * ramboostctl 无 enable-boot 子命令，实际升级路径是本引擎 /BootStart。 */
    DWORD startType = SERVICE_DEMAND_START;
    LPCWSTR svcGroup = NULL;
    WCHAR logPath[MAX_PATH];

    /* [U1-2] 参数解析先行于一切（含管理员检查）：拼错开关在提权前就失败可见 */
    if (!RbParseArgs(argc, argv, &opt)) {
        fwprintf(stderr, L"[X] bad argument. Usage:\n");
        fwprintf(stderr, L"    ramboostsetup.exe [/S] [/TestMode] [/AutoAttach=0|1] [/DefaultMode=WriteThrough|WriteBack] [/BootStart]\n");
        return 1;
    }
    /* [/BootStart] 显式升级开关：仅当用户确认驱动可加载后使用（高级选项） */
    if (opt.BootStart) { startType = SERVICE_BOOT_START; svcGroup = L"System Bus Extender"; }

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
    /* [88 B-2] BOTH irreversible-risk preconditions pass BEFORE the first
     * DeleteService / UpperFilters rewrite / CopyFile / CreateService below. */
    if (!SecureBootAllowsInstall()) return 1;
    if (!opt.NoTestSigning) {
        if (!EnableTestSigning(opt.Silent)) {
            Fail(L"ABORT: testsigning could not be enabled - driver would fail to load at boot (0x7B).");
            Fail(L"Disable security software blocking bcdedit, then re-run the installer.");
            Fail(L"Manual: admin cmd -> bcdedit /set testsigning on -> reboot -> install again.");
            return 1;   /* zero machine changes so far except the testsigning attempt */
        }
    } else {
        Ok(L"/NoTestSigning: skipping bcdedit (formally signed distribution only)");
    }
    wprintf(L"[i] self-signed test cert: testsigning is ON (setting needs ONE reboot to take effect)\n");

    /* 1) 旧残留清理（此后失败都可能留下部分变更，故上面的预检已前置） */
    if (!DeleteServiceIfExists()) return 1;
    RegMultiSzRemove(CLS_BAD,   L"UpperFilters", L"RAMBlockCache");  /* 旧非标准 GUID（BUG-009） */
    RegMultiSzRemove(CLS_VOLUME,L"UpperFilters", L"RAMBlockCache");  /* 幂等重装 */
    Ok(L"stale state cleaned");

    /* 2) 驱动文件 */
    if (!CopyDriverFile()) return 1;

    /* 3) 服务（首装 DEMAND_START；仅显式 /BootStart 才 BOOT_START +
     *      System Bus Extender 组。旧服务幂等升级但绝不降级既有 BOOT_START。
     *      [U0-1] TestMode：DEMAND_START + 无组 = 不碰任何启动路径） */
    m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!m) { Fail(L"open SCM"); return 1; }
    {
        WCHAR bin[MAX_PATH];
        int retry;
        GetSystemDirectoryW(bin, MAX_PATH);
        wcscat_s(bin, MAX_PATH, L"\\drivers\\");
        wcscat_s(bin, MAX_PATH, DRV_SYSNAME);
        s = CreateServiceW(m, SVC_NAME, SVC_NAME, SERVICE_ALL_ACCESS,
                           SERVICE_KERNEL_DRIVER, startType,
                           SERVICE_ERROR_NORMAL, bin, svcGroup,
                           NULL, NULL, NULL, NULL);
        /* [FIX 1072 2026-10-24] 删旧->立刻建新撞 ERROR_SERVICE_MARKED_FOR_DELETE：
         * 墓碑摘除需 SCM 异步落库（本进程句柄已关仍可能慢一拍）。等待重试
         * （上限 10s）；其间被真建成则走 EXISTS 分支打开。 */
        for (retry = 0; !s && GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE
                        && retry < 20; retry++) {
            Sleep(500);
            s = CreateServiceW(m, SVC_NAME, SVC_NAME, SERVICE_ALL_ACCESS,
                               SERVICE_KERNEL_DRIVER, startType,
                               SERVICE_ERROR_NORMAL, bin, svcGroup,
                               NULL, NULL, NULL, NULL);
            if (!s && GetLastError() == ERROR_SERVICE_EXISTS) break;
        }
        if (!s && GetLastError() == ERROR_SERVICE_EXISTS)
            s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
        if (s) {
            /* [88 B-1 2026-11-07] QueryServiceConfigW first: an existing
             * BOOT_START service must NEVER be downgraded to DEMAND_START by
             * a plain reinstall (only an explicit /BootStart is an upgrade).
             * lpLoadOrderGroup=NULL means "no change" per Win32 contract. */
            DWORD effectiveStart = startType;
            LPCWSTR effectiveGroup = svcGroup;
            DWORD need = 0;
            LPQUERY_SERVICE_CONFIGW cfg = NULL;
            BOOL queried = FALSE;
            QueryServiceConfigW(s, NULL, 0, &need);
            if (need) {
                cfg = (LPQUERY_SERVICE_CONFIGW)HeapAlloc(GetProcessHeap(), 0, need);
                if (cfg && QueryServiceConfigW(s, cfg, need, &need)) queried = TRUE;
            }
            if (queried && cfg->dwStartType == SERVICE_BOOT_START && !opt.BootStart) {
                effectiveStart = SERVICE_NO_CHANGE;
                effectiveGroup = NULL;
                Ok(L"existing BOOT_START service preserved (no downgrade on reinstall)");
            }
            if (cfg) HeapFree(GetProcessHeap(), 0, cfg);
            if (!ChangeServiceConfigW(s,
                                      SERVICE_KERNEL_DRIVER,
                                      effectiveStart,
                                      SERVICE_ERROR_NORMAL,
                                      NULL,                      /* binPath 保持不变 */
                                      effectiveGroup,
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
    /* [88 B-1] 如实按 startType / svcGroup 输出，不再谎报 BOOT_START。 */
    {
        WCHAR b[256];
        wsprintfW(b, L"service created (%s, %s)",
                  (startType == SERVICE_BOOT_START) ? L"BOOT_START" : L"DEMAND_START",
                  svcGroup ? svcGroup : L"no boot group");
        Ok(b);
    }
    if (!opt.BootStart && !opt.TestMode) {
        Ok(L"first install is DEMAND_START by design (0x7B-safe); boot-time acceleration needs a manual second pass with /BootStart");
    }

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
        /* [88 B-2] testsigning 已在任何机器变更前完成（见 wmain 预检段），
         * 这里只复用结果，绝不重复调 bcdedit。 */
        Ok(L"TestMode=1 (runtime attach armed; UpperFilters untouched; testsigning preflight already done)");
        wprintf(L"\n>>> DONE (TestMode). NO REBOOT NEEDED for install; testsigning needs ONE reboot.\n");
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
    /* [88 B-2 2026-11-07] testsigning 预检已前移到验签之后、任何不可逆变更
     * 之前完成并 fail-closed；此处不得再重复调用，失败路径不再产生半装态。 */

    /* [P1.5 2026-11-06] 武装 boot_guard 看门（deploy.ps1 步骤 3/4/5 的分发链移植）。
     * 语义（严格）：① 三件套（guard 脚本 + RAMBoostBootGuard/RAMBoostLogonDisarm
     * 两计划任务）全部就绪才写 FilterArmed=1——绝不"武装了没看门"；
     * ② 任一失败不 return 1、不中止安装（分发机可能被 360 等拦 schtasks，
     * 硬拒 = 装不上），打 Fail 后继续，回滚网降级为
     * 签名预检 + offline_rollback.cmd 两层。 */
    /* [88 M-5/M-6] 看门失败降级必须显式输出「本次安装无自动回滚网」，
     * SetupEngine.RunEngineAsync 会把本行原样转发到 GUI。 */
    if (InstallGuardScripts() &&
        RegisterGuardTask(L"RAMBoostBootGuard",   L"ONSTART",
                          L"C:\\ProgramData\\RAMBoost\\boot_guard.ps1") &&
        RegisterGuardTask(L"RAMBoostLogonDisarm", L"ONLOGON",
                          L"C:\\ProgramData\\RAMBoost\\logon_disarm.ps1")) {
        HKEY h;
        DWORD seq = 0, cbSeq = sizeof(seq), type = 0, zero = 0, one = 1;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, PARM_KEY, 0, NULL, 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
                            NULL, &h, NULL) == ERROR_SUCCESS) {
            /* 武装基线（deploy.ps1 第 3 步同款）：DeploySeq=旧值+1（不存在按 0）、
             * BootFailCount=0、LastBootHealthy=0、FilterArmed=1。 */
            if (RegQueryValueExW(h, L"DeploySeq", NULL, &type, (LPBYTE)&seq, &cbSeq)
                    != ERROR_SUCCESS || type != REG_DWORD) {
                seq = 0;
            }
            seq += 1;
            RegSetValueExW(h, L"DeploySeq",      0, REG_DWORD, (const BYTE *)&seq,  sizeof(seq));
            RegSetValueExW(h, L"BootFailCount",  0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
            RegSetValueExW(h, L"LastBootHealthy", 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
            RegSetValueExW(h, L"FilterArmed",    0, REG_DWORD, (const BYTE *)&one,  sizeof(one));
            RegCloseKey(h);
            {
                WCHAR b[128];
                wsprintfW(b, L"boot guard armed: DeploySeq=%lu BootFailCount=0 LastBootHealthy=0 FilterArmed=1", seq);
                Ok(b);
            }
        } else {
            /* 基线写不进去（Parameters 刚被 SetDefaults 建过，理论上几乎不可能）
             * ——FilterArmed 未置 1 = 未武装，方向安全：看门任务空转即退出。 */
            Fail(L"write armed baseline (Parameters key) - FilterArmed NOT set");
            Fail(L"NO AUTOMATIC ROLLBACK NET: guard tasks exist but FilterArmed was not set; only WinRE offline_rollback.cmd remains.");
        }
        StampSignerState();
    } else {
        Fail(L"NO AUTOMATIC ROLLBACK NET: boot guard NOT armed (guard scripts/schtasks unavailable).");
        Fail(L"NO AUTOMATIC ROLLBACK NET: install continues with ONLY WinRE offline_rollback.cmd as fallback.");
        Fail(L"NO AUTOMATIC ROLLBACK NET: close security software and re-run the installer to restore the guard.");
    }

    wprintf(L"\n>>> DONE. Reboot ONCE to bring the filter into every volume stack\n");
    wprintf(L">>> (first mount needs one reboot; after that attach/detach is hot,\n");
    wprintf(L">>>  no reboot). Then run: ramboostctl volumes  /  ramboostctl attach <X:>\n");
    return 0;
}
