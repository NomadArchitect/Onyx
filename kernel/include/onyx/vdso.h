/*
* Copyright (c) 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#ifndef _KERNEL_VDSO_H
#define _KERNEL_VDSO_H

#include <stdbool.h>
#include <time.h>

#include <sys/time.h>

#include <onyx/clock.h>
#include <fixed_point/fixed_point.h>

struct vdso_time
{
	struct fp_32_64 ticks_per_ns;
#ifdef __x86_64__
	bool using_tsc;
#endif
};

struct vdso_clock_time
{
	time_t epoch;
	hrtime_t tick;
};

#ifdef __cplusplus
extern "C" {
#endif

void vdso_init(void);
void *vdso_map(void);
int vdso_update_time(clockid_t id, struct clock_time *time);

#ifdef __cplusplus
}
#endif

#endif
