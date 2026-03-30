/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_K22INFO_H
#define _UAPI_LINUX_K22INFO_H

#include <linux/types.h>

struct k22info {
	char comm[64];                  /* name of the executable */
	pid_t pid;                      /* process ID */
	pid_t parent_pid;               /* parent process ID */
	pid_t first_child_pid;          /* PID of first child */
	pid_t next_sibling_pid;         /* PID of next sibling */
	unsigned long nvcsw;            /* number of voluntary context switches */
	unsigned long nivcsw;           /* number of involuntary context switches */
	unsigned long start_time;       /* monotonic start time in nanoseconds */
};
#endif /* _UAPI_LINUX_K22INFO_H */
