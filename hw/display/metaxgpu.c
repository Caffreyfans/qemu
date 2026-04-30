#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "system/memory.h"
#include "qemu/units.h"
#include "ui/console.h"

#define TYPE_MX_GPU "metaxgpu"
#define MX_GPU(obj) OBJECT_CHECK(MXGPUState, (obj), TYPE_MX_GPU)

#define MMIO_SIZE (0x1000)
#define FB_BAR_SIZE (32 * MiB)
#define FB_BPP (4)

/**
 * MMIO register offsets
 */
#define REG_WIDTH (0x00)
#define REG_HEIGHT (0x04)
#define REG_FB_SIZE (0x08)
#define REG_FLUSH (0x0C)

#define REG_MODE (0x10)
#define REG_MAX_MODE (0x14)
#define REG_STRIDE (0x18)

typedef struct MXGPUMode
{
    uint32_t width;
    uint32_t height;
} MXGPUMode;

typedef struct MXGPUState
{
    PCIDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion vram;
    QemuConsole *con;

    uint32_t current_mode;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} MXGPUState;

static const MXGPUMode mxgpu_modes[] = {
    {800, 600},
    {1024, 768},
    {1280, 720},
    {1920, 1080},
    {2560, 1440},
    {3840, 2160},
};

#define MXGPU_MAX_MODE ARRAY_SIZE(mxgpu_modes)

static void mxgpu_update_display(void *opaque)
{
    MXGPUState *s = (MXGPUState *)opaque;
    DisplaySurface *surface;
    uint8_t *src;
    uint8_t *dst;
    int dst_stride;
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

    dst_stride = surface_stride(surface);

    for (y = 0; y < s->height; y++)
    {
        memcpy(dst + y * dst_stride,
               src + y * s->stride,
               s->width * FB_BPP);
    }

    dpy_gfx_update(s->con, 0, 0, s->width, s->height);
}

static const GraphicHwOps mxgpu_graphic_ops = {
    .gfx_update = mxgpu_update_display,
};

static void mxgpu_set_mode(MXGPUState *s, uint32_t mode)
{
    if (mode >= MXGPU_MAX_MODE)
    {
        return;
    }

    s->current_mode = mode;
    s->width = mxgpu_modes[mode].width;
    s->height = mxgpu_modes[mode].height;
    s->stride = s->width * FB_BPP;

    if (s->con != NULL)
    {
        qemu_console_resize(s->con, s->width, s->height);
        graphic_hw_update(s->con);
    }
}

static uint64_t mxgpu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    MXGPUState *s = (MXGPUState *)opaque;
    uint64_t data = 0;
    switch (addr)
    {
    case REG_WIDTH:
    {
        data = s->width;
        break;
    }
    case REG_HEIGHT:
    {
        data = s->height;
        break;
    }
    case REG_FB_SIZE:
    {
        data = s->width * s->height * FB_BPP;
        break;
    }
    case REG_MODE:
    {
        data = s->current_mode;
        break;
    }
    case REG_MAX_MODE:
    {
        data = MXGPU_MAX_MODE;
        break;
    }
    case REG_STRIDE:
    {
        data = s->stride;
        break;
    }
    default:
        data = 0;
        break;
    }
    return data;
}

static void mxgpu_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MXGPUState *s = (MXGPUState *)opaque;
    switch (addr)
    {
    case REG_MODE:
    {
        mxgpu_set_mode(s, (uint32_t)val);
        break;
    }
    case REG_FLUSH:
    {
        if (s->con != NULL)
        {
            graphic_hw_update(s->con);
        }
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps mxgpu_mmio_ops = {
    .read = mxgpu_mmio_read,
    .write = mxgpu_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    }};

static void mxgpu_realize(PCIDevice *pdev, Error **errp)
{
    MXGPUState *s = MX_GPU(pdev);

    /**
     * Default mode: 0
     */
    mxgpu_set_mode(s, 0);

    memory_region_init_io(
        &s->mmio,
        OBJECT(s),
        &mxgpu_mmio_ops,
        s,
        "mxgpu-mmio",
        MMIO_SIZE
    );
    pci_register_bar(
        pdev,
        0,
        PCI_BASE_ADDRESS_SPACE_MEMORY,
        &s->mmio
    );

    memory_region_init_ram(
        &s->vram,
        OBJECT(s),
        "mxgpu-vram",
        FB_BAR_SIZE,
        errp);
    if (*errp)
    {
        return;
    }

    pci_register_bar(
        pdev,
        1,
        PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_PREFETCH,
        &s->vram);

    s->con = graphic_console_init(DEVICE(s), 0, &mxgpu_graphic_ops, s);
    qemu_console_resize(s->con, s->width, s->height);
}

static void mxgpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = mxgpu_realize;

    k->vendor_id = 0x9999;
    k->device_id = 0x0001;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_DISPLAY_VGA;

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo mxgpu_info = {
    .name = TYPE_MX_GPU,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(MXGPUState),
    .class_init = mxgpu_class_init,
    .interfaces = (const InterfaceInfo[]){
        {INTERFACE_CONVENTIONAL_PCI_DEVICE},
        {},
    },
};

static void mxgpu_register_types(void)
{
    type_register_static(&mxgpu_info);
}

type_init(mxgpu_register_types)