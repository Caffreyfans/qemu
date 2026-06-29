#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/crc32c.h"
#include "system/memory.h"
#include "qemu/units.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "hw/display/dpcd.h"
#include "hw/display/edid.h"

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

#define REG_DP_HPD (0x20)
#define REG_DP_LINK_RATE (0x24)
#define REG_DP_LANE_COUNT (0x28)
#define REG_DP_TRAINING_STATUS (0x2C)
#define REG_DP_AUX_ADDR (0x30)
#define REG_DP_AUX_DATA (0x34)
#define REG_DP_AUX_STATUS (0x38)
#define REG_DP_EDID_SIZE (0x3C)

#define REG_PIPE_CONTROL (0x40)
#define REG_PIXEL_CLOCK_KHZ (0x44)
#define REG_H_TOTAL (0x48)
#define REG_H_SYNC (0x4C)
#define REG_V_TOTAL (0x50)
#define REG_V_SYNC (0x54)

#define REG_FLUSH_X (0x60)
#define REG_FLUSH_Y (0x64)
#define REG_FLUSH_WIDTH (0x68)
#define REG_FLUSH_HEIGHT (0x6C)

#define REG_DP_MAIN_LINK_STATUS (0x70)
#define REG_DP_MAIN_LINK_BYTES_LO (0x74)
#define REG_DP_MAIN_LINK_BYTES_HI (0x78)
#define REG_DP_MSA_REQUIRED_KBPS (0x7C)
#define REG_DP_MSA_AVAILABLE_KBPS (0x80)
#define REG_DP_TRAINING_STATE (0x84)
#define REG_DP_FRAME_COUNT (0x88)
#define REG_DP_FRAME_CRC (0x8C)
#define REG_DP_TEST_PATTERN (0x90)

#define PIPE_CONTROL_ENABLE (1u << 0)
#define PIPE_CONTROL_BLANK (1u << 1)

#define DP_HPD_CONNECTED (1u << 0)
#define DP_TRAINING_DONE (1u << 0)
#define DP_AUX_STATUS_ACK (1u << 0)
#define DP_AUX_STATUS_NACK (1u << 1)
#define DP_MAIN_LINK_ACTIVE (1u << 0)
#define DP_MAIN_LINK_TRAINED (1u << 1)
#define DP_MAIN_LINK_BANDWIDTH_OK (1u << 2)

#define DP_DPCD_SIZE (0x600)
#define DP_EDID_SIZE (256)
#define DP_AUX_EDID_BASE (0x5000)
#define DP_AUX_EDID_END (DP_AUX_EDID_BASE + DP_EDID_SIZE)
#define DP_DPCD_LANE_COUNT_MASK (0x1f)
#define DP_DPCD_TRAINING_PATTERN_SET (0x102)

typedef enum MXGPUDpTrainingState {
    MXGPU_DP_TRAINING_IDLE = 0,
    MXGPU_DP_TRAINING_CLOCK_RECOVERY = 1,
    MXGPU_DP_TRAINING_CHANNEL_EQUALIZATION = 2,
    MXGPU_DP_TRAINING_TRAINED = 3,
    MXGPU_DP_TRAINING_FAILED = 4,
} MXGPUDpTrainingState;

typedef enum MXGPUTestPattern {
    MXGPU_TEST_PATTERN_FRAMEBUFFER = 0,
    MXGPU_TEST_PATTERN_COLOR_BARS = 1,
    MXGPU_TEST_PATTERN_CHECKERBOARD = 2,
    MXGPU_TEST_PATTERN_GRADIENT = 3,
} MXGPUTestPattern;

typedef struct MXGPUMode {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_clock_khz;
    uint32_t h_total;
    uint32_t h_sync_start;
    uint32_t h_sync_end;
    uint32_t v_total;
    uint32_t v_sync_start;
    uint32_t v_sync_end;
} MXGPUMode;

typedef struct MXGPUState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion vram;
    QemuConsole *con;
    uint8_t *scanout_buffer;

    uint32_t current_mode;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pipe_control;

    uint32_t pixel_clock_khz;
    uint32_t h_total;
    uint32_t h_sync;
    uint32_t v_total;
    uint32_t v_sync;

    uint32_t flush_x;
    uint32_t flush_y;
    uint32_t flush_width;
    uint32_t flush_height;

    uint32_t dp_hpd;
    uint32_t dp_link_rate;
    uint32_t dp_lane_count;
    uint32_t dp_training_status;
    uint32_t dp_aux_addr;
    uint32_t dp_aux_status;
    uint32_t dp_main_link_status;
    uint32_t dp_training_state;
    uint32_t dp_frame_count;
    uint32_t dp_frame_crc;
    uint32_t dp_test_pattern;
    uint64_t dp_main_link_bytes;

    qemu_edid_info edid_info;
    uint8_t edid[DP_EDID_SIZE];
    uint8_t dpcd[DP_DPCD_SIZE];
} MXGPUState;

