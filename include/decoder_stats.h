#ifndef DECODER_STATS_H
#define DECODER_STATS_H

#include "axi_regs.h"
#include <stdio.h>
#include <stdint.h>

#define DEC_STATS_MAP_SIZE 0x1000

// Default base addresses (can/should be overridden)
#ifndef DEC_STATS_BASE_ETM
#pragma message("WARNING: EDGE_STATS_BASE not defined, using placeholder 0x80000000 - override for your Vivado project")
#define DEC_STATS_BASE_ETM 0x80020000
#endif

/* Register offsets */
typedef enum {
    DEC_STATS_CTRL           = 0x00,
    DEC_STATS_TOTAL          = 0x04,
    DEC_STATS_IDLE           = 0x08,
    DEC_STATS_PACKET_CNT     = 0x0C,
    DEC_STATS_PACKET_BST_CNT = 0x10,
    DEC_STATS_PACKET_MIN_BST = 0x14,
    DEC_STATS_PACKET_MAX_BST = 0x18,
    DEC_STATS_PACKET_SUM_BST = 0x1C,
    DEC_STATS_PACKET_GAP_CNT = 0x20,
    DEC_STATS_PACKET_MIN_GAP = 0x24,
    DEC_STATS_PACKET_MAX_GAP = 0x28,
    DEC_STATS_PACKET_SUM_GAP = 0x2C,
    DEC_STATS_ATOM_CNT       = 0x30,
    DEC_STATS_ATOM_BST_CNT   = 0x34,
    DEC_STATS_ATOM_MIN_BST   = 0x38,
    DEC_STATS_ATOM_MAX_BST   = 0x3C,
    DEC_STATS_ATOM_SUM_BST   = 0x40,
    DEC_STATS_ATOM_GAP_CNT   = 0x44,
    DEC_STATS_ATOM_MIN_GAP   = 0x48,
    DEC_STATS_ATOM_MAX_GAP   = 0x4C,
    DEC_STATS_ATOM_SUM_GAP   = 0x50,
    /* Packet gap histogram: 0, 1-4, 5-15, 16-255, 256+ */
    DEC_STATS_PKT_GAP_H0     = 0x54,
    DEC_STATS_PKT_GAP_H1     = 0x58,
    DEC_STATS_PKT_GAP_H2     = 0x5C,
    DEC_STATS_PKT_GAP_H3     = 0x60,
    DEC_STATS_PKT_GAP_H4     = 0x64,
    /* Packet burst histogram: 1, 2-3, 4-15, 16+ */
    DEC_STATS_PKT_BST_H0     = 0x68,
    DEC_STATS_PKT_BST_H1     = 0x6C,
    DEC_STATS_PKT_BST_H2     = 0x70,
    DEC_STATS_PKT_BST_H3     = 0x74,
    /* Atom gap histogram: 0, 1-4, 5-15, 16-255, 256+ */
    DEC_STATS_ATM_GAP_H0     = 0x78,
    DEC_STATS_ATM_GAP_H1     = 0x7C,
    DEC_STATS_ATM_GAP_H2     = 0x80,
    DEC_STATS_ATM_GAP_H3     = 0x84,
    DEC_STATS_ATM_GAP_H4     = 0x88,
    /* Atom burst histogram: 1, 2-3, 4-15, 16+ */
    DEC_STATS_ATM_BST_H0     = 0x8C,
    DEC_STATS_ATM_BST_H1     = 0x90,
    DEC_STATS_ATM_BST_H2     = 0x94,
    DEC_STATS_ATM_BST_H3     = 0x98,
} dec_stats_reg_t;

typedef axi_regs_t dec_stats_t;

/* Function mapping to axi_regs defaults */
static inline int  dec_stats_open(dec_stats_t *handle)
    { return axi_regs_open(handle, DEC_STATS_BASE_ETM, DEC_STATS_MAP_SIZE); }
static inline void dec_stats_close(dec_stats_t *handle)
    { axi_regs_close(handle); }
static inline void dec_stats_enable(dec_stats_t *handle)
    { axi_regs_write(handle, DEC_STATS_CTRL, 3); }
