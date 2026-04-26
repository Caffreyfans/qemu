#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "system/memory.h"
#include "qemu/units.h"

#define TYPE_FAKE_GPU "fakegpu"
#define FAKE_GPU(obj) OBJECT_CHECK(FakeGPUState, (obj), TYPE_FAKE_GPU)

#define FB_WIDTH 1024
#define FB_HEIGHT 768
#define FB_BPP 4
#define FB_SIZE (FB_WIDTH * FB_HEIGHT * FB_BPP)  /* 1024x768x32bpp */
#define FB_BAR_SIZE (4 * MiB)

typedef struct FakeGPUState {
    PCIDevice parent_obj;

    MemoryRegion fb_mmio;
    uint8_t *fb;
} FakeGPUState;

/* MMIO read */
static uint64_t fakegpu_read(void *opaque, hwaddr addr, unsigned size)
{
    FakeGPUState *s = opaque;

    if (addr + size > FB_SIZE) {
        return 0;
    }

    uint64_t val = 0;
    memcpy(&val, s->fb + addr, size);
    return val;
}

/* MMIO write */
static void fakegpu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    FakeGPUState *s = opaque;

    if (addr + size > FB_SIZE) {
        return;
    }

    memcpy(s->fb + addr, &val, size);
}

static const MemoryRegionOps fakegpu_ops = {
    .read = fakegpu_read,
    .write = fakegpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* 初始化 PCI 设备 */
static void fakegpu_realize(PCIDevice *pdev, Error **errp)
{
    FakeGPUState *s = FAKE_GPU(pdev);

    /* 分配 framebuffer */
    s->fb = g_malloc0(FB_SIZE);

    /* 初始化 MMIO */
    memory_region_init_io(&s->fb_mmio, OBJECT(s), &fakegpu_ops, s,
                          "fakegpu-fb", FB_BAR_SIZE);

    /* 注册 BAR0 */
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->fb_mmio);

}

static void fakegpu_uninit(PCIDevice *pdev)
{
    FakeGPUState *s = FAKE_GPU(pdev);
    g_free(s->fb);
}

/* class init */
static void fakegpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = fakegpu_realize;
    k->exit = fakegpu_uninit;

    k->vendor_id = 0xAAAA;
    k->device_id = 0xBBBB;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_DISPLAY_XGA;

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo fakegpu_info = {
    .name          = TYPE_FAKE_GPU,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(FakeGPUState),
    .class_init    = fakegpu_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void fakegpu_register_types(void)
{
    type_register_static(&fakegpu_info);
}

type_init(fakegpu_register_types)