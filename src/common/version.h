/* 唯一版本事实来源。改版本只改这里。 */
#define RB_VERSION_MAJOR 10
#define RB_VERSION_MINOR 4
#define RB_VERSION_PATCH 1
#define RB_VERSION_BUILD 19

/* 营销发布名（对外称呼，与机器版本双轨；见 README「版本与命名」节）。
 * [2026-10-19] 显式化：此前 gen_version.ps1 用机器版本拼发布名（V10.3.4 Build19），
 * 与 README/UI 实际展示的 V10.4.3 漂移——跑一次生成脚本即把 Version.cs 写坏。
 * 现发布名由此组宏显式定义，两轨互不干扰。 */
#define RB_RELEASE_MAJOR 10
#define RB_RELEASE_MINOR 5
#define RB_RELEASE_PATCH 0

#define RB_STR2(x) #x
#define RB_STR(x)  RB_STR2(x)
#define RB_VERSION_STRING  RB_STR(RB_VERSION_MAJOR) "." RB_STR(RB_VERSION_MINOR) \
                           "." RB_STR(RB_VERSION_PATCH) "." RB_STR(RB_VERSION_BUILD)
#define RB_RELEASE_STRING  "V" RB_STR(RB_RELEASE_MAJOR) "." RB_STR(RB_RELEASE_MINOR) \
                           "." RB_STR(RB_RELEASE_PATCH) " Build " RB_STR(RB_VERSION_BUILD)

/* 宽字符版（内核 RbLog / UI 绑定用） */
#define RB_WSTR2(x) L ## x
#define RB_WSTR(x)  RB_WSTR2(x)
#define RB_VERSION_WSTRING  RB_WSTR(RB_STR(RB_VERSION_MAJOR)) L"." RB_WSTR(RB_STR(RB_VERSION_MINOR)) \
                           L"." RB_WSTR(RB_STR(RB_VERSION_PATCH)) L"." RB_WSTR(RB_STR(RB_VERSION_BUILD))
