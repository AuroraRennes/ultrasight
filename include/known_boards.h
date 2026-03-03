/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) ARM Limited, 2013-2016. All rights reserved. */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

/* Modifications Copyright 2025 Inria, CNRS, IRISA, CentraleSupelec */
/*
 * Changes made by Volker Stolz, Guillaume Hiet & Quentin Ducasse on 2025-04-23:
 * - Removed setup of other boards
 * - Added ZCU-104 setup
 */

#ifndef CS_TRACE_KNOWN_BOARDS_H
#define CS_TRACE_KNOWN_BOARDS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csregistration.h"

static int do_registration_zcu104(struct cs_devices_t *devices)
{
  enum { A53_0, A53_1, A53_2, A53_3 };

  int i;
  cs_device_t funnel1, funnel2, etf1, etf2, rep, etr, tpiu, cti0, cti1, stm,
      tsgen, ftm;

  cs_exclude_range(0xFE9E0000, 0xFEC00000); /* Exclude Cortex-R5 components */
  cs_register_romtable(0xFE800000);         /* ROM table registration */

  /* CTI affinities */
  cs_device_set_affinity(cs_device_register(0xFEC20000), A53_0);
  cs_device_set_affinity(cs_device_register(0xFED20000), A53_1);
  cs_device_set_affinity(cs_device_register(0xFEE20000), A53_2);
  cs_device_set_affinity(cs_device_register(0xFEF20000), A53_3);

  /* PMU affinities */
  cs_device_set_affinity(cs_device_register(0xFEC30000), A53_0);
  cs_device_set_affinity(cs_device_register(0xFED30000), A53_1);
  cs_device_set_affinity(cs_device_register(0xFEE30000), A53_2);
  cs_device_set_affinity(cs_device_register(0xFEF30000), A53_3);

  /* ETM affinities */
  cs_device_set_affinity(cs_device_register(0xFEC40000), A53_0);
  cs_device_set_affinity(cs_device_register(0xFED40000), A53_1);
  cs_device_set_affinity(cs_device_register(0xFEE40000), A53_2);
  cs_device_set_affinity(cs_device_register(0xFEF40000), A53_3);

  /* STM configuration */
  stm = cs_device_get(0xFE9C0000);
  devices->itm = stm;
  cs_stm_config_master(stm, 0, 0xF8000000);  // to 0xF8FFFFFF, or 16 MB
  cs_stm_select_master(stm, 0);

  /* FTM configuration */
  ftm = cs_device_get(0xFE9D0000);

  /* Timestamp Generator */
  tsgen = cs_device_get(0xFE900000);

  /* All ETMs feed into funnel1, funnel0 is used by the Cortex-R5 and unused in
   * our case */
  funnel1 = cs_device_get(0xFE920000);
  cs_atb_register(cs_cpu_get_device(A53_0, CS_DEVCLASS_SOURCE), 0, funnel1, 0);
  cs_atb_register(cs_cpu_get_device(A53_1, CS_DEVCLASS_SOURCE), 0, funnel1, 1);
  cs_atb_register(cs_cpu_get_device(A53_2, CS_DEVCLASS_SOURCE), 0, funnel1, 2);
  cs_atb_register(cs_cpu_get_device(A53_3, CS_DEVCLASS_SOURCE), 0, funnel1, 3);

  /* Funnel 1 connects into ETF1 */
  etf1 = cs_device_get(0xFE940000);
  cs_atb_register(funnel1, 0, etf1, 0);

  /* STM feeds into funnel2 on port 0, ETF1 on port 2 and funnel0 on port 3
   * NOTE: This port are not mentionned (or could not find a reference) in the
   * TRM */
  funnel2 = cs_device_get(0xFE930000);
  cs_atb_register(stm, 0, funnel2, 0);
  cs_atb_register(etf1, 0, funnel2, 2);
  /* R5 input would be here! */

  /* Funnel 2 connects into ETF2 */
  etf2 = cs_device_get(0xFE950000);
  cs_atb_register(funnel2, 0, etf2, 0);

  /* ETF2 feeds into the replicator outputting to TPIU or ETR */
  rep = cs_atb_add_replicator(2);
  /* rep = cs_device_get(0xFE960000); */

  cs_atb_register(etf2, 0, rep, 0);

  etr = cs_device_get(0xFE970000);
  cs_atb_register(rep, 0, etr, 0);

  tpiu = cs_device_get(0xFE980000);
  cs_atb_register(rep, 1, tpiu, 0);

  /* Sink configuration */
  devices->etb = etr; /* Main buffer (i.e. where the trace is stored in the end) */
  devices->tpiu = tpiu; /* Sent to PL through EMIO */
  devices->trace_sinks[0] = etf1; /* FIFO as sinks */
  devices->trace_sinks[1] = etf2; /* FIFO as sinks */
  devices->num_trace_sinks = 2;

  /* CTI SETUP, according to Table 39-8, p.1190 in US+ TRM */
  /* There are two main CTIs, 1 is for ETR/ETF/TPIU, 2 is for FTM/STM */
  /* There are 2 for R5 (1/core) and for for the A53 (1/core) */
  cti0 = cs_device_register(0xFE990000);
  cti1 = cs_device_register(0xFE9A0000);

  /* ETF */
  /* ins */
  cs_cti_connect_trigsrc(etf1, CS_TRIGOUT_ETB_FULL, cs_cti_trigsrc(cti0, 0));
  cs_cti_connect_trigsrc(etf1, CS_TRIGOUT_ETB_ACQCOMP, cs_cti_trigsrc(cti0, 1));
  cs_cti_connect_trigsrc(etf2, CS_TRIGOUT_ETB_FULL, cs_cti_trigsrc(cti0, 2));
  cs_cti_connect_trigsrc(etf2, CS_TRIGOUT_ETB_ACQCOMP, cs_cti_trigsrc(cti0, 3));
  /* outs */
  cs_cti_connect_trigdst(cs_cti_trigdst(cti0, 0), etf1, CS_TRIGIN_ETB_FLUSHIN);
  cs_cti_connect_trigdst(cs_cti_trigdst(cti0, 1), etf1, CS_TRIGIN_ETB_TRIGIN);
  cs_cti_connect_trigdst(cs_cti_trigdst(cti0, 2), etf2, CS_TRIGIN_ETB_FLUSHIN);
  cs_cti_connect_trigdst(cs_cti_trigdst(cti0, 3), etf2, CS_TRIGIN_ETB_TRIGIN);

  /* ETR */
  /* ins */
  cs_cti_connect_trigsrc(etr, CS_TRIGOUT_ETB_FULL, cs_cti_trigsrc(cti0, 4));
  cs_cti_connect_trigsrc(etr, CS_TRIGOUT_ETB_ACQCOMP, cs_cti_trigsrc(cti0, 5));
  /* outs */
  cs_cti_connect_trigdst(cs_cti_trigdst(cti0, 4), etr, CS_TRIGIN_ETB_FLUSHIN);
  cs_cti_connect_trigdst(cs_cti_trigdst(cti0, 5), etr, CS_TRIGIN_ETB_TRIGIN);

  /* TPIU */
  /* outs */
  cs_cti_connect_trigdst(cs_cti_trigdst(cti0, 6), tpiu, CS_TRIGIN_ETB_FLUSHIN);
  cs_cti_connect_trigdst(cs_cti_trigdst(cti0, 7), tpiu, CS_TRIGIN_ETB_TRIGIN);

  /* STM */
  /* ins */
  cs_cti_connect_trigsrc(stm, CS_TRIGOUT_STM_TRIGOUTSPTE,
                         cs_cti_trigsrc(cti1, 4));
  cs_cti_connect_trigsrc(stm, CS_TRIGOUT_STM_TRIGOUTSW,
                         cs_cti_trigsrc(cti1, 5));
  cs_cti_connect_trigsrc(stm, CS_TRIGOUT_STM_TRIGOUTHETE,
                         cs_cti_trigsrc(cti1, 6));
  cs_cti_connect_trigsrc(stm, CS_TRIGOUT_STM_ASYNCOUT, cs_cti_trigsrc(cti1, 7));
  /* outs */
#if 0
  // Hardware events if needed.
  cs_cti_connect_trigdst(
    cs_cti_trigdst(cti1, 4),
    stm, CS_TRIGIN_STM_HWEVENT_0
  );
  cs_cti_connect_trigdst(
    cs_cti_trigdst(cti1, 5),
    stm, CS_TRIGIN_STM_HWEVENT_1
  );
#endif

  devices->tsgen = tsgen;

  for (i = 0; i < 4; i++) {
    /* The 0xD03 represents the Cortex A53 */
    /* FIXME: Clarify the provenance of this value */
    /* devices->cpu_id[i] = cpu_id[i]; */
    devices->cpu_id[i] = 0xD03;
  }

  return 0;
}


