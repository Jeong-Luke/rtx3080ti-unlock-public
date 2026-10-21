/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host harness for the real uefi/entry.c.
 *
 * The production image is compiled unchanged.  Boot services and the small
 * gnu-efi library surface used by entry.c are backed by recording mocks.
 * halt_after_attempt() is terminated by making BS->Stall(1000000) longjmp;
 * there is no test-only branch in production code.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"
#include "../kernel/gpu_unlock/unlock_610.h"
#include <efiprot.h>

struct gu_env gu;
EFI_BOOT_SERVICES *BS;
EFI_SYSTEM_TABLE *ST;

extern EFI_STATUS EFIAPI efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE *);

static const u8 partition_guid[16] = {
    0x67, 0x45, 0x23, 0x01, 0xab, 0x89, 0xef, 0xcd,
    0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
};

static const CHAR16 boot_path[] = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

void *gu_memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *gu_memset(void *dst, int value, size_t n)
{
    u8 *d = dst;
    while (n--) *d++ = (u8)value;
    return dst;
}

int gu_memcmp(const void *left, const void *right, size_t n)
{
    const u8 *a = left, *b = right;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return 0;
}

size_t gu_strlen(const char *text)
{
    size_t n = 0;
    while (text[n]) n++;
    return n;
}

static int guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    return !gu_memcmp(a, b, sizeof(*a));
}

static EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

static EFI_HANDLE launch_image = (EFI_HANDLE)(uintptr_t)0x1000;
static EFI_HANDLE pci_handle = (EFI_HANDLE)(uintptr_t)0x2000;
static EFI_HANDLE windows_image = (EFI_HANDLE)(uintptr_t)0x3000;

static u8 config_space[256];
static char mock_config[4096];
static UINTN mock_config_length;
static EFI_STATUS mock_config_result;

static u8 file_path[4] = { 0x7f, 0xff, 4, 0 };
static EFI_HANDLE file_device_handle;
static u8 pci_dp[22];
static u8 gop_dp_other[22];

struct mock_gop {
    EFI_HANDLE handle;
    u8 dp[22];
    EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
};

static struct mock_gop gops[2];
static unsigned int gop_count;
static unsigned int gop_query_calls, gop_set_calls, gop_blt_calls;

static unsigned int pci_reads, pci_writes, mem_writes, bar_attr_calls;
static unsigned int watchdog_calls, load_image_calls, start_calls;
static unsigned int load_shared_calls, unlock_calls, release_calls;
static unsigned int write_log_calls, print_calls, console_output_calls;
static unsigned int console_after_reset, gop_handle_calls, clock_calls;
static unsigned int call_seq, load_image_seq, first_gpu_work_seq, start_seq;

static EFI_HANDLE load_image_selected, load_image_parent, start_handle;
static EFI_DEVICE_PATH_PROTOCOL *load_image_path;
static BOOLEAN load_image_boot_policy;
static VOID *load_image_source;
static UINTN load_image_source_size;
static EFI_STATUS load_image_result = EFI_SUCCESS;
static EFI_STATUS start_result = EFI_SUCCESS;
static UINTN start_exit_data_size;
static CHAR16 *start_exit_data;

static EFI_STATUS watchdog_result = EFI_SUCCESS;
static UINTN watchdog_timeout;
static UINT64 watchdog_code;
static UINTN watchdog_data_size;
static CHAR16 *watchdog_data;

static int load_shared_result;
static int unlock_result;
static unsigned int unlock_live_dma;
static bool unlock_retained, unlock_io_fault;
static int clock_result;
static bool reset_done;

static unsigned int write_log_fail_at;
static char captured_log[32768];
static size_t captured_log_length;

static jmp_buf halt_jmp;
static int halt_armed, halt_entered;

struct mock_volume {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
    EFI_FILE_PROTOCOL root;
    EFI_FILE_PROTOCOL file;
    u8 dp[46];
    bool boot;
};

static struct mock_volume volumes[3];
static unsigned int volume_count;
static EFI_LOADED_IMAGE_PROTOCOL loaded_image;
static EFI_SYSTEM_TABLE system_table;
static EFI_BOOT_SERVICES boot_services;
static SIMPLE_TEXT_OUTPUT_INTERFACE con_out;
static EFI_PCI_IO_PROTOCOL mock_pci;

static EFI_STATUS EFIAPI mock_handle_protocol(EFI_HANDLE, EFI_GUID *, VOID **);
static EFI_STATUS EFIAPI mock_locate_handle_buffer(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *,
    VOID *, UINTN *, EFI_HANDLE **);
static EFI_STATUS EFIAPI mock_load_image(BOOLEAN, EFI_HANDLE, EFI_DEVICE_PATH_PROTOCOL *,
    VOID *, UINTN, EFI_HANDLE *);
static EFI_STATUS EFIAPI mock_start_image(EFI_HANDLE, UINTN *, CHAR16 **);
static EFI_STATUS EFIAPI mock_set_watchdog(UINTN, UINT64, UINTN, CHAR16 *);
static EFI_STATUS EFIAPI mock_stall(UINTN);
static EFI_STATUS EFIAPI mock_output_string(SIMPLE_TEXT_OUTPUT_INTERFACE *, CHAR16 *);
static EFI_STATUS EFIAPI mock_pci_read(EFI_PCI_IO_PROTOCOL *, EFI_PCI_IO_PROTOCOL_WIDTH,
    UINT32, UINTN, VOID *);
static EFI_STATUS EFIAPI mock_pci_write(EFI_PCI_IO_PROTOCOL *, EFI_PCI_IO_PROTOCOL_WIDTH,
    UINT32, UINTN, VOID *);
static EFI_STATUS EFIAPI mock_mem_write(EFI_PCI_IO_PROTOCOL *, EFI_PCI_IO_PROTOCOL_WIDTH,
    UINT8, UINT64, UINTN, VOID *);
