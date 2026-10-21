/* SPDX-License-Identifier: GPL-2.0-only */
#include "env.h"
#include "policy.h"
#include "../kernel/gpu_unlock/unlock_610.h"

static EFI_GUID pci_id=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID fs_id=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID image_id=EFI_LOADED_IMAGE_PROTOCOL_GUID;

static EFI_STATUS open_self_volume(EFI_HANDLE image)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
    EFI_STATUS st=uefi_call_wrapper(BS->HandleProtocol,3,image,&image_id,(void **)&loaded);
    if(EFI_ERROR(st))return st;
    st=uefi_call_wrapper(BS->HandleProtocol,3,loaded->DeviceHandle,&fs_id,(void **)&fs);
    if(EFI_ERROR(st))return st;
    return uefi_call_wrapper(fs->OpenVolume,2,fs,&gu.root);
}
static EFI_STATUS find_card(const struct gu_options *o,bool *found)
{
    EFI_HANDLE *handles=NULL;UINTN count=0;*found=false;
    EFI_STATUS st=uefi_call_wrapper(BS->LocateHandleBuffer,5,ByProtocol,&pci_id,NULL,&count,&handles);
    if(EFI_ERROR(st))return st;
    for(UINTN i=0;i<count;i++) {
        EFI_PCI_IO_PROTOCOL *pci=NULL;UINTN segment,bus,device,function;
        u32 words[64];u8 *cfg=(u8 *)words;
        if(EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol,3,handles[i],&pci_id,(void **)&pci)))continue;
        if(EFI_ERROR(uefi_call_wrapper(pci->Pci.Read,5,pci,EfiPciIoWidthUint32,0,64,words)))continue;
        if(EFI_ERROR(uefi_call_wrapper(pci->GetLocation,5,pci,&segment,&bus,&device,&function)))continue;
        bool selected_bdf=o->has_target && segment==o->segment && bus==o->bus &&
                          device==o->device && function==o->function;
        if((words[0]&0xffff)==0x10de || selected_bdf)
            gu_log("PCI %04x:%02x:%02x.%u identity=%08x subsystem=%08x command=%#x\n",
                   (unsigned int)segment,(unsigned int)bus,(unsigned int)device,(unsigned int)function,
                   words[0],words[11],(unsigned int)(words[1]&0xffff));
        if(!o->has_target || segment!=o->segment || bus!=o->bus || device!=o->device || function!=o->function)continue;
        if(*found){st=EFI_COMPROMISED_DATA;goto out;}
        if(!rtx3080ti_board_matches(words[0]&0xffff,words[0]>>16,
                                    words[11]&0xffff,words[11]>>16)) {
            gu_log("Configured BDF is not exact target 10de:2208 / subsystem 10de:1535; refusing\n");
            st=EFI_UNSUPPORTED;goto out;
        }
        if(o->execute && ((words[11]&0xffff)!=o->board_vendor || (words[11]>>16)!=o->board_device)) {
            gu_log("Explicit subsystem identity mismatch; no hardware work\n");st=EFI_ACCESS_DENIED;goto out;
        }
        int cap=gu_capability_offset(cfg,0x10),pm=gu_capability_offset(cfg,1);
        if(cap<0 || pm<0){st=EFI_COMPROMISED_DATA;goto out;}
        void *resources=NULL;u64 supported=0;
        st=uefi_call_wrapper(pci->GetBarAttributes,4,pci,0,&supported,&resources);
        if(EFI_ERROR(st))goto out;
        gu.bar_length=gu_bar_resource_length(resources,46);FreePool(resources);
        if(gu.bar_length<0x842000 || !cap){st=EFI_UNSUPPORTED;goto out;}
        gu.pci=pci;gu.pci_handle=handles[i];gu.pcie_cap=(unsigned int)cap;gu.dma_mask=~0ull;
        gu.card=(struct pci_dev){.vendor=0x10de,.device=RTX3080TI_PCI_ID,
            .subsystem_vendor=words[11]&0xffff,.subsystem_device=words[11]>>16,
            .current_state=PCI_D0,.pm_cap=pm,.refs=1,
            .dev={.dma_mask=&gu.dma_mask,.coherent_dma_mask=~0ull}};
        *found=true;st=EFI_SUCCESS;
        gu_log("Selected exact 10de:2208 / subsystem 10de:1535; BAR0 length=%#llx PCIe cap=%#x PM cap=%#x\n",
               (unsigned long long)gu.bar_length,gu.pcie_cap,(unsigned int)pm);