static inline void dec_stats_disable(dec_stats_t *handle)
    { axi_regs_write(handle, DEC_STATS_CTRL, 0); }
static inline uint32_t dec_stats_read(dec_stats_t *handle, dec_stats_reg_t reg)
    { return axi_regs_read(handle, reg); }

/* Description used in the print */
static axi_reg_desc_t dec_stats_descs[] = {
    {"Total cycles",            DEC_STATS_TOTAL},
    {"Idle cycles",             DEC_STATS_IDLE},
    {"Packet count",            DEC_STATS_PACKET_CNT},
    {"Packet burst count",      DEC_STATS_PACKET_BST_CNT},
    {"Min packet burst length", DEC_STATS_PACKET_MIN_BST},
    {"Max packet burst length", DEC_STATS_PACKET_MAX_BST},
    {"Sum packet burst length", DEC_STATS_PACKET_SUM_BST},
    {"Packet gap count",        DEC_STATS_PACKET_GAP_CNT},
    {"Min packet gap length",   DEC_STATS_PACKET_MIN_GAP},
    {"Max packet gap length",   DEC_STATS_PACKET_MAX_GAP},
    {"Sum packet gap length",   DEC_STATS_PACKET_SUM_GAP},
    {"Atom count",              DEC_STATS_ATOM_CNT},
    {"Atom burst count",        DEC_STATS_ATOM_BST_CNT},
    {"Min atom burst length",   DEC_STATS_ATOM_MIN_BST},
    {"Max atom burst length",   DEC_STATS_ATOM_MAX_BST},
    {"Sum atom burst length",   DEC_STATS_ATOM_SUM_BST},
    {"Atom gap count",          DEC_STATS_ATOM_GAP_CNT},
    {"Min atom gap length",     DEC_STATS_ATOM_MIN_GAP},
    {"Max atom gap length",     DEC_STATS_ATOM_MAX_GAP},
    {"Sum atom gap length",     DEC_STATS_ATOM_SUM_GAP},
};

static inline void dec_stats_print(dec_stats_t *h)
    { axi_regs_print(h, dec_stats_descs,
                     sizeof(dec_stats_descs)/sizeof(dec_stats_descs[0])); }

/* ------------------------------------------------------------------ */
/* Histogram helper                                                   */
/* ------------------------------------------------------------------ */

