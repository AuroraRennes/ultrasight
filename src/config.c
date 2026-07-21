/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

/* Modifications Copyright 2025 Inria, CNRS, IRISA, CentraleSupelec */
/*
 * Changes made by Quentin Ducasse on 2025-04-23:
 * - Removed decoder related code
 * - Added comments for clarity
 */

#include "config.h"

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>

#include "csregistration.h"
#include "csregisters.h"

#define SHOW_ETM_CONFIG 0

const bool return_stack = false;
const bool branch_broadcast = true;
const bool cycle_count = false;

extern unsigned long etr_ram_addr;
extern size_t etr_ram_size;
extern int registration_verbose;
extern bool use_etr;

/**
 * Set the ETB to manual flush and wait for the end of the trace.
 */
void cs_etb_flush_and_wait_stop(struct cs_devices_t *devices)
{
  unsigned int ffcr_val, status_val;
  if (cs_sink_is_enabled(devices->etb)) {
    /* Read the value of the Flush and Format Control Register. */
    ffcr_val = cs_device_read(devices->etb, CS_ETB_FLFMT_CTRL);
    /* Set the manual flush bit and write it back */
    ffcr_val |= CS_ETB_FLFMT_CTRL_FOnMan;
    cs_device_write(devices->etb, CS_ETB_FLFMT_CTRL, ffcr_val);
    /* Wait for the ETB to stop collecting data, i.e. waiting for the FtEmpty
       bit to be set in CS_ETB_STATUS */
    if (cs_device_wait(devices->etb, CS_ETB_STATUS, CS_ETB_STATUS_FtEmpty,
                       CS_REG_WAITBITS_ALL_1, 0, &status_val) != 0) {
      fprintf(stderr,
              "[!] ETB collection not stopped on flush on trigger. STS: 0x%08x\n",
              status_val);
    }
  } else {
    fprintf(stderr, "[!] ETB is not activated, flush cancelled.\n");
  }
}

/* Flush the TPIU and wait for the formatter to stop, ensuring no packet is lost. */
void cs_tpiu_flush_and_wait_stop(struct cs_devices_t *devices)
{
  unsigned int status_val;
  if (devices->tpiu != NULL) {
    cs_device_set(devices->tpiu, CS_TPIU_FLFMT_CTRL,
                  CS_TPIU_FLFMT_CTRL_StopFl | CS_TPIU_FLFMT_CTRL_FOnMan);
    if (cs_device_wait(devices->tpiu, CS_TPIU_FLFMT_STATUS,
                       CS_TPIU_FLFMT_STATUS_FtStopped,
                       CS_REG_WAITBITS_ALL_1, 0, &status_val) != 0) {
      fprintf(stderr,
              "[!] TPIU flush did not complete. STS: 0x%08x\n",
              status_val);
    }
  } else {
    fprintf(stderr, "[!] TPIU is not present, flush cancelled.\n");
  }
}

/**
 * Define the address ranges of the ETMv4 by configuring the address
 * comparators.
 */
static void set_etmv4_addr_range(struct map_info *range,
                                 struct _adrcmp *addr_comp,
                                 unsigned int acc_type_ex)
{
  /*  Excluded access type: all exception secure/not secure (0, 1, 2) and a
   * user-defined excluded access type. */
  const unsigned int acc_type =
      CS_ETMV4_ACATR_ExEL0_S | CS_ETMV4_ACATR_ExEL1_S | CS_ETMV4_ACATR_ExEL2_S |
      CS_ETMV4_ACATR_ExEL1_NS | CS_ETMV4_ACATR_ExEL2_NS | acc_type_ex;

  /* NULL check for both */
  if (!range || !addr_comp) {
    fprintf(stderr, "[!] No range or address comparator, ETMv4 address not set.");
    return;
  }

  /* Address comparator setup (0 for start address, 1 for end address):
    - acvr_l, address comparator value (low)
    - acvr_h,               -         (high)
    - acatr_l, address comparator type (low)  */
  addr_comp[0].acvr_l = range->start & 0xFFFFFFFF;
  addr_comp[0].acvr_h = (range->start >> 32) & 0xFFFFFFFF;
  addr_comp[0].acatr_l = acc_type;
  addr_comp[1].acvr_l = range->end & 0xFFFFFFFF;
  addr_comp[1].acvr_h = (range->end >> 32) & 0xFFFFFFFF;
  addr_comp[1].acatr_l = acc_type;
}

