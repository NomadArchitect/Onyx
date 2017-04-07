/*----------------------------------------------------------------------
 * Copyright (C) 2017 Pedro Falcato
 *
 * This file is part of Onyx, and is made available under
 * the terms of the GNU General Public License version 2.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *----------------------------------------------------------------------*/
#ifndef _KERNEL_BINFMT_H
#define _KERNEL_BINFMT_H
#include <stdint.h>

#include <kernel/vfs.h>

struct binfmt_args;
typedef int (*binfmt_handler_t)(struct binfmt_args *);
struct binfmt
{
	uint8_t *signature;
	size_t size_signature;
	binfmt_handler_t callback;
	struct binfmt *next;
};
struct binfmt_args
{
	uint8_t *file_signature;
	char *filename;
	char **argv, **envp;
	vfsnode_t *file;
};

int load_binary(struct binfmt_args *);
int install_binfmt(struct binfmt *);
#endif