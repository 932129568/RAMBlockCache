//! RAMBlockCache 原生桥接插件 —— 路径 B（Node 直接 DeviceIoControl）。
//!
//! 契约：`bridge/native/README.md`（与 WPF `DriverService.cs` 的 P/Invoke 逐条对齐）：
//!   - 设备路径 `\\.\RAMBlockCache`，GENERIC_READ，share=3，OPEN_EXISTING
//!   - 控制 IOCTL `0x00222000`，单缓冲 8224B（16B 信封 + 8208B union）
//!   - 信封：Type@0 / Size@4=8224 / Status@8 / Reserved@12；载荷在 p+16
//!   - 发送前载荷区必须清零（FIX C8 / 警告-6：堆垃圾会随 IOCTL 送进驱动）
//!   - 应答 Status 读偏移 8（METHOD_BUFFERED 输入输出共用 SystemBuffer，
//!     IOCTL 失败时同样可读）；非 0 即错，语义与 DriverService [P2-14] 一致
//!   - Win32 2/6/21 = 设备对象级失效：作废句柄，下次 rb_open 自动重开（FIX S7-14）
//!
//! 零第三方 Win 绑定依赖：kernel32 直接 extern 声明。

#![allow(non_snake_case)]

use std::ffi::c_void;
use std::sync::Mutex;

use napi::bindgen_prelude::Buffer;
use napi::{Error, Result, Status};
use napi_derive::napi;

const BUF_SIZE: usize = 8224;
const IOCTL_RB_REQUEST: u32 = 0x0022_2000;
const GENERIC_READ: u32 = 0x8000_0000;
const OPEN_EXISTING: u32 = 3;

/// 0 = 未打开（句柄值不可能是 0；INVALID_HANDLE_VALUE = -1 作为 usize 是巨大值）
static HANDLE: Mutex<usize> = Mutex::new(0);

#[link(name = "kernel32")]
extern "system" {
    fn CreateFileW(
        lpName: *const u16,
        dwDesiredAccess: u32,
        dwShareMode: u32,
        lpSecurityAttributes: *const c_void,
        dwCreationDisposition: u32,
        dwFlagsAndAttributes: u32,
        hTemplateFile: *const c_void,
    ) -> *mut c_void;
    fn DeviceIoControl(
        hDevice: *mut c_void,
        dwIoControlCode: u32,
        lpInBuffer: *mut c_void,
        nInBufferSize: u32,
        lpOutBuffer: *mut c_void,
        nOutBufferSize: u32,
        lpBytesReturned: *mut u32,
        lpOverlapped: *mut c_void,
    ) -> i32;
    fn CloseHandle(hObject: *mut c_void) -> i32;
    fn GetLastError() -> u32;
    // —— P1-6 超时支持（2026-10-31）：OVERLAPPED 等待 + 超时取消 ——
    fn CreateEventW(
        lpEventAttributes: *const c_void,
        bManualReset: i32,
        bInitialState: i32,
        lpName: *const u16,
    ) -> *mut c_void;
    fn WaitForSingleObject(hHandle: *mut c_void, dwMilliseconds: u32) -> u32;
    fn CancelIoEx(hFile: *mut c_void, lpOverlapped: *const OVERLAPPED) -> i32;
    fn GetOverlappedResult(
        hFile: *mut c_void,
        lpOverlapped: *const OVERLAPPED,
        lpNumberOfBytesTransferred: *mut u32,
        bWait: i32,
    ) -> i32;
}

/// OVERLAPPED 手工布局（winbase.h）；只用 hEvent 字段
#[repr(C)]
struct OVERLAPPED {
    Internal: usize,
    InternalHigh: usize,
    Offset: u32,
    OffsetHigh: u32,
    hEvent: *mut c_void,
}