/**
 * Configure the ETMv4 registers
 */
static int configure_etmv4_addr_range_cid(cs_device_t etm,
                                          struct map_info *range,
                                          int range_count, unsigned long cid)
{
  cs_etmv4_config_t tconfig;
  int error_count;
  size_t cididx;
  size_t addridx;

  /* Initialize an ETM configuration structure */
  cs_etm_config_init_ex(etm, &tconfig);
  /* These flags a*/
  tconfig.flags = CS_ETMC_TRACE_ENABLE | CS_ETMC_CONFIG | CS_ETMC_EVENTSELECT;
  /* Read the configuration from the ETM device hardware. */
  cs_etm_config_get_ex(etm, &tconfig);

  /* Disable VMID (virtual context id) */
  if (tconfig.scv4->idr2.bits.vmidsize > 0) {
    /* VMID trace must be disabled to use context ID trace only. */
    tconfig.configr.bits.vmid = 0;
  }
  /* Setup CID (context id) in the static configuration */
  if (tconfig.scv4->idr2.bits.cidsize > 0 && cid > 0) {
    tconfig.configr.bits.cid = 1; /* Enable */
  } else {
    tconfig.configr.bits.cid = 0; /* Disable */
  }

  /** Configure specific features (if supported by the device):
   *    - return stack: enable a return stack of addresses for faster tracing
   *    - branch broadcast: add address packets before indirect jumps
   *    - cycle count: add cycle counting packets
   */
  if (return_stack) tconfig.configr.bits.rs = 1; /* set the return stack */
  if (branch_broadcast)
    tconfig.configr.bits.bb = 1;                 /* set the branch broadcast */
  if (cycle_count) tconfig.configr.bits.cci = 1; /* set the cycle count */

  /* If a context id is specified, set up the comparator */
  if (cid > 0) {
    cididx = 0;
    /* Low/high part of the comparator id */
    tconfig.cxid_comps[cididx].cidcvr_l = cid & 0xFFFFFFFF;
    tconfig.cxid_comps[cididx].cidcvr_h = (cid >> 32) & 0xFFFFFFFF;
    /* Enable control and setup a mask */
    tconfig.cidcctlr0 &= ~(1 << cididx);
    tconfig.cxid_comps_acc_mask |= (1 << cididx);
    /* Activate the comparator id in the main flags */
    tconfig.flags |= CS_ETMC_CXID_COMP;
  }

  /* Set up address range filtering */
  addridx = 0;
  /* Note: Assumes range[0] is the tracee itself. */
  /* Set the address range for the tracee program */
  set_etmv4_addr_range(&range[0], &tconfig.addr_comps[addridx],
                       cid > 0 ? (cididx << 4) | (0x1 << 2) : 0);
  /* Activates the address comparator mask */
  tconfig.addr_comps_acc_mask |= 0x3 << addridx;
  tconfig.viiectlr |= 1 << (addridx / 2);

  tconfig.flags |= CS_ETMC_ADDR_COMP;

  /* Mark the configuration ready to be written back into the above registers on
   * 'put' */
  cs_etm_config_put_ex(etm, &tconfig);

  /* If needed, show the resulting configuration */
  if (registration_verbose > 0) {
    show_etm_config(etm);
  }

  /* Check for errors */
  error_count = cs_error_count();
  if (error_count > 0) {
    fprintf(stderr, "%u errors reported when configuring ETM\n", error_count);
    return -1;
  }

  return 0;
}

/**
 * Printing the ETM config
 */
