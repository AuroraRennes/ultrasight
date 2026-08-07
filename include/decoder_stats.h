#ifndef DECODER_STATS_H
#define DECODER_STATS_H

#include "axi_regs.h"
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#define DECODER_STATS_MAP_SIZE 0x1000

// Default base addresses (can/should be overridden)
#ifndef DECODER_STATS_BASE_ETM
#pragma message("WARNING: DECODER_STATS_BASE_ETM not defined, using placeholder 0x80020000 - override for your Vivado project")
#define DECODER_STATS_BASE_ETM 0x80020000
#endif

/* Three levels of granularity are tracked here:
 *   packet:       one 32-bit decoder input transfer (a cycle where any of
 *                 the 4 sequential sub-decode ports produced a result).
 *   atom packet:  one decoded ETM packet carrying one or more atoms.
 *   atom element: one individual taken/not-taken (E/N) outcome bundled
 *                 inside an atom packet , what ARM's ETM spec itself
 *                 calls an "atom". A single atom packet can carry 1 to 24
 *                 atom elements. */

/* Register offsets.
 *
 * Every stat is a 64-bit counter in the fabric but the AXI-Lite data bus is
 * 32 bits, so each one occupies an 8-byte pair: the low word at the offset
 * below, the high word 4 bytes above it (DECODER_STATS_HI). Read both and
 * recombine with decoder_stats_read64().
 *
 * The pair is safe to read in two transactions as long as stats are disabled
 * first: the fabric gates every accumulator on stats_en, so decoder_stats_disable()
 * freezes the counters and the two halves cannot tear. */
typedef enum {
    DECODER_STATS_CTRL           = 0x000,
    DECODER_STATS_TOTAL          = 0x008,
    DECODER_STATS_IDLE           = 0x010,
    DECODER_STATS_PACKET_CNT     = 0x018,
    DECODER_STATS_PACKET_BST_CNT = 0x020,
    DECODER_STATS_PACKET_MIN_BST = 0x028,
    DECODER_STATS_PACKET_MAX_BST = 0x030,
    DECODER_STATS_PACKET_SUM_BST = 0x038,
    DECODER_STATS_PACKET_GAP_CNT = 0x040,
    DECODER_STATS_PACKET_MIN_GAP = 0x048,
    DECODER_STATS_PACKET_MAX_GAP = 0x050,
    DECODER_STATS_PACKET_SUM_GAP = 0x058,
    DECODER_STATS_ATOM_PKT_CNT     = 0x060,
    DECODER_STATS_ATOM_PKT_BST_CNT = 0x068,
    DECODER_STATS_ATOM_PKT_MIN_BST = 0x070,
    DECODER_STATS_ATOM_PKT_MAX_BST = 0x078,
    DECODER_STATS_ATOM_PKT_SUM_BST = 0x080,
    DECODER_STATS_ATOM_PKT_GAP_CNT = 0x088,
    DECODER_STATS_ATOM_PKT_MIN_GAP = 0x090,
    DECODER_STATS_ATOM_PKT_MAX_GAP = 0x098,
    DECODER_STATS_ATOM_PKT_SUM_GAP = 0x0A0,
    /* Packet gap histogram: 0, 1-4, 5-15, 16-255, 256+ */
    DECODER_STATS_PKT_GAP_H0     = 0x0A8,
    DECODER_STATS_PKT_GAP_H1     = 0x0B0,
    DECODER_STATS_PKT_GAP_H2     = 0x0B8,
    DECODER_STATS_PKT_GAP_H3     = 0x0C0,
    DECODER_STATS_PKT_GAP_H4     = 0x0C8,
    /* Packet burst histogram: 1, 2-3, 4-15, 16+ */
    DECODER_STATS_PKT_BST_H0     = 0x0D0,
    DECODER_STATS_PKT_BST_H1     = 0x0D8,
    DECODER_STATS_PKT_BST_H2     = 0x0E0,
    DECODER_STATS_PKT_BST_H3     = 0x0E8,
    /* Atom packet gap histogram: 0, 1-4, 5-15, 16-255, 256+ */
    DECODER_STATS_ATOM_PKT_GAP_H0 = 0x0F0,
    DECODER_STATS_ATOM_PKT_GAP_H1 = 0x0F8,
    DECODER_STATS_ATOM_PKT_GAP_H2 = 0x100,
    DECODER_STATS_ATOM_PKT_GAP_H3 = 0x108,
    DECODER_STATS_ATOM_PKT_GAP_H4 = 0x110,
    /* Atom packet burst histogram: 1, 2-3, 4-15, 16+ */
    DECODER_STATS_ATOM_PKT_BST_H0 = 0x118,
    DECODER_STATS_ATOM_PKT_BST_H1 = 0x120,
    DECODER_STATS_ATOM_PKT_BST_H2 = 0x128,
    DECODER_STATS_ATOM_PKT_BST_H3 = 0x130,
    /* Sum of atom elements over all valid atom packets: true atom element
     * throughput, vs. DECODER_STATS_ATOM_PKT_CNT which only counts packets
     * regardless of how many elements (atom_nb) each one bundles. */
    DECODER_STATS_ATOM_ELEM_SUM  = 0x138,
    /* Atom element size histogram: 1, 2, 3, 4, 5, 6-11, 12-24.
     * 1-5 are exact values (ETMv4 formats F1-F5 always encode a fixed
     * atom_nb), 6-11/12-24 are ranges over the variable-length F6 format. */
    DECODER_STATS_ATOM_ELEM_H0   = 0x140,
    DECODER_STATS_ATOM_ELEM_H1   = 0x148,
    DECODER_STATS_ATOM_ELEM_H2   = 0x150,
    DECODER_STATS_ATOM_ELEM_H3   = 0x158,
    DECODER_STATS_ATOM_ELEM_H4   = 0x160,
    DECODER_STATS_ATOM_ELEM_H5   = 0x168,
    DECODER_STATS_ATOM_ELEM_H6   = 0x170,
} decoder_stats_reg_t;