static const MXGPUMode mxgpu_modes[] = {
    {800, 600, 40000, 1056, 840, 968, 628, 601, 605},
    {1024, 768, 65000, 1344, 1048, 1184, 806, 771, 777},
    {1280, 800, 83500, 1680, 1352, 1480, 831, 803, 809},
    {1920, 1080, 148500, 2200, 2008, 2052, 1125, 1084, 1089},
    {2560, 1440, 241500, 2720, 2608, 2640, 1481, 1443, 1448},
    {3840, 2160, 533250, 4000, 3888, 3920, 2222, 2163, 2168},
};

#define MXGPU_MAX_MODE ARRAY_SIZE(mxgpu_modes)

static void mxgpu_init_dpcd(MXGPUState *s)
{
    memset(s->dpcd, 0, sizeof(s->dpcd));

    s->dpcd[DPCD_REVISION] = DPCD_REV_1_0;
    s->dpcd[DPCD_MAX_LINK_RATE] = DPCD_5_4GBPS;
    s->dpcd[DPCD_MAX_LANE_COUNT] = DPCD_FOUR_LANES;
    s->dpcd[DPCD_RECEIVE_PORT0_CAP_0] = DPCD_EDID_PRESENT;
    s->dpcd[DPCD_RECEIVE_PORT0_CAP_1] = 0xFF;
    s->dpcd[DPCD_LANE0_1_STATUS] = DPCD_LANE0_CR_DONE
                                  | DPCD_LANE0_CHANNEL_EQ_DONE
                                  | DPCD_LANE0_SYMBOL_LOCKED
                                  | DPCD_LANE1_CR_DONE
                                  | DPCD_LANE1_CHANNEL_EQ_DONE
                                  | DPCD_LANE1_SYMBOL_LOCKED;
    s->dpcd[DPCD_LANE2_3_STATUS] = DPCD_LANE2_CR_DONE
                                  | DPCD_LANE2_CHANNEL_EQ_DONE
                                  | DPCD_LANE2_SYMBOL_LOCKED
                                  | DPCD_LANE3_CR_DONE
                                  | DPCD_LANE3_CHANNEL_EQ_DONE
                                  | DPCD_LANE3_SYMBOL_LOCKED;
    s->dpcd[DPCD_LANE_ALIGN_STATUS_UPDATED] = DPCD_INTERLANE_ALIGN_DONE;
    s->dpcd[DPCD_SINK_STATUS] = DPCD_RECEIVE_PORT_0_STATUS;
}

static uint8_t mxgpu_aux_read(MXGPUState *s, uint32_t addr)
{
    if (addr < sizeof(s->dpcd)) {
        s->dp_aux_status = DP_AUX_STATUS_ACK;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mxgpu: DP AUX read DPCD[0x%05x] -> 0x%02x\n",
                      addr, s->dpcd[addr]);
        return s->dpcd[addr];
    }

    if (addr >= DP_AUX_EDID_BASE && addr < DP_AUX_EDID_END) {
        s->dp_aux_status = DP_AUX_STATUS_ACK;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mxgpu: DP AUX read EDID[0x%03x] -> 0x%02x\n",
                      addr - DP_AUX_EDID_BASE,
                      s->edid[addr - DP_AUX_EDID_BASE]);
        return s->edid[addr - DP_AUX_EDID_BASE];
    }

    s->dp_aux_status = DP_AUX_STATUS_NACK;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "mxgpu: DP AUX read 0x%05x -> NACK\n", addr);
    return 0;
}

