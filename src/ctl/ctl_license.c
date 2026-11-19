// [142 拆分 2026-11-10] ramboostctl.c 拆分（B3+/134 §6）：本 TU 为 B-14 license（本地试用无上限口径）。纯搬运，零语义改动。
#include "ctl_internal.h"

/* ---------- [契约 B-14] license：许可（本地试用无上限口径） ---------- */
/* 许可文件 %ProgramData%\RAMBlockCache\license.dat（安装器写入随 Setup 波次；
 * 本波次交付 CLI 读+激活）。密钥格式（占位算法，交付报告注明）：
 *   RBC1-<payload 10 字符>-<hmac8>
 *   hmac8 = HMAC-SHA256(占位密钥, payload) 前 4 字节 hex；
 *   payload 首字符：P=专业版 I=个人版（其余=正式版）。
 * [用户产品裁决 2026-10-23 23:42] 试用时间无上限，不设限制——本地许可口径废除
 * 30 天倒计时：恒 Trial、remainingDays=-1（契约"-1=无限期"语义）、不转 expiring/
 * expired；trialStartDate 仍落文件（仅诊断记录，不参与任何门槛计算）。
 * 09 移交报告原"30 天试用口径冻结"作废，以本裁决为准；B-14 服务端点就绪后
 * 由服务端点接管口径。 */
#define RB_LICENSE_TRIAL_DAYS (-1)   /* -1 = 无限期/不适用（契约语义） */
#define RB_LICENSE_HMAC_KEY   "RAMBlockCache-placeholder-secret-v106"  /* 占位密钥：正式版随授权服务替换 */

static BOOL LicHmac8(const char *payload, char out9[9])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    UCHAR hash[32];
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
        return FALSE;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return FALSE;
    }
    BCryptHashData(h, (PUCHAR)RB_LICENSE_HMAC_KEY, (ULONG)strlen(RB_LICENSE_HMAC_KEY), 0);
    BCryptHashData(h, (PUCHAR)payload, (ULONG)strlen(payload), 0);
    if (!BCRYPT_SUCCESS(BCryptFinishHash(h, hash, sizeof(hash), 0))) {
        BCryptDestroyHash(h);
        BCryptCloseAlgorithmProvider(alg, 0);
        return FALSE;
    }
    _snprintf_s(out9, 9, _TRUNCATE, "%02x%02x%02x%02x",
                hash[0], hash[1], hash[2], hash[3]);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return TRUE;
}

static void LicFilePath(char *out, size_t cap)
{
    char *prog = getenv("ProgramData");
    if (!prog || !prog[0]) prog = "C:\\ProgramData";
    _snprintf_s(out, cap, _TRUNCATE, "%s\\RAMBlockCache\\license.dat", prog);
}

/* 读 license.dat 的 key=value 行；键缺失返回空串。 */
static void LicRead(const char *key, char *out, size_t cap)
{
    char path[MAX_PATH], line[512];
    FILE *f;
    size_t klen = strlen(key);
    out[0] = '\0';
    LicFilePath(path, sizeof(path));
    if (fopen_s(&f, path, "r") != 0 || !f) return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *v = line + klen + 1;
            char *nl = strpbrk(v, "\r\n");
            if (nl) *nl = '\0';
            strncpy_s(out, cap, v, _TRUNCATE);
            break;
        }
    }
    fclose(f);
}

static void LicWrite(const char *key, const char *value)
{
    char path[MAX_PATH], dir[MAX_PATH];
    FILE *f;
    char *nl;
    LicFilePath(path, sizeof(path));
    strncpy_s(dir, sizeof(dir), path, _TRUNCATE);
    nl = strrchr(dir, '\\');
    if (nl) *nl = '\0';
    CreateDirectoryA(dir, NULL);
    /* 追加写（同键重复时取首次出现—— LicRead 语义一致） */
    if (fopen_s(&f, path, "a") != 0 || !f) {
        fprintf(stderr, "[错误] 写许可文件失败：%s\n", path);
        return;
    }
    fprintf(f, "%s=%s\n", key, value);
    fclose(f);
}

/* "yyyy-MM-dd" → FILETIME；失败返回 FALSE。 */
static BOOL LicParseDate(const char *s, FILETIME *ft)
{
    SYSTEMTIME st;
    int y, m, d;
    if (sscanf_s(s, "%d-%d-%d", &y, &m, &d) != 3) return FALSE;
    memset(&st, 0, sizeof(st));
    st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = (WORD)d;
    return SystemTimeToFileTime(&st, ft);
}