const ERROR_IO_PENDING: u32 = 997;
const WAIT_OBJECT_0: u32 = 0;
const WAIT_TIMEOUT: u32 = 0x0000_0102;
/// 控制通道兜底超时：本会话实测 IOCTL P95 ≤ 330ms，3s 为 9 倍余量（P1-6）
const REQUEST_TIMEOUT_MS: u32 = 3000;
// [FEAT-LOGQ 2026-11-05] 日志分页 IOCTL：CTL_CODE(0x22, 0x809, METHOD_BUFFERED, ANY)
//   上下文见 src/ctl/rb_logq.h（512 条环 × 96 字符；旧 GET_LOG 仅 64×48）
const IOCTL_RB_LOGQ: u32 = (0x22u32 << 16) | (0x809u32 << 2);
// [USER-REQ 2026-11-05] 开机行为配置 IOCTL：CTL_CODE(0x22, 0x80A, BUFFERED, ANY)
const IOCTL_RB_BOOTCFG: u32 = (0x22u32 << 16) | (0x80Au32 << 2);
const BOOTCFG_SIZE: usize = 80;
const LOGQ_BATCH: usize = 32;
const LOGQ_REC_SIZE: usize = 224;
const LOGQ_RESP_SIZE: usize = 16 + LOGQ_REC_SIZE * LOGQ_BATCH;   // 7184


fn device_path() -> Vec<u16> {
    "\\\\.\\RAMBlockCache\0".encode_utf16().collect()
}

/// 句柄失效自愈（S7-14 同款）：设备对象级失效时作废句柄，下次 rb_open 重开。
fn invalidate_handle_if_device_gone(win32: u32) {
    if matches!(win32, 2 | 6 | 21) {
        let mut h = HANDLE.lock().unwrap();
        if *h != 0 {
            unsafe { CloseHandle(*h as *mut c_void) };
            *h = 0;
        }
    }
}

/// 打开控制设备。已打开时幂等返回（与 DriverService 懒打开一致）。
#[napi]
pub fn rb_open() -> Result<()> {
    let mut h = HANDLE.lock().unwrap();
    if *h != 0 {
        return Ok(());
    }
    let path = device_path();
    let handle = unsafe {
        CreateFileW(
            path.as_ptr(),
            GENERIC_READ,
            3, // FILE_SHARE_READ | FILE_SHARE_WRITE（与 DriverService 一致）
            std::ptr::null(),
            OPEN_EXISTING,
            0,
            std::ptr::null(),
        )
    };
    if handle as isize == INVALID_HANDLE_VALUE_ISIZE {
        let w32 = unsafe { GetLastError() };
        return Err(Error::new(
            Status::GenericFailure,
            format!("无法打开 \\\\.\\RAMBlockCache（驱动未加载或未以管理员运行）：Win32 {w32}"),
        ));
    }
    *h = handle as usize;
    Ok(())
}

const INVALID_HANDLE_VALUE_ISIZE: isize = -1;

/// 单请求收发。payload 为 null 时清零整个载荷区（警告-6）；非 null 时按字节拷到 p+16。
/// [USER-REQ 2026-11-05] 读写开机行为策略（独立 IOCTL 0x80A）。
/// buf 为 80 字节 RB_BOOTCFG：Get@0 / AutoAttach@4 / ResumeOnBoot@8 /
/// Reserved@12 / ResumeLetters@16(32 wchar)。返回同尺寸应答缓冲。
#[napi]
pub fn rb_boot_cfg(buf: Buffer) -> Result<Buffer> {
    let h = { *HANDLE.lock().unwrap() };
    if h == 0 {
        return Err(Error::new(
            Status::GenericFailure,
            "设备未打开：先调用 rb_open()",
        ));
    }
    let mut io = vec![0u8; BOOTCFG_SIZE];
    let n = buf.len().min(BOOTCFG_SIZE);
    io[..n].copy_from_slice(&buf[..n]);
    let mut returned: u32 = 0;
    let ok = unsafe {
        DeviceIoControl(
            h as *mut c_void,
            IOCTL_RB_BOOTCFG,
            io.as_mut_ptr() as *mut c_void,
            BOOTCFG_SIZE as u32,
            io.as_mut_ptr() as *mut c_void,
            BOOTCFG_SIZE as u32,
            &mut returned,
            std::ptr::null_mut(),
        )
    };
    if ok == 0 {
        let w32 = unsafe { GetLastError() };
        invalidate_handle_if_device_gone(w32);
        return Err(Error::new(
            Status::GenericFailure,
            format!("DeviceIoControl 0x{IOCTL_RB_BOOTCFG:X} 失败：Win32 {w32}"),
        ));
    }
    Ok(Buffer::from(io))
}

