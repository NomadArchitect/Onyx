/*----------------------------------------------------------------------
 * Copyright (C) 2016 Pedro Falcato
 *
 * This file is part of Spartix, and is made available under
 * the terms of the GNU General Public License version 2.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *----------------------------------------------------------------------*/
#include <kernel/spinlock.h>
#include <stdio.h>
#include <kernel/compiler.h>
void acquire(spinlock_t * lock)
{
	if (lock->lock == 1) {
		wait(lock);
	}
	__sync_lock_test_and_set(&lock->lock, 1);
}

void release(spinlock_t * lock)
{
	__sync_lock_release(&lock->lock);
}

void wait(spinlock_t * lock)
{
	while (lock->lock == 1);
}