typedef axi_regs_t decoder_stats_t;

/* Function mapping to axi_regs defaults */
static inline int  decoder_stats_open(decoder_stats_t *handle)
    { return axi_regs_open(handle, DECODER_STATS_BASE_ETM, DECODER_STATS_MAP_SIZE); }
static inline void decoder_stats_close(decoder_stats_t *handle)
    { axi_regs_close(handle); }
static inline void decoder_stats_enable(decoder_stats_t *handle)
    { axi_regs_write(handle, DECODER_STATS_CTRL, 3); }
static inline void decoder_stats_disable(decoder_stats_t *handle)
    { axi_regs_write(handle, DECODER_STATS_CTRL, 0); }
static inline uint32_t decoder_stats_read(decoder_stats_t *handle, decoder_stats_reg_t reg)
    { return axi_regs_read(handle, reg); }

/* High word of a stat: 4 bytes above its low word. */
#define DECODER_STATS_HI(reg) ((reg) + 4)

/* Full 64-bit value of a stat. Disable stats first (decoder_stats_disable) so the
 * counter is frozen across the two reads. */
static inline uint64_t decoder_stats_read64(decoder_stats_t *handle, decoder_stats_reg_t reg)
{
    uint32_t lo = axi_regs_read(handle, reg);
    uint32_t hi = axi_regs_read(handle, DECODER_STATS_HI(reg));
    return ((uint64_t)hi << 32) | lo;
}

