// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vbios_610.h"

static void w32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = v >> (8 * i);
}

#include "vbios_fixture.h"

#define NVGI_FIXTURE_SIZE 0x10000
#define NVGI_PCI_BASE 0x9200

static void nvgi_fixture(u8 *buffer, int version, u32 pci_offset)
{
	memset(buffer, 0, NVGI_FIXTURE_SIZE);
	w32(buffer, 0x4947564e); /* NVGI */
	w32(buffer + 4, ((u32)version << 8) | (0x20u << 16));
	if (version == 1 || version == 2) {
		w32(buffer + 0x24, pci_offset);
	} else if (version == 3) {
		w32(buffer + 8, 0x19c0);
		w32(buffer + 0x19c0, 0x4000);
		w32(buffer + 0x5000, 0x44524652); /* RFRD */
		w32(buffer + 0x5008, pci_offset);
	}
}

static void nvgi_vbios_fixture(u8 *buffer, int version, u32 pci_offset)
{
	nvgi_fixture(buffer, version, pci_offset);
	vbios_fixture(buffer + pci_offset, 6);
}

static void expect_parse_error(const u8 *rom, size_t length, int error)
{
	struct gpu_unlock_fwsec fw;

	assert(gpu_unlock_vbios_fwsec(rom, length, &fw) == error);
}

static void expect_truncated(const u8 *rom, size_t length)
{
	struct gpu_unlock_fwsec fw;
	u8 *short_rom = malloc(length ? length : 1);

	assert(short_rom);
	if (length)
		memcpy(short_rom, rom, length);
	assert(gpu_unlock_vbios_fwsec(short_rom, length, &fw) == -EINVAL);
	free(short_rom);
}

