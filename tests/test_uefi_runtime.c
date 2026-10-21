/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "env.h"

EFI_BOOT_SERVICES *BS;
static int allocs,maps,unmaps,frees,fail_map,short_map,fail_unmap;
static int fail_free,free_attempts,active_mappings;
static void *last_host;
static u64 device_address=0x12345000;
static UINTN last_pages,last_bytes;
static EFI_STATUS EFIAPI mock_alloc(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE t,
    EFI_MEMORY_TYPE type,UINTN pages,void **host,UINT64 attrs)
{
    assert(t==AllocateAnyPages && type==EfiBootServicesData && !attrs);
    allocs++;last_pages=pages;*host=calloc(pages,4096);last_host=*host;return *host?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI mock_map(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION op,
    void *host,UINTN *bytes,EFI_PHYSICAL_ADDRESS *addr,void **mapping)
{
    assert(op==EfiPciIoOperationBusMasterCommonBuffer);
    maps++;last_bytes=*bytes;if(fail_map)return EFI_DEVICE_ERROR;
    if(short_map)*bytes=1;*addr=device_address;*mapping=host;active_mappings++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_unmap(EFI_PCI_IO_PROTOCOL *p,void *mapping)
{
    assert(mapping);unmaps++;if(fail_unmap)return EFI_DEVICE_ERROR;
    assert(active_mappings>0);active_mappings--;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_free(EFI_PCI_IO_PROTOCOL *p,UINTN pages,void *host)
{
    assert(pages==last_pages);free_attempts++;
    if(fail_free)return EFI_DEVICE_ERROR;
    assert(!active_mappings);frees++;free(host);return EFI_SUCCESS;
}
static int mmio_reads,mmio_writes,fail_mmio;
static EFI_STATUS EFIAPI mock_read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 bar,UINT64 offset,UINTN count,void *buffer)
{
    assert(bar==0 && offset==0x1234 && count==1 && width==EfiPciIoWidthUint32);
    mmio_reads++;*(u32 *)buffer=0x172000a1;return fail_mmio?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 bar,UINT64 offset,UINTN count,void *buffer)
{ assert(bar==0 && offset==0x1234 && count==1 && *(u32 *)buffer==7);mmio_writes++;return EFI_SUCCESS; }

static u32 flr_identity;
static u16 flr_pending;
static int flr_requests,identity_reads;
static int pending_clear_after,pending_reads,flr_before_pending_clear;
static int pending_cleared,flr_write_before_pending_clear;
static int pci_read_fail_offset=-1,pci_write_fail_offset=-1;
static unsigned int pci_write_count,stall_1000,stall_100000;
static u8 pci_config[256];

static u16 config_read16(UINT32 offset)
{
    assert(offset+2<=sizeof(pci_config));
    return (u16)(pci_config[offset]|((u16)pci_config[offset+1]<<8));
}
static u32 config_read32(UINT32 offset)
{
    assert(offset+4<=sizeof(pci_config));
    return (u32)pci_config[offset]|((u32)pci_config[offset+1]<<8)|
           ((u32)pci_config[offset+2]<<16)|((u32)pci_config[offset+3]<<24);
}
static void config_write16(UINT32 offset,u16 value)
{
    assert(offset+2<=sizeof(pci_config));
    pci_config[offset]=(u8)value;pci_config[offset+1]=(u8)(value>>8);
}
static void config_write32(UINT32 offset,u32 value)
{
    assert(offset+4<=sizeof(pci_config));
    for(unsigned int i=0;i<4;i++)pci_config[offset+i]=(u8)(value>>(8*i));
}
static EFI_STATUS EFIAPI mock_pci_read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT32 offset,UINTN count,void *buffer)
{
    if(width==EfiPciIoWidthUint32 && offset==0 && count==1)identity_reads++;
    if(pci_read_fail_offset>=0 && (int)offset==pci_read_fail_offset)return EFI_DEVICE_ERROR;
    if(width==EfiPciIoWidthUint32) {
        if(offset==0 && count==1) {
            *(u32 *)buffer=flr_identity;
        } else {
            for(UINTN i=0;i<count;i++)((u32 *)buffer)[i]=config_read32(offset+4*(UINT32)i);
        }
    } else if(width==EfiPciIoWidthUint16) {
        assert(count==1);
        if(offset==0x4a) {
            u16 pending=flr_pending;
            pending_reads++;
            if(pending_clear_after<0)pending=PCI_EXP_DEVSTA_TRPND;
            else if(pending_clear_after && pending_reads>=pending_clear_after)pending=0;
            if((pending&PCI_EXP_DEVSTA_TRPND) && flr_requests)flr_before_pending_clear=1;
            pending_cleared=!(pending&PCI_EXP_DEVSTA_TRPND);
            *(u16 *)buffer=pending;
        } else *(u16 *)buffer=config_read16(offset);
    } else return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_pci_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT32 offset,UINTN count,void *buffer)
{
    pci_write_count++;
    if(pci_write_fail_offset>=0 && (int)offset==pci_write_fail_offset)return EFI_DEVICE_ERROR;
    if(width==EfiPciIoWidthUint32) {
        for(UINTN i=0;i<count;i++)config_write32(offset+4*(UINT32)i,((u32 *)buffer)[i]);
    } else if(width==EfiPciIoWidthUint16) {
        assert(count==1);
        if(offset==0x48 && (*(u16 *)buffer&(1u<<15))) {
            if(!pending_cleared) {
                flr_write_before_pending_clear=1;
                return EFI_DEVICE_ERROR;
            }
            flr_requests++;
        }
        config_write16(offset,*(u16 *)buffer);
    } else return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_stall(UINTN microseconds)
{
    assert(microseconds==100000 || microseconds==1000);
    if(microseconds==100000)stall_100000++;else stall_1000++;
    return EFI_SUCCESS;
}

static struct _EFI_FILE_HANDLE mock_root_protocol,mock_log_protocol;
static char mock_log_storage[GU_LOG_CAP+4096];
static size_t mock_log_storage_length;
static UINT64 mock_file_position;
static unsigned int file_open_calls,file_set_position_calls,file_write_calls;
static unsigned int file_flush_calls,file_close_calls,file_delete_calls;
static bool mock_file_opened,limit_file_write;
static UINTN file_write_limit;
static EFI_STATUS file_open_result,file_set_position_result,file_write_result;
static EFI_STATUS file_flush_result,file_close_result;
static const char existing_log_sentinel[]="existing-log-sentinel\n";
static const char checkpoint_begin[]="\n=== GPU-UNLOCK LOG CHECKPOINT BEGIN ===\n";
static const char checkpoint_truncated[]="\n[log truncated: buffer capacity reached]\n";
static const char checkpoint_end[]="\n=== GPU-UNLOCK LOG CHECKPOINT END ===\n";
static char expected_log[GU_LOG_CAP+1024];

static EFI_STATUS EFIAPI mock_file_delete(EFI_FILE_HANDLE self)
{
    assert(self==&mock_root_protocol || self==&mock_log_protocol);
    file_delete_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI mock_file_open(EFI_FILE_HANDLE self,EFI_FILE_HANDLE *new_file,
    CHAR16 *filename,UINT64 mode,UINT64 attributes)
{
    static const CHAR16 expected_path[]=L"\\ga102-3080ti-log.txt";
    assert(self==&mock_root_protocol && filename);
    assert(!memcmp(filename,expected_path,sizeof(expected_path)));
    assert(mode&EFI_FILE_MODE_WRITE);assert(attributes==0);
    assert(mock_log_storage_length>=sizeof(existing_log_sentinel)-1);
    file_open_calls++;
    *new_file=NULL;
    if(EFI_ERROR(file_open_result))return file_open_result;
    *new_file=&mock_log_protocol;mock_file_opened=true;mock_file_position=0;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_file_set_position(EFI_FILE_HANDLE self,UINT64 position)
{
    assert(self==&mock_log_protocol && mock_file_opened);
    assert(position==~0ULL);file_set_position_calls++;
    if(EFI_ERROR(file_set_position_result))return file_set_position_result;
    mock_file_position=mock_log_storage_length;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_file_write(EFI_FILE_HANDLE self,UINTN *size,VOID *buffer)
{
    assert(self==&mock_log_protocol && mock_file_opened && size && buffer);
    assert(mock_file_position==mock_log_storage_length);file_write_calls++;
    if(EFI_ERROR(file_write_result)){*size=0;return file_write_result;}
    UINTN accepted=*size;
    if(limit_file_write && accepted>file_write_limit)accepted=file_write_limit;
    assert(accepted<=sizeof(mock_log_storage)-mock_log_storage_length);
    memcpy(mock_log_storage+mock_log_storage_length,buffer,accepted);
    mock_log_storage_length+=accepted;mock_file_position+=accepted;*size=accepted;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mock_file_flush(EFI_FILE_HANDLE self)
{
    assert(self==&mock_log_protocol && mock_file_opened);file_flush_calls++;
    return file_flush_result;
}
static EFI_STATUS EFIAPI mock_file_close(EFI_FILE_HANDLE self)
{
    assert(self==&mock_log_protocol && mock_file_opened);file_close_calls++;
    mock_file_opened=false;return file_close_result;
}
static void init_file_protocol_mocks(void)
{
    mock_root_protocol.Open=mock_file_open;mock_root_protocol.Delete=mock_file_delete;
    mock_log_protocol.Delete=mock_file_delete;mock_log_protocol.SetPosition=mock_file_set_position;
    mock_log_protocol.Write=mock_file_write;mock_log_protocol.Flush=mock_file_flush;
    mock_log_protocol.Close=mock_file_close;
    gu.root=&mock_root_protocol;
}
static void reset_file_mock_controls(void)
{
    mock_file_position=0;file_open_calls=file_set_position_calls=file_write_calls=0;
    file_flush_calls=file_close_calls=file_delete_calls=0;mock_file_opened=false;
    limit_file_write=false;file_write_limit=0;
    file_open_result=file_set_position_result=file_write_result=EFI_SUCCESS;
    file_flush_result=file_close_result=EFI_SUCCESS;
}
static void reset_file_mock(void)
{
    memcpy(mock_log_storage,existing_log_sentinel,sizeof(existing_log_sentinel)-1);
    mock_log_storage_length=sizeof(existing_log_sentinel)-1;
    reset_file_mock_controls();
}
static void set_test_log(const char *body)
{
    size_t length=strlen(body);assert(length<sizeof(gu.log));
    memcpy(gu.log,body,length+1);gu.log_length=length;gu.log_truncated=false;
}
static size_t add_expected_checkpoint(char *output,size_t offset,const char *body,
    size_t body_length,bool truncated)
{
    memcpy(output+offset,checkpoint_begin,sizeof(checkpoint_begin)-1);
    offset+=sizeof(checkpoint_begin)-1;
    memcpy(output+offset,body,body_length);offset+=body_length;
    if(truncated) {
        memcpy(output+offset,checkpoint_truncated,sizeof(checkpoint_truncated)-1);
        offset+=sizeof(checkpoint_truncated)-1;
    }
    memcpy(output+offset,checkpoint_end,sizeof(checkpoint_end)-1);
    return offset+sizeof(checkpoint_end)-1;
}
static void assert_log_storage(const char *expected,size_t length)
{
    assert(mock_log_storage_length==length);
    assert(!memcmp(mock_log_storage,expected,length));
    assert(file_delete_calls==0);
}
static void test_complete_log_checkpoints(void)
{
    reset_file_mock();size_t expected_length=sizeof(existing_log_sentinel)-1;
    memcpy(expected_log,existing_log_sentinel,expected_length);
    set_test_log("first complete snapshot");
    assert(gu_write_log()==EFI_SUCCESS);
    expected_length=add_expected_checkpoint(expected_log,expected_length,gu.log,
        gu.log_length,false);
    assert_log_storage(expected_log,expected_length);
    assert(file_open_calls==1 && file_set_position_calls==1 && file_write_calls>0);
    assert(file_flush_calls==1 && file_close_calls==1 && !mock_file_opened);

    set_test_log("second complete snapshot");
    assert(gu_write_log()==EFI_SUCCESS);
    expected_length=add_expected_checkpoint(expected_log,expected_length,gu.log,
        gu.log_length,false);
    assert_log_storage(expected_log,expected_length);
    assert(file_open_calls==2 && file_set_position_calls==2 && file_write_calls>1);
    assert(file_flush_calls==2 && file_close_calls==2 && !mock_file_opened);
}
static void test_log_failure_paths(void)
{
    reset_file_mock();set_test_log("checkpoint body");
    file_open_result=EFI_DEVICE_ERROR;
    assert(gu_write_log()==EFI_DEVICE_ERROR);
    assert(file_open_calls==1 && !file_set_position_calls && !file_write_calls);
    assert(!file_flush_calls && !file_close_calls && !mock_file_opened);
    assert_log_storage(existing_log_sentinel,sizeof(existing_log_sentinel)-1);

    reset_file_mock();set_test_log("checkpoint body");
    file_set_position_result=EFI_DEVICE_ERROR;
    assert(gu_write_log()==EFI_DEVICE_ERROR);
    assert(file_open_calls==1 && file_set_position_calls==1 && !file_write_calls);
    assert(!file_flush_calls && file_close_calls==1 && !mock_file_opened);
    assert_log_storage(existing_log_sentinel,sizeof(existing_log_sentinel)-1);

    reset_file_mock();set_test_log("checkpoint body");
    file_write_result=EFI_DEVICE_ERROR;
    assert(EFI_ERROR(gu_write_log()));
    assert(file_write_calls>0 && file_flush_calls==1 && file_close_calls==1);
    assert(!mock_file_opened);
    assert_log_storage(existing_log_sentinel,sizeof(existing_log_sentinel)-1);

    reset_file_mock();set_test_log("checkpoint body");
    limit_file_write=true;file_write_limit=5;
    assert(EFI_ERROR(gu_write_log()));
    assert(file_write_calls==1 && file_flush_calls==1 && file_close_calls==1);
    assert(!mock_file_opened);
    assert(mock_log_storage_length==sizeof(existing_log_sentinel)-1+5);
    assert(!memcmp(mock_log_storage,existing_log_sentinel,sizeof(existing_log_sentinel)-1));
    assert(!memcmp(mock_log_storage+sizeof(existing_log_sentinel)-1,checkpoint_begin,5));
    assert(file_delete_calls==0);

    reset_file_mock();set_test_log("checkpoint body");
    file_flush_result=EFI_DEVICE_ERROR;
    assert(gu_write_log()==EFI_DEVICE_ERROR);
    size_t expected_length=sizeof(existing_log_sentinel)-1;
    memcpy(expected_log,existing_log_sentinel,expected_length);
    expected_length=add_expected_checkpoint(expected_log,expected_length,gu.log,
        gu.log_length,false);
    assert_log_storage(expected_log,expected_length);
    assert(file_flush_calls==1 && file_close_calls==1 && !mock_file_opened);

    reset_file_mock();set_test_log("checkpoint body");
    file_close_result=EFI_DEVICE_ERROR;
    assert(gu_write_log()==EFI_DEVICE_ERROR);
    expected_length=sizeof(existing_log_sentinel)-1;
    memcpy(expected_log,existing_log_sentinel,expected_length);
    expected_length=add_expected_checkpoint(expected_log,expected_length,gu.log,
        gu.log_length,false);
    assert_log_storage(expected_log,expected_length);
    assert(file_flush_calls==1 && file_close_calls==1 && !mock_file_opened);
}
static void test_partial_append_preserves_committed_prefix(void)
{
    reset_file_mock();set_test_log("committed checkpoint");
    size_t committed_length=sizeof(existing_log_sentinel)-1;
    memcpy(expected_log,existing_log_sentinel,committed_length);
    assert(gu_write_log()==EFI_SUCCESS);
    committed_length=add_expected_checkpoint(expected_log,committed_length,gu.log,
        gu.log_length,false);
    assert_log_storage(expected_log,committed_length);

    reset_file_mock_controls();set_test_log("next checkpoint");
    limit_file_write=true;file_write_limit=7;
    assert(EFI_ERROR(gu_write_log()));
    assert(file_write_calls==1 && file_flush_calls==1 && file_close_calls==1);
    assert(mock_log_storage_length==committed_length+7);
    assert(!memcmp(mock_log_storage,expected_log,committed_length));
    assert(!memcmp(mock_log_storage+committed_length,checkpoint_begin,7));
    assert(file_delete_calls==0 && !mock_file_opened);
}
static void test_log_truncation_checkpoint(void)
{
    static const char prior_log_bytes[]="pre-existing log bytes";
    reset_file_mock();set_test_log(prior_log_bytes);
    size_t input_length=(size_t)GU_LOG_CAP+64;
    char *input=malloc(input_length+1);assert(input);
    memset(input,'T',input_length);input[input_length]=0;
    gu_log("%s",input);free(input);
    assert(gu.log_truncated && gu.log_length==GU_LOG_CAP-1);
    assert(!memcmp(gu.log,prior_log_bytes,sizeof(prior_log_bytes)-1));
    for(size_t i=sizeof(prior_log_bytes)-1;i<gu.log_length;i++)assert(gu.log[i]=='T');
    assert(gu.log[GU_LOG_CAP-1]==0);

    size_t expected_length=sizeof(existing_log_sentinel)-1;
    memcpy(expected_log,existing_log_sentinel,expected_length);
    expected_length=add_expected_checkpoint(expected_log,expected_length,gu.log,
        gu.log_length,true);
    assert(gu_write_log()==EFI_BAD_BUFFER_SIZE);
    assert_log_storage(expected_log,expected_length);
    assert(file_open_calls==1 && file_set_position_calls==1 && file_write_calls>0);
    assert(file_flush_calls==1 && file_close_calls==1 && !mock_file_opened);
}

static void reset_pci_mock(void)
{
    memset(pci_config,0,sizeof(pci_config));
    config_write16(4,0x0007);
    for(unsigned int i=0;i<6;i++)config_write32(16+4*i,0x10000000u+i*0x1000u);
    config_write16(0x48,0x0006);
    pci_config[0x34]=0x40;pci_config[0x40]=0x10;pci_config[0x41]=0x50;
    pci_config[0x50]=0x01;pci_config[0x51]=0;
    flr_identity=0x220810de;flr_pending=0;
    flr_requests=identity_reads=0;pci_write_count=0;
    pending_clear_after=pending_reads=flr_before_pending_clear=0;
    pending_cleared=flr_write_before_pending_clear=0;
    pci_read_fail_offset=pci_write_fail_offset=-1;
    stall_1000=stall_100000=0;
    gu.pcie_cap=0x40;gu.io_fault=false;
}

static void test_pci_state_roundtrip(void)
{
    reset_pci_mock();
    memset(gu.saved_bars,0,sizeof(gu.saved_bars));gu.saved_command=0;
    assert(pci_save_state(&gu.card)==0);
    assert(gu.saved_command==0x0007);
    for(unsigned int i=0;i<6;i++)assert(gu.saved_bars[i]==0x10000000u+i*0x1000u);
    assert(pci_write_count==0); /* Saving state must not write config space. */

    config_write16(4,0);
    for(unsigned int i=0;i<6;i++)config_write32(16+4*i,0);
    pci_restore_state(&gu.card);
    assert(pci_write_count==7); /* Six BARs followed by Command. */
    assert(config_read16(4)==0x0003); /* BME cleared; IO/MEM preserved. */
    for(unsigned int i=0;i<6;i++)assert(config_read32(16+4*i)==0x10000000u+i*0x1000u);
    assert(!gu.io_fault);

    /* A read failure in Command or any BAR aborts before any write. */
    for(unsigned int i=0;i<7;i++) {
        reset_pci_mock();
        UINT32 offset=i?16+4*(i-1):4;
        pci_read_fail_offset=(int)offset;
        assert(pci_save_state(&gu.card)==-EIO);
        assert(pci_write_count==0);
    }

    /* A failed BAR restore is fatal but must not skip the remaining writes. */
    reset_pci_mock();
    assert(pci_save_state(&gu.card)==0);
    for(unsigned int i=0;i<6;i++)config_write32(16+4*i,0);
    pci_write_fail_offset=20;pci_write_count=0;gu.io_fault=false;
    pci_restore_state(&gu.card);
    assert(gu.io_fault && pci_write_count==7);
    assert(config_read16(4)==0x0003);

    /* A failed Command restore is also recorded and does not hide BAR writes. */
    reset_pci_mock();
    assert(pci_save_state(&gu.card)==0);
    config_write16(4,0);
    pci_write_fail_offset=4;pci_write_count=0;gu.io_fault=false;
    pci_restore_state(&gu.card);
    assert(gu.io_fault && pci_write_count==7);
    assert(config_read16(4)==0);

    reset_pci_mock();
    config_write16(4,0x0002);
    pci_set_master(&gu.card);assert(config_read16(4)==0x0006);
    pci_clear_master(&gu.card);assert(config_read16(4)==0x0002);
    pci_read_fail_offset=4;
    pci_set_master(&gu.card);assert(config_read16(4)==0x0002);
    pci_clear_master(&gu.card);assert(config_read16(4)==0x0002);
}

static void test_pcie_flr_behavior(void)
{
    reset_pci_mock();
    assert(pcie_flr(&gu.card)==0);
    assert(flr_requests==1 && identity_reads==1 && stall_100000==1);
    assert(config_read16(0x48)==0x8006); /* Bit 15 set; other CTRL bits kept. */
    assert(pci_write_count==1 && !flr_write_before_pending_clear);

    /* Pending must clear before the FLR request; each poll sleeps 1000 us. */
    reset_pci_mock();
    flr_pending=PCI_EXP_DEVSTA_TRPND;pending_clear_after=4;
    assert(pcie_flr(&gu.card)==0);
    assert(flr_requests==1 && stall_1000==3 && stall_100000==1);
    assert(!flr_before_pending_clear && !flr_write_before_pending_clear);

    /* A never-clearing pending bit times out without issuing FLR. */
    reset_pci_mock();
    flr_pending=PCI_EXP_DEVSTA_TRPND;pending_clear_after=-1;
    assert(pcie_flr(&gu.card)==-ETIMEDOUT);
    assert(flr_requests==0 && pci_write_count==0);
    assert(stall_1000==999 && !flr_before_pending_clear);
    assert(!flr_write_before_pending_clear);

    reset_pci_mock();
    pci_read_fail_offset=0x48;
    assert(pcie_flr(&gu.card)==-EIO && flr_requests==0);

    reset_pci_mock();
    pci_write_fail_offset=0x48;
    assert(pcie_flr(&gu.card)==-EIO && flr_requests==0 && pci_write_count==1);

    reset_pci_mock();
    pci_read_fail_offset=0;
    assert(pcie_flr(&gu.card)==-ETIMEDOUT);
    /* The identity loop sleeps on its final iteration too. */
    assert(identity_reads==1000 && flr_requests==1 && stall_1000==1000);

    for(unsigned int i=0;i<2;i++) {
        reset_pci_mock();
        flr_identity=i?0x250410de:0x220410de;
        assert(pcie_flr(&gu.card)==-ETIMEDOUT);
        assert(identity_reads==1000 && flr_requests==1 && stall_1000==1000);
    }
}

int main(void)
{
    EFI_PCI_IO_PROTOCOL pci={.AllocateBuffer=mock_alloc,.Map=mock_map,.Unmap=mock_unmap,
        .FreeBuffer=mock_free,.Mem={.Read=mock_read,.Write=mock_write},
        .Pci={.Read=mock_pci_read,.Write=mock_pci_write}};
    EFI_BOOT_SERVICES services={.Stall=mock_stall};BS=&services;
    init_file_protocol_mocks();
    gu.pci=&pci;gu.card.dev.coherent_dma_mask=0xffffffffull;
    dma_addr_t address;void *p=dma_alloc_coherent(&gu.card.dev,5,&address,0);
    assert(p && address==device_address && last_pages==1 && last_bytes==4096);
    /* Device addresses come from Map, never from a cast of the host pointer. */
    assert((u64)p!=address);dma_free_coherent(&gu.card.dev,5,p,address);
    assert(allocs==1 && maps==1 && unmaps==1 && frees==1);
    assert(!gu_live_dma_count());
    fail_map=1;assert(!dma_alloc_coherent(&gu.card.dev,4096,&address,0));fail_map=0;
    assert(unmaps==1 && frees==2);
    short_map=1;assert(!dma_alloc_coherent(&gu.card.dev,4096,&address,0));short_map=0;
    assert(unmaps==2 && frees==3);
    device_address=0xfffff000;
    assert(!dma_alloc_coherent(&gu.card.dev,8192,&address,0));
    assert(unmaps==3 && frees==4);
    assert(!dma_alloc_coherent(&gu.card.dev,0,&address,0));
    assert(!dma_alloc_coherent(&gu.card.dev,~(size_t)0,&address,0));
    assert(allocs==4);
    device_address=0x12345000;
    p=dma_alloc_coherent(&gu.card.dev,4096,&address,0);assert(p);
    fail_unmap=1;dma_free_coherent(&gu.card.dev,4096,p,address);
    assert(gu.retained && frees==4); /* Still mapped: do not free or boot an OS. */
    assert(gu_live_dma_count()==1);
    fail_unmap=0;dma_free_coherent(&gu.card.dev,4096,p,address);assert(frees==5);
    gu.retained=false;gu.bar_length=0x1000000;
    assert(!gu_live_dma_count());
    /* Rejected successful mappings must remain owned if Unmap fails. */
    for(int rejection=0;rejection<2;rejection++) {
        short_map=rejection==0;device_address=rejection?0xfffff000:0x12345000;
        int old_frees=frees,old_attempts=free_attempts,old_unmaps=unmaps;
        fail_unmap=1;
        assert(!dma_alloc_coherent(&gu.card.dev,8192,&address,0));
        assert(unmaps==old_unmaps+1 && frees==old_frees && free_attempts==old_attempts);
        assert(gu.retained && gu_live_dma_count()==1 && active_mappings==1);
        fail_unmap=0;short_map=0;
        dma_free_coherent(&gu.card.dev,8192,last_host,device_address);
        assert(!gu_live_dma_count() && !active_mappings && frees==old_frees+1);
        gu.retained=false;
    }
    /* FreeBuffer failure retains ownership without repeating a completed Unmap. */
    device_address=0x12345000;short_map=1;fail_free=1;
    int old_frees=frees,old_unmaps=unmaps;
    assert(!dma_alloc_coherent(&gu.card.dev,4096,&address,0));
    assert(gu.retained && gu_live_dma_count()==1 && !active_mappings);
    assert(frees==old_frees && unmaps==old_unmaps+1);
    fail_free=0;short_map=0;
    dma_free_coherent(&gu.card.dev,4096,last_host,device_address);
    assert(!gu_live_dma_count() && frees==old_frees+1 && unmaps==old_unmaps+1);
    gu.retained=false;
    /* A failed Map has no mapping to unmap, but its allocation is still owned. */
    fail_map=1;fail_free=1;old_frees=frees;old_unmaps=unmaps;
    assert(!dma_alloc_coherent(&gu.card.dev,4096,&address,0));
    assert(gu.retained && gu_live_dma_count()==1 && !active_mappings);
    assert(frees==old_frees && unmaps==old_unmaps);
    fail_map=0;fail_free=0;
    dma_free_coherent(&gu.card.dev,4096,last_host,0);
    assert(!gu_live_dma_count() && frees==old_frees+1 && unmaps==old_unmaps);
    gu.retained=false;
    test_pci_state_roundtrip();
    test_pcie_flr_behavior();
    gu_log("id=%08x count=%zu bytes=%#llx text=%s error=%d",0x220810de,
           (size_t)123,0x100000000ull,"ok",-22);
    assert(!gu_strcmp(gu.log,"id=220810de count=123 bytes=0x100000000 text=ok error=-22"));
    assert(!gu.log_truncated);
    test_complete_log_checkpoints();
    test_log_failure_paths();
    test_partial_append_preserves_committed_prefix();
    test_log_truncation_checkpoint();
    assert(readl((u8 *)GU_MMIO_COOKIE+0x1234)==0x172000a1 && mmio_reads==1);
    writel(7,(u8 *)GU_MMIO_COOKIE+0x1234);assert(mmio_writes==1);
    fail_mmio=1;readl((u8 *)GU_MMIO_COOKIE+0x1234);assert(gu.io_fault);
    writel(7,(u8 *)GU_MMIO_COOKIE+0x1234);assert(mmio_writes==1);
    gu.io_fault=false;
    assert(readl((u8 *)GU_MMIO_COOKIE+gu.bar_length)==~0u && mmio_reads==2);
    writel(7,(u8 *)GU_MMIO_COOKIE+gu.bar_length);assert(gu.io_fault && mmio_writes==1);
    puts("UEFI DMA Map/unmap, retained buffers, MMIO fail-closed, PCI state, FLR and log-checkpoint tests passed");
    return 0;
}
