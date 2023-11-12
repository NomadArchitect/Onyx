/*
 * Copyright (c) 2023 Pedro Falcato
 * This file is part of Onyx, and is released under the terms of the MIT License
 * check LICENSE at the root directory for more information
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _ONYX_MM_SHMEM_H
#define _ONYX_MM_SHMEM_H

#include <stddef.h>

struct file;

/**
 * @brief Create a new shmem file
 *
 * @param len Length, in bytes
 * @return Opened struct file, or NULL
 */
struct file *anon_get_shmem(size_t len);

#endif