/* Description used in the print */
static axi_reg_desc_t decoder_stats_descs[] = {
    {"Total cycles",                  DECODER_STATS_TOTAL},
    {"Idle cycles",                   DECODER_STATS_IDLE},
    {"Packet count",                  DECODER_STATS_PACKET_CNT},
    {"Packet burst count",            DECODER_STATS_PACKET_BST_CNT},
    {"Min packet burst length",       DECODER_STATS_PACKET_MIN_BST},
    {"Max packet burst length",       DECODER_STATS_PACKET_MAX_BST},
    {"Sum packet burst length",       DECODER_STATS_PACKET_SUM_BST},
    {"Packet gap count",              DECODER_STATS_PACKET_GAP_CNT},
    {"Min packet gap length",         DECODER_STATS_PACKET_MIN_GAP},
    {"Max packet gap length",         DECODER_STATS_PACKET_MAX_GAP},
    {"Sum packet gap length",         DECODER_STATS_PACKET_SUM_GAP},
    {"Atom packet count",             DECODER_STATS_ATOM_PKT_CNT},
    {"Atom packet burst count",       DECODER_STATS_ATOM_PKT_BST_CNT},
    {"Min atom packet burst length",  DECODER_STATS_ATOM_PKT_MIN_BST},
    {"Max atom packet burst length",  DECODER_STATS_ATOM_PKT_MAX_BST},
    {"Sum atom packet burst length",  DECODER_STATS_ATOM_PKT_SUM_BST},
    {"Atom packet gap count",         DECODER_STATS_ATOM_PKT_GAP_CNT},
    {"Min atom packet gap length",    DECODER_STATS_ATOM_PKT_MIN_GAP},
    {"Max atom packet gap length",    DECODER_STATS_ATOM_PKT_MAX_GAP},
    {"Sum atom packet gap length",    DECODER_STATS_ATOM_PKT_SUM_GAP},
    {"Atom element sum (true count)", DECODER_STATS_ATOM_ELEM_SUM},
};

static inline void decoder_stats_print(decoder_stats_t *h)
    { axi_regs_print(h, decoder_stats_descs,
                     sizeof(decoder_stats_descs)/sizeof(decoder_stats_descs[0])); }

/* ------------------------------------------------------------------ */
/* Histogram helper                                                   */
/* ------------------------------------------------------------------ */

static inline void _dec_hist(const char *title,
                              const char  *labels[],
                              uint64_t     values[],
                              int          n)
{
    uint64_t total = 0;
    uint64_t max   = 0;
    for (int i = 0; i < n; i++) {
        total += values[i];
        if (values[i] > max) max = values[i];
    }

    fprintf(stderr, "  %-20s  (total=%" PRIu64 ")\n", title, total);
    for (int i = 0; i < n; i++) {
        float    pct  = total ? 100.0f * values[i] / total : 0.0f;
        int      bars = (max > 0) ? (int)(32.0f * (float)values[i] / (float)max) : 0;
        fprintf(stderr, "    %-10s [", labels[i]);
        for (int b = 0; b < 32; b++) fputc(b < bars ? '#' : ' ', stderr);
        fprintf(stderr, "] %8" PRIu64 " (%5.1f%%)\n", values[i], pct);
    }
}

/* ------------------------------------------------------------------ */
/* Detailed print                                                       */
/* ------------------------------------------------------------------ */