#ifdef GU_HANDOFF_ONLY
        for(unsigned int j=0;j<64;j+=4)
            gu_log("BASELINE PCI offset=%#x: %08x %08x %08x %08x\n",
                   j*4,words[j],words[j+1],words[j+2],words[j+3]);
#endif
    }
    st=EFI_SUCCESS;
out:
    FreePool(handles);return st;
}
#ifndef GU_PROBE_ONLY
static EFI_STATUS load_windows(EFI_HANDLE parent,EFI_HANDLE *image,const struct gu_options *options)
{
    EFI_HANDLE *handles=NULL,selected=NULL;UINTN count=0,matches=0;
    const CHAR16 *path=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    EFI_STATUS st=uefi_call_wrapper(BS->LocateHandleBuffer,5,ByProtocol,&fs_id,NULL,&count,&handles);
    if(EFI_ERROR(st))return st;
    for(UINTN i=0;i<count;i++) {
        if(options->has_windows_partition) {
            EFI_DEVICE_PATH_PROTOCOL *dp=DevicePathFromHandle(handles[i]);
            if(!dp || gu_partition_matches((const u8 *)dp,DevicePathSize(dp),options->windows_partition)!=1)continue;
        }
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_FILE_HANDLE root=NULL,file=NULL;
        if(EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol,3,handles[i],&fs_id,(void **)&fs)))continue;
        if(EFI_ERROR(uefi_call_wrapper(fs->OpenVolume,2,fs,&root)))continue;
        st=uefi_call_wrapper(root->Open,5,root,&file,(CHAR16 *)path,EFI_FILE_MODE_READ,0);
        if(!EFI_ERROR(st)){matches++;selected=handles[i];uefi_call_wrapper(file->Close,1,file);}
        uefi_call_wrapper(root->Close,1,root);
    }
    FreePool(handles);
    if(matches!=1){gu_log("Expected exactly one Windows Boot Manager%s, found %u\n",
        options->has_windows_partition?" on configured GPT partition":"",(unsigned int)matches);return EFI_NOT_FOUND;}
    gu_log("Unique Windows Boot Manager selected%s\n",options->has_windows_partition?" by GPT partition GUID":"");
    EFI_DEVICE_PATH_PROTOCOL *dp=FileDevicePath(selected,(CHAR16 *)path);
    if(!dp)return EFI_OUT_OF_RESOURCES;
    st=uefi_call_wrapper(BS->LoadImage,6,FALSE,parent,dp,NULL,0,image);FreePool(dp);
    return st;
}
#ifndef GU_HANDOFF_ONLY
static void halt_after_attempt(int ret)
{
    gu_log("STOP: experiment status=%d retained=%u io_fault=%u. Full power off required; Windows will NOT start.\n",
           ret,(unsigned int)gu.retained,(unsigned int)gu.io_fault);
    gu_write_log();
    /* Never return to BDS/ExitBootServices with possibly live GPU DMA.
     * Console GOP may have been reset; no firmware graphics calls here. */
    for(;;)uefi_call_wrapper(BS->Stall,1,1000000);
}
#endif
#endif
#ifdef GU_HANDOFF_ONLY
/* Metadata only: never SetMode, reconnect a driver, touch a framebuffer, or
 * replay GPU registers. A valid GOP structure is not proof of live scanout.
 * Windows inherits the boot framebuffer; this baseline tests that handoff
 * separately from the destructive GPU transaction. */