/// [FEAT-LOGQ 2026-11-05] 分页读取驱动日志（独立 IOCTL 0x809，不进 RB_MSG 信封）。
///
/// 解决的问题：旧 GET_LOG 受 8224B 信封限制，只能一次返回 64 条 × 48 字符——
/// 故障复盘时历史 8 秒即被覆盖、61 处文案被截断。本接口按 Seq 游标增量拉取
/// 512 条环内完整文本（96 字符），桥接可据此落盘为持久日志。
///
/// 参数：fromSeq=上次返回的 lastSeq（0=从头）；maxCount<=32。
/// 返回：原始应答缓冲（16B 头 + n×224B 记录），由 JS 侧按 ABI 解析。
#[napi]
pub fn rb_log_page(from_seq: i64, max_count: u32) -> Result<Buffer> {
    let h = { *HANDLE.lock().unwrap() };
    if h == 0 {
        return Err(Error::new(
            Status::GenericFailure,
            "设备未打开：先调用 rb_open()",
        ));
    }
    let mut buf = vec![0u8; LOGQ_RESP_SIZE];
    // 请求：FromSeq@0(u64) / MaxCount@8(u32) / Reserved@12
    buf[0..8].copy_from_slice(&(from_seq as u64).to_le_bytes());
    let mc = if max_count == 0 || max_count > LOGQ_BATCH as u32 { LOGQ_BATCH as u32 } else { max_count };
    buf[8..12].copy_from_slice(&mc.to_le_bytes());

    let mut returned: u32 = 0;
    let ok = unsafe {
        DeviceIoControl(
            h as *mut c_void,
            IOCTL_RB_LOGQ,
            buf.as_mut_ptr() as *mut c_void,
            LOGQ_RESP_SIZE as u32,
            buf.as_mut_ptr() as *mut c_void,
            LOGQ_RESP_SIZE as u32,
            &mut returned,
            std::ptr::null_mut(),
        )
    };
    if ok == 0 {
        let w32 = unsafe { GetLastError() };
        invalidate_handle_if_device_gone(w32);
        return Err(Error::new(
            Status::GenericFailure,
            format!("DeviceIoControl 0x{IOCTL_RB_LOGQ:X} 失败：Win32 {w32}"),
        ));
    }
    Ok(Buffer::from(buf))
}