static int do_registration_zynq7000(struct cs_devices_t *devices)
{
  /* Taken from UG585, Table 28-2 and 28-5 */
  enum { A9_0, A9_1 };

  cs_device_t funnel, tpiu, etb, ptm0, ptm1, rep;

  cs_register_romtable(0xF8880000); /* Cortex-A9 ROM table registration */

  /* CTI Affinities */
  cs_device_set_affinity(cs_device_register(0xF8898000), A9_0);
  cs_device_set_affinity(cs_device_register(0xF8899000), A9_1);

  /* PMU Affinities */
  cs_device_set_affinity(cs_device_register(0xF8891000), A9_0);
  cs_device_set_affinity(cs_device_register(0xF8893000), A9_1);

  /* ETM Affinities */
  cs_device_set_affinity(cs_device_register(0xF889C000), A9_0);
  cs_device_set_affinity(cs_device_register(0xF889D000), A9_1);

  /* Funnel Configuration */
  funnel = cs_device_get(0xF8804000);
  cs_atb_register(cs_cpu_get_device(A9_0, CS_DEVCLASS_SOURCE), 0, funnel, 0);
  cs_atb_register(cs_cpu_get_device(A9_1, CS_DEVCLASS_SOURCE), 0, funnel, 1);

  /* Port 2 is for FTM, 3 for ITM (not supported for now), 4-7 are unused */
  /* cs_atb_register(ftm, 0, funnel, 0); */
  /* cs_atb_register(itm, 0, funnel, 1); */

  /* Replicator configuration, gets input from funnel and outputs to ETB and TPIU */
  rep = cs_atb_add_replicator(2);
  cs_atb_register(funnel, 0, rep, 0);

  /* No mention of port numbers for the replicator output, gathered from the decompiled device tree */
  tpiu = cs_device_get(0xF8803000);
  cs_atb_register(rep, 0, tpiu, 0);

  etb = cs_device_get(0xF8801000);
  cs_atb_register(rep, 1, etb, 0);

  /* Sink configuration */
  devices->etb = etb;
  devices->tpiu = tpiu;
  devices->num_trace_sinks = 0;

  return 0;
}

const struct board known_boards[] = {
    {
        .do_registration = do_registration_zcu104,
        .n_cpu = 4,
        .hardware = "ZCU-104",
    },
    {
        .do_registration = do_registration_zynq7000,
        .n_cpu = 2,
        .hardware = "ZYNQ-7000",
    },
    {}};

#endif /* CS_TRACE_KNOWN_BOARDS_H */