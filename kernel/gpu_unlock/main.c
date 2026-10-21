// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>
#include "target.h"
#include "unlock_610.h"

static char *target;
static bool execute;
static bool ss1_full;
/* Sentinel means no board was selected. Never infer consent from enumeration. */
static unsigned int board_vendor = 0x10000;
static unsigned int board_device = 0x10000;
module_param(board_vendor, uint, 0444);
module_param(board_device, uint, 0444);
MODULE_PARM_DESC(board_vendor, "Expected PCI subsystem vendor, required for execute=1");
MODULE_PARM_DESC(board_device, "Expected PCI subsystem device, required for execute=1");
module_param(target, charp, 0444);
MODULE_PARM_DESC(target, "Exact full PCI address; no all/list support");
module_param(execute, bool, 0444);
module_param(ss1_full, bool, 0444);
MODULE_PARM_DESC(ss1_full, "Opt in to unverified SS1=0x8 experiment; TF32 effect unknown (default false)");
MODULE_PARM_DESC(execute, "Run unverified RTX 3080 Ti V67/SS0 experiment (default false)");

static int __init rtx3080ti_unlock_init(void)
{
    struct pci_dev *pdev = NULL;
    struct gpu_unlock_610_shared shared = {0};
    int ret;
    if (!target || !*target) {
        pr_info("rtx3080ti_unlock: disabled; no target specified\n");
        return 0;
    }
    while ((pdev = pci_get_device(PCI_VENDOR_ID_NVIDIA, RTX3080TI_PCI_ID, pdev))) {
        if (strcmp(pci_name(pdev), target))
            continue;
        if (!rtx3080ti_board_matches(pdev->vendor, pdev->device,
                                     pdev->subsystem_vendor, pdev->subsystem_device)) {
            pci_err(pdev, "rtx3080ti_unlock: exact 10de:2208 / 10de:1535 identity required\n");
            pci_dev_put(pdev);
            return -ENODEV;
        }
        if (!execute) {
            pci_info(pdev, "rtx3080ti_unlock: matched; execute=0, no firmware request or MMIO\n");
            pci_dev_put(pdev);
            return 0;
        }
        if (board_vendor > 0xffff || board_device > 0xffff ||
            !board_vendor || !board_device) {
            pci_err(pdev, "rtx3080ti_unlock: execute requires board_vendor and board_device from preflight\n");
            pci_dev_put(pdev);
            return -EINVAL;
        }
        if (pdev->subsystem_vendor != board_vendor ||
            pdev->subsystem_device != board_device) {
            pci_err(pdev, "rtx3080ti_unlock: subsystem does not match explicit board parameters\n");
            pci_dev_put(pdev);
            return -ENODEV;
        }
        pci_dev_lock(pdev);
        if (pdev->driver) {
            pci_err(pdev, "rtx3080ti_unlock: bound card refused; cold pre-driver boot required\n");
            pci_dev_unlock(pdev);
            pci_dev_put(pdev);
            return -EBUSY;
        }
        pci_warn(pdev, "rtx3080ti_unlock: EXPERIMENTAL V67 + SS0; SS1 policy=%s; TF32 effect unverified\n",
                 ss1_full ? "full=0x8 (opt-in)" : "preserve");
        ret = gpu_unlock_610_load_shared(pdev, &shared);
        if (!ret)
            ret = gpu_unlock_610_unlock(pdev, &shared, ss1_full ?
                    GPU_UNLOCK_SS1_FULL_EXPERIMENT : GPU_UNLOCK_SS1_PRESERVE);
        gpu_unlock_610_release_shared(&shared);
        pci_dev_unlock(pdev);
        if (ret)
            pci_err(pdev, "rtx3080ti_unlock: experiment failed (%d); power off before retry\n", ret);
        else
            pci_info(pdev, "rtx3080ti_unlock: SS0 applied and FLR completed; benchmark required\n");
        pci_dev_put(pdev);
        /* After hardware work init must succeed: a failed transaction may
         * retain DMA buffers and pin THIS_MODULE until full power removal. */
        return 0;
    }
    pr_err("rtx3080ti_unlock: no eligible RTX 3080 Ti at %s\n", target);
    return -ENODEV;
}
static void __exit rtx3080ti_unlock_exit(void) {}
module_init(rtx3080ti_unlock_init);
module_exit(rtx3080ti_unlock_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental RTX 3080 Ti 12GB GA102 pre-driver V67/SS0 rate override");
MODULE_VERSION("0.4.0-rtx3080ti");