static EFI_STATUS capture_boot_display(void)
{
    EFI_GUID gop_id=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *target=DevicePathFromHandle(gu.pci_handle);
    if(!target)return EFI_NOT_FOUND;
    UINTN target_size=DevicePathSize(target);
    if(target_size<=sizeof(EFI_DEVICE_PATH_PROTOCOL))return EFI_UNSUPPORTED;
    UINTN prefix=target_size-sizeof(EFI_DEVICE_PATH_PROTOCOL);
    EFI_HANDLE *handles=NULL;UINTN count=0,matched=0;
    EFI_STATUS st=uefi_call_wrapper(BS->LocateHandleBuffer,5,ByProtocol,&gop_id,NULL,&count,&handles);
    if(EFI_ERROR(st))return st;
    for(UINTN i=0;i<count;i++) {
        EFI_DEVICE_PATH_PROTOCOL *path=DevicePathFromHandle(handles[i]);
        if(!path || DevicePathSize(path)<target_size || gu_memcmp(target,path,prefix))continue;
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;
        st=uefi_call_wrapper(BS->HandleProtocol,3,handles[i],&gop_id,(void **)&gop);
        if(EFI_ERROR(st))goto out;
        if(!gop || !gop->Mode || !gop->Mode->Info ||
           gop->Mode->SizeOfInfo<sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION) ||
           gop->Mode->Mode>=gop->Mode->MaxMode) {
            st=EFI_UNSUPPORTED;goto out;
        }
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info=gop->Mode->Info;
        UINT64 base=gop->Mode->FrameBufferBase,bytes=gop->Mode->FrameBufferSize;
        if((info->PixelFormat!=PixelRedGreenBlueReserved8BitPerColor &&
            info->PixelFormat!=PixelBlueGreenRedReserved8BitPerColor) ||
           !info->HorizontalResolution || !info->VerticalResolution ||
           info->PixelsPerScanLine<info->HorizontalResolution ||
           !base || !bytes || base>~0ull-(bytes-1) ||
           (UINT64)info->PixelsPerScanLine>bytes/4/info->VerticalResolution) {
            st=EFI_UNSUPPORTED;goto out;
        }
        matched++;
        gu_log("BASELINE target GOP mode=%u max=%u width=%u height=%u stride=%u format=%u framebuffer=%#llx bytes=%#llx\n",
               gop->Mode->Mode,gop->Mode->MaxMode,info->HorizontalResolution,
               info->VerticalResolution,info->PixelsPerScanLine,(unsigned int)info->PixelFormat,
               (unsigned long long)base,(unsigned long long)bytes);
    }
    st=matched?EFI_SUCCESS:EFI_NOT_FOUND;