int main(int argc, char **argv)
{
	u8 rom[4096], image[1280] = {0};
	u8 nvgi[NVGI_FIXTURE_SIZE];
	struct gpu_unlock_fwsec fw;
	u32 offset;
	/* Upstream GA107 log (regression fixture, not GA106 capture): 0xe300 IMEM + 0x980 DMEM in a padded 0xed00 image. */
	{
		u32 bytes = 0;
		struct gpu_unlock_fwsec geometry = {
			.image_size=0xed00, .data_off=0xe300, .dmem_size=0x980,
		};
		assert(!gpu_unlock_fwsec_dmem_transfer(&geometry,&bytes) && bytes==0xa00);
		geometry.image_size=0xec80;
		assert(gpu_unlock_fwsec_dmem_transfer(&geometry,&bytes)==-ERANGE);
		geometry.image_size=0xed00; geometry.data_off++;
		assert(gpu_unlock_fwsec_dmem_transfer(&geometry,&bytes)==-EINVAL);
		geometry.data_off=0xe300; geometry.dmem_size=0xffffffff;
		assert(gpu_unlock_fwsec_dmem_transfer(&geometry,&bytes)==-EINVAL);
	}

	for (int stride = 6; stride <= 8; stride += 2) {
		vbios_fixture(rom, stride);
		assert(!gpu_unlock_vbios_fwsec(rom, sizeof(rom), &fw));
		assert(fw.image_off == 684 && fw.image_size == 1280);
		assert(fw.sig_off == 300 && fw.data_off == 256 && fw.dmem_size == 1024);
	}
	/* Allocate the actual short length: a larger backing array hides overreads. */
	for (size_t len = 0; len < 4096; len++) {
		u8 *short_rom = malloc(len ? len : 1);
		assert(short_rom);
		memcpy(short_rom, rom, len);
		assert(gpu_unlock_vbios_fwsec(short_rom, len, &fw));
		free(short_rom);
	}
	/* The public input ceiling remains inclusive at 1 MiB. */
	{
		const size_t max_input = 1024 * 1024;
		u8 *large_rom = calloc(1, max_input + 1);

		assert(large_rom);
		vbios_fixture(large_rom, 6);
		assert(!gpu_unlock_vbios_fwsec(large_rom, max_input, &fw));
		assert(gpu_unlock_vbios_fwsec(large_rom, max_input + 1, &fw) == -EINVAL);
		free(large_rom);
	}
	/* IFR/NVGI v1 and v2 fixed-data pointers, and v3's flash-status/RFRD
	 * directory, all select the same synthetic PCI ROM. Results rebase to
	 * the original caller buffer while preserving descriptor geometry. */
	for (int version = 1; version <= 3; version++) {
		nvgi_vbios_fixture(nvgi, version, NVGI_PCI_BASE);
		assert(!gpu_unlock_vbios_fwsec(nvgi, sizeof(nvgi), &fw));
		assert(fw.image_off == NVGI_PCI_BASE + 684);
		assert(fw.sig_off == NVGI_PCI_BASE + 300);
		assert(fw.image_size == 1280 && fw.data_off == 256 && fw.dmem_size == 1024);
	}
	/* Truncation at each NVGI lookup layer and at the selected image end. */
	nvgi_vbios_fixture(nvgi, 1, NVGI_PCI_BASE);
	expect_truncated(nvgi, 0x27); /* v1/v2 extended-offset pointer */
	expect_truncated(nvgi, NVGI_PCI_BASE + 0x19); /* PCI ROM header */
	expect_truncated(nvgi, NVGI_PCI_BASE + 684 + 1280 - 1); /* firmware image */
	nvgi_vbios_fixture(nvgi, 3, NVGI_PCI_BASE);
	expect_truncated(nvgi, 0x19c3); /* flash-status pointer */
	expect_truncated(nvgi, 0x500b); /* RFRD pointer */
	/* Unsupported IFR versions differ from malformed signatures and bounds. */
	nvgi_fixture(nvgi, 4, NVGI_PCI_BASE);
	expect_parse_error(nvgi, sizeof(nvgi), -EOPNOTSUPP);
	nvgi_vbios_fixture(nvgi, 1, NVGI_PCI_BASE);
	w32(nvgi, 0x4956474e);
	expect_parse_error(nvgi, sizeof(nvgi), -EINVAL);
	nvgi_vbios_fixture(nvgi, 3, NVGI_PCI_BASE);
	w32(nvgi + 0x5000, 0);
	expect_parse_error(nvgi, sizeof(nvgi), -EINVAL); /* RFRD signature */
	nvgi_vbios_fixture(nvgi, 3, NVGI_PCI_BASE);
	w32(nvgi + 0x19c0, 0xfffff800);
	w32(nvgi + 0x800, 0x44524652);
	w32(nvgi + 0x808, NVGI_PCI_BASE);
	expect_parse_error(nvgi, sizeof(nvgi), -EINVAL); /* wrapped flash-status + 4096 */
	nvgi_vbios_fixture(nvgi, 3, NVGI_PCI_BASE);
	w32(nvgi + 0x5008, 0xfffffffc);
	expect_parse_error(nvgi, sizeof(nvgi), -EINVAL); /* wrapped/out-of-range PCI pointer */
	nvgi_vbios_fixture(nvgi, 1, 0x9202);
	expect_parse_error(nvgi, sizeof(nvgi), -EINVAL); /* PCI offset must be 4-byte aligned */
	nvgi_vbios_fixture(nvgi, 1, NVGI_PCI_BASE);
	w32(nvgi + NVGI_PCI_BASE + 32, 0);
	expect_parse_error(nvgi, sizeof(nvgi), -EINVAL); /* selected PCIR signature */
	nvgi_vbios_fixture(nvgi, 1, NVGI_PCI_BASE);
	w32(nvgi + 0x24, 0xfffffffc);
	expect_parse_error(nvgi, sizeof(nvgi), -EINVAL); /* aligned, wrapped PCI pointer */
	nvgi_vbios_fixture(nvgi, 1, NVGI_PCI_BASE);
	w32(nvgi + NVGI_PCI_BASE + 100, 0xfffffff0);
	expect_parse_error(nvgi, sizeof(nvgi), -EINVAL); /* wrapped BIT table pointer */
	nvgi_vbios_fixture(nvgi, 1, NVGI_PCI_BASE);
	w32(nvgi + NVGI_PCI_BASE + 142, 0xfffffff0);
	expect_parse_error(nvgi, sizeof(nvgi), -EINVAL); /* wrapped descriptor pointer */
	/* Valid-looking data beyond the selected chain is not part of it:
	 * reject an out-of-chain BIT table, descriptor, or image despite trailing
	 * bytes that would otherwise make each structure readable. */
	for (int trailing = 0; trailing < 3; trailing++) {
		nvgi_vbios_fixture(nvgi, 1, NVGI_PCI_BASE);
		nvgi[NVGI_PCI_BASE + 48] = trailing == 2 ? 1 : 4;
		if (trailing == 0) {
			w32(nvgi + NVGI_PCI_BASE + 100, 3000);
			nvgi[NVGI_PCI_BASE + 3000] = 2; /* would be an unsupported table */
		} else if (trailing == 1) {
			w32(nvgi + NVGI_PCI_BASE + 142, 2100);
			w32(nvgi + NVGI_PCI_BASE + 2100, (44u << 16) | (4u << 8) | 1);
		}
		expect_parse_error(nvgi, sizeof(nvgi), -EINVAL);
	}
	/* The BIT itself may be visible only as trailing bytes after the selected
	 * chain. Its checksum-valid header points to an otherwise valid table and
	 * descriptor there, so the chain limit must reject its out-of-chain token. */
	{
		const u32 bit = NVGI_PCI_BASE + 500;
		u32 sum = 0;

		nvgi_vbios_fixture(nvgi, 1, NVGI_PCI_BASE);
		nvgi[NVGI_PCI_BASE + 48] = 1;
		memset(nvgi + NVGI_PCI_BASE + 64, 0, 12);
		w32(nvgi + NVGI_PCI_BASE + 100, 1000);
		memcpy(nvgi + NVGI_PCI_BASE + 1000, rom + 128, 18);
		w32(nvgi + NVGI_PCI_BASE + 1014, 1200);
		memcpy(nvgi + NVGI_PCI_BASE + 1200, rom + 256, 428 + 1280);
		nvgi[bit] = 0xff; nvgi[bit + 1] = 0xb8;
		memcpy(nvgi + bit + 2, "BIT", 3);
		nvgi[bit + 8] = 12; nvgi[bit + 9] = 6; nvgi[bit + 10] = 1;
		for (u32 i = 0; i < 11; i++)
			sum += nvgi[bit + i];
		nvgi[bit + 11] = (u8)(0 - sum);
		nvgi[bit + 12] = 0x70; nvgi[bit + 13] = 2;
		nvgi[bit + 14] = 4; nvgi[bit + 16] = 100;
		expect_parse_error(nvgi, sizeof(nvgi), -EINVAL);
	}
	/* NPDE is aligned in the absolute caller-buffer address, not relative to a
	 * PCI base that is 4-byte aligned but not 16-byte aligned. */
	{
		const u32 pci_base = 0x9104;
		const u32 second_start = 4096;
		const u32 second_pcir = pci_base + second_start + 32;
		const u32 npde = (second_pcir + 24 + 15) & ~15u;
		u8 *second;
		u8 *extension;

		nvgi_vbios_fixture(nvgi, 1, pci_base);
		nvgi[pci_base + 53] = 0; /* first ROM continues */
		second = nvgi + pci_base + second_start;
		second[0] = 0x55; second[1] = 0xaa; second[0x18] = 32;
		memcpy(second + 32, "PCIR", 4);
		second[42] = 24; second[48] = 1;
		second[52] = 0xe0; second[53] = 0x80;
		extension = nvgi + npde;
		memcpy(extension, "NPDE", 4);
		extension[5] = 1; extension[6] = 10; extension[8] = 8;
		w32(nvgi + pci_base + 100, second_start + 128);
		memcpy(nvgi + pci_base + second_start + 128, rom + 128, 18);
		w32(nvgi + pci_base + second_start + 142, second_start + 256);
		memcpy(nvgi + pci_base + second_start + 256, rom + 256, 428 + 1280);
		assert(!gpu_unlock_vbios_fwsec(nvgi, sizeof(nvgi), &fw));
		assert(fw.image_off == pci_base + second_start + 684);
		assert(fw.sig_off == pci_base + second_start + 300);
	}
	/* Legacy + EFI with NPDE continuation + VN/NPDS extension. The BIT
	 * pointers address the extension as if it followed the legacy image. */
	for (int ext_len = 10; ext_len <= 11; ext_len++) {
		u8 chain[8192] = {0};
		vbios_fixture(rom, 6);
		memcpy(chain, rom, 4096);
		chain[53] = 0;
		chain[4096] = 0x55; chain[4097] = 0xaa; chain[4096+0x18] = 32;
		memcpy(chain+4096+32, "PCIR", 4);
		chain[4096+42] = 24; chain[4096+48] = 8;
		chain[4096+52] = 3; chain[4096+53] = 0x80;
		memcpy(chain+4096+64, "NPDE", 4);
		chain[4096+69] = 1; chain[4096+70] = ext_len;
		chain[4096+72] = 1; /* one 512-byte subimage */
		chain[4608] = 'V'; chain[4609] = 'N'; chain[4608+0x18] = 32;
		memcpy(chain+4608+32, "NPDS", 4);
		chain[4608+42] = 24; chain[4608+48] = 7;
		chain[4608+52] = 0xe0; chain[4608+53] = 0x80;
		w32(chain+100, 4096+128);
		memcpy(chain+4608+128, rom+128, 18);
		w32(chain+4608+142, 4096+256);
		memcpy(chain+4608+256, rom+256, 428+1280);
		assert(!gpu_unlock_vbios_fwsec(chain, sizeof(chain), &fw));
		assert(fw.image_off == 4608+684);
		assert(gpu_unlock_vbios_fwsec(chain, 4608, &fw) == -EINVAL);
		/* RGIS/BB77 is also supported, malformed data signatures are not. */
		chain[4608] = 0x77; chain[4609] = 0xbb;
		memcpy(chain+4608+32, "RGIS", 4);
		assert(!gpu_unlock_vbios_fwsec(chain, sizeof(chain), &fw));
		chain[4096+70] = 9;
		assert(gpu_unlock_vbios_fwsec(chain, sizeof(chain), &fw) == -EINVAL);
		chain[4096+70] = ext_len; chain[4096+72] = 0;
		assert(gpu_unlock_vbios_fwsec(chain, sizeof(chain), &fw) == -EINVAL);
	}
	vbios_fixture(rom, 8);
	w32(rom + 264, 0xffffffff);
	assert(gpu_unlock_vbios_fwsec(rom, sizeof(rom), &fw) == -EINVAL);
	vbios_fixture(rom, 8);
	w32(rom + 288, 0xffffffff);
	assert(gpu_unlock_vbios_fwsec(rom, sizeof(rom), &fw) == -EINVAL);
	vbios_fixture(rom, 8);
	assert(!gpu_unlock_vbios_fwsec(rom, sizeof(rom), &fw));
	image[256] = 1; image[257] = 4; image[258] = 8; image[259] = 2;
	w32(image + 260, 4); w32(image + 264, 32);
	image[292] = 3; image[294] = 64;
	w32(image + 296, 128); w32(image + 300, 44);
	assert(!gpu_unlock_fwsec_patch_frts(image, &fw, 0x100000));
	assert(image[332] == 0x15 && image[384] == 1);
	fw.interface_off = fw.dmem_size - 1;
	assert(gpu_unlock_fwsec_patch_frts(image, &fw, 0x100000) == -EINVAL);
	fw.interface_off = 0;
	w32(image + 264, 0xfffffff0);
	assert(gpu_unlock_fwsec_patch_frts(image, &fw, 0x100000) == -EINVAL);
	assert(!gpu_unlock_fwsec_sig_offset(7, 0, 384, &offset) && offset == 0);
	assert(!gpu_unlock_fwsec_sig_offset(5, 2, 384, &offset) && offset == 384);
	assert(gpu_unlock_fwsec_sig_offset(1, 1, 384, &offset) == -ERANGE);
	/* Optional integration check; normal tests contain no board-owned blobs. */
	if (argc == 2) {
		FILE *prom = fopen(argv[1], "rb");
		u8 *image;
		size_t got;

		assert(prom);
		assert(!fseek(prom, 0, SEEK_END));
		got = ftell(prom);
		assert(got == 1024 * 1024);
		rewind(prom);
		image = malloc(got);
		assert(image && fread(image, 1, got, prom) == got);
		fclose(prom);
		assert(!gpu_unlock_vbios_fwsec(image, got, &fw));
		assert(fw.ucode_id == 9 && fw.sig_count == 3 && fw.sig_versions == 7);
		assert(fw.engine_mask == 0x400 && fw.imem_size == 0xda00 && fw.dmem_size == 0x800);
		assert(fw.image_off == 0x4289c && fw.image_size == 0xe200);
		assert(fw.pkc_off == 0x5a4 && fw.interface_off == 0x1c);
		/* Revision 05 recorded Booter fuse version 1. That selects signature slot 1. */
		assert(!gpu_unlock_fwsec_sig_offset(fw.sig_versions, 1, 384, &offset) && offset == 384);
		assert(!gpu_unlock_fwsec_patch_frts(image + fw.image_off, &fw, 0x5ffe00000ull));
		{
			const u8 *cmd = image + fw.image_off + fw.data_off + 0x7c0;
			u32 frts4k = (u32)cmd[32] | ((u32)cmd[33] << 8) | ((u32)cmd[34] << 16) | ((u32)cmd[35] << 24);

			assert(frts4k == (0x5ffe00000ull >> 12));
			assert(cmd[36] == 0 && cmd[37] == 1 && cmd[40] == 2);
		}
		free(image);
	}
	/* Optional read-only check for the approved 2 MiB ROM. Only the first
	 * 1 MiB is passed to the parser, matching unlock_transaction_fwsec's PROM
	 * acquisition rather than parsing a padded or full-file buffer. */
	if (argc == 3 && !strcmp(argv[1], "--rtx3080ti-rom")) {
		FILE *prom = fopen(argv[2], "rb");
		u8 *prefix;
		size_t got;
		const size_t prefix_size = 1024 * 1024;
		u32 dmem_bytes = 0;

		assert(prom);
		assert(!fseek(prom, 0, SEEK_END));
		got = ftell(prom);
		assert(got == (size_t)2048000);
		rewind(prom);
		prefix = malloc(prefix_size);
		assert(prefix && fread(prefix, 1, prefix_size, prom) == prefix_size);
		fclose(prom);
		assert(!gpu_unlock_vbios_fwsec(prefix, prefix_size, &fw));
		assert(fw.image_off == 0x9200 + 0x45264);
		assert(fw.sig_off == 0x9200 + 0x44de4);
		assert(fw.image_size == 0xea00 && fw.imem_size == 0xe200);
		assert(fw.data_off == 0xe200 && fw.dmem_size == 0x800);
		assert(fw.pkc_off == 0x5a4 && fw.interface_off == 0x1c);
		assert(fw.engine_mask == 0x400 && fw.ucode_id == 9);
		assert(fw.sig_count == 3 && fw.sig_versions == 7 && fw.sig_bytes == 0x480);
		assert((u64)fw.image_off + fw.image_size <= (u64)prefix_size);
		assert(!gpu_unlock_fwsec_dmem_transfer(&fw, &dmem_bytes));
		assert(dmem_bytes == 0x800);
		free(prefix);
	}
	puts("vbios valid-chain, malformed range, patch and signature checks passed");
	return 0;
}
