// [142 拆分 2026-11-10] ramboostctl.c 拆分（B3+/134 §6）：本 TU 为 B-6 signature（WinVerifyTrust）+ CliPrintWide。纯搬运，零语义改动。
#include "ctl_internal.h"

/* ---------- [契约 B-6] signature：驱动签名态（WinVerifyTrust） ---------- */
/* 判定口径（与反作弊警示的保守分支对齐，写死在注释里）：
 *   - WinVerifyTrust 失败（含 TRUST_E_NOSIGNATURE 无签名）→ testSigned=1（保守）；
 *   - 成功但证书链根不受信/仅自签名/部分链 → testSigned=1（测试根特征）；
 *   - 成功且链根受信 → testSigned=0，signer=签名者主体名。
 *   驱动可达时把实测结果写 HKLM\...\RAMBlockCache\Parameters（TestSigned/Signer），
 *   供 GET_VERSION 的 RB_VERSION.TestSigned/Signer 在下次驱动加载时反映（B-6 前端
 *   契约 {testSigned, signer} 的注册表面来源；驱动侧默认值保守=1）。 */
/* ---------- [契约 B-6] signature：驱动签名态（WinVerifyTrust 实测） ---------- */
/* [FIX F-1] 宽字符安全输出：printf("%ls") 在 stdout 重定向（管道/文件）时按 C
 * 本地域转换宽串，遇中文路径首个非 ASCII 字符即截断（实测 D:\大模型\... 显示成
 * D:\）。本助手：控制台附着→WriteConsoleW（原生宽字符，中文完整）；重定向→
 * WideCharToMultiByte(CP_ACP) 转窄串后 printf（中文系统 CP_ACP 可表示中文）。
 * 仅用于签名态展示；协议结构化行保持 %ls（机器消费不受影响）。 */
void CliPrintWide(const WCHAR *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  n = 0;
    if (s == NULL) s = L"";
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &n)) {
        DWORD written = 0;
        WriteConsoleW(h, s, (DWORD)wcslen(s), &written, NULL);
        return;
    }
    {
        int need = WideCharToMultiByte(CP_ACP, 0, s, -1, NULL, 0, NULL, NULL);
        char *buf = (char *)malloc(need > 0 ? (size_t)need : 1);
        if (buf) {
            if (need > 0 && WideCharToMultiByte(CP_ACP, 0, s, -1, buf, need, NULL, NULL) > 0)
                fputs(buf, stdout);
            else
                fputs("(unprintable)", stdout);
            free(buf);
        }
    }
}

