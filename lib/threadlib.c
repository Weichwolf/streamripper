/* threadlib.c - jonclegg@yahoo.com
 * really bad threading library from hell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#if WIN32
#include <process.h>
#include <windows.h>
#elif __UNIX__
#include <pthread.h>
#elif __BEOS__
#include <be/kernel/OS.h>
#endif
#include <stdio.h>
#include <time.h>
#include "srtypes.h"
#include "threadlib.h"
#include "debug.h"

/******************************************************************************
 * Public functions
 *****************************************************************************/
#if !WIN32
struct thread_start {
    void (*callback)(void *);
    void *argument;
};
static void *thread_entry(void *opaque)
{
    struct thread_start start = *(struct thread_start *)opaque;
    free(opaque);
    start.callback(start.argument);
    return NULL;
}
#endif

error_code
threadlib_beginthread (THREAD_HANDLE *thread, void (*callback)(void *), 
		       void* arg)
{
#if WIN32
    BeginThread (*thread, callback, arg);
#else
    struct thread_start *start = malloc(sizeof(*start));
    if (!start)
        return SR_ERROR_CANT_ALLOC_MEMORY;
    start->callback = callback;
    start->argument = arg;
    if (pthread_create(thread, NULL, thread_entry, start) != 0) {
        free(start);
        return SR_ERROR_CANT_CREATE_THREAD;
    }
#endif
    //if (thread->thread_handle == NULL)	// don't feel like porting this
    //	return SR_ERROR_CANT_CREATE_THREAD;

    return SR_SUCCESS;
}

void
threadlib_waitforclose (THREAD_HANDLE *thread)
{
    WaitForThread (*thread);
}

HSEM
threadlib_create_sem(void)
{
    HSEM s;
    SemInit (s);
    return s;
}

error_code
threadlib_waitfor_sem (HSEM *e)
{
    if (!e)
	return SR_ERROR_INVALID_PARAM;
    SemWait (*e);
    return SR_SUCCESS;
}

error_code
threadlib_signal_sem(HSEM *e)
{
    if (!e)
	return SR_ERROR_INVALID_PARAM;
    SemPost (*e);
    return SR_SUCCESS;
}

void
threadlib_destroy_sem (HSEM *e)
{
    (void)e;
    SemDestroy (*e);
}
