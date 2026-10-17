/* 唯一版本事实来源。改版本只改这里。 */
#define RB_VERSION_MAJOR 10
#define RB_VERSION_MINOR 3
#define RB_VERSION_PATCH 4
#define RB_VERSION_BUILD 19

#define RB_STR2(x) #x
#define RB_STR(x)  RB_STR2(x)
#define RB_VERSION_STRING  RB_STR(RB_VERSION_MAJOR) "." RB_STR(RB_VERSION_MINOR) \
                           "." RB_STR(RB_VERSION_PATCH) "." RB_STR(RB_VERSION_BUILD)

/* 宽字符版（内核 RbLog / UI 绑定用） */
#define RB_WSTR2(x) L ## x
#define RB_WSTR(x)  RB_WSTR2(x)
#define RB_VERSION_WSTRING  RB_WSTR(RB_STR(RB_VERSION_MAJOR)) L"." RB_WSTR(RB_STR(RB_VERSION_MINOR)) \
                           L"." RB_WSTR(RB_STR(RB_VERSION_PATCH)) L"." RB_WSTR(RB_STR(RB_VERSION_BUILD))