static void mxgpu_aux_write(MXGPUState *s, uint32_t addr, uint8_t val)
{
    if (addr < sizeof(s->dpcd)) {
        s->dpcd[addr] = val;
        s->dp_aux_status = DP_AUX_STATUS_ACK;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mxgpu: DP AUX write DPCD[0x%05x] <- 0x%02x\n",
                      addr, val);

        if (addr == DP_DPCD_TRAINING_PATTERN_SET) {
            if (val == 1) {
                s->dp_training_state = MXGPU_DP_TRAINING_CLOCK_RECOVERY;
            } else if (val == 2) {
                s->dp_training_state =
                    MXGPU_DP_TRAINING_CHANNEL_EQUALIZATION;
            } else if (s->dp_training_status & DP_TRAINING_DONE) {
                s->dp_training_state = MXGPU_DP_TRAINING_TRAINED;
            } else {
                s->dp_training_state = MXGPU_DP_TRAINING_IDLE;
            }
        }
        return;
    }

    s->dp_aux_status = DP_AUX_STATUS_NACK;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "mxgpu: DP AUX write 0x%05x <- 0x%02x NACK\n",
                  addr, val);
}

static uint64_t mxgpu_dp_link_capacity_kbps(MXGPUState *s)
{
    uint64_t lane_count = s->dp_lane_count & DP_DPCD_LANE_COUNT_MASK;

    if (lane_count == 0 || s->dp_link_rate == 0) {
        return 0;
    }

    /*
     * DPCD link-rate units are 270 Mbps. DP 1.x main-link payload carries
     * 8 useful bits per 10 encoded bits, so keep only the payload bandwidth.
     */
    return (uint64_t)s->dp_link_rate * 270000 * lane_count * 8 / 10;
}

static uint64_t mxgpu_dp_mode_required_kbps(MXGPUState *s)
{
    return (uint64_t)s->pixel_clock_khz * FB_BPP * 8;
}

static void mxgpu_dp_dump_msa(MXGPUState *s)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "mxgpu: DP MSA mode=%u %ux%u clock=%u kHz "
                  "htotal=%u hsync=%u-%u vtotal=%u vsync=%u-%u "
                  "required=%" PRIu64 " kbps available=%" PRIu64 " kbps\n",
                  s->current_mode, s->width, s->height, s->pixel_clock_khz,
                  s->h_total, s->h_sync & 0xffff, s->h_sync >> 16,
                  s->v_total, s->v_sync & 0xffff, s->v_sync >> 16,
                  mxgpu_dp_mode_required_kbps(s),
                  mxgpu_dp_link_capacity_kbps(s));
}

static bool mxgpu_dp_main_link_ready(MXGPUState *s)
{
    s->dp_main_link_status = 0;

    if ((s->dp_hpd & DP_HPD_CONNECTED) == 0) {
        return false;
    }

    if ((s->dp_training_status & DP_TRAINING_DONE) == 0) {
        if (s->dp_training_state == MXGPU_DP_TRAINING_TRAINED) {
            s->dp_training_state = MXGPU_DP_TRAINING_IDLE;
        }
        return false;
    }
    s->dp_training_state = MXGPU_DP_TRAINING_TRAINED;
    s->dp_main_link_status |= DP_MAIN_LINK_TRAINED;

    if (mxgpu_dp_link_capacity_kbps(s) < mxgpu_dp_mode_required_kbps(s)) {
        s->dp_training_state = MXGPU_DP_TRAINING_FAILED;
        return false;
    }
    s->dp_main_link_status |= DP_MAIN_LINK_BANDWIDTH_OK;

    if ((s->pipe_control & PIPE_CONTROL_ENABLE) == 0 ||
        (s->pipe_control & PIPE_CONTROL_BLANK) != 0) {
        return false;
    }

    s->dp_main_link_status |= DP_MAIN_LINK_ACTIVE;
    return true;
}