static inline void decoder_stats_print_detailed(decoder_stats_t *h)
{
    uint64_t total       = decoder_stats_read64(h, DECODER_STATS_TOTAL);
    uint64_t idle        = decoder_stats_read64(h, DECODER_STATS_IDLE);
    uint64_t pkt_cnt     = decoder_stats_read64(h, DECODER_STATS_PACKET_CNT);
    uint64_t pkt_bst_cnt = decoder_stats_read64(h, DECODER_STATS_PACKET_BST_CNT);
    uint64_t pkt_min_bst = decoder_stats_read64(h, DECODER_STATS_PACKET_MIN_BST);
    uint64_t pkt_max_bst = decoder_stats_read64(h, DECODER_STATS_PACKET_MAX_BST);
    uint64_t pkt_sum_bst = decoder_stats_read64(h, DECODER_STATS_PACKET_SUM_BST);
    uint64_t pkt_gap_cnt = decoder_stats_read64(h, DECODER_STATS_PACKET_GAP_CNT);
    uint64_t pkt_min_gap = decoder_stats_read64(h, DECODER_STATS_PACKET_MIN_GAP);
    uint64_t pkt_max_gap = decoder_stats_read64(h, DECODER_STATS_PACKET_MAX_GAP);
    uint64_t pkt_sum_gap = decoder_stats_read64(h, DECODER_STATS_PACKET_SUM_GAP);
    uint64_t atom_pkt_cnt     = decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_CNT);
    uint64_t atom_pkt_bst_cnt = decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_BST_CNT);
    uint64_t atom_pkt_min_bst = decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_MIN_BST);
    uint64_t atom_pkt_max_bst = decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_MAX_BST);
    uint64_t atom_pkt_sum_bst = decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_SUM_BST);
    uint64_t atom_pkt_gap_cnt = decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_GAP_CNT);
    uint64_t atom_pkt_min_gap = decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_MIN_GAP);
    uint64_t atom_pkt_max_gap = decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_MAX_GAP);
    uint64_t atom_pkt_sum_gap = decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_SUM_GAP);
    uint64_t atom_elem_sum    = decoder_stats_read64(h, DECODER_STATS_ATOM_ELEM_SUM);

    uint64_t pkt_gap_hist[] = {
        decoder_stats_read64(h, DECODER_STATS_PKT_GAP_H0),  /* 0       */
        decoder_stats_read64(h, DECODER_STATS_PKT_GAP_H1),  /* 1-4     */
        decoder_stats_read64(h, DECODER_STATS_PKT_GAP_H2),  /* 5-15    */
        decoder_stats_read64(h, DECODER_STATS_PKT_GAP_H3),  /* 16-255  */
        decoder_stats_read64(h, DECODER_STATS_PKT_GAP_H4),  /* 256+    */
    };
    uint64_t pkt_bst_hist[] = {
        decoder_stats_read64(h, DECODER_STATS_PKT_BST_H0),  /* 1       */
        decoder_stats_read64(h, DECODER_STATS_PKT_BST_H1),  /* 2-3     */
        decoder_stats_read64(h, DECODER_STATS_PKT_BST_H2),  /* 4-15    */
        decoder_stats_read64(h, DECODER_STATS_PKT_BST_H3),  /* 16+     */
    };
    uint64_t atom_pkt_gap_hist[] = {
        decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_GAP_H0),  /* 0       */
        decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_GAP_H1),  /* 1-4     */
        decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_GAP_H2),  /* 5-15    */
        decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_GAP_H3),  /* 16-255  */
        decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_GAP_H4),  /* 256+    */
    };
    uint64_t atom_pkt_bst_hist[] = {
        decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_BST_H0),  /* 1       */
        decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_BST_H1),  /* 2-3     */
        decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_BST_H2),  /* 4-15    */
        decoder_stats_read64(h, DECODER_STATS_ATOM_PKT_BST_H3),  /* 16+     */
    };
    uint64_t atom_elem_hist[] = {
        decoder_stats_read64(h, DECODER_STATS_ATOM_ELEM_H0),   /* 1       */
        decoder_stats_read64(h, DECODER_STATS_ATOM_ELEM_H1),   /* 2       */
        decoder_stats_read64(h, DECODER_STATS_ATOM_ELEM_H2),   /* 3       */
        decoder_stats_read64(h, DECODER_STATS_ATOM_ELEM_H3),   /* 4       */
        decoder_stats_read64(h, DECODER_STATS_ATOM_ELEM_H4),   /* 5       */
        decoder_stats_read64(h, DECODER_STATS_ATOM_ELEM_H5),   /* 6-11    */
        decoder_stats_read64(h, DECODER_STATS_ATOM_ELEM_H6),   /* 12-24   */
    };

    const char *gap_labels[]  = { "0cy", "1-4cy", "5-15cy", "16-255cy", "256+cy" };
    const char *bst_labels[]  = { "1",   "2-3",   "4-15",   "16+"               };
    const char *elem_labels[] = { "1", "2", "3", "4", "5", "6-11", "12-24"      };

    uint64_t active = (total >= idle) ? total - idle : 0;

    fprintf(stderr, "\n=== Decoder Stats ===\n");
    fprintf(stderr, "  Total cycles  : %" PRIu64 "\n", total);
    fprintf(stderr, "  Active cycles : %" PRIu64 " (%.1f%%)\n",
            active, total ? 100.0f * (float)active / (float)total : 0.0f);
    fprintf(stderr, "  Idle cycles   : %" PRIu64 " (%.1f%%)\n",
            idle,   total ? 100.0f * (float)idle   / (float)total : 0.0f);

    fprintf(stderr, "\n--- Packets ---\n");
    fprintf(stderr, "  Count         : %" PRIu64 "\n", pkt_cnt);
    fprintf(stderr, "  Bursts        : %" PRIu64 "  avg=%.2f  min=%" PRIu64 "  max=%" PRIu64 "\n",
            pkt_bst_cnt,
            pkt_bst_cnt ? (float)pkt_sum_bst / (float)pkt_bst_cnt : 0.0f,
            pkt_min_bst, pkt_max_bst);
    fprintf(stderr, "  Gaps          : %" PRIu64 "  avg=%.2f  min=%" PRIu64 "  max=%" PRIu64 "\n",
            pkt_gap_cnt,
            pkt_gap_cnt ? (float)pkt_sum_gap / (float)pkt_gap_cnt : 0.0f,
            pkt_min_gap, pkt_max_gap);
    fprintf(stderr, "  Rate          : %.4f pkts/cycle\n",
            total ? (float)pkt_cnt / (float)total : 0.0f);

    _dec_hist("Packet gap dist.",   gap_labels, pkt_gap_hist, 5);
    _dec_hist("Packet burst dist.", bst_labels, pkt_bst_hist, 4);

    fprintf(stderr, "\n--- Atom packets ---\n");
    fprintf(stderr, "  Count         : %" PRIu64 "\n", atom_pkt_cnt);
    fprintf(stderr, "  Bursts        : %" PRIu64 "  avg=%.2f  min=%" PRIu64 "  max=%" PRIu64 "\n",
            atom_pkt_bst_cnt,
            atom_pkt_bst_cnt ? (float)atom_pkt_sum_bst / (float)atom_pkt_bst_cnt : 0.0f,
            atom_pkt_min_bst, atom_pkt_max_bst);
    fprintf(stderr, "  Gaps          : %" PRIu64 "  avg=%.2f  min=%" PRIu64 "  max=%" PRIu64 "\n",
            atom_pkt_gap_cnt,
            atom_pkt_gap_cnt ? (float)atom_pkt_sum_gap / (float)atom_pkt_gap_cnt : 0.0f,
            atom_pkt_min_gap, atom_pkt_max_gap);
    fprintf(stderr, "  Rate          : %.4f atom packets/cycle\n",
            total ? (float)atom_pkt_cnt / (float)total : 0.0f);
    fprintf(stderr, "  Writer load   : %.1f%% (drain rate = 1/3 cy)\n",
            total ? 100.0f * ((float)atom_pkt_cnt / (float)total) * 3.0f : 0.0f);

    _dec_hist("Atom packet gap dist.",   gap_labels, atom_pkt_gap_hist, 5);
    _dec_hist("Atom packet burst dist.", bst_labels, atom_pkt_bst_hist, 4);

    fprintf(stderr, "\n--- Atom elements ---\n");
    fprintf(stderr, "  Count         : %" PRIu64 " (avg %.2f elements/atom packet)\n",
            atom_elem_sum, atom_pkt_cnt ? (float)atom_elem_sum / (float)atom_pkt_cnt : 0.0f);
    fprintf(stderr, "  Note          : atom packet count above only counts packets;\n"
                    "                  this is the real element throughput.\n");

    _dec_hist("Atom element dist.", elem_labels, atom_elem_hist, 7);
}

