#ifndef EDGE_EXTRACTOR_H
#define EDGE_EXTRACTOR_H

#include "axi_regs.h"

#define EDGE_EXTRACTOR_MAP_SIZE 0x1000

// Default base addresses (can/should be overridden)
#ifndef EDGE_EXTRACTOR_BASE
#pragma message("WARNING: EDGE_EXTRACTOR_BASE not defined, using placeholder 0x80021000 - override for your Vivado project")
#define EDGE_EXTRACTOR_BASE 0x80021000
#endif

/* Register offsets */
typedef enum {
    EDGE_EXTRACTOR_CTRL        = 0x00,  // bit 0 = stats_reset, [2:1] = hash_mode, bit 3 = hash_mode_we
    EDGE_EXTRACTOR_TOTAL_LO    = 0x04,  // edges_total, lower 32 bits
    EDGE_EXTRACTOR_TOTAL_HI    = 0x08,  // edges_total, upper 32 bits (latched at last LO read)
    EDGE_EXTRACTOR_OVERFLOW    = 0x0C,  // fifo_overflow_count
    EDGE_EXTRACTOR_FREEZE_DROP = 0x10,  // freeze_drop_count
    EDGE_EXTRACTOR_RANGE_EN    = 0x14,  // bit 0 = range filter enable (bounds from decoder_axi)
    EDGE_EXTRACTOR_RANGE_DROP  = 0x18,  // range_drop_count
} edge_extractor_reg_t;

/* Control register bits */
#define EDGE_EXTRACTOR_CTRL_STATS_RESET      (1 << 0)
#define EDGE_EXTRACTOR_CTRL_HASH_MODE_SHIFT  1
#define EDGE_EXTRACTOR_CTRL_HASH_MODE_MASK   (0x3 << EDGE_EXTRACTOR_CTRL_HASH_MODE_SHIFT)
#define EDGE_EXTRACTOR_CTRL_HASH_MODE_WE     (1 << 3)

/* Hash mode encoding for CTRL bits [2:1] (edge_extractor's hash_pkg.vhd) */
typedef enum {
    EDGE_HASH_NONE      = 0x0,  // passthrough, index = effective_addr
    EDGE_HASH_FUZZSIGHT = 0x1,  // N-atom count XORed into address bits [63:48]
    EDGE_HASH_STALKER   = 0x2,  // N-atom count added onto the address (RAID'24 Stalker)
} edge_hash_mode_t;

typedef axi_regs_t edge_extractor_t;

/* Function mapping to axi_regs defaults */
static inline int  edge_extractor_open(edge_extractor_t *handle)
    { return axi_regs_open(handle, EDGE_EXTRACTOR_BASE,EDGE_EXTRACTOR_MAP_SIZE); }
static inline void edge_extractor_close(edge_extractor_t *handle)
    { axi_regs_close(handle); }
static inline void edge_extractor_reset(edge_extractor_t *handle)
    { axi_regs_write(handle, EDGE_EXTRACTOR_CTRL, EDGE_EXTRACTOR_CTRL_STATS_RESET); }
static inline uint32_t edge_extractor_read(edge_extractor_t *handle, edge_extractor_reg_t reg)
    { return axi_regs_read(handle, reg); }

/* Select the hashing mode used by edge_extractor. Must only be called while the
 * decoder's freeze request is asserted (between execution sessions) */
static inline void edge_extractor_set_hash_mode(edge_extractor_t *handle, edge_hash_mode_t mode)
{
    axi_regs_write(handle, EDGE_EXTRACTOR_CTRL,
                    EDGE_EXTRACTOR_CTRL_HASH_MODE_WE |
                    ((mode << EDGE_EXTRACTOR_CTRL_HASH_MODE_SHIFT) & EDGE_EXTRACTOR_CTRL_HASH_MODE_MASK));
}

/* Read back the active hash mode from CTRL bits [2:1]. */
static inline edge_hash_mode_t edge_extractor_get_hash_mode(edge_extractor_t *handle)
{
    uint32_t ctrl = axi_regs_read(handle, EDGE_EXTRACTOR_CTRL);
    return (edge_hash_mode_t)((ctrl & EDGE_EXTRACTOR_CTRL_HASH_MODE_MASK) >> EDGE_EXTRACTOR_CTRL_HASH_MODE_SHIFT);
}

/* Drop atom packets whose effective address is outside the decoder's trace
 * range, set with decoder_axi_set_range() (e.g. the traced binary's text
 * mapping from /proc/pid/maps). The same range filters the decoder's exceptions,
 * so both always agree. Replaces the ETM's own address range filter, which
 * overflows the ETM FIFO. Off at reset. Like the hash mode, only change it
 * while the freeze request is asserted. */
static inline void edge_extractor_set_range_filter(edge_extractor_t *handle, int enable)
    { axi_regs_write(handle, EDGE_EXTRACTOR_RANGE_EN, enable ? 1 : 0); }

/* Description used in the print */
static axi_reg_desc_t edge_extractor_descs[] = {
    {"Edges total (lo)", EDGE_EXTRACTOR_TOTAL_LO},
    {"FIFO overflow", EDGE_EXTRACTOR_OVERFLOW},
    {"Freeze drops",  EDGE_EXTRACTOR_FREEZE_DROP},
    {"Range drops",   EDGE_EXTRACTOR_RANGE_DROP},
};

/* Full 64-bit edges_total. The hardware latches the high word on the LO read
 * so a lo-then-hi pair is a consistent snapshot even though the counter keeps
 * running between the two AXI-Lite reads. */
static inline uint64_t edge_extractor_read_edges_total(edge_extractor_t *handle) {
    uint32_t lo = axi_regs_read(handle, EDGE_EXTRACTOR_TOTAL_LO);
    uint32_t hi = axi_regs_read(handle, EDGE_EXTRACTOR_TOTAL_HI);
    return ((uint64_t)hi << 32) | lo;
}

static inline void edge_extractor_print(edge_extractor_t *h)
    { axi_regs_print(h, edge_extractor_descs,
                     sizeof(edge_extractor_descs) / sizeof(edge_extractor_descs[0])); }

#endif