void show_etm_config(cs_device_t etm)
{
  /* ETMv4 config type */
  assert(cs_device_has_class(etm, CS_DEVCLASS_SOURCE));
  cs_etmv4_config_t *t4config;
  t4config = NULL;

  /* Check the ETM version, bail out if not v4 */
  if (CS_ETMVERSION_MAJOR(cs_etm_get_version(etm)) < CS_ETMVERSION_ETMv4)
    return;

  /* Initialize the ETMv4 config structure */
  cs_etm_config_init_ex(etm, t4config);
  /* Activate all flags */
  t4config->flags = CS_ETMC_ALL;
  /* Get and print the config */
  cs_etm_config_get_ex(etm, t4config);
  cs_etm_config_print_ex(etm, t4config);
}

/**
 * Initialize ETM for version 4
 */
int init_etm(cs_device_t dev)
{
  int rc;
  cs_etmv4_config_t v4config;
  /* Verify that the device is a source ETM */
  assert(cs_device_has_class(dev, CS_DEVCLASS_SOURCE));
  int etm_version = cs_etm_get_version(dev);

  /* Set to a 'clean' state - clears events & values, retains ctrl and ID,
   * ensuring it is programmable */
  if ((rc = cs_etm_clean(dev)) != 0) {
    fprintf(stderr, "Failed to set ETM/PTM into clean state\n");
    return rc;
  }

  /** Program up some basic trace control.
   *   Set up to trace all instructions.
   *   ETMv4 support only
   */
  assert(CS_ETMVERSION_IS_ETMV4(etm_version));

  /* ETMv4 initialisation */
  cs_etm_config_init_ex(dev, &v4config);
  v4config.flags = CS_ETMC_CONFIG;
  cs_etm_config_get_ex(dev, &v4config);

  /* Enable the trace with parameters */
  v4config.flags |= CS_ETMC_TRACE_ENABLE | CS_ETMC_EVENTSELECT;
  v4config.victlr   =
    CS_ETMV4_VICTLR_ExEL0_S | CS_ETMV4_VICTLR_ExEL1_S | CS_ETMV4_VICTLR_ExEL2_S | CS_ETMV4_VICTLR_ExEL3_S |
    CS_ETMV4_VICTLR_ExEL1_NS | CS_ETMV4_VICTLR_ExEL2_NS | CS_ETMV4_VICTLR_SSSTATUS | CS_ETMV4_VICTLR_ALWAYS ;
  v4config.viiectlr = 1;   /* no address range */
  v4config.vissctlr = 0;   /* no start stop points */

  /* Disable all event tracing  */
  v4config.eventctlr0r = 0;
  v4config.eventctlr1r = 0;

  /* Disable overflow & sync */
  v4config.stallcrlr = (1 << 13); /* no overflow */
  v4config.syncpr = 0;            /* no sync */
  cs_etm_config_put_ex(dev, &v4config);

  return 0;
}

/**
 * Configure the trace, setup the different elements.
 */
int configure_trace(const struct board *board, struct cs_devices_t *devices,
                    struct map_info *range, int range_count, pid_t pid)
{
  int i, r, error_count;

  if (!board || !devices) {
    fprintf(stderr, "[!] Board or devices empty\n");
    return -1;
  }

  /* Ensure TPIU isn't generating back-pressure */
  cs_disable_tpiu();

  if (use_etr) {
    /* While programming, ensure we are not collecting trace to the main buffer */
    cs_sink_disable(devices->etb);
  }
  /* Check all PTMs */
  for (i = 0; i < board->n_cpu; ++i) {
    /* Try to get the cpu, attribute an ID, and initialize the ETM */
    devices->ptm[i] = cs_cpu_get_device(i, CS_DEVCLASS_SOURCE);
    if (devices->ptm[i] == CS_ERRDESC) {
      fprintf(stderr, "[!] Failed to get trace source for CPU #%d\n", i);
      return -1;
    }
    if (cs_set_trace_source_id(devices->ptm[i], 0x10 + i) < 0) {
      fprintf(stderr, "[!] Failed to set valid trace source ID for CPU #%d\n",
              i);
      return -1;
    }
    if (init_etm(devices->ptm[i]) < 0) {
      fprintf(stderr, "[!] Failed to initialize ETM for CPU #%d\n", i);
      return -1;
    }
  }

  /* Set STM trace ID */
  if (cs_set_trace_source_id(devices->itm, 0x20) < 0) {
    fprintf(stderr, "[!] Failed to set valid trace source ID STM\n");
    return -1;
  }