out:
    FreePool(handles);return st;
}
#endif
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *system)
{
    InitializeLib(image,system);
    Print(L"RTX 3080 Ti 12GB EFI experiment 0.4.0-rtx3080ti\r\n");
#ifdef GU_HANDOFF_ONLY
    Print(L"Windows handoff baseline: this app performs no GPU transaction.\r\n");
#else
    Print(L"Default: PCI inventory only. No Windows driver is installed.\r\n");
#endif
    EFI_STATUS st=open_self_volume(image);
    if(EFI_ERROR(st)){Print(L"Cannot open launch volume: %r\r\n",st);return st;}
    gu_log("RTX 3080 Ti EFI 0.4.0-rtx3080ti; hardware, Windows 617.42, and VBIOS 94.02.71.00.83 unvalidated\n");
    struct gu_options options={0};void *config=NULL;UINTN config_size=0;
    st=gu_read_file(L"\\ga102-3080ti\\config.txt",&config,&config_size,4096);
    if(EFI_ERROR(st)) {
        gu_log("No configuration: read-only PCI inventory\n");
    } else {
        if(gu_parse_options(config,config_size,&options)){FreePool(config);gu_log("Invalid or duplicate config fields; refusing\n");gu_write_log();return EFI_INVALID_PARAMETER;}
        FreePool(config);
    }
#if defined(GU_PROBE_ONLY) || defined(GU_HANDOFF_ONLY)
#ifdef GU_HANDOFF_ONLY
    if(options.execute)gu_log("Handoff-only image: execute ignored\n");
#else
    if(options.execute)gu_log("Probe-only image: execute ignored\n");
#endif
    options.execute=false;
#endif
    bool found=false;
    st=find_card(&options,&found);
    if(EFI_ERROR(st)){gu_log("PCI selection refused, EFI status=%#llx\n",(unsigned long long)st);gu_write_log();return st;}
#ifdef GU_HANDOFF_ONLY
    if(!found || !options.windows) {
        gu_log("HANDOFF BASELINE refused: exact target and windows=1 are required\n");
        gu_write_log();return EFI_NOT_FOUND;
    }
    st=capture_boot_display();
    if(EFI_ERROR(st)) {
        gu_log("HANDOFF BASELINE target linear GOP unavailable: %#llx\n",(unsigned long long)st);
        gu_write_log();return st;
    }
    EFI_HANDLE baseline_windows=NULL;
    st=load_windows(image,&baseline_windows,&options);
    if(EFI_ERROR(st)){gu_log("HANDOFF BASELINE preload failed: %#llx\n",(unsigned long long)st);gu_write_log();return st;}
    gu_log("HANDOFF BASELINE: zero GPU config writes, zero MMIO, zero firmware requests by this app. Starting preloaded Windows; no reset or unlock performed.\n");
    st=gu_write_log();
    if(EFI_ERROR(st))return st;
    st=uefi_call_wrapper(BS->StartImage,3,baseline_windows,NULL,NULL);
    gu_log("HANDOFF BASELINE Windows Boot Manager returned: %#llx; return is not proof of Windows boot\n",(unsigned long long)st);
    gu_write_log();
    return EFI_ERROR(st)?st:EFI_ABORTED;
#endif
    if(!options.execute) {
        gu_log("PROBE COMPLETE: zero GPU config writes, zero MMIO, zero firmware requests\n");
        Print(L"%a",gu.log);
        st=gu_write_log();
        if(EFI_ERROR(st)){Print(L"Probe log write failed: %r\r\n",st);return st;}
        Print(L"Probe complete. See ga102-3080ti-log.txt on this volume.\r\n");return EFI_SUCCESS;
    }
#if !defined(GU_PROBE_ONLY) && !defined(GU_HANDOFF_ONLY)
    if(!found){gu_log("Exact configured target not found\n");gu_write_log();return EFI_NOT_FOUND;}
    int ret=gu_init_clock();
    if(ret){gu_log("Invariant TSC unavailable; cannot enforce timeouts\n");gu_write_log();return EFI_UNSUPPORTED;}
    EFI_HANDLE windows=NULL;
    if(options.windows) {
        st=load_windows(image,&windows,&options);
        if(EFI_ERROR(st)){gu_log("Cannot preload unique Windows Boot Manager: %#llx\n",(unsigned long long)st);gu_write_log();return st;}
    }
    /* Checkpoint the log before touching any GPU state. */
    gu_log("EXECUTE requested: exact target and board matched; SS1 policy=%s\n",options.ss1_full?"full=8":"preserve");
    if(EFI_ERROR(gu_write_log()))return EFI_DEVICE_ERROR;
    /* A firmware app watchdog must not reboot a retained-DMA failure into BDS. */
    st=uefi_call_wrapper(BS->SetWatchdogTimer,4,0,0,0,NULL);
    if(EFI_ERROR(st)){gu_log("Cannot disable app watchdog; refusing transaction\n");gu_write_log();return st;}
    Print(L"Experimental GPU transaction starts. Screen may go blank.\r\n");
    Print(L"On failure this app halts. Power off fully; inspect ga102-3080ti-log.txt.\r\n");
    struct gpu_unlock_610_shared shared={0};
    ret=gpu_unlock_610_load_shared(&gu.card,&shared);
    if(!ret)ret=gpu_unlock_610_unlock(&gu.card,&shared,options.ss1_full?GPU_UNLOCK_SS1_FULL_EXPERIMENT:GPU_UNLOCK_SS1_PRESERVE);
    gpu_unlock_610_release_shared(&shared);
    if(ret || gu.retained || gu.io_fault || gu_live_dma_count())halt_after_attempt(ret?ret:-EIO);
    gu_log("SS0/SS1 readback and FLR containment passed; actual Windows handoff and speedup still require measurement\n");
    if(EFI_ERROR(gu_write_log()))halt_after_attempt(-EIO);
    if(!options.windows)halt_after_attempt(0);
    st=uefi_call_wrapper(BS->StartImage,3,windows,NULL,NULL);
    gu_log("Windows Boot Manager returned: %#llx\n",(unsigned long long)st);
    halt_after_attempt(-EIO);
#endif
    return EFI_SUCCESS;
}
