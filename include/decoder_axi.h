#ifndef DECODER_AXI_H
#define DECODER_AXI_H

#include <inttypes.h>

#include "axi_regs.h"

#define DECODER_AXI_MAP_SIZE 0x1000

// Default base addresses (can/should be overridden)
#ifndef DECODER_AXI_BASE
#pragma message("WARNING: DECODER_AXI_BASE not defined, using placeholder 0x80000000 - override for your Vivado project")
#define DECODER_AXI_BASE 0x80022000
#endif

/* Register offsets */
typedef enum {
    DECODER_AXI_CTRL           = 0x00, // bit 0 = error count reset, bit 1 = soft reset
    DECODER_AXI_STATUS         = 0x04, // bit 0 = soft_reset_done
    DECODER_AXI_FRAME          = 0x08, // frame errors
    DECODER_AXI_BS_GEN         = 0x0C, // bytestream gen errors
    DECODER_AXI_DEMUX          = 0x10, // demux errors
    DECODER_AXI_OVERFLOW       = 0x14, // ETM overflow packets
    DECODER_AXI_RANGE_BASE_LO  = 0x18, // Lower 32-bits of the base address
    DECODER_AXI_RANGE_BASE_HI  = 0x1C, // Upper 32-bits of the base address
    DECODER_AXI_RANGE_END_LO   = 0x20, // Lower 32-bits of the end address
    DECODER_AXI_RANGE_END_HI   = 0x24, // Upper 32-bits of the end address
    // 64-bit saturating count of raw trace bytes captured (4 bytes per
    // valid TPIU word, before any downstream overflow/drop). Reading
    // DECODER_AXI_BYTE_CNT_LO latches the high word for the next
    // DECODER_AXI_BYTE_CNT_HI read, so always read LO then HI.
    DECODER_AXI_BYTE_CNT_LO    = 0x28, // byte_count, lower 32 bits
    DECODER_AXI_BYTE_CNT_HI    = 0x2C, // byte_count, upper 32 bits (latched at last LO read)
} decoder_axi_reg_t;

/* Control register bits */
#define DECODER_AXI_CTRL_STATS_RESET  (0x1 << 0)
#define DECODER_AXI_CTRL_SOFT_RESET   (0x1 << 1)

/* Status register bits */
#define DECODER_AXI_STATUS_RESET_DONE (0x1 << 0)

typedef axi_regs_t decoder_axi_t;

/* Function mapping to axi_regs defaults */
static inline int  decoder_axi_open(decoder_axi_t *handle)
    { return axi_regs_open(handle, DECODER_AXI_BASE, DECODER_AXI_MAP_SIZE); }
static inline void decoder_axi_close(decoder_axi_t *handle)
    { axi_regs_close(handle); }
static inline void decoder_axi_stats_reset(decoder_axi_t *handle)
    { axi_regs_write(handle, DECODER_AXI_CTRL, DECODER_AXI_CTRL_STATS_RESET); }
static inline uint32_t decoder_axi_read(decoder_axi_t *handle, decoder_axi_reg_t reg)
    { return axi_regs_read(handle, reg); }

static inline void decoder_axi_soft_reset(decoder_axi_t *handle) {
    /* Trigger soft reset of decoder pipeline */
    axi_regs_write(handle, DECODER_AXI_CTRL, DECODER_AXI_CTRL_SOFT_RESET);
    /* Poll until reset complete (shift register drained) */
    while (!(axi_regs_read(handle, DECODER_AXI_STATUS) & DECODER_AXI_STATUS_RESET_DONE))
        ;
}

/* Range writers */
static inline void decoder_axi_set_range_base(decoder_axi_t *handle, uint64_t base) {
    axi_regs_write(handle, DECODER_AXI_RANGE_BASE_LO, (uint32_t)(base & 0xFFFFFFFF));
    axi_regs_write(handle, DECODER_AXI_RANGE_BASE_HI, (uint32_t)(base >> 32));
}

static inline void decoder_axi_set_range_end(decoder_axi_t *handle, uint64_t end) {
    axi_regs_write(handle, DECODER_AXI_RANGE_END_LO, (uint32_t)(end & 0xFFFFFFFF));
    axi_regs_write(handle, DECODER_AXI_RANGE_END_HI, (uint32_t)(end >> 32));
}

static inline void decoder_axi_set_range(decoder_axi_t *handle, uint64_t base, uint64_t end) {
    decoder_axi_set_range_base(handle, base);
    decoder_axi_set_range_end(handle, end);
}

static inline void decoder_axi_print_range(decoder_axi_t *handle) {
    uint32_t base_lo = axi_regs_read(handle, DECODER_AXI_RANGE_BASE_LO);
    uint32_t base_hi = axi_regs_read(handle, DECODER_AXI_RANGE_BASE_HI);
    uint32_t end_lo  = axi_regs_read(handle, DECODER_AXI_RANGE_END_LO);
    uint32_t end_hi  = axi_regs_read(handle, DECODER_AXI_RANGE_END_HI);
    uint64_t base = ((uint64_t)base_hi << 32) | base_lo;
    uint64_t end  = ((uint64_t)end_hi  << 32) | end_lo;
    fprintf(stderr, "[.] decoder range: 0x%016lx - 0x%016lx\n", base, end);
}

/* Raw trace byte count (64-bit, saturating). Must read LO before HI: the
 * hardware latches the high word on the LO read so a lo-then-hi pair is
 * always a consistent snapshot even though the counter keeps incrementing
 * between the two AXI-Lite reads. */
static inline uint64_t decoder_axi_read_byte_count(decoder_axi_t *handle) {
    uint32_t lo = axi_regs_read(handle, DECODER_AXI_BYTE_CNT_LO);
    uint32_t hi = axi_regs_read(handle, DECODER_AXI_BYTE_CNT_HI);
    return ((uint64_t)hi << 32) | lo;
}

/* Description used in the print */
static axi_reg_desc_t decoder_axi_descs[] = {
    {"Frame errors",          DECODER_AXI_FRAME},
    {"Bytestream gen errors", DECODER_AXI_BS_GEN},
    {"Demux errors",          DECODER_AXI_DEMUX},
    {"ETM overflow packets",  DECODER_AXI_OVERFLOW},
};

static inline void decoder_axi_print(decoder_axi_t *h) {
    axi_regs_print(h, decoder_axi_descs,
                    sizeof(decoder_axi_descs) / sizeof(decoder_axi_descs[0]));
    fprintf(stderr, "  %-24s %" PRIu64 "\n", "Raw trace bytes",
            decoder_axi_read_byte_count(h));
}

/* ------------------------------------------------------------------ */
/* CSV export                                                          */
/* ------------------------------------------------------------------ */

#define DECODER_AXI_CSV_HEADER \
    "frame_errors,bs_gen_errors,demux_errors,overflow_count,raw_trace_bytes"

/* Appends one CSV row (no trailing newline) with the error counters and
 * raw byte count from this block. */
static inline void decoder_axi_write_csv_row(decoder_axi_t *h, FILE *f)
{
    fprintf(f, "%u,%u,%u,%u,%" PRIu64,
            axi_regs_read(h, DECODER_AXI_FRAME),
            axi_regs_read(h, DECODER_AXI_BS_GEN),
            axi_regs_read(h, DECODER_AXI_DEMUX),
            axi_regs_read(h, DECODER_AXI_OVERFLOW),
            decoder_axi_read_byte_count(h));
}

#endif