static EFI_STATUS EFIAPI mock_get_location(EFI_PCI_IO_PROTOCOL *, UINTN *, UINTN *,
    UINTN *, UINTN *);
static EFI_STATUS EFIAPI mock_get_bar_attributes(EFI_PCI_IO_PROTOCOL *, UINT8, UINT64 *,
    VOID **);
static EFI_STATUS EFIAPI mock_gop_query_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32,
    UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
static EFI_STATUS EFIAPI mock_gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32);
static EFI_STATUS EFIAPI mock_gop_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *, EFI_GRAPHICS_OUTPUT_BLT_OPERATION,
    UINTN, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN);
static void reset_gops(void);

static void config_write16(UINT32 offset, u16 value)
{
    check(offset + 2 <= sizeof(config_space), "config u16 offset");
    config_space[offset] = (u8)value;
    config_space[offset + 1] = (u8)(value >> 8);
}

static void config_write32(UINT32 offset, u32 value)
{
    check(offset + 4 <= sizeof(config_space), "config u32 offset");
    for (unsigned int i = 0; i < 4; i++)
        config_space[offset + i] = (u8)(value >> (8 * i));
}

static u32 config_read32(UINT32 offset)
{
    check(offset + 4 <= sizeof(config_space), "config read offset");
    return (u32)config_space[offset] |
           ((u32)config_space[offset + 1] << 8) |
           ((u32)config_space[offset + 2] << 16) |
           ((u32)config_space[offset + 3] << 24);
}

static void put32(u8 *p, u32 value)
{
    for (unsigned int i = 0; i < 4; i++) p[i] = (u8)(value >> (8 * i));
}

/* PciRoot(0x0)/Pci(0x21,device,0x0), 22 bytes including the end node. */
static void build_pci_path(u8 *path, u8 device)
{
    gu_memset(path, 0, 22);
    path[0] = 2;
    path[1] = 1;
    path[2] = 12;
    path[3] = 0;
    put32(path + 4, 0x0A0341D0); /* PNP0A03 */
    put32(path + 8, 0);
    path[12] = 1;
    path[13] = 1;
    path[14] = 6;
    path[15] = 0;
    path[16] = 0;
    path[17] = device;
    path[18] = 0x7f;
    path[19] = 0xff;
    path[20] = 4;
    path[21] = 0;
}

static void reset_config_space(void)
{
    gu_memset(config_space, 0, sizeof(config_space));
    config_write32(0, 0x220810de);       /* 10de:2208 */
    config_write32(44, 0x153510de);      /* subsystem 10de:1535 */
    config_write16(4, 0x0002);           /* memory decode, BME off */
    config_space[6] = 0x10;              /* capabilities list present */
    config_space[0x34] = 0x40;
    config_space[0x40] = 0x10;           /* PCIe capability */
    config_space[0x41] = 0x50;
    config_space[0x50] = 0x01;           /* power-management capability */
    config_space[0x51] = 0;
}

static void set_volume_path(struct mock_volume *volume, const u8 guid[16], bool match)
{
    gu_memset(volume->dp, 0, sizeof(volume->dp));
    volume->dp[0] = 4;
    volume->dp[1] = 1;
    volume->dp[2] = 42;
    volume->dp[3] = 0;
    gu_memcpy(volume->dp + 24, guid, 16);
    if (!match) volume->dp[24] ^= 1;
    volume->dp[40] = 2;
    volume->dp[41] = 2;
    volume->dp[42] = 0x7f;
    volume->dp[43] = 0xff;
    volume->dp[44] = 4;
    volume->dp[45] = 0;
}

