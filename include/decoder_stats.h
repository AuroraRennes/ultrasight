#ifndef DECODER_STATS_H
#define DECODER_STATS_H

#include "axi_regs.h"
#include <stdio.h>
#include <stdint.h>

#define DECODER_STATS_MAP_SIZE 0x1000

// Default base addresses (can/should be overridden)
#ifndef DECODER_STATS_BASE_ETM
#pragma message("WARNING: DECODER_STATS_BASE_ETM not defined, using placeholder 0x80000000 - override for your Vivado project")
#define DECODER_STATS_BASE_ETM 0x80020000
#endif

/* Three levels of granularity are tracked here:
 *   packet:       one 32-bit decoder input transfer (a cycle where any of
 *                 the 4 sequential sub-decode ports produced a result).
 *   atom packet:  one decoded ETM packet carrying one or more atoms.
 *   atom element: one individual taken/not-taken (E/N) outcome bundled
 *                 inside an atom packet -- what ARM's ETM spec itself
 *                 calls an "atom". A single atom packet can carry 1 to 24
 *                 atom elements. */

/* Register offsets */
typedef enum {
    DECODER_STATS_CTRL           = 0x00,
    DECODER_STATS_TOTAL          = 0x04,
    DECODER_STATS_IDLE           = 0x08,
    DECODER_STATS_PACKET_CNT     = 0x0C,
    DECODER_STATS_PACKET_BST_CNT = 0x10,
    DECODER_STATS_PACKET_MIN_BST = 0x14,
    DECODER_STATS_PACKET_MAX_BST = 0x18,
    DECODER_STATS_PACKET_SUM_BST = 0x1C,
    DECODER_STATS_PACKET_GAP_CNT = 0x20,
    DECODER_STATS_PACKET_MIN_GAP = 0x24,
    DECODER_STATS_PACKET_MAX_GAP = 0x28,
    DECODER_STATS_PACKET_SUM_GAP = 0x2C,
    DECODER_STATS_ATOM_PKT_CNT     = 0x30,
    DECODER_STATS_ATOM_PKT_BST_CNT = 0x34,
    DECODER_STATS_ATOM_PKT_MIN_BST = 0x38,
    DECODER_STATS_ATOM_PKT_MAX_BST = 0x3C,
    DECODER_STATS_ATOM_PKT_SUM_BST = 0x40,
    DECODER_STATS_ATOM_PKT_GAP_CNT = 0x44,
    DECODER_STATS_ATOM_PKT_MIN_GAP = 0x48,
    DECODER_STATS_ATOM_PKT_MAX_GAP = 0x4C,
    DECODER_STATS_ATOM_PKT_SUM_GAP = 0x50,
    /* Packet gap histogram: 0, 1-4, 5-15, 16-255, 256+ */
    DECODER_STATS_PKT_GAP_H0     = 0x54,
    DECODER_STATS_PKT_GAP_H1     = 0x58,
    DECODER_STATS_PKT_GAP_H2     = 0x5C,
    DECODER_STATS_PKT_GAP_H3     = 0x60,
    DECODER_STATS_PKT_GAP_H4     = 0x64,
    /* Packet burst histogram: 1, 2-3, 4-15, 16+ */
    DECODER_STATS_PKT_BST_H0     = 0x68,
    DECODER_STATS_PKT_BST_H1     = 0x6C,
    DECODER_STATS_PKT_BST_H2     = 0x70,
    DECODER_STATS_PKT_BST_H3     = 0x74,
    /* Atom packet gap histogram: 0, 1-4, 5-15, 16-255, 256+ */
    DECODER_STATS_ATOM_PKT_GAP_H0 = 0x78,
    DECODER_STATS_ATOM_PKT_GAP_H1 = 0x7C,
    DECODER_STATS_ATOM_PKT_GAP_H2 = 0x80,
    DECODER_STATS_ATOM_PKT_GAP_H3 = 0x84,
    DECODER_STATS_ATOM_PKT_GAP_H4 = 0x88,
    /* Atom packet burst histogram: 1, 2-3, 4-15, 16+ */
    DECODER_STATS_ATOM_PKT_BST_H0 = 0x8C,
    DECODER_STATS_ATOM_PKT_BST_H1 = 0x90,
    DECODER_STATS_ATOM_PKT_BST_H2 = 0x94,
    DECODER_STATS_ATOM_PKT_BST_H3 = 0x98,
    /* Sum of atom elements over all valid atom packets: true atom element
     * throughput, vs. DECODER_STATS_ATOM_PKT_CNT which only counts packets
     * regardless of how many elements (atom_nb) each one bundles. */
    DECODER_STATS_ATOM_ELEM_SUM  = 0x9C,
    /* Atom element size histogram: 1, 2, 3, 4, 5, 6-11, 12-24.
     * 1-5 are exact values (ETMv4 formats F1-F5 always encode a fixed
     * atom_nb), 6-11/12-24 are ranges over the variable-length F6 format. */
    DECODER_STATS_ATOM_ELEM_H0   = 0xA0,
    DECODER_STATS_ATOM_ELEM_H1   = 0xA4,
    DECODER_STATS_ATOM_ELEM_H2   = 0xA8,
    DECODER_STATS_ATOM_ELEM_H3   = 0xAC,
    DECODER_STATS_ATOM_ELEM_H4   = 0xB0,
    DECODER_STATS_ATOM_ELEM_H5   = 0xB4,
    DECODER_STATS_ATOM_ELEM_H6   = 0xB8,
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

static inline void _decoder_hist(const char *title,
                              const char  *labels[],
                              uint32_t     values[],
                              int          n)
{
    uint32_t total = 0;
    uint32_t max   = 0;
    for (int i = 0; i < n; i++) {
        total += values[i];
        if (values[i] > max) max = values[i];
    }

    fprintf(stderr, "  %-20s  (total=%u)\n", title, total);
    for (int i = 0; i < n; i++) {
        float    pct  = total ? 100.0f * values[i] / total : 0.0f;
        int      bars = (max > 0) ? (int)(32.0f * (float)values[i] / (float)max) : 0;
        fprintf(stderr, "    %-10s [", labels[i]);
        for (int b = 0; b < 32; b++) fputc(b < bars ? '#' : ' ', stderr);
        fprintf(stderr, "] %8u (%5.1f%%)\n", values[i], pct);
    }
}

/* ------------------------------------------------------------------ */
/* Detailed print                                                       */
/* ------------------------------------------------------------------ */

static inline void decoder_stats_print_detailed(decoder_stats_t *h)
{
    uint32_t total       = axi_regs_read(h, DECODER_STATS_TOTAL);
    uint32_t idle        = axi_regs_read(h, DECODER_STATS_IDLE);
    uint32_t pkt_cnt     = axi_regs_read(h, DECODER_STATS_PACKET_CNT);
    uint32_t pkt_bst_cnt = axi_regs_read(h, DECODER_STATS_PACKET_BST_CNT);
    uint32_t pkt_min_bst = axi_regs_read(h, DECODER_STATS_PACKET_MIN_BST);
    uint32_t pkt_max_bst = axi_regs_read(h, DECODER_STATS_PACKET_MAX_BST);
    uint32_t pkt_sum_bst = axi_regs_read(h, DECODER_STATS_PACKET_SUM_BST);
    uint32_t pkt_gap_cnt = axi_regs_read(h, DECODER_STATS_PACKET_GAP_CNT);
    uint32_t pkt_min_gap = axi_regs_read(h, DECODER_STATS_PACKET_MIN_GAP);
    uint32_t pkt_max_gap = axi_regs_read(h, DECODER_STATS_PACKET_MAX_GAP);
    uint32_t pkt_sum_gap = axi_regs_read(h, DECODER_STATS_PACKET_SUM_GAP);
    uint32_t atom_pkt_cnt     = axi_regs_read(h, DECODER_STATS_ATOM_PKT_CNT);
    uint32_t atom_pkt_bst_cnt = axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_CNT);
    uint32_t atom_pkt_min_bst = axi_regs_read(h, DECODER_STATS_ATOM_PKT_MIN_BST);
    uint32_t atom_pkt_max_bst = axi_regs_read(h, DECODER_STATS_ATOM_PKT_MAX_BST);
    uint32_t atom_pkt_sum_bst = axi_regs_read(h, DECODER_STATS_ATOM_PKT_SUM_BST);
    uint32_t atom_pkt_gap_cnt = axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_CNT);
    uint32_t atom_pkt_min_gap = axi_regs_read(h, DECODER_STATS_ATOM_PKT_MIN_GAP);
    uint32_t atom_pkt_max_gap = axi_regs_read(h, DECODER_STATS_ATOM_PKT_MAX_GAP);
    uint32_t atom_pkt_sum_gap = axi_regs_read(h, DECODER_STATS_ATOM_PKT_SUM_GAP);
    uint32_t atom_elem_sum    = axi_regs_read(h, DECODER_STATS_ATOM_ELEM_SUM);

    uint32_t pkt_gap_hist[] = {
        axi_regs_read(h, DECODER_STATS_PKT_GAP_H0),  /* 0       */
        axi_regs_read(h, DECODER_STATS_PKT_GAP_H1),  /* 1-4     */
        axi_regs_read(h, DECODER_STATS_PKT_GAP_H2),  /* 5-15    */
        axi_regs_read(h, DECODER_STATS_PKT_GAP_H3),  /* 16-255  */
        axi_regs_read(h, DECODER_STATS_PKT_GAP_H4),  /* 256+    */
    };
    uint32_t pkt_bst_hist[] = {
        axi_regs_read(h, DECODER_STATS_PKT_BST_H0),  /* 1       */
        axi_regs_read(h, DECODER_STATS_PKT_BST_H1),  /* 2-3     */
        axi_regs_read(h, DECODER_STATS_PKT_BST_H2),  /* 4-15    */
        axi_regs_read(h, DECODER_STATS_PKT_BST_H3),  /* 16+     */
    };
    uint32_t atom_pkt_gap_hist[] = {
        axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H0),  /* 0       */
        axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H1),  /* 1-4     */
        axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H2),  /* 5-15    */
        axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H3),  /* 16-255  */
        axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H4),  /* 256+    */
    };
    uint32_t atom_pkt_bst_hist[] = {
        axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_H0),  /* 1       */
        axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_H1),  /* 2-3     */
        axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_H2),  /* 4-15    */
        axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_H3),  /* 16+     */
    };
    uint32_t atom_elem_hist[] = {
        axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H0),   /* 1       */
        axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H1),   /* 2       */
        axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H2),   /* 3       */
        axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H3),   /* 4       */
        axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H4),   /* 5       */
        axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H5),   /* 6-11    */
        axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H6),   /* 12-24   */
    };

    const char *gap_labels[]  = { "0cy", "1-4cy", "5-15cy", "16-255cy", "256+cy" };
    const char *bst_labels[]  = { "1",   "2-3",   "4-15",   "16+"               };
    const char *elem_labels[] = { "1", "2", "3", "4", "5", "6-11", "12-24"      };

    uint32_t active = (total >= idle) ? total - idle : 0;

    fprintf(stderr, "\n=== Decoder Stats ===\n");
    fprintf(stderr, "  Total cycles  : %u\n", total);
    fprintf(stderr, "  Active cycles : %u (%.1f%%)\n",
            active, total ? 100.0f * (float)active / (float)total : 0.0f);
    fprintf(stderr, "  Idle cycles   : %u (%.1f%%)\n",
            idle,   total ? 100.0f * (float)idle   / (float)total : 0.0f);

    fprintf(stderr, "\n--- Packets ---\n");
    fprintf(stderr, "  Count         : %u\n", pkt_cnt);
    fprintf(stderr, "  Bursts        : %u  avg=%.2f  min=%u  max=%u\n",
            pkt_bst_cnt,
            pkt_bst_cnt ? (float)pkt_sum_bst / (float)pkt_bst_cnt : 0.0f,
            pkt_min_bst, pkt_max_bst);
    fprintf(stderr, "  Gaps          : %u  avg=%.2f  min=%u  max=%u\n",
            pkt_gap_cnt,
            pkt_gap_cnt ? (float)pkt_sum_gap / (float)pkt_gap_cnt : 0.0f,
            pkt_min_gap, pkt_max_gap);
    fprintf(stderr, "  Rate          : %.4f pkts/cycle\n",
            total ? (float)pkt_cnt / (float)total : 0.0f);

    _decoder_hist("Packet gap dist.",   gap_labels, pkt_gap_hist, 5);
    _decoder_hist("Packet burst dist.", bst_labels, pkt_bst_hist, 4);

    fprintf(stderr, "\n--- Atom packets ---\n");
    fprintf(stderr, "  Count         : %u\n", atom_pkt_cnt);
    fprintf(stderr, "  Bursts        : %u  avg=%.2f  min=%u  max=%u\n",
            atom_pkt_bst_cnt,
            atom_pkt_bst_cnt ? (float)atom_pkt_sum_bst / (float)atom_pkt_bst_cnt : 0.0f,
            atom_pkt_min_bst, atom_pkt_max_bst);
    fprintf(stderr, "  Gaps          : %u  avg=%.2f  min=%u  max=%u\n",
            atom_pkt_gap_cnt,
            atom_pkt_gap_cnt ? (float)atom_pkt_sum_gap / (float)atom_pkt_gap_cnt : 0.0f,
            atom_pkt_min_gap, atom_pkt_max_gap);
    fprintf(stderr, "  Rate          : %.4f atom packets/cycle\n",
            total ? (float)atom_pkt_cnt / (float)total : 0.0f);
    fprintf(stderr, "  Writer load   : %.1f%% (drain rate = 1/3 cy)\n",
            total ? 100.0f * ((float)atom_pkt_cnt / (float)total) * 3.0f : 0.0f);

    _decoder_hist("Atom packet gap dist.",   gap_labels, atom_pkt_gap_hist, 5);
    _decoder_hist("Atom packet burst dist.", bst_labels, atom_pkt_bst_hist, 4);

    fprintf(stderr, "\n--- Atom elements ---\n");
    fprintf(stderr, "  Count         : %u (avg %.2f elements/atom packet)\n",
            atom_elem_sum, atom_pkt_cnt ? (float)atom_elem_sum / (float)atom_pkt_cnt : 0.0f);
    fprintf(stderr, "  Note          : atom packet count above only counts packets;\n"
                    "                  this is the real element throughput.\n");

    _decoder_hist("Atom element dist.", elem_labels, atom_elem_hist, 7);
}