int cli_license(int argc, char **argv)
{
    char startBuf[64] = "", keyBuf[256] = "", typeBuf[64] = "", actBuf[64] = "";
    const char *state;
    BOOL valid = FALSE;
    char payload[16] = "", hmac[16] = "", expect[9];

    if (argc < 3) {
        fprintf(stderr, "[错误] 用法: ramboostctl license status | activate <key>\n");
        return 2;
    }
    if (_stricmp(argv[2], "activate") == 0) {
        const char *key = (argc >= 4) ? argv[3] : NULL;
        if (!key) {
            fprintf(stderr, "[错误] 用法: ramboostctl license activate <key>\n");
            return 2;
        }
        /* 格式：RBC1-<payload 10>-<hmac8> */
        if (strncmp(key, "RBC1-", 5) != 0 ||
            strlen(key) != 5 + 10 + 1 + 8) {
            fprintf(stderr, "[错误] 密钥格式无效（应为 RBC1-<10 位载荷>-<8 位校验>）。\n");
            printf("license ok=0 errorCode=4 errorText=\"bad key format\"\n");
            return 1;
        }
        strncpy_s(payload, sizeof(payload), key + 5, 10);
        strncpy_s(hmac, sizeof(hmac), key + 5 + 10 + 1, 8);
        if (!LicHmac8(payload, expect) || _stricmp(hmac, expect) != 0) {
            fprintf(stderr, "[错误] 密钥校验失败（HMAC 不匹配；占位算法口径）。\n");
            printf("license ok=0 errorCode=4 errorText=\"hmac mismatch (placeholder algorithm)\"\n");
            return 1;
        }
        LicWrite("licenseKey", key);
        LicWrite("licenseType", payload[0] == 'P' ? "专业版" :
                                payload[0] == 'I' ? "个人版" : "正式版");
        {
            char today[32];
            SYSTEMTIME st;
            GetLocalTime(&st);
            _snprintf_s(today, sizeof(today), _TRUNCATE, "%04d-%02d-%02d",
                        st.wYear, st.wMonth, st.wDay);
            LicWrite("activatedDate", today);
        }
        printf("许可已激活（%s；激活日期已记录）。\n",
               payload[0] == 'P' ? "专业版" : payload[0] == 'I' ? "个人版" : "正式版");
        printf("license ok=1 state=active\n");
        /* [FIX F9] 诚实披露：当前许可为本地登记（占位 HMAC+无消费点），
         * 激活与否不影响任何功能——明示给用户，不制造"解锁"错觉。 */
        printf("[说明] 当前版本激活仅作本地登记，不影响任何功能（试用无上限口径）。\n");
        return 0;
    }

    if (_stricmp(argv[2], "status") != 0) {
        fprintf(stderr, "[错误] 未知子命令: %s（status | activate <key>）\n", argv[2]);
        return 2;
    }

    LicRead("trialStartDate", startBuf, sizeof(startBuf));
    LicRead("licenseKey", keyBuf, sizeof(keyBuf));
    LicRead("licenseType", typeBuf, sizeof(typeBuf));
    LicRead("activatedDate", actBuf, sizeof(actBuf));

    if (!startBuf[0]) {
        /* 首次运行：落今天为试用起始日 */
        SYSTEMTIME st;
        char today[32];
        GetLocalTime(&st);
        _snprintf_s(today, sizeof(today), _TRUNCATE, "%04d-%02d-%02d",
                    st.wYear, st.wMonth, st.wDay);
        LicWrite("trialStartDate", today);
        strncpy_s(startBuf, sizeof(startBuf), today, _TRUNCATE);
    }
    if (keyBuf[0]) {
        /* 已激活过：重新校验一次密钥（占位算法） */
        if (strncmp(keyBuf, "RBC1-", 5) == 0 && strlen(keyBuf) == 5 + 10 + 1 + 8) {
            strncpy_s(payload, sizeof(payload), keyBuf + 5, 10);
            strncpy_s(hmac, sizeof(hmac), keyBuf + 5 + 10 + 1, 8);
            valid = LicHmac8(payload, expect) && _stricmp(hmac, expect) == 0;
        }
    }

    /* [用户裁决 10-23] 试用无上限：不再做任何到期计算（usedDays/remaining 及
     * 日期差逻辑整体移除）；trialStartDate 仅落盘作诊断记录。 */
    if (valid)
        state = "active";
    else
        state = "trial";   /* 无上限：永不 expired */

    printf("许可状态（B-14 · 本地试用无上限口径）\n");
    printf("  状态: %s   试用起始: %s   剩余: %s\n",
           valid ? "已激活" : "试用中（无时间限制）",
           startBuf, valid ? "—" : "无限期");
    if (valid)
        printf("  许可类型: %s   激活日期: %s\n",
               typeBuf[0] ? typeBuf : "正式版", actBuf[0] ? actBuf : "—");
    /* 双格式：六字段与 LicenseStatusInfo DTO 逐字对应（-1=无限期/不适用） */
    printf("license state=%s trialStartDate=%s trialDays=%d remainingDays=%d "
           "licenseType=%s activatedDate=%s\n",
           state, startBuf, RB_LICENSE_TRIAL_DAYS, RB_LICENSE_TRIAL_DAYS,
           typeBuf[0] ? typeBuf : "", actBuf[0] ? actBuf : "");
    return 0;
}