static EFI_STATUS EFIAPI mock_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,
    EFI_FILE_HANDLE *root)
{
    for (unsigned int i = 0; i < volume_count; i++) {
        if (self == &volumes[i].fs) {
            *root = &volumes[i].root;
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI mock_root_open(EFI_FILE_HANDLE self, EFI_FILE_HANDLE *new_file,
    CHAR16 *filename, UINT64 mode, UINT64 attributes)
{
    (void)mode;
    (void)attributes;
    for (unsigned int i = 0; i < volume_count; i++) {
        if (self != &volumes[i].root) continue;
        if (!volumes[i].boot || gu_memcmp(filename, boot_path, sizeof(boot_path)))
            return EFI_NOT_FOUND;
        *new_file = &volumes[i].file;
        return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI mock_root_close(EFI_FILE_HANDLE self)
{
    (void)self;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_file_close(EFI_FILE_HANDLE self)
{
    (void)self;
    return EFI_SUCCESS;
}

static void reset_gops(void)
{
    gu_memset(gops, 0, sizeof(gops));
    gop_count = 2;
    build_pci_path(pci_dp, 0);
    build_pci_path(gop_dp_other, 1);
    for (unsigned int i = 0; i < 2; i++) {
        gops[i].handle = &gops[i];
        gops[i].gop.QueryMode = mock_gop_query_mode;
        gops[i].gop.SetMode = mock_gop_set_mode;
        gops[i].gop.Blt = mock_gop_blt;
        gops[i].gop.Mode = &gops[i].mode;
        gops[i].mode.MaxMode = 1;
        gops[i].mode.Mode = 0;
        gops[i].mode.Info = &gops[i].info;
        gops[i].mode.SizeOfInfo = sizeof(gops[i].info);
        gops[i].mode.FrameBufferBase = 0xE0000000ull;
        gops[i].mode.FrameBufferSize = 0x01000000u;
        gops[i].info.Version = 0;
        gops[i].info.HorizontalResolution = 1920;
        gops[i].info.VerticalResolution = 1080;
        gops[i].info.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
        gops[i].info.PixelsPerScanLine = 1920;
        gu_memcpy(gops[i].dp, i ? gop_dp_other : pci_dp, sizeof(pci_dp));
    }
}

static void reset_volumes(void)
{
    gu_memset(volumes, 0, sizeof(volumes));
    volume_count = 3;
    for (unsigned int i = 0; i < volume_count; i++) {
        volumes[i].fs.OpenVolume = mock_open_volume;
        volumes[i].root.Open = mock_root_open;
        volumes[i].root.Close = mock_root_close;
        volumes[i].file.Close = mock_file_close;
        set_volume_path(&volumes[i], partition_guid, i == 1);
    }
    volumes[0].boot = false;
    volumes[1].boot = true;
    volumes[2].boot = true;

    gu_memset(&loaded_image, 0, sizeof(loaded_image));
    loaded_image.Revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
    loaded_image.DeviceHandle = &volumes[0];
}

static void set_config_text(const char *text)
{
    int written = snprintf(mock_config, sizeof(mock_config), "%s", text);
    check(written > 0 && (size_t)written < sizeof(mock_config), "config text fits");
    mock_config_length = (UINTN)written;
    mock_config_result = EFI_SUCCESS;
}

static void reset_counters(void)
{
    pci_reads = pci_writes = mem_writes = bar_attr_calls = 0;
    watchdog_calls = load_image_calls = start_calls = 0;
    load_shared_calls = unlock_calls = release_calls = 0;
    write_log_calls = print_calls = console_output_calls = 0;
    console_after_reset = gop_handle_calls = clock_calls = 0;
    gop_query_calls = gop_set_calls = gop_blt_calls = 0;
    call_seq = load_image_seq = first_gpu_work_seq = start_seq = 0;
    load_image_selected = load_image_parent = start_handle = NULL;
    load_image_path = NULL;
    load_image_boot_policy = FALSE;
    load_image_source = NULL;
    load_image_source_size = 0;
    load_image_result = EFI_SUCCESS;
    start_result = EFI_SUCCESS;
    start_exit_data_size = 0;
    start_exit_data = NULL;
    watchdog_result = EFI_SUCCESS;
    watchdog_timeout = 0;
    watchdog_code = 0;
    watchdog_data_size = 0;
    watchdog_data = NULL;
    load_shared_result = 0;
    unlock_result = 0;
    unlock_live_dma = 0;
    unlock_retained = false;
    unlock_io_fault = false;
    clock_result = 0;
    reset_done = false;
    write_log_fail_at = 0;
    captured_log[0] = 0;
    captured_log_length = 0;
    halt_entered = 0;
    file_device_handle = NULL;
}

static void reset_all(void)
{
    gu_memset(&gu, 0, sizeof(gu));
    gu_memset(&system_table, 0, sizeof(system_table));
    gu_memset(&boot_services, 0, sizeof(boot_services));
    gu_memset(&con_out, 0, sizeof(con_out));

    con_out.OutputString = mock_output_string;
    system_table.BootServices = &boot_services;
    system_table.ConOut = &con_out;

    boot_services.HandleProtocol = mock_handle_protocol;
    boot_services.LocateHandleBuffer = mock_locate_handle_buffer;
    boot_services.LoadImage = mock_load_image;
    boot_services.StartImage = mock_start_image;
    boot_services.SetWatchdogTimer = mock_set_watchdog;
    boot_services.Stall = mock_stall;

    gu_memset(&mock_pci, 0, sizeof(mock_pci));
    mock_pci.Pci.Read = mock_pci_read;
    mock_pci.Pci.Write = mock_pci_write;
    mock_pci.Mem.Write = mock_mem_write;
    mock_pci.GetLocation = mock_get_location;
    mock_pci.GetBarAttributes = mock_get_bar_attributes;
    gu.pci = &mock_pci;

    ST = &system_table;
    BS = &boot_services;

    reset_counters();
    reset_volumes();
    reset_gops();
    reset_config_space();

    set_config_text(
        "target=0000:21:00.0\n"
        "board_vendor=0x10de\n"
        "board_device=0x1535\n"
        "execute=1\n"
        "ss1_full=0\n"
        "windows=1\n"
        "windows_partition=01234567-89ab-cdef-8123-456789abcdef\n");
}

/* gnu-efi library surface used by entry.c. */
VOID InitializeLib(IN EFI_HANDLE image, IN EFI_SYSTEM_TABLE *system)
{
    (void)image;
    ST = system;
    BS = system->BootServices;
}

static EFI_STATUS EFIAPI mock_output_string(SIMPLE_TEXT_OUTPUT_INTERFACE *self,
    CHAR16 *string)
{
    (void)self;
    (void)string;
    console_output_calls++;
    if (reset_done) console_after_reset++;
    return EFI_SUCCESS;
}

UINTN Print(IN CONST CHAR16 *fmt, ...)
{
    (void)fmt;
    print_calls++;
    if (ST && ST->ConOut && ST->ConOut->OutputString)
        return (UINTN)ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)L"print");
    return 0;
}

VOID FreePool(IN VOID *p)
{
    if (p && p != (VOID *)file_path) free(p);
}

EFI_DEVICE_PATH_PROTOCOL *DevicePathFromHandle(IN EFI_HANDLE handle)
{
    if (handle == pci_handle) return (EFI_DEVICE_PATH_PROTOCOL *)pci_dp;
    for (unsigned int i = 0; i < volume_count; i++) {
        if (handle == &volumes[i]) return (EFI_DEVICE_PATH_PROTOCOL *)volumes[i].dp;
    }
    for (unsigned int i = 0; i < gop_count; i++) {
        if (handle == gops[i].handle) return (EFI_DEVICE_PATH_PROTOCOL *)gops[i].dp;
    }
    return NULL;
}

UINTN DevicePathSize(IN EFI_DEVICE_PATH_PROTOCOL *device_path)
{
    if (!device_path) return 0;
    const u8 *path = (const u8 *)device_path;
    UINTN total = 0;
    for (unsigned int node = 0; node < 64; node++) {
        if (total + 4 > 4096) return 0;
        UINTN length = (UINTN)path[total + 2] | ((UINTN)path[total + 3] << 8);
        if (length < 4 || total + length > 4096) return 0;
        total += length;
        if (path[total - length] == 0x7f && path[total - length + 1] == 0xff)
            return total;
    }
    return 0;
}

EFI_DEVICE_PATH_PROTOCOL *FileDevicePath(IN EFI_HANDLE device, IN CHAR16 *filename)
{
    (void)filename;
    file_device_handle = device;
    return (EFI_DEVICE_PATH_PROTOCOL *)file_path;
}

/* Entry-owned runtime helpers. */
void gu_log(const char *fmt, ...)
{
    va_list args;
    if (gu.log_length >= GU_LOG_CAP - 1) {
        gu.log_truncated = true;
        return;
    }
    va_start(args, fmt);
    int written = vsnprintf(gu.log + gu.log_length, GU_LOG_CAP - gu.log_length, fmt, args);
    va_end(args);
    if (written < 0) return;
    size_t room = GU_LOG_CAP - 1 - gu.log_length;
    if ((size_t)written > room) {
        gu.log_length = GU_LOG_CAP - 1;
        gu.log_truncated = true;
    } else {
        gu.log_length += (size_t)written;
    }
}

EFI_STATUS gu_read_file(const CHAR16 *name, void **data, UINTN *length, UINTN limit)
{
    (void)name;
    (void)limit;
    *data = NULL;
    *length = 0;
    if (mock_config_result != EFI_SUCCESS) return mock_config_result;
    void *copy = malloc(mock_config_length ? mock_config_length : 1);
    check(copy != NULL, "config allocation");
    gu_memcpy(copy, mock_config, mock_config_length);
    *data = copy;
    *length = mock_config_length;
    return EFI_SUCCESS;
}

EFI_STATUS gu_write_log(void)
{
    write_log_calls++;
    if (write_log_fail_at && write_log_calls == write_log_fail_at)
        return EFI_DEVICE_ERROR;
    if (captured_log_length + gu.log_length < sizeof(captured_log)) {
        gu_memcpy(captured_log + captured_log_length, gu.log, gu.log_length);
        captured_log_length += gu.log_length;
        captured_log[captured_log_length] = 0;
    }
    return EFI_SUCCESS;
}

int gu_init_clock(void)
{
    clock_calls++;
    return clock_result;
}

unsigned int gu_live_dma_count(void)
{
    return unlock_live_dma;
}

int gpu_unlock_610_load_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared)
{
    (void)pdev;
    load_shared_calls++;
    if (!first_gpu_work_seq) first_gpu_work_seq = ++call_seq;
    if (load_shared_result) return load_shared_result;
    shared->gsp = (const struct firmware *)(uintptr_t)1;
    return 0;
}

void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared)
{
    release_calls++;
    shared->gsp = NULL;
}

int gpu_unlock_610_unlock(struct pci_dev *pdev, const struct gpu_unlock_610_shared *shared,
    enum gpu_unlock_ss1_policy policy)
{
    (void)pdev;
    (void)policy;
    check(shared->gsp != NULL, "unlock requires loaded shared firmware");
    unlock_calls++;
    if (!first_gpu_work_seq) first_gpu_work_seq = ++call_seq;
    reset_done = true;
    gu.retained = unlock_retained;
    gu.io_fault = unlock_io_fault;
    return unlock_result;
}

/* Boot services. */
static EFI_STATUS EFIAPI mock_handle_protocol(EFI_HANDLE handle, EFI_GUID *protocol,
    VOID **interface)
{
    if (guid_equal(protocol, &image_guid)) {
        if (handle != launch_image) return EFI_NOT_FOUND;
        *interface = &loaded_image;
        return EFI_SUCCESS;
    }
    if (guid_equal(protocol, &fs_guid)) {
        for (unsigned int i = 0; i < volume_count; i++) {
            if (handle == &volumes[i]) {
                *interface = &volumes[i].fs;
                return EFI_SUCCESS;
            }
        }
        return EFI_NOT_FOUND;
    }
    if (guid_equal(protocol, &pci_guid)) {
        if (handle != pci_handle) return EFI_NOT_FOUND;
        *interface = &mock_pci;
        return EFI_SUCCESS;
    }
    if (guid_equal(protocol, &gop_guid)) {
        for (unsigned int i = 0; i < gop_count; i++) {
            if (handle == gops[i].handle) {
                *interface = &gops[i].gop;
                gop_handle_calls++;
                return EFI_SUCCESS;
            }
        }
        return EFI_NOT_FOUND;
    }
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI mock_locate_handle_buffer(EFI_LOCATE_SEARCH_TYPE type,
    EFI_GUID *protocol, VOID *search_key, UINTN *count, EFI_HANDLE **buffer)
{
    (void)type;
    (void)search_key;
    if (guid_equal(protocol, &pci_guid)) {
        *count = 1;
        *buffer = malloc(sizeof(EFI_HANDLE));
        check(*buffer != NULL, "PCI handle allocation");
        (*buffer)[0] = pci_handle;
        return EFI_SUCCESS;
    }
    if (guid_equal(protocol, &fs_guid)) {
        *count = volume_count;
        *buffer = malloc(volume_count * sizeof(EFI_HANDLE));
        check(*buffer != NULL, "filesystem handle allocation");
        for (unsigned int i = 0; i < volume_count; i++) (*buffer)[i] = &volumes[i];
        return EFI_SUCCESS;
    }
    if (guid_equal(protocol, &gop_guid)) {
        *count = gop_count;
        *buffer = malloc(gop_count * sizeof(EFI_HANDLE));
        check(*buffer != NULL, "GOP handle allocation");
        for (unsigned int i = 0; i < gop_count; i++) (*buffer)[i] = gops[i].handle;
        return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI mock_load_image(BOOLEAN boot_policy, EFI_HANDLE parent,
    EFI_DEVICE_PATH_PROTOCOL *device_path, VOID *source, UINTN source_size,
    EFI_HANDLE *image)
{
    load_image_calls++;
    load_image_seq = ++call_seq;
    load_image_boot_policy = boot_policy;
    load_image_parent = parent;
    load_image_path = device_path;
    load_image_source = source;
    load_image_source_size = source_size;
    load_image_selected = file_device_handle;
    if (load_image_result != EFI_SUCCESS) return load_image_result;
    *image = windows_image;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_start_image(EFI_HANDLE image, UINTN *exit_data_size,
    CHAR16 **exit_data)
{
    start_calls++;
    start_seq = ++call_seq;
    start_handle = image;
    start_exit_data_size = exit_data_size ? *exit_data_size : 0;
    start_exit_data = exit_data ? *exit_data : NULL;
    return start_result;
}

static EFI_STATUS EFIAPI mock_set_watchdog(UINTN timeout, UINT64 code,
    UINTN data_size, CHAR16 *data)
{
    watchdog_calls++;
    watchdog_timeout = timeout;
    watchdog_code = code;
    watchdog_data_size = data_size;
    watchdog_data = data;
    return watchdog_result;
}

static EFI_STATUS EFIAPI mock_stall(UINTN microseconds)
{
    if (microseconds == 1000000 && halt_armed) {
        halt_entered++;
        longjmp(halt_jmp, 1);
    }
    return EFI_SUCCESS;
}

/* EFI_PCI_IO_PROTOCOL callbacks. */
static EFI_STATUS EFIAPI mock_pci_read(EFI_PCI_IO_PROTOCOL *self,
    EFI_PCI_IO_PROTOCOL_WIDTH width, UINT32 offset, UINTN count, VOID *buffer)
{
    (void)self;
    pci_reads++;
    if (width == EfiPciIoWidthUint32) {
        for (UINTN i = 0; i < count; i++)
            ((u32 *)buffer)[i] = config_read32(offset + 4 * (UINT32)i);
    } else if (width == EfiPciIoWidthUint16) {
        check(count == 1, "PCI u16 read count");
        ((u16 *)buffer)[0] = (u16)(config_space[offset] |
            ((u16)config_space[offset + 1] << 8));
    } else {
        return EFI_UNSUPPORTED;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_pci_write(EFI_PCI_IO_PROTOCOL *self,
    EFI_PCI_IO_PROTOCOL_WIDTH width, UINT32 offset, UINTN count, VOID *buffer)
{
    (void)self;
    (void)width;
    (void)offset;
    (void)count;
    (void)buffer;
    pci_writes++;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_mem_write(EFI_PCI_IO_PROTOCOL *self,
    EFI_PCI_IO_PROTOCOL_WIDTH width, UINT8 bar, UINT64 offset, UINTN count, VOID *buffer)
{
    (void)self;
    (void)width;
    (void)bar;
    (void)offset;
    (void)count;
    (void)buffer;
    mem_writes++;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_get_location(EFI_PCI_IO_PROTOCOL *self, UINTN *segment,
    UINTN *bus, UINTN *device, UINTN *function)
{
    (void)self;
    *segment = 0;
    *bus = 0x21;
    *device = 0;
    *function = 0;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_get_bar_attributes(EFI_PCI_IO_PROTOCOL *self,
    UINT8 bar, UINT64 *supports, VOID **resources)
{
    (void)self;
    check(bar == 0, "only BAR0 attributes are requested");
    bar_attr_calls++;
    u8 *descriptor = calloc(1, 46);
    check(descriptor != NULL, "BAR attribute allocation");
    descriptor[0] = 0x8a;
    descriptor[1] = 0x2b;
    descriptor[38] = 0x00;
    descriptor[39] = 0x20;
    descriptor[40] = 0x84;
    if (supports) *supports = 0;
    if (resources) *resources = descriptor;
    return EFI_SUCCESS;
}

static struct mock_gop *find_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL *self)
{
    for (unsigned int i = 0; i < gop_count; i++)
        if (self == &gops[i].gop) return &gops[i];
    return NULL;
}

static EFI_STATUS EFIAPI mock_gop_query_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
    UINT32 mode_number, UINTN *size_of_info,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info)
{
    struct mock_gop *gop = find_gop(self);
    if (!gop || !gop->gop.Mode || !gop->mode.Info || !gop->mode.SizeOfInfo)
        return EFI_DEVICE_ERROR;
    if (mode_number >= gop->mode.MaxMode) return EFI_UNSUPPORTED;
    *size_of_info = gop->mode.SizeOfInfo;
    *info = gop->mode.Info;
    gop_query_calls++;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
    UINT32 mode_number)
{
    (void)self;
    (void)mode_number;
    gop_set_calls++;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI mock_gop_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *blt_buffer, EFI_GRAPHICS_OUTPUT_BLT_OPERATION blt_operation,
    UINTN source_x, UINTN source_y, UINTN destination_x, UINTN destination_y,
    UINTN width, UINTN height, UINTN delta)
{
    (void)self; (void)blt_buffer; (void)blt_operation;
    (void)source_x; (void)source_y; (void)destination_x; (void)destination_y;
    (void)width; (void)height; (void)delta;
    gop_blt_calls++;
    return EFI_SUCCESS;
}

static int run_entry(EFI_STATUS *status)
{
    halt_entered = 0;
    halt_armed = 1;
    if (setjmp(halt_jmp) == 0) {
        *status = efi_main(launch_image, &system_table);
        halt_armed = 0;
        return 0;
    }
    halt_armed = 0;
    return 1;
}

#if !defined(GU_PROBE_ONLY) && !defined(GU_HANDOFF_ONLY)
static void test_preload_before_gpu_work(void)
{
    reset_all();
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    (void)status;
    check(halted, "successful attempt must halt after StartImage returns");
    check(load_image_calls == 1, "LoadImage called exactly once");
    check(load_image_seq && first_gpu_work_seq && load_image_seq < first_gpu_work_seq,
        "LoadImage precedes all GPU work");
    check(load_image_selected == &volumes[1], "configured Windows volume selected");
    check(load_image_parent == launch_image, "LoadImage parent is the launching image");
    check(load_image_path == (EFI_DEVICE_PATH_PROTOCOL *)file_path,
        "LoadImage receives the bootmgfw device path");
    check(load_image_boot_policy == FALSE, "LoadImage is not a boot-policy load");
    check(load_image_source == NULL && load_image_source_size == 0,
        "LoadImage has no source buffer");
    check(load_shared_calls == 1 && unlock_calls == 1 && release_calls == 1,
        "shared load/unlock/release sequence");
    check(start_calls == 1 && start_handle == windows_image,
        "StartImage gets the preloaded image");
    check(start_seq > first_gpu_work_seq, "StartImage follows GPU work");
    check(start_exit_data_size == 0 && start_exit_data == NULL,
        "StartImage is called without exit data");
    check(pci_reads == 1, "one read-only PCI inventory pass");
    check(print_calls > 0 && console_output_calls > 0,
        "pre-transaction console messages are present");
    check(bar_attr_calls == 1, "BAR0 attributes were queried once");
    check(clock_calls == 1, "normal path initializes the invariant clock once");
    check(pci_writes == 0 && mem_writes == 0, "entry performs no GPU writes");
    check(console_after_reset == 0, "no console output after simulated reset");
    check(gop_handle_calls == 0 && gop_query_calls == 0 &&
        gop_set_calls == 0 && gop_blt_calls == 0, "normal path never touches GOP");
    check(halt_entered == 1, "halt is entered exactly once");
    check(strstr(captured_log, "STOP:") != NULL, "halt checkpoint written");
}

static void test_unique_configured_volume(void)
{
    reset_all();
    gu_memcpy(volumes[2].dp, volumes[1].dp, sizeof(volumes[2].dp));
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_NOT_FOUND, "duplicate bootmgfw is refused");
    check(load_image_calls == 0 && start_calls == 0,
        "duplicate boot manager performs no load/start");
    check(watchdog_calls == 0 && load_shared_calls == 0 && unlock_calls == 0,
        "duplicate boot manager performs zero GPU work");
    check(pci_writes == 0 && mem_writes == 0, "duplicate boot manager writes nothing");
}

static void test_preload_failure_zero_work(void)
{
    reset_all();
    volumes[1].boot = false;
    volumes[2].boot = false;
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_NOT_FOUND, "missing bootmgfw is refused");
    check(load_image_calls == 0 && start_calls == 0 && watchdog_calls == 0,
        "preload failure performs no watchdog/load/start");
    check(load_shared_calls == 0 && unlock_calls == 0,
        "preload failure performs no GPU work");
    check(pci_writes == 0 && mem_writes == 0, "preload failure writes nothing");
}

static void test_load_image_failure_zero_work(void)
{
    reset_all();
    load_image_result = EFI_SECURITY_VIOLATION;
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_SECURITY_VIOLATION, "LoadImage failure is returned");
    check(load_image_calls == 1, "LoadImage was attempted once");
    check(watchdog_calls == 0 && load_shared_calls == 0 && unlock_calls == 0,
        "LoadImage failure performs zero GPU work");
    check(start_calls == 0 && pci_writes == 0 && mem_writes == 0,
        "LoadImage failure starts/writes nothing");
}

static void test_checkpoint_failure_zero_work(void)
{
    reset_all();
    write_log_fail_at = 1;
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_DEVICE_ERROR, "checkpoint failure returns an error");
    check(watchdog_calls == 0 && load_shared_calls == 0 && unlock_calls == 0,
        "checkpoint failure performs zero GPU work");
    check(start_calls == 0 && pci_writes == 0 && mem_writes == 0,
        "checkpoint failure starts/writes nothing");
}

static void test_watchdog_failure_zero_work(void)
{
    reset_all();
    watchdog_result = EFI_DEVICE_ERROR;
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_DEVICE_ERROR, "watchdog failure returns an error");
    check(watchdog_calls == 1 && watchdog_timeout == 0 && watchdog_code == 0 &&
        watchdog_data_size == 0 && watchdog_data == NULL,
        "watchdog is disabled with the expected arguments");
    check(load_shared_calls == 0 && unlock_calls == 0 && start_calls == 0,
        "watchdog failure performs zero GPU work");
    check(pci_writes == 0 && mem_writes == 0, "watchdog failure writes nothing");
}

static void run_terminal_case(const char *name, int shared_result, int unlock_ret,
    bool retained, bool io_fault, unsigned int live_dma, unsigned int log_fail,
    const char *config, EFI_STATUS image_result, int expect_load_image,
    int expect_start, int expect_unlock)
{
    reset_all();
    if (config) set_config_text(config);
    load_shared_result = shared_result;
    unlock_result = unlock_ret;
    unlock_retained = retained;
    unlock_io_fault = io_fault;
    unlock_live_dma = live_dma;
    write_log_fail_at = log_fail;
    start_result = image_result;

    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    (void)status;
    check(halted, name);
    check(load_image_calls == expect_load_image, name);
    check(load_shared_calls == 1, name);
    check(unlock_calls == expect_unlock, name);
    check(start_calls == expect_start, name);
    check(pci_writes == 0 && mem_writes == 0, name);
    check(console_after_reset == 0, name);
    check(gop_handle_calls == 0, name);
    check(strstr(captured_log, "STOP:") != NULL, name);
}

static void test_terminal_failures(void)
{
    run_terminal_case("shared firmware failure", -EIO, 0, false, false, 0, 0,
        NULL, EFI_SUCCESS, 1, 0, 0);
    run_terminal_case("unlock failure", 0, -EIO, false, false, 0, 0,
        NULL, EFI_SUCCESS, 1, 0, 1);
    run_terminal_case("retained DMA", 0, 0, true, false, 0, 0,
        NULL, EFI_SUCCESS, 1, 0, 1);
    run_terminal_case("MMIO fault", 0, 0, false, true, 0, 0,
        NULL, EFI_SUCCESS, 1, 0, 1);
    run_terminal_case("live DMA", 0, 0, false, false, 1, 0,
        NULL, EFI_SUCCESS, 1, 0, 1);
    run_terminal_case("post-attempt log failure", 0, 0, false, false, 0, 2,
        NULL, EFI_SUCCESS, 1, 0, 1);
    run_terminal_case("no Windows requested", 0, 0, false, false, 0, 0,
        "target=0000:21:00.0\n"
        "board_vendor=0x10de\n"
        "board_device=0x1535\n"
        "execute=1\n"
        "ss1_full=0\n"
        "windows=0\n",
        EFI_SUCCESS, 0, 0, 1);
    run_terminal_case("StartImage error", 0, 0, false, false, 0, 0,
        NULL, EFI_DEVICE_ERROR, 1, 1, 1);
    run_terminal_case("successful StartImage return", 0, 0, false, false, 0, 0,
        NULL, EFI_SUCCESS, 1, 1, 1);
}
#endif

#ifdef GU_HANDOFF_ONLY
static void check_handoff_no_gpu_work(void)
{
    check(load_shared_calls == 0 && unlock_calls == 0 && release_calls == 0,
        "handoff baseline performs no shared/unlock/DMA work");
    check(watchdog_calls == 0 && clock_calls == 0 &&
        watchdog_timeout == 0 && watchdog_code == 0 &&
        watchdog_data_size == 0 && watchdog_data == NULL,
        "handoff baseline uses no watchdog or clock");
    check(pci_writes == 0 && mem_writes == 0,
        "handoff baseline performs no PCI or MMIO writes");
    check(console_after_reset == 0, "handoff baseline emits no post-reset console output");
    check(halt_entered == 0, "handoff baseline never enters terminal halt");
}

static void test_handoff_success_and_abort_status(void)
{
    reset_all();
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_ABORTED,
        "StartImage success return maps to EFI_ABORTED");
    check(load_image_calls == 1 && start_calls == 1, "handoff preloads and starts once");
    check(load_image_seq && start_seq > load_image_seq, "StartImage follows LoadImage");
    check(first_gpu_work_seq == 0, "handoff performs no GPU transaction work");
    check(load_image_selected == &volumes[1] && load_image_parent == launch_image,
        "handoff loads the configured Windows image from the launching parent");
    check(load_image_path == (EFI_DEVICE_PATH_PROTOCOL *)file_path,
        "handoff LoadImage uses the bootmgfw device path");
    check(!load_image_boot_policy && load_image_source == NULL &&
        load_image_source_size == 0, "handoff LoadImage has no source buffer");
    check(start_handle == windows_image, "handoff StartImage gets the preloaded image");
    check(start_exit_data_size == 0 && start_exit_data == NULL,
        "handoff StartImage is called without exit data");
    check(print_calls > 0 && console_output_calls > 0,
        "handoff emits its pre-transaction console messages");
    check(pci_reads == 1 && bar_attr_calls == 1, "handoff records one PCI inventory pass");
    check(gop_handle_calls >= 1, "target-associated GOP was opened");
    check(gop_query_calls == 0, "GOP metadata is read directly, without QueryMode");
    check(gop_set_calls == 0 && gop_blt_calls == 0, "GOP metadata is read-only");
    check(strstr(captured_log, "BASELINE PCI offset=0x10:") != NULL &&
        strstr(captured_log, "BASELINE PCI offset=0xf0:") != NULL,
        "handoff logs the full 256-byte PCI inventory");
    check(strstr(captured_log, "BASELINE target GOP mode=0") != NULL,
        "handoff logs target GOP mode metadata");
    check(strstr(captured_log, "Handoff-only image: execute ignored") != NULL &&
        strstr(captured_log, "Probe-only image: execute ignored") == NULL,
        "handoff identifies its own image when ignoring execute");
    check_handoff_no_gpu_work();
}

static void test_handoff_target_and_windows_required(void)
{
    reset_all();
    set_config_text(
        "target=0000:22:00.0\n"
        "board_vendor=0x10de\n"
        "board_device=0x1535\n"
        "execute=1\n"
        "windows=1\n"
        "windows_partition=01234567-89ab-cdef-8123-456789abcdef\n");
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_NOT_FOUND, "wrong target is refused");
    check(load_image_calls == 0 && start_calls == 0, "wrong target never loads/starts Windows");
    check(pci_writes == 0 && mem_writes == 0, "wrong target writes nothing");
    check_handoff_no_gpu_work();

    reset_all();
    set_config_text(
        "target=0000:21:00.0\n"
        "board_vendor=0x10de\n"
        "board_device=0x1535\n"
        "execute=1\n"
        "windows=0\n"
        "windows_partition=01234567-89ab-cdef-8123-456789abcdef\n");
    status = 0;
    halted = run_entry(&status);
    check(!halted && status == EFI_NOT_FOUND, "windows=0 is refused");
    check(load_image_calls == 0 && start_calls == 0, "windows=0 never loads/starts Windows");
    check(pci_writes == 0 && mem_writes == 0, "windows=0 writes nothing");
    check_handoff_no_gpu_work();
}

static void test_handoff_gop_association_and_metadata(void)
{
    reset_all();
    gop_count = 1;
    gu_memcpy(gops[0].dp, gop_dp_other, sizeof(gops[0].dp));
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_NOT_FOUND, "nonmatching GOP does not qualify");
    check(gop_handle_calls == 0, "nonmatching GOP is not opened");
    check(load_image_calls == 0 && start_calls == 0, "no matching GOP blocks Windows load");
    check(pci_writes == 0 && mem_writes == 0, "no matching GOP writes nothing");
    check_handoff_no_gpu_work();

    for (unsigned int variant = 0; variant < 12; variant++) {
        reset_all();
        gop_count = 1;
        switch (variant) {
        case 0: gops[0].gop.Mode = NULL; break;
        case 1: gops[0].mode.Info = NULL; break;
        case 2: gops[0].mode.SizeOfInfo = 0; break;
        case 3: gops[0].mode.Mode = 1; break; /* Mode >= MaxMode */
        case 4: gops[0].info.PixelFormat = PixelBltOnly; break;
        case 5: gops[0].info.HorizontalResolution = 0; break;
        case 6: gops[0].info.VerticalResolution = 0; break;
        case 7: gops[0].info.PixelsPerScanLine = 1919; break;
        case 8: gops[0].mode.FrameBufferBase = 0; break;
        case 9: gops[0].mode.FrameBufferSize = 0; break;
        case 10:
            gops[0].mode.FrameBufferBase = ~0ull - 0x1000;
            gops[0].mode.FrameBufferSize = 0x01000000;
            break;
        default: gops[0].mode.FrameBufferSize = 0x00400000; break;
        }
        status = 0;
        halted = run_entry(&status);
        check(!halted && status == EFI_UNSUPPORTED,
            "invalid target GOP metadata is refused");
        check(load_image_calls == 0 && start_calls == 0,
            "invalid target GOP metadata blocks Windows load");
        check(pci_writes == 0 && mem_writes == 0,
            "invalid target GOP metadata writes nothing");
        check_handoff_no_gpu_work();
    }
}

static void test_handoff_preload_and_log_failures(void)
{
    reset_all();
    volumes[1].boot = false;
    volumes[2].boot = false;
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_NOT_FOUND, "preload failure returns an error");
    check(load_image_calls == 0 && start_calls == 0, "preload failure never starts Windows");
    check(pci_writes == 0 && mem_writes == 0, "preload failure writes nothing");
    check_handoff_no_gpu_work();

    reset_all();
    load_image_result = EFI_SECURITY_VIOLATION;
    status = 0;
    halted = run_entry(&status);
    check(!halted && status == EFI_SECURITY_VIOLATION, "LoadImage failure is returned");
    check(load_image_calls == 1 && start_calls == 0, "LoadImage failure never starts Windows");
    check(pci_writes == 0 && mem_writes == 0, "LoadImage failure writes nothing");
    check_handoff_no_gpu_work();

    reset_all();
    write_log_fail_at = 1;
    status = 0;
    halted = run_entry(&status);
    check(!halted && status == EFI_DEVICE_ERROR, "post-preload log failure is returned");
    check(start_calls == 0, "post-preload log failure never starts Windows");
    check(pci_writes == 0 && mem_writes == 0, "post-preload log failure writes nothing");
    check_handoff_no_gpu_work();
}