/// 单请求收发。payload 为 null 时清零整个载荷区（警告-6）；非 null 时按字节拷到 p+16。
/// 返回完整 8224B 应答缓冲（信封 + 载荷），由调用方按 rb_proto.h 解码。
#[napi]
pub fn rb_request(r#type: u32, payload: Option<Buffer>) -> Result<Buffer> {
    let mt = r#type; // 内联格式化捕获不能写 {type}（关键字）
    let h = { *HANDLE.lock().unwrap() };
    if h == 0 {
        return Err(Error::new(
            Status::GenericFailure,
            "设备未打开：先调用 rb_open()",
        ));
    }

    let mut buf = vec![0u8; BUF_SIZE];
    buf[0..4].copy_from_slice(&mt.to_le_bytes()); // Type@0
    buf[4..8].copy_from_slice(&(BUF_SIZE as u32).to_le_bytes()); // Size@4
    // Status@8 / Reserved@12 随 vec![] 清零
    if let Some(p) = &payload {
        let n = p.len().min(BUF_SIZE - 16);
        buf[16..16 + n].copy_from_slice(&p[..n]);
    }

    let mut returned: u32 = 0;
    // [P1-6] OVERLAPPED 异步提交 + 有界等待 + 超时取消：
    // 驱动挂死时 3s 返回错误，桥接事件循环不再被冻结（原同步调用无解）。
    let ev = unsafe { CreateEventW(std::ptr::null(), 0, 0, std::ptr::null()) };
    if ev.is_null() {
        return Err(Error::new(
            Status::GenericFailure,
            format!("CreateEvent 失败：Win32 {}", unsafe { GetLastError() }),
        ));
    }
    let mut ov = OVERLAPPED {
        Internal: 0,
        InternalHigh: 0,
        Offset: 0,
        OffsetHigh: 0,
        hEvent: ev,
    };
    let mut ok = unsafe {
        DeviceIoControl(
            h as *mut c_void,
            IOCTL_RB_REQUEST,
            buf.as_mut_ptr() as *mut c_void,
            BUF_SIZE as u32,
            buf.as_mut_ptr() as *mut c_void,
            BUF_SIZE as u32,
            &mut returned,
            &mut ov as *mut OVERLAPPED as *mut c_void,
        )
    };
    if ok == 0 {
        let submit_err = unsafe { GetLastError() };
        if submit_err == ERROR_IO_PENDING {
            let w = unsafe { WaitForSingleObject(ev, REQUEST_TIMEOUT_MS) };
            if w == WAIT_TIMEOUT {
                unsafe { CancelIoEx(h as *mut c_void, &ov) };
                unsafe { CloseHandle(ev) };
                // METHOD_BUFFERED 完成时内核仍会回写用户缓冲——超时路径上 IRP
                // 可能未完成，绝不能释放 buf：故意泄漏 8KB（病态场景代价可忽略）
                std::mem::forget(buf);
                return Err(Error::new(
                    Status::GenericFailure,
                    format!(
                        "控制通道超时：IOCTL {mt} 超过 {REQUEST_TIMEOUT_MS}ms 无响应（P1-6），已取消"
                    ),
                ));
            }
            if w != WAIT_OBJECT_0 {
                let e = unsafe { GetLastError() };
                unsafe { CloseHandle(ev) };
                return Err(Error::new(
                    Status::GenericFailure,
                    format!("WaitForSingleObject 失败：Win32 {e}"),
                ));
            }
            let mut bytes: u32 = 0;
            ok = unsafe { GetOverlappedResult(h as *mut c_void, &ov, &mut bytes, 0) };
        } else {
            // 同步失败路径：原样走下方错误处理（Win32 87 等）
        }
    }
    unsafe { CloseHandle(ev) };

    // [P2-14] 非 0 即错；先看驱动 NTSTATUS（更精确），再看 Win32
    let status = u32::from_le_bytes(buf[8..12].try_into().unwrap());
    if status != 0 {
        return Err(Error::new(
            Status::GenericFailure,
            format!("驱动返回错误 0x{status:08X}（消息 {mt}）"),
        ));
    }
    if ok == 0 {
        let w32 = unsafe { GetLastError() };
        invalidate_handle_if_device_gone(w32);
        return Err(Error::new(
            Status::GenericFailure,
            format!("DeviceIoControl 0x{IOCTL_RB_REQUEST:X} 消息 {mt} 失败：Win32 {w32}"),
        ));
    }
    Ok(Buffer::from(buf))
}

/// 关闭句柄。进程退出不依赖本调用（内核回收），但供桥接层有序排空。
#[napi]
pub fn rb_close() -> Result<()> {
    let mut h = HANDLE.lock().unwrap();
    if *h != 0 {
        unsafe { CloseHandle(*h as *mut c_void) };
        *h = 0;
    }
    Ok(())
}