/* ------------------------------------------------------------------ */
/* CSV export                                                          */
/* ------------------------------------------------------------------ */

/* Column header for decoder_stats_write_csv_row(), so a caller building a
 * combined CSV can emit a matching header once. Must stay in the same order
 * as decoder_stats_csv_regs below. */
#define DECODER_STATS_CSV_HEADER \
    "total_cycles,idle_cycles," \
    "pkt_cnt,pkt_bst_cnt,pkt_min_bst,pkt_max_bst,pkt_sum_bst," \
    "pkt_gap_cnt,pkt_min_gap,pkt_max_gap,pkt_sum_gap," \
    "pkt_gap_h_0,pkt_gap_h_1_4,pkt_gap_h_5_15,pkt_gap_h_16_255,pkt_gap_h_256p," \
    "pkt_bst_h_1,pkt_bst_h_2_3,pkt_bst_h_4_15,pkt_bst_h_16p," \
    "atom_pkt_cnt,atom_pkt_bst_cnt,atom_pkt_min_bst,atom_pkt_max_bst,atom_pkt_sum_bst," \
    "atom_pkt_gap_cnt,atom_pkt_min_gap,atom_pkt_max_gap,atom_pkt_sum_gap," \
    "atom_pkt_gap_h_0,atom_pkt_gap_h_1_4,atom_pkt_gap_h_5_15,atom_pkt_gap_h_16_255,atom_pkt_gap_h_256p," \
    "atom_pkt_bst_h_1,atom_pkt_bst_h_2_3,atom_pkt_bst_h_4_15,atom_pkt_bst_h_16p," \
    "atom_elem_sum," \
    "atom_elem_h_1,atom_elem_h_2,atom_elem_h_3,atom_elem_h_4,atom_elem_h_5,atom_elem_h_6_11,atom_elem_h_12_24"