int cli_signature(int argc, char **argv)
{
    WCHAR path[MAX_PATH];
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO wf;
    WINTRUST_DATA wd;
    LONG tr;
    BOOL testSigned = TRUE;
    WCHAR signer[256];
    HCERTSTORE hStore = NULL;
    HCRYPTMSG hMsg = NULL;
    HKEY k = NULL;
    DWORD disp;

    signer[0] = L'\0';
    if (argc >= 3) {
        /* [FIX F-1] argv 是 ANSI 码页字节（main 的窄参），必须按 CP_ACP 转宽串；
         * 原 CP_UTF8 把 GBK 字节当 UTF-8 解析 → 中文路径断在首个多字节序列
         * （实测 D:\大模型\... 显示成 D:\ 且 WinVerifyTrust 验错文件）。 */
        if (!MultiByteToWideChar(CP_ACP, 0, argv[2], -1, path, MAX_PATH)) {
            fprintf(stderr, "[错误] 路径转换失败\n");
            return 2;
        }
    } else {
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 32) {
            fprintf(stderr, "[错误] 取系统目录失败\n");
            return 1;
        }
        wcscat(path, L"\\drivers\\RAMBlockCache.sys");
    }

    memset(&wf, 0, sizeof(wf));
    wf.cbStruct = sizeof(wf);
    wf.pcwszFilePath = path;
    memset(&wd, 0, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;   /* 离线机器不联网吊销检查 */
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &wf;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    tr = WinVerifyTrust(NULL, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;   /* 释放 WinVerifyTrust 状态数据 */
    WinVerifyTrust(NULL, &action, &wd);

    if (tr == ERROR_SUCCESS) {
        /* 取签名者证书（驱动经目录签名：PKCS7 内嵌或分离） */
        if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, path,
                             CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED |
                             CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
                             CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                             NULL, NULL, NULL, &hStore, &hMsg, NULL) && hMsg && hStore) {
            DWORD cb = 0;
            CMSG_SIGNER_INFO *si = NULL;
            if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &cb) && cb > 0) {
                si = (CMSG_SIGNER_INFO *)malloc(cb);
                if (si && CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, si, &cb)) {
                    CERT_INFO ci;
                    PCCERT_CONTEXT cert;
                    ci.Issuer = si->Issuer;
                    ci.SerialNumber = si->SerialNumber;
                    cert = CertFindCertificateInStore(hStore,
                        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                        CERT_FIND_SUBJECT_CERT, &ci, NULL);
                    if (cert) {
                        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
                                           signer, 256);
                        {
                            CERT_CHAIN_PARA para;
                            PCCERT_CHAIN_CONTEXT chain = NULL;
                            memset(&para, 0, sizeof(para));
                            para.cbSize = sizeof(para);
                            if (CertGetCertificateChain(NULL, cert, NULL, NULL, &para,
                                                        0, NULL, &chain) && chain) {
                                DWORD err = chain->TrustStatus.dwErrorStatus;
                                if (err & (CERT_TRUST_IS_UNTRUSTED_ROOT |
                                           CERT_TRUST_IS_PARTIAL_CHAIN |
                                           CERT_TRUST_IS_CYCLIC))
                                    testSigned = TRUE;    /* 测试根/部分链：保守 */
                                else if (chain->cChain <= 1 && chain->rgpChain[0]->cElement <= 1)
                                    testSigned = TRUE;    /* 仅自签名：保守 */
                                else
                                    testSigned = FALSE;   /* 链根受信：正式签名 */
                                CertFreeCertificateChain(chain);
                            } /* 取链失败保持保守 TRUE */
                        }
                        CertFreeCertificateContext(cert);
                    } /* 找不到签名者证书保持保守 TRUE */
                }
                if (si) free(si);
            }
        } /* 无证书信息保持保守 TRUE */
        if (hStore) CertCloseStore(hStore, 0);
        if (hMsg) CryptMsgClose(hMsg);
    } else {
        /* 未签名/签名无效/不受信：保守判测试签名；仍尝试取签名者名（可能有证书但链无效） */
        if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, path,
                             CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED |
                             CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
                             CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                             NULL, NULL, NULL, &hStore, &hMsg, NULL) && hMsg && hStore) {
            DWORD cb = 0;
            CMSG_SIGNER_INFO *si = NULL;
            if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &cb) && cb > 0) {
                si = (CMSG_SIGNER_INFO *)malloc(cb);
                if (si && CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, si, &cb)) {
                    CERT_INFO ci;
                    PCCERT_CONTEXT cert;
                    ci.Issuer = si->Issuer;
                    ci.SerialNumber = si->SerialNumber;
                    cert = CertFindCertificateInStore(hStore,
                        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                        CERT_FIND_SUBJECT_CERT, &ci, NULL);
                    if (cert) {
                        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
                                           signer, 256);
                        CertFreeCertificateContext(cert);
                    }
                }
                if (si) free(si);
            }
            if (hStore) CertCloseStore(hStore, 0);
            if (hMsg) CryptMsgClose(hMsg);
        }
    }

    /* 驱动可达 → 写注册表供 GET_VERSION 反映（下次加载生效；已在跑的驱动不热更） */
    if (rb_connect() == 0) {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Services\\RAMBlockCache\\Parameters",
                0, NULL, 0, KEY_SET_VALUE, NULL, &k, &disp) == ERROR_SUCCESS) {
            DWORD ts = testSigned ? 1u : 0u;
            RegSetValueExW(k, L"TestSigned", 0, REG_DWORD, (BYTE *)&ts, sizeof(ts));
            RegSetValueExW(k, L"Signer", 0, REG_SZ, (BYTE *)signer,
                           (DWORD)(wcslen(signer) + 1) * sizeof(WCHAR));
            RegCloseKey(k);
            printf("已写入驱动注册表（TestSigned/Signer）：GET_VERSION 在下次驱动加载时反映。\n");
        }
    }

    printf("驱动签名态（B-6）\n");
    printf("  文件: ");
    CliPrintWide(path);
    printf("\n");
    printf("  WinVerifyTrust: %s\n", tr == ERROR_SUCCESS ? "通过" : "未通过（保守判测试签名）");
    printf("  testSigned: %s   signer: ",
           testSigned ? "是（测试签名/未知——反作弊警示将弹出）" : "否（正式签名）");
    if (signer[0]) CliPrintWide(signer); else printf("（未取到）");
    printf("\n");
    printf("signature testSigned=%d signer=\"%ls\"\n", testSigned ? 1 : 0,
           signer[0] ? signer : L"");
    return 0;
}