static void mxgpu_dp_main_link_transfer(MXGPUState *s,
                                        uint8_t *dst, int dst_stride,
                                        const uint8_t *src,
                                        uint32_t x, uint32_t y0,
                                        uint32_t width, uint32_t height)
{
    uint32_t y;
    uint32_t crc;

    crc = 0xffffffffu;

    for (y = y0; y < y0 + height; y++) {
        uint8_t *dst_row = dst + y * dst_stride + x * FB_BPP;

        if (s->dp_test_pattern == MXGPU_TEST_PATTERN_FRAMEBUFFER) {
            const uint8_t *src_row = src + y * s->stride + x * FB_BPP;

            memcpy(dst_row, src_row, width * FB_BPP);
        } else {
            uint32_t px;

            for (px = 0; px < width; px++) {
                uint32_t sx = x + px;
                uint32_t pixel;

                switch (s->dp_test_pattern) {
                case MXGPU_TEST_PATTERN_COLOR_BARS:
                    pixel = ((sx * 8 / MAX(s->width, 1u)) & 1) ? 0x00ffffff :
                            ((sx * 8 / MAX(s->width, 1u)) & 2) ? 0x0000ff00 :
                            ((sx * 8 / MAX(s->width, 1u)) & 4) ? 0x00ff0000 :
                            0x000000ff;
                    break;
                case MXGPU_TEST_PATTERN_CHECKERBOARD:
                    pixel = (((sx / 32) ^ (y / 32)) & 1) ? 0x00f0f0f0 :
                            0x00202020;
                    break;
                case MXGPU_TEST_PATTERN_GRADIENT:
                    pixel = ((sx * 255 / MAX(s->width - 1, 1u)) << 16) |
                            ((y * 255 / MAX(s->height - 1, 1u)) << 8) |
                            0x40;
                    break;
                default:
                    pixel = 0;
                    break;
                }

                stl_le_p(dst_row + px * FB_BPP, pixel);
            }
        }

        crc = crc32c(crc, dst_row, width * FB_BPP);
    }

    s->dp_frame_count++;
    s->dp_frame_crc = crc ^ 0xffffffffu;
    s->dp_main_link_bytes += (uint64_t)width * height * FB_BPP;
}

static void mxgpu_update_display(void *opaque)
{
    MXGPUState *s = (MXGPUState *)opaque;
    DisplaySurface *surface;
    uint8_t *src;
    uint8_t *dst;
    int dst_stride;
    uint32_t x;
    uint32_t y0;
    uint32_t width;
    uint32_t height;

    if (s->con == NULL || !mxgpu_dp_main_link_ready(s)) {
        return;
    }

    surface = qemu_console_surface(s->con);
    if (surface == NULL) {
        return;
    }

    src = memory_region_get_ram_ptr(&s->vram);
    if (src == NULL) {
        return;
    }

    dst = surface_data(surface);
    if (dst == NULL) {
        return;
    }

    if (surface_bits_per_pixel(surface) != 32) {
        return;
    }

    x = s->flush_x;
    y0 = s->flush_y;
    width = s->flush_width;
    height = s->flush_height;
    if (width == 0 || height == 0) {
        x = 0;
        y0 = 0;
        width = s->width;
        height = s->height;
    }
    if (x >= s->width || y0 >= s->height) {
        return;
    }
    if (width > s->width - x) {
        width = s->width - x;
    }
    if (height > s->height - y0) {
        height = s->height - y0;
    }

    dst_stride = surface_stride(surface);

    mxgpu_dp_main_link_transfer(s, dst, dst_stride, src,
                                x, y0, width, height);

    dpy_gfx_update(s->con, x, y0, width, height);
}

static const GraphicHwOps mxgpu_graphic_ops = {
    .gfx_update = mxgpu_update_display,
};

static void mxgpu_set_mode(MXGPUState *s, uint32_t mode)
{
    if (mode >= MXGPU_MAX_MODE) {
        return;
    }

    s->current_mode = mode;
    s->width = mxgpu_modes[mode].width;
    s->height = mxgpu_modes[mode].height;
    s->stride = s->width * FB_BPP;
    s->pixel_clock_khz = mxgpu_modes[mode].pixel_clock_khz;
    s->h_total = mxgpu_modes[mode].h_total;
    s->h_sync = (mxgpu_modes[mode].h_sync_start & 0xffff) |
                (mxgpu_modes[mode].h_sync_end << 16);
    s->v_total = mxgpu_modes[mode].v_total;
    s->v_sync = (mxgpu_modes[mode].v_sync_start & 0xffff) |
                (mxgpu_modes[mode].v_sync_end << 16);
    s->flush_x = 0;
    s->flush_y = 0;
    s->flush_width = s->width;
    s->flush_height = s->height;
    mxgpu_dp_dump_msa(s);

    if (s->con != NULL) {
        DisplaySurface *surface;
        uint8_t *old_scanout_buffer;
        uint8_t *new_scanout_buffer;

        new_scanout_buffer = g_malloc0(s->stride * s->height);

        surface = qemu_create_displaysurface_from(s->width, s->height,
                                                  PIXMAN_LE_x8r8g8b8,
                                                  s->stride,
                                                  new_scanout_buffer);
        old_scanout_buffer = s->scanout_buffer;
        s->scanout_buffer = new_scanout_buffer;
        dpy_gfx_replace_surface(s->con, surface);
        g_free(old_scanout_buffer);
        graphic_hw_update(s->con);
    }
}

