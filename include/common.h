/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2021 Ricerca Security, Inc. All rights reserved. */

/* Modifications Copyright 2025 Inria, CNRS, IRISA, CentraleSupelec */
/*
 * Changes made by Quentin Ducasse on 2025-04-23:
 * - Removed decoder related code
 */

#ifndef CS_TRACE_COMMON_H
#define CS_TRACE_COMMON_H

#include <stdbool.h>
#include <sys/types.h>

int get_trace_id(int cpu);
int fetch_trace(void);
int decode_trace(void);
int init_trace(pid_t parent_pid, pid_t pid);
void fini_trace(void);
int start_trace(pid_t pid, bool use_pid_trace);
int stop_trace(bool disable_all);
void trace_suspend_callback(void);
void trace_resume_callback(void);
int export_trace_with_config(int n);

#endif /* CS_TRACE_COMMON_H */