static void test_handoff_start_image_error(void)
{
    reset_all();
    start_result = EFI_DEVICE_ERROR;
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && status == EFI_DEVICE_ERROR, "StartImage error returns normally");
    check(load_image_calls == 1 && start_calls == 1, "StartImage error was attempted once");
    check(pci_writes == 0 && mem_writes == 0, "StartImage error writes nothing");
    check_handoff_no_gpu_work();
}
#endif

#ifdef GU_PROBE_ONLY
static void test_probe_only(void)
{
    reset_all();
    EFI_STATUS status = 0;
    int halted = run_entry(&status);
    check(!halted && halt_entered == 0 && status == EFI_SUCCESS,
        "probe-only path returns success");
    check(pci_reads > 0 && bar_attr_calls == 1,
        "probe performs read-only PCI inventory");
    check(pci_writes == 0 && mem_writes == 0, "probe performs zero GPU writes");
    check(load_image_calls == 0 && start_calls == 0, "probe never loads/starts Windows");
    check(load_image_selected == NULL && load_image_parent == NULL &&
        load_image_path == NULL && !load_image_boot_policy &&
        load_image_source == NULL && load_image_source_size == 0,
        "probe records no LoadImage state");
    check(start_handle == NULL && start_exit_data_size == 0 && start_exit_data == NULL,
        "probe records no StartImage state");
    check(call_seq == 0 && load_image_seq == 0 && first_gpu_work_seq == 0 && start_seq == 0,
        "probe records no handoff sequence");
    check(print_calls > 0 && console_output_calls > 0 && console_after_reset == 0,
        "probe console messages present without a reset epoch");
    check(clock_calls == 0 && gop_handle_calls == 0 && gop_query_calls == 0 &&
        gop_set_calls == 0 && gop_blt_calls == 0, "probe performs no clock/GOP work");
    check(watchdog_timeout == 0 && watchdog_code == 0 &&
        watchdog_data_size == 0 && watchdog_data == NULL,
        "probe records no watchdog state");
    check(watchdog_calls == 0 && load_shared_calls == 0 && unlock_calls == 0,
        "probe performs zero transaction work");
    check(write_log_calls >= 1, "probe writes its log");
    check(strstr(captured_log, "PROBE COMPLETE") != NULL, "probe log is complete");
}
#endif

int main(void)
{
    reset_all();
#ifdef GU_PROBE_ONLY
    test_probe_only();
    puts("UEFI entry probe-only: zero GPU writes/LoadImage/StartImage");
#elif defined(GU_HANDOFF_ONLY)
    test_handoff_success_and_abort_status();
    test_handoff_target_and_windows_required();
    test_handoff_gop_association_and_metadata();
    test_handoff_preload_and_log_failures();
    test_handoff_start_image_error();
    puts("UEFI handoff-only baseline: GOP metadata, preload/start and no-GPU-work tests passed");
#else
    test_preload_before_gpu_work();
    test_unique_configured_volume();
    test_preload_failure_zero_work();
    test_load_image_failure_zero_work();
    test_checkpoint_failure_zero_work();
    test_watchdog_failure_zero_work();
    test_terminal_failures();
    puts("UEFI entry handoff: preload ordering, volume uniqueness, halt and no-console tests passed");
#endif
    return 0;
}
