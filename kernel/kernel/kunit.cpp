/*
 * Copyright (c) 2022 Pedro Falcato
 * This file is part of Onyx, and is released under the terms of the MIT License
 * check LICENSE at the root directory for more information
 *
 * SPDX-License-Identifier: MIT
 */
#include <onyx/init.h>
#include <onyx/kunit.h>
#include <onyx/vector.h>

namespace internal
{

cul::vector<onx::test *> tests;

}

void test_register(onx::test *t)
{
    bool success = internal::tests.push_back(t);

    if (!success)
        panic("Failed to register test %s\n", t->name_);
}

void kunit_do_tests()
{
    unsigned int failed = 0;
    unsigned int done = 0;

    for (auto t : internal::tests)
    {
        t->do_test();
        if (!t->__success_)
            failed++;
        done++;
    }

    printk("--------- KUnit tests done -- %u tests executed, %u failed ---------\n", done, failed);
    printf("kunit: tests done -- %u tests executed, %u failed\n", done, failed);
}

INIT_LEVEL_CORE_KERNEL_ENTRY(kunit_do_tests);
