/* rbc_lba_probe.c —— [O-9 A1 2026-11-08] 文件起始 LBA 探针（FSCTL_GET_RETRIEVAL_POINTERS）
 * usage: rbc_lba_probe <file1> [file2] ...
 * out :  <firstExtentLBA>\t<sizeBytes>\t<path>   （稀疏/失败打 -1）
 */
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t **argv)
{
    for (int i = 1; i < argc; i++) {
        HANDLE h = CreateFileW(argv[i], FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, 0, NULL);
        long long lba = -1, size = -1;
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li; GetFileSizeEx(h, &li); size = li.QuadPart;
            STARTING_VCN_INPUT_BUFFER in; in.StartingVcn.QuadPart = 0;
            UCHAR out[4096]; DWORD ret = 0;
            if (DeviceIoControl(h, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof(in),
                                out, sizeof(out), &ret, NULL) && ret >= 24) {
                RETRIEVAL_POINTERS_BUFFER *r = (RETRIEVAL_POINTERS_BUFFER *)out;
                if (r->ExtentCount >= 1) {
                    long long lcn = (long long)r->Extents[0].Lcn.QuadPart;
                    DWORD bpc = 0;
                    /* LCN×簇大小：簇大小从卷上取不到就由调用方换算，这里直接打 LCN 并按 4K 簇打 LBA */
                    lba = lcn;   /* 暂存 LCN，下面用 FSCTL 取簇大小换算 */
                    wchar_t vol[8];
                    vol[0] = L'\\'; vol[1] = L'\\'; vol[2] = L'.'; vol[3] = L'\\';
                    vol[4] = argv[i][0]; vol[5] = L':'; vol[6] = 0;   /* \\.\X: */
                    HANDLE vh = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            NULL, OPEN_EXISTING, 0, NULL);
                    if (vh != INVALID_HANDLE_VALUE) {
                        NTFS_VOLUME_DATA_BUFFER nv; DWORD br = 0;
                        if (DeviceIoControl(vh, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                                            &nv, sizeof(nv), &br, NULL))
                            bpc = nv.BytesPerCluster;
                        CloseHandle(vh);
                    }
                    lba = (bpc && lcn >= 0) ? lcn * (long long)bpc : lcn;
                }
            }
            CloseHandle(h);
        }
        wprintf(L"%lld\t%lld\t%s\n", lba, size, argv[i]);
    }
    return 0;
}
