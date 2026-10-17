/* Minimal unistd shim for MSVC test builds: usleep(ms granularity). */
#ifndef RB_UNISTD_SHIM_H
#define RB_UNISTD_SHIM_H
#include <windows.h>
static __inline void usleep(unsigned long microseconds)
{ Sleep((DWORD)((microseconds + 999) / 1000)); }
#endif