static inline void _dec_hist(const char *title,
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

static inline void dec_stats_print_detailed(dec_stats_t *h)
{
    uint32_t total       = axi_regs_read(h, DEC_STATS_TOTAL);
    uint32_t idle        = axi_regs_read(h, DEC_STATS_IDLE);
    uint32_t pkt_cnt     = axi_regs_read(h, DEC_STATS_PACKET_CNT);
    uint32_t pkt_bst_cnt = axi_regs_read(h, DEC_STATS_PACKET_BST_CNT);
    uint32_t pkt_min_bst = axi_regs_read(h, DEC_STATS_PACKET_MIN_BST);
    uint32_t pkt_max_bst = axi_regs_read(h, DEC_STATS_PACKET_MAX_BST);
    uint32_t pkt_sum_bst = axi_regs_read(h, DEC_STATS_PACKET_SUM_BST);
    uint32_t pkt_gap_cnt = axi_regs_read(h, DEC_STATS_PACKET_GAP_CNT);
    uint32_t pkt_min_gap = axi_regs_read(h, DEC_STATS_PACKET_MIN_GAP);
    uint32_t pkt_max_gap = axi_regs_read(h, DEC_STATS_PACKET_MAX_GAP);
    uint32_t pkt_sum_gap = axi_regs_read(h, DEC_STATS_PACKET_SUM_GAP);
    uint32_t atm_cnt     = axi_regs_read(h, DEC_STATS_ATOM_CNT);
    uint32_t atm_bst_cnt = axi_regs_read(h, DEC_STATS_ATOM_BST_CNT);
    uint32_t atm_min_bst = axi_regs_read(h, DEC_STATS_ATOM_MIN_BST);
    uint32_t atm_max_bst = axi_regs_read(h, DEC_STATS_ATOM_MAX_BST);
    uint32_t atm_sum_bst = axi_regs_read(h, DEC_STATS_ATOM_SUM_BST);
    uint32_t atm_gap_cnt = axi_regs_read(h, DEC_STATS_ATOM_GAP_CNT);
    uint32_t atm_min_gap = axi_regs_read(h, DEC_STATS_ATOM_MIN_GAP);
    uint32_t atm_max_gap = axi_regs_read(h, DEC_STATS_ATOM_MAX_GAP);
    uint32_t atm_sum_gap = axi_regs_read(h, DEC_STATS_ATOM_SUM_GAP);

    uint32_t pkt_gap_hist[] = {
        axi_regs_read(h, DEC_STATS_PKT_GAP_H0),  /* 0       */
        axi_regs_read(h, DEC_STATS_PKT_GAP_H1),  /* 1-4     */
        axi_regs_read(h, DEC_STATS_PKT_GAP_H2),  /* 5-15    */
        axi_regs_read(h, DEC_STATS_PKT_GAP_H3),  /* 16-255  */
        axi_regs_read(h, DEC_STATS_PKT_GAP_H4),  /* 256+    */
    };
    uint32_t pkt_bst_hist[] = {
        axi_regs_read(h, DEC_STATS_PKT_BST_H0),  /* 1       */
        axi_regs_read(h, DEC_STATS_PKT_BST_H1),  /* 2-3     */
        axi_regs_read(h, DEC_STATS_PKT_BST_H2),  /* 4-15    */
        axi_regs_read(h, DEC_STATS_PKT_BST_H3),  /* 16+     */
    };
    uint32_t atm_gap_hist[] = {
        axi_regs_read(h, DEC_STATS_ATM_GAP_H0),  /* 0       */
        axi_regs_read(h, DEC_STATS_ATM_GAP_H1),  /* 1-4     */
        axi_regs_read(h, DEC_STATS_ATM_GAP_H2),  /* 5-15    */
        axi_regs_read(h, DEC_STATS_ATM_GAP_H3),  /* 16-255  */
        axi_regs_read(h, DEC_STATS_ATM_GAP_H4),  /* 256+    */
    };
    uint32_t atm_bst_hist[] = {
        axi_regs_read(h, DEC_STATS_ATM_BST_H0),  /* 1       */
        axi_regs_read(h, DEC_STATS_ATM_BST_H1),  /* 2-3     */
        axi_regs_read(h, DEC_STATS_ATM_BST_H2),  /* 4-15    */
        axi_regs_read(h, DEC_STATS_ATM_BST_H3),  /* 16+     */
    };

    const char *gap_labels[] = { "0cy", "1-4cy", "5-15cy", "16-255cy", "256+cy" };
    const char *bst_labels[] = { "1",   "2-3",   "4-15",   "16+"               };

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

    _dec_hist("Packet gap dist.",   gap_labels, pkt_gap_hist, 5);
    _dec_hist("Packet burst dist.", bst_labels, pkt_bst_hist, 4);

    fprintf(stderr, "\n--- Atoms ---\n");
    fprintf(stderr, "  Count         : %u\n", atm_cnt);
    fprintf(stderr, "  Bursts        : %u  avg=%.2f  min=%u  max=%u\n",
            atm_bst_cnt,
            atm_bst_cnt ? (float)atm_sum_bst / (float)atm_bst_cnt : 0.0f,
            atm_min_bst, atm_max_bst);
    fprintf(stderr, "  Gaps          : %u  avg=%.2f  min=%u  max=%u\n",
            atm_gap_cnt,
            atm_gap_cnt ? (float)atm_sum_gap / (float)atm_gap_cnt : 0.0f,
            atm_min_gap, atm_max_gap);
    fprintf(stderr, "  Rate          : %.4f atoms/cycle\n",
            total ? (float)atm_cnt / (float)total : 0.0f);
    fprintf(stderr, "  Writer load   : %.1f%% (drain rate = 1/3 cy)\n",
            total ? 100.0f * ((float)atm_cnt / (float)total) * 3.0f : 0.0f);

    _dec_hist("Atom gap dist.",   gap_labels, atm_gap_hist, 5);
    _dec_hist("Atom burst dist.", bst_labels, atm_bst_hist, 4);
}

#endif