#ifndef DECODER_ERRORS_H
#define DECODER_ERRORS_H

#include "axi_regs.h"

#define DECODER_ERRORS_MAP_SIZE 0x1000

// Default base addresses (can/should be overridden)
#ifndef DECODER_ERRORS_BASE
#pragma message("WARNING: EDGE_STATS_BASE not defined, using placeholder 0x80000000 - override for your Vivado project")
#define DECODER_ERRORS_BASE 0x80000000
#endif

/* Register offsets */
typedef enum {
    DECODER_ERRORS_CTRL    = 0x00,  // bit 0 = error count reset
    DECODER_ERRORS_FRAME   = 0x04,  // edges_total
    DECODER_ERRORS_BS_GEN  = 0x08,  // fifo_overflow_count
    DECODER_ERRORS_DEMUX   = 0x0C,  // freeze_drop_count
} decoder_errors_reg_t;

/* Control register bits */
#define DECODER_ERRORS_CTRL_STATS_RESET  0x1 << 0
#define DECODER_ERRORS_CTRL_FLUSH        0x1 << 1

typedef axi_regs_t decoder_errors_t;

/* Function mapping to axi_regs defaults */
static inline int  decoder_errors_open(decoder_errors_t *handle)
    { return axi_regs_open(handle, DECODER_ERRORS_BASE,DECODER_ERRORS_MAP_SIZE); }
static inline void decoder_errors_close(decoder_errors_t *handle)
    { axi_regs_close(handle); }
static inline void decoder_errors_reset(decoder_errors_t *handle)
    { axi_regs_write(handle, DECODER_ERRORS_CTRL, DECODER_ERRORS_CTRL_STATS_RESET); }
static inline void decoder_errors_flush(decoder_errors_t *handle)
    { axi_regs_write(handle, DECODER_ERRORS_CTRL, DECODER_ERRORS_CTRL_FLUSH); }
static inline uint32_t decoder_errors_read(decoder_errors_t *handle, decoder_errors_reg_t reg)
    { return axi_regs_read(handle, reg); }

/* Description used in the print */
static axi_reg_desc_t decoder_errors_descs[] = {
    {"Frame errors",  DECODER_ERRORS_FRAME},
    {"Bytestream gen errors", DECODER_ERRORS_BS_GEN},
    {"Demux errors",  DECODER_ERRORS_DEMUX},
};

static inline void decoder_errors_print(decoder_errors_t *h)
    { axi_regs_print(h, decoder_errors_descs,
                     sizeof(decoder_errors_descs) / sizeof(decoder_errors_descs[0])); }

#endif