/* ---------- [237 2026-11-14] 只读 Authenticode 验签（T4 取证用） ---------- */
/* 与 cli_signature 的区别：不写注册表、不取签名者证书、不连驱动，只回答
 * 「这个文件此刻能否被 WinVerifyTrust 判为 Valid」。cli_signature 会写
 * HKLM\...\Parameters，因此不能用在 RB-t4-signature-verify.cmd 的只读取证里。
 *
 * 关键：显式以 FILE_SHARE_READ|WRITE|DELETE 打开并把句柄交给
 * WINTRUST_FILE_INFO.hFile。不传 hFile 时 WinVerifyTrust 自行按路径重开，
 * 共享模式不可控；文件被驱动或其它实例持有时会以共享冲突失败，上层只看到
 * UNREADABLE。注意 T4 根因尚未证实，此路径只是提供一条不经过 PowerShell
 * 的独立证据，不宣称已闭环 T4。
 *
 * 严格只读：GENERIC_READ + OPEN_EXISTING，无写、无 IOCTL、无注册表。
 * 退出码：0=Valid 1=Invalid 2=Unreadable/Missing/usage。 */
int cli_sigverify(int argc, char **argv)
{
    WCHAR path[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    WINTRUST_FILE_INFO fi;
    WINTRUST_DATA wd;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG st;

    if (argc < 3 || !argv[2][0]) {
        fprintf(stderr, "usage: ramboostctl sigverify <path>\n");
        return 2;
    }
    /* argv 是 CP_ACP 窄参（见本文件 FIX F-1），不能用 CP_UTF8。 */
    if (!MultiByteToWideChar(CP_ACP, 0, argv[2], -1, path, MAX_PATH)) {
        printf("status=Unreadable winerr=%lu hr=0x80070057\n",
               (unsigned long)GetLastError());
        return 2;
    }

    hFile = CreateFileW(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("status=Unreadable winerr=%lu hr=0x00000000\n",
               (unsigned long)GetLastError());
        return 2;
    }

    memset(&fi, 0, sizeof(fi));
    fi.cbStruct = sizeof(WINTRUST_FILE_INFO);
    fi.pcwszFilePath = path;
    fi.hFile = hFile;
    fi.pgKnownSubject = NULL;

    memset(&wd, 0, sizeof(wd));
    wd.cbStruct = sizeof(WINTRUST_DATA);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    st = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    CloseHandle(hFile);

    if (st == ERROR_SUCCESS) {
        printf("status=Valid winerr=0 hr=0x00000000\n");
        return 0;
    }
    printf("status=Invalid winerr=0 hr=0x%08lX\n", (unsigned long)st);
    return 1;
}