static uint64_t mxgpu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    MXGPUState *s = (MXGPUState *)opaque;
    uint64_t data = 0;
    switch (addr) {
    case REG_WIDTH:
        data = s->width;
        break;
    case REG_HEIGHT:
        data = s->height;
        break;
    case REG_FB_SIZE:
        data = s->width * s->height * FB_BPP;
        break;
    case REG_MODE:
        data = s->current_mode;
        break;
    case REG_MAX_MODE:
        data = MXGPU_MAX_MODE;
        break;
    case REG_STRIDE:
        data = s->stride;
        break;
    case REG_PIPE_CONTROL:
        data = s->pipe_control;
        break;
    case REG_PIXEL_CLOCK_KHZ:
        data = s->pixel_clock_khz;
        break;
    case REG_H_TOTAL:
        data = s->h_total;
        break;
    case REG_H_SYNC:
        data = s->h_sync;
        break;
    case REG_V_TOTAL:
        data = s->v_total;
        break;
    case REG_V_SYNC:
        data = s->v_sync;
        break;
    case REG_FLUSH_X:
        data = s->flush_x;
        break;
    case REG_FLUSH_Y:
        data = s->flush_y;
        break;
    case REG_FLUSH_WIDTH:
        data = s->flush_width;
        break;
    case REG_FLUSH_HEIGHT:
        data = s->flush_height;
        break;
    case REG_DP_HPD:
        data = s->dp_hpd;
        break;
    case REG_DP_LINK_RATE:
        data = s->dp_link_rate;
        break;
    case REG_DP_LANE_COUNT:
        data = s->dp_lane_count;
        break;
    case REG_DP_TRAINING_STATUS:
        data = s->dp_training_status;
        break;
    case REG_DP_AUX_ADDR:
        data = s->dp_aux_addr;
        break;
    case REG_DP_AUX_DATA:
        data = mxgpu_aux_read(s, s->dp_aux_addr);
        s->dp_aux_addr++;
        break;
    case REG_DP_AUX_STATUS:
        data = s->dp_aux_status;
        break;
    case REG_DP_EDID_SIZE:
        data = qemu_edid_size(s->edid);
        break;
    case REG_DP_MAIN_LINK_STATUS:
        mxgpu_dp_main_link_ready(s);
        data = s->dp_main_link_status;
        break;
    case REG_DP_MAIN_LINK_BYTES_LO:
        data = s->dp_main_link_bytes & 0xffffffffu;
        break;
    case REG_DP_MAIN_LINK_BYTES_HI:
        data = s->dp_main_link_bytes >> 32;
        break;
    case REG_DP_MSA_REQUIRED_KBPS:
        data = mxgpu_dp_mode_required_kbps(s);
        break;
    case REG_DP_MSA_AVAILABLE_KBPS:
        data = mxgpu_dp_link_capacity_kbps(s);
        break;
    case REG_DP_TRAINING_STATE:
        data = s->dp_training_state;
        break;
    case REG_DP_FRAME_COUNT:
        data = s->dp_frame_count;
        break;
    case REG_DP_FRAME_CRC:
        data = s->dp_frame_crc;
        break;
    case REG_DP_TEST_PATTERN:
        data = s->dp_test_pattern;
        break;
    default:
        data = 0;
        break;
    }
    return data;
}

