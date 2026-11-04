/*
 * Copyright 2026 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <reent.h>
#include <stdlib.h>
#include <sys/types.h>

#include "errno.h"
#include "mmosal.h"

/*
 * Note: This function is defined in stdlib.h.
 * It is called internally by malloc()
 */
void *_malloc_r(struct _reent *r, size_t size) _NOTHROW
{
    /* Note: This function may be called from __libc_init_array()
     * in C++ applications when running through the global constructor list.
     * It is not safe to call pvPortMalloc_() before FreeRTOS is initialised when
     * using HEAP5. So C++ applications must use HEAP4 and not HEAP5.
     */
    void *ret = mmosal_malloc(size);
    if (ret == NULL)
    {
        r->_errno = ENOMEM;
    }
    return ret;
}

/*
 * Note: This function is defined in stdlib.h.
 * It is called internally by calloc()
 */
void *_calloc_r(struct _reent *r, size_t nitems, size_t size)
{
    /* Note: This function may be called from __libc_init_array()
     * in C++ applications when running through the global constructor list.
     * It is not safe to call pvPortMalloc_() before FreeRTOS is initialised when
     * using HEAP5. So C++ applications must use HEAP4 and not HEAP5.
     */
    void *ret = mmosal_calloc(nitems, size);
    if (ret == NULL)
    {
        r->_errno = ENOMEM;
    }
    return ret;
}

/*
 * Note: This function is defined in stdlib.h
 * It is called internally by realloc()
 */
void *_realloc_r(struct _reent *r, void *bp, size_t size) _NOTHROW
{
    void *ret = mmosal_realloc(bp, size);
    if (ret == NULL)
    {
        r->_errno = ENOMEM;
    }
    return ret;
}

/*
 * Note: This function is defined in stdlib.h
 * It is called internally by free()
 */
void _free_r(struct _reent *r, void *bp) _NOTHROW
{
    (void)r;
    mmosal_free(bp);
}