/* ------------------------------------------------------------------ */
/* CSV export                                                          */
/* ------------------------------------------------------------------ */

/* Column header for decoder_stats_write_csv_row(), so a caller building a
 * combined CSV (see cs-trace.c) can emit a matching header once. */
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

/* Appends one CSV row (no trailing newline) with every raw counter read
 * from this block -- the same values decoder_stats_print_detailed() uses,
 * just unformatted so a caller can log a run per line across binaries. */
static inline void decoder_stats_write_csv_row(decoder_stats_t *h, FILE *f)
{
    fprintf(f, "%u,%u,",
            axi_regs_read(h, DECODER_STATS_TOTAL),
            axi_regs_read(h, DECODER_STATS_IDLE));
    fprintf(f, "%u,%u,%u,%u,%u,",
            axi_regs_read(h, DECODER_STATS_PACKET_CNT),
            axi_regs_read(h, DECODER_STATS_PACKET_BST_CNT),
            axi_regs_read(h, DECODER_STATS_PACKET_MIN_BST),
            axi_regs_read(h, DECODER_STATS_PACKET_MAX_BST),
            axi_regs_read(h, DECODER_STATS_PACKET_SUM_BST));
    fprintf(f, "%u,%u,%u,%u,",
            axi_regs_read(h, DECODER_STATS_PACKET_GAP_CNT),
            axi_regs_read(h, DECODER_STATS_PACKET_MIN_GAP),
            axi_regs_read(h, DECODER_STATS_PACKET_MAX_GAP),
            axi_regs_read(h, DECODER_STATS_PACKET_SUM_GAP));
    fprintf(f, "%u,%u,%u,%u,%u,",
            axi_regs_read(h, DECODER_STATS_PKT_GAP_H0),
            axi_regs_read(h, DECODER_STATS_PKT_GAP_H1),
            axi_regs_read(h, DECODER_STATS_PKT_GAP_H2),
            axi_regs_read(h, DECODER_STATS_PKT_GAP_H3),
            axi_regs_read(h, DECODER_STATS_PKT_GAP_H4));
    fprintf(f, "%u,%u,%u,%u,",
            axi_regs_read(h, DECODER_STATS_PKT_BST_H0),
            axi_regs_read(h, DECODER_STATS_PKT_BST_H1),
            axi_regs_read(h, DECODER_STATS_PKT_BST_H2),
            axi_regs_read(h, DECODER_STATS_PKT_BST_H3));
    fprintf(f, "%u,%u,%u,%u,%u,",
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_CNT),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_CNT),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_MIN_BST),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_MAX_BST),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_SUM_BST));
    fprintf(f, "%u,%u,%u,%u,",
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_CNT),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_MIN_GAP),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_MAX_GAP),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_SUM_GAP));
    fprintf(f, "%u,%u,%u,%u,%u,",
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H0),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H1),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H2),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H3),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_GAP_H4));
    fprintf(f, "%u,%u,%u,%u,",
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_H0),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_H1),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_H2),
            axi_regs_read(h, DECODER_STATS_ATOM_PKT_BST_H3));
    fprintf(f, "%u,", axi_regs_read(h, DECODER_STATS_ATOM_ELEM_SUM));
    fprintf(f, "%u,%u,%u,%u,%u,%u,%u",
            axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H0),
            axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H1),
            axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H2),
            axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H3),
            axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H4),
            axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H5),
            axi_regs_read(h, DECODER_STATS_ATOM_ELEM_H6));
}

#endif