  /* Permanently unlocks devices, starting from the top */
  cs_checkpoint();

  /* Check that all ETMs use version 4 */
  for (i = 0; i < board->n_cpu; ++i) {
    if (CS_ETMVERSION_MAJOR(cs_etm_get_version(devices->ptm[i])) >=
        CS_ETMVERSION_ETMv4) {
      r = configure_etmv4_addr_range_cid(devices->ptm[i], range, range_count,
                                         (unsigned long)pid);
    } else {
      fprintf(stderr, "[!] Unsupported ETM for CPU #%d\n", i);
      continue;
    }
    if (r != 0) {
      fprintf(stderr, "[!] Configuration failed for CPU #%d\n", i);
      return r;
    }
  }

  /* Setup stop on flush for the main buffer */
  if (use_etr) {
    unsigned int ffcr_val;
    ffcr_val = cs_device_read(devices->etb, CS_ETB_FLFMT_CTRL);
    ffcr_val |= CS_ETB_FLFMT_CTRL_StopFl;
    if (cs_device_write(devices->etb, CS_ETB_FLFMT_CTRL, ffcr_val) != 0) {
      fprintf(stderr, "[!] Failed to set stop on flush\n");
    }
  }


  /* Count and display configuration errors */
  error_count = cs_error_count();
  if (error_count > 0) {
    fprintf(stderr, "%u errors reported when configuring trace\n", error_count);
    return -1;
  }

  return 0;
}

/**
 * Reconfigure only the CID comparator on the traced CPU's ETM.
 * ETM must be disabled before calling (call disable_cs_trace first).
 * Called once per fuzzing iteration with the new child PID.
 */
int reconfigure_cid(const struct board *board, struct cs_devices_t *devices, pid_t pid)
{
    int error_count;
    size_t cididx = 0;

    for (int i = 0; i < board->n_cpu; i++) {
        cs_etmv4_config_t tconfig;
        cs_device_t etm = devices->ptm[i];

        cs_etm_config_init_ex(etm, &tconfig);
        tconfig.flags = CS_ETMC_CXID_COMP | CS_ETMC_CONFIG;
        cs_etm_config_get_ex(etm, &tconfig);

        /* Explicitly enable CID filtering — do not rely on the hardware
         * preserving configr.bits.cid across disable/enable cycles. */
        if (tconfig.scv4->idr2.bits.vmidsize > 0)
            tconfig.configr.bits.vmid = 0;
        if (tconfig.scv4->idr2.bits.cidsize > 0)
            tconfig.configr.bits.cid = 1;

        tconfig.cxid_comps[cididx].cidcvr_l = (unsigned long)pid & 0xFFFFFFFF;
        tconfig.cxid_comps[cididx].cidcvr_h = ((unsigned long)pid >> 32) & 0xFFFFFFFF;
        tconfig.cidcctlr0 &= ~(1 << cididx);
        tconfig.cxid_comps_acc_mask |= (1 << cididx);

        cs_etm_config_put_ex(etm, &tconfig);
    }

    error_count = cs_error_count();
    if (error_count > 0) {
        fprintf(stderr, "[!] %d errors reconfiguring CID\n", error_count);
        return -1;
    }
    return 0;
}

/**
 * Switch ETM branch broadcast mode (bb_mode).
 * bb_mode=1 -> branch broadcast enabled (address mode, like 0x4C000050)
 * bb_mode=0 -> atom mode (like 0x4C000040)
 * Must be called only when tracing is stopped.
 */
int set_etm_bb_mode(const struct board *board, struct cs_devices_t *devices, int bb_mode)
{
    int i, error_count;

    if (!board || !devices) return -1;

    for (i = 0; i < board->n_cpu; i++) {
        cs_etmv4_config_t tconfig;
        cs_device_t etm = devices->ptm[i];

        cs_etm_config_init_ex(etm, &tconfig);
        tconfig.flags = CS_ETMC_CONFIG;
        cs_etm_config_get_ex(etm, &tconfig);

        tconfig.configr.bits.bb = bb_mode ? 1 : 0;

        tconfig.flags = CS_ETMC_CONFIG;
        cs_etm_config_put_ex(etm, &tconfig);
    }

    error_count = cs_error_count();
    if (error_count > 0) {
        fprintf(stderr, "[!] %d errors setting bb_mode=%d\n", error_count, bb_mode);
        return -1;
    }

    return 0;
}