static void mxgpu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MXGPUState *s = (MXGPUState *)opaque;
    switch (addr) {
    case REG_MODE:
        mxgpu_set_mode(s, (uint32_t)val);
        break;
    case REG_FLUSH:
        if (val == 1) {
            s->flush_x = 0;
            s->flush_y = 0;
            s->flush_width = s->width;
            s->flush_height = s->height;
        }
        if (s->con != NULL) {
            graphic_hw_update(s->con);
        }
        break;
    case REG_PIPE_CONTROL:
        s->pipe_control = (uint32_t)val;
        mxgpu_dp_main_link_ready(s);
        if (s->con != NULL) {
            graphic_hw_update(s->con);
        }
        break;
    case REG_PIXEL_CLOCK_KHZ:
        s->pixel_clock_khz = (uint32_t)val;
        break;
    case REG_H_TOTAL:
        s->h_total = (uint32_t)val;
        break;
    case REG_H_SYNC:
        s->h_sync = (uint32_t)val;
        break;
    case REG_V_TOTAL:
        s->v_total = (uint32_t)val;
        break;
    case REG_V_SYNC:
        s->v_sync = (uint32_t)val;
        break;
    case REG_FLUSH_X:
        s->flush_x = (uint32_t)val;
        break;
    case REG_FLUSH_Y:
        s->flush_y = (uint32_t)val;
        break;
    case REG_FLUSH_WIDTH:
        s->flush_width = (uint32_t)val;
        break;
    case REG_FLUSH_HEIGHT:
        s->flush_height = (uint32_t)val;
        break;
    case REG_DP_LINK_RATE:
        s->dp_link_rate = (uint32_t)val;
        mxgpu_dp_main_link_ready(s);
        break;
    case REG_DP_LANE_COUNT:
        s->dp_lane_count = (uint32_t)val;
        mxgpu_dp_main_link_ready(s);
        break;
    case REG_DP_TRAINING_STATUS:
        s->dp_training_status = (uint32_t)val ? DP_TRAINING_DONE : 0;
        s->dp_training_state = val ? MXGPU_DP_TRAINING_TRAINED :
                              MXGPU_DP_TRAINING_IDLE;
        mxgpu_dp_main_link_ready(s);
        break;
    case REG_DP_AUX_ADDR:
        s->dp_aux_addr = (uint32_t)val;
        break;
    case REG_DP_AUX_DATA:
        mxgpu_aux_write(s, s->dp_aux_addr, val);
        s->dp_aux_addr++;
        break;
    case REG_DP_TEST_PATTERN:
        s->dp_test_pattern = (uint32_t)val;
        if (s->dp_test_pattern > MXGPU_TEST_PATTERN_GRADIENT) {
            s->dp_test_pattern = MXGPU_TEST_PATTERN_FRAMEBUFFER;
        }
        if (s->con != NULL) {
            graphic_hw_update(s->con);
        }
        break;
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
    }
};

static const Property mxgpu_properties[] = {
    DEFINE_EDID_PROPERTIES(MXGPUState, edid_info),
};

static void mxgpu_realize(PCIDevice *pdev, Error **errp)
{
    MXGPUState *s = MX_GPU(pdev);

    /**
     * Default mode: 0
     */
    mxgpu_set_mode(s, 0);
    s->pipe_control = PIPE_CONTROL_ENABLE;
    s->dp_hpd = DP_HPD_CONNECTED;
    s->dp_link_rate = DPCD_5_4GBPS;
    s->dp_lane_count = DPCD_FOUR_LANES;
    s->dp_training_status = DP_TRAINING_DONE;
    s->dp_training_state = MXGPU_DP_TRAINING_TRAINED;
    s->dp_aux_status = DP_AUX_STATUS_ACK;
    s->dp_main_link_status = 0;
    s->dp_frame_count = 0;
    s->dp_frame_crc = 0;
    s->dp_test_pattern = MXGPU_TEST_PATTERN_FRAMEBUFFER;
    s->dp_main_link_bytes = 0;
    mxgpu_init_dpcd(s);

    if (s->edid_info.vendor == NULL) {
        s->edid_info.vendor = "QEM";
    }
    if (s->edid_info.name == NULL) {
        s->edid_info.name = "MetaX DP";
    }
    if (s->edid_info.prefx == 0) {
        s->edid_info.prefx = 1920;
    }
    if (s->edid_info.prefy == 0) {
        s->edid_info.prefy = 1080;
    }
    if (s->edid_info.maxx == 0) {
        s->edid_info.maxx = 3840;
    }
    if (s->edid_info.maxy == 0) {
        s->edid_info.maxy = 2160;
    }
    if (s->edid_info.refresh_rate == 0) {
        s->edid_info.refresh_rate = 60000;
    }
    qemu_edid_generate(s->edid, sizeof(s->edid), &s->edid_info);

    memory_region_init_io(&s->mmio, OBJECT(s), &mxgpu_mmio_ops, s,
                          "mxgpu-mmio", MMIO_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    if (!memory_region_init_ram_flags_nomigrate(&s->vram, OBJECT(s),
                                                "mxgpu-vram", FB_BAR_SIZE,
                                                RAM_PRIVATE, errp)) {
        return;
    }

    pci_register_bar(pdev, 1,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                         PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->vram);

    s->con = graphic_console_init(DEVICE(s), 0, &mxgpu_graphic_ops, s);
    mxgpu_set_mode(s, s->current_mode);
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
    device_class_set_props(dc, mxgpu_properties);
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
