#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "system/memory.h"
#include "qemu/units.h"
#include "ui/console.h"

#define TYPE_FAKE_GPU "fakegpu"
#define FAKE_GPU(obj) OBJECT_CHECK(FakeGPUState, (obj), TYPE_FAKE_GPU)

#define FB_WIDTH 800
#define FB_HEIGHT 800
#define FB_BPP 4

#define FB_SIZE (FB_WIDTH * FB_HEIGHT * FB_BPP)
#define FB_BAR_SIZE (4 * MiB)

typedef struct FakeGPUState
{
    PCIDevice parent_obj;
    MemoryRegion vram;
    QemuConsole *con;
} FakeGPUState;

static void fakegpu_update_display(void *opaque)
{
    FakeGPUState *s = opaque;
    DisplaySurface *surface;
    uint8_t *src;
    uint8_t *dst;
    int stride;
    int y;

    if (s->con == NULL)
    {
        return;
    }

    surface = qemu_console_surface(s->con);
    if (surface == NULL)
    {
        return;
    }

    src = memory_region_get_ram_ptr(&s->vram);
    if (src == NULL)
    {
        return;
    }

    dst = surface_data(surface);
    if (dst == NULL)
    {
        return;
    }

    stride = surface_stride(surface);

    for (y = 0; y < FB_HEIGHT; y++)
    {
        memcpy(dst + y * stride,
               src + y * FB_WIDTH * FB_BPP,
               FB_WIDTH * FB_BPP);
    }

    dpy_gfx_update(s->con, 0, 0, FB_WIDTH, FB_HEIGHT);
}

static const GraphicHwOps fakegpu_graphic_ops = {
    .gfx_update = fakegpu_update_display,
};

static void fakegpu_realize(PCIDevice *pdev, Error **errp)
{
    FakeGPUState *s = FAKE_GPU(pdev);

    memory_region_init_ram(
        &s->vram,
        OBJECT(s),
        "fakegpu-vram",
        FB_BAR_SIZE,
        errp);
    if (*errp)
    {
        return;
    }

    pci_register_bar(
        pdev,
        0,
        PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_PREFETCH,
        &s->vram);

    s->con = graphic_console_init(DEVICE(s), 0, &fakegpu_graphic_ops, s);
    qemu_console_resize(s->con, FB_WIDTH, FB_HEIGHT);
}

static void fakegpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = fakegpu_realize;

    k->vendor_id = 0xAAAA;
    k->device_id = 0xBBBB;
    k->revision = 0x00;
    k->class_id = 0x0380;

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo fakegpu_info = {
    .name = TYPE_FAKE_GPU,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(FakeGPUState),
    .class_init = fakegpu_class_init,
    .interfaces = (const InterfaceInfo[]){
        {INTERFACE_CONVENTIONAL_PCI_DEVICE},
        {},
    },
};

static void fakegpu_register_types(void)
{
    type_register_static(&fakegpu_info);
}

type_init(fakegpu_register_types)