/**
 * Trace enable, setting up and enabling ETR, ETF
 */
int enable_trace(const struct board *board, struct cs_devices_t *devices)
{
  int i, error_count;

  /* Sanity check */
  if (!board || !devices) {
    return -1;
  }

  /* Setup and enable ETR as the main sink and trace buffer */
  if (use_etr) {
    if (cs_sink_etr_setup(devices->etb, etr_ram_addr, etr_ram_size,
                          board->etr_axictl) != 0) {
      fprintf(stderr, "[!] Failed to setup ETR\n");
      return -1;
    }
    if (cs_sink_enable(devices->etb) != 0) {
      fprintf(stderr, "[!] Failed to enable ETR\n");
      return -1;
    }
  }

  /* Setup TPIU to export the trace to the PL.
   * WARNING: This requires the TPIU registers to be powered (i.e. a psu_init that includes TPIU) */
  if(devices->tpiu != NULL) {
    if(cs_sink_enable(devices->tpiu)){
      fprintf(stderr, "[!] Failed to setup TPIU\n");
    return -1;
    }
  }

  /* Setup and enable ETFs as HW FIFO sinks in the system (there are two on the
   * ZCU104) */
  for (i = 0; i < devices->num_trace_sinks; i++) {
    if (cs_sink_etf_setup(devices->trace_sinks[i], CS_TMC_MODE_HWFIFO) != 0) {
      fprintf(stderr, "[!] Failed to setup ETF %d\n", i + 1);
      return -1;
    }
    /* FIXME: Redundancy? */
    // if (cs_sink_enable(devices->trace_sinks[i]) != 0) {
    //   fprintf(stderr, "Failed to enable ETF %d\n", i);
    //   return -1;
    // }
    if (cs_tmc_hw_fifo_enable(devices->trace_sinks[i], /*bufwm=*/0x0) != 0) {
      fprintf(stderr, "[!] Could not enable sinks as hw fifo %d/%d\n", i + 1,
              devices->num_trace_sinks);
      return -1;
    }
  }

  /* Enable sources, ETMs */
  for (i = 0; i < board->n_cpu; ++i) {
    cs_trace_enable(devices->ptm[i]);
  }

  /* Enable STM */
  if (cs_trace_swstim_enable_all_ports(devices->itm) < 0) {
    return -1;
  }
  if (cs_trace_swstim_set_sync_repeat(devices->itm, 32) < 0) {
    return -1;
  }
  cs_trace_enable(devices->itm);

  /* Permanently unlocks devices, starting from the top */
  cs_checkpoint();

  /* If needed, show the current cross-trigger configuration */
  if (registration_verbose > 0) {
    cs_cti_diag();
  }

  /* Check for errors */
  error_count = cs_error_count();
  if (error_count > 0) {
    fprintf(stderr, "%u errors reported when enabling trace\n", error_count);
    return -1;
  }

  return 0;
}

/**
 * Trace disable, flushing the main buffer, then disabling sources and sinks
 */
int disable_trace(const struct board *board, struct cs_devices_t *devices)
{
  int i, error_count;

  if (!board || !devices) {
    return -1;
  }

  if (use_etr) {
    /* Set FFCR:FlushMan bit to stop capture. */
    cs_etb_flush_and_wait_stop(devices);
  }

  cs_tpiu_flush_and_wait_stop(devices);
  /* TPIU already flushed and stopped above, no cs_sink_disable needed */

  /* Disable source ETMs */
  for (i = 0; i < board->n_cpu; ++i) {
    cs_trace_disable(devices->ptm[i]);
  }

  /* Disable STM */
  cs_trace_disable(devices->itm);

  /* Disable intermediate sinks (ETFs) */
  for (i = 0; i < devices->num_trace_sinks; i++) {
    if (devices->trace_sinks[i]) {
      cs_tmc_hw_fifo_disable(devices->trace_sinks[i]);
    }
  }

  if (use_etr) {
    /* Disable the main sink (ETR) */
    cs_sink_disable(devices->etb);
  }

  /* If needed, show the ETM config */
  if (registration_verbose > 1) {
    for (i = 0; i < board->n_cpu; ++i) {
      show_etm_config(devices->ptm[i]);
    }
  }

  /* Check for errors */
  error_count = cs_error_count();
  if (error_count > 0) {
    fprintf(stderr, "%u errors reported when disabling trace\n", error_count);
    return -1;
  }

  return 0;
}