/* Registers in the order DECODER_STATS_CSV_HEADER names them. Kept as a table
 * rather than a run of fprintf calls so the header and the row cannot drift
 * apart in count or order. */
static const decoder_stats_reg_t decoder_stats_csv_regs[] = {
    DECODER_STATS_TOTAL,          DECODER_STATS_IDLE,
    DECODER_STATS_PACKET_CNT,     DECODER_STATS_PACKET_BST_CNT,
    DECODER_STATS_PACKET_MIN_BST, DECODER_STATS_PACKET_MAX_BST,
    DECODER_STATS_PACKET_SUM_BST,
    DECODER_STATS_PACKET_GAP_CNT, DECODER_STATS_PACKET_MIN_GAP,
    DECODER_STATS_PACKET_MAX_GAP, DECODER_STATS_PACKET_SUM_GAP,
    DECODER_STATS_PKT_GAP_H0, DECODER_STATS_PKT_GAP_H1, DECODER_STATS_PKT_GAP_H2,
    DECODER_STATS_PKT_GAP_H3, DECODER_STATS_PKT_GAP_H4,
    DECODER_STATS_PKT_BST_H0, DECODER_STATS_PKT_BST_H1, DECODER_STATS_PKT_BST_H2,
    DECODER_STATS_PKT_BST_H3,
    DECODER_STATS_ATOM_PKT_CNT,     DECODER_STATS_ATOM_PKT_BST_CNT,
    DECODER_STATS_ATOM_PKT_MIN_BST, DECODER_STATS_ATOM_PKT_MAX_BST,
    DECODER_STATS_ATOM_PKT_SUM_BST,
    DECODER_STATS_ATOM_PKT_GAP_CNT, DECODER_STATS_ATOM_PKT_MIN_GAP,
    DECODER_STATS_ATOM_PKT_MAX_GAP, DECODER_STATS_ATOM_PKT_SUM_GAP,
    DECODER_STATS_ATOM_PKT_GAP_H0, DECODER_STATS_ATOM_PKT_GAP_H1,
    DECODER_STATS_ATOM_PKT_GAP_H2, DECODER_STATS_ATOM_PKT_GAP_H3,
    DECODER_STATS_ATOM_PKT_GAP_H4,
    DECODER_STATS_ATOM_PKT_BST_H0, DECODER_STATS_ATOM_PKT_BST_H1,
    DECODER_STATS_ATOM_PKT_BST_H2, DECODER_STATS_ATOM_PKT_BST_H3,
    DECODER_STATS_ATOM_ELEM_SUM,
    DECODER_STATS_ATOM_ELEM_H0, DECODER_STATS_ATOM_ELEM_H1,
    DECODER_STATS_ATOM_ELEM_H2, DECODER_STATS_ATOM_ELEM_H3,
    DECODER_STATS_ATOM_ELEM_H4, DECODER_STATS_ATOM_ELEM_H5,
    DECODER_STATS_ATOM_ELEM_H6,
};

/* Appends one CSV row (no trailing newline) with every raw counter read from
 * this block -- the same values decoder_stats_print_detailed() uses, just
 * unformatted so a caller can log a run per line across binaries. The counters
 * are 64-bit pairs, so disable stats first for a non-tearing snapshot. */
static inline void decoder_stats_write_csv_row(decoder_stats_t *h, FILE *f)
{
    size_t n = sizeof(decoder_stats_csv_regs) / sizeof(decoder_stats_csv_regs[0]);
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%s%" PRIu64, i ? "," : "",
                decoder_stats_read64(h, decoder_stats_csv_regs[i]));
}

#endif