/**
 * Enable all (and only them) sinks in the system (ETR + 2 ETFs)
 */
int enable_trace_sinks_only(const struct board *board, struct cs_devices_t *devices)
{
  int i, error_count;

  /* Sanity check */
  if (!devices) {
    return -1;
  }

  if (use_etr) {
    /* Setup and enable ETR as the main sink and trace buffer */
    if (cs_sink_etr_setup(devices->etb, etr_ram_addr, etr_ram_size,
                        board->etr_axictl) != 0) {
      fprintf(stderr, "[!] Failed to setup ETR\n");
      return -1;
    }
    if (cs_sink_enable(devices->etb) != 0) {
      fprintf(stderr, "[!] Failed to enable ETR\n");
      return -1;
    }
  }

  /* Setup TPIU to export the trace to the PL.
   * WARNING: This requires the TPIU registers to be powered (i.e. a psu_init that includes TPIU) */
  if (devices->tpiu != NULL) {
    if (cs_sink_enable(devices->tpiu) != 0) {
      fprintf(stderr, "[!] Failed to enable TPIU\n");
      return -1;
    }
  }

  /* Enable both ETFs, the main trace buffer */
  for (i = 0; i < devices->num_trace_sinks; i++) {
    if (cs_sink_etf_setup(devices->trace_sinks[i], CS_TMC_MODE_HWFIFO) != 0) {
      fprintf(stderr, "[!] Failed to setup ETF %d\n", i);
      return -1;
    }
    /* FIXME: HW FIFO? */
    // if (cs_sink_enable(devices->trace_sinks[0]) != 0) {
    //   fprintf(stderr, "Failed to enable ETF %d\n", i);
    //   return -1;
    // }
    if (cs_tmc_hw_fifo_enable(devices->trace_sinks[i], /*bufwm=*/0x0) != 0) {
      fprintf(stderr, "[!] Could not enable sinks as hw fifo %d/%d\n", i + 1,
              devices->num_trace_sinks);
      return -1;
    }
  }

  /* Permanently unlocks devices, starting from the top */
  cs_checkpoint();

  /* Check for errors */
  error_count = cs_error_count();
  if (error_count > 0) {
    fprintf(stderr, "%u errors reported when enabling trace\n", error_count);
    return -1;
  }

  return 0;
}

/**
 * Disable all (and only them) sinks in the system (ETR + 2 ETFs)
 */
int disable_trace_sinks_only(struct cs_devices_t *devices)
{
  int i, error_count;

  /* Sanity check */
  if (!devices) {
    return -1;
  }

  if (use_etr) {
    /* Set FFCR:FlushMan bit to stop capture. */
    cs_etb_flush_and_wait_stop(devices);
  }
  cs_tpiu_flush_and_wait_stop(devices);

  /* Disable intermediate sinks (ETFs) */
  // for (i = 0; i < devices->num_trace_sinks; i++) {
  //   if (devices->trace_sinks[i]) {
  //     cs_sink_disable(devices->trace_sinks[i]);
  //   }
  // }
  for (i = 0; i < devices->num_trace_sinks; i++) {
    if (devices->trace_sinks[i]) {
        cs_tmc_hw_fifo_disable(devices->trace_sinks[i]);
    }
  }

  if (use_etr) {
    /* Disable the main sink (ETR) */
    cs_sink_disable(devices->etb);
  }


  /* Check for errors */
  error_count = cs_error_count();
  if (error_count > 0) {
    fprintf(stderr, "%u errors reported when disabling trace\n", error_count);
    return -1;
  }

  return 0;
}
