/*
 * Copyright (C) 2010-2016 Alexander Saprykin <xelfium@gmail.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
 */

#include "pmem.h"
#include "pmutex.h"
#include "puthread.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>

#include <windows.h>

typedef HANDLE puthread_hdl;

struct _PUThread {
	puthread_hdl		hdl;
	pboolean		joinable;
	PUThreadPriority	prio;
};

struct _PUThreadKey {
	DWORD			key_idx;
	PDestroyFunc		free_func;
};

typedef struct __PUThreadDestructor _PUThreadDestructor;

struct __PUThreadDestructor {
	DWORD			key_idx;
	PDestroyFunc		free_func;
	_PUThreadDestructor	*next;
};

static _PUThreadDestructor * volatile __tls_destructors;
static PMutex *__tls_mutex = NULL;

static int p_uthread_priority_map[P_UTHREAD_PRIORITY_HIGHEST + 1];

void
__p_uthread_init (void)
{
	if (__tls_mutex == NULL)
		__tls_mutex = p_mutex_new ();

	p_uthread_priority_map[P_UTHREAD_PRIORITY_LOWEST]	= THREAD_PRIORITY_LOWEST;
	p_uthread_priority_map[P_UTHREAD_PRIORITY_LOW]		= THREAD_PRIORITY_BELOW_NORMAL;
	p_uthread_priority_map[P_UTHREAD_PRIORITY_NORMAL]	= THREAD_PRIORITY_NORMAL;
	p_uthread_priority_map[P_UTHREAD_PRIORITY_HIGH]		= THREAD_PRIORITY_ABOVE_NORMAL;
	p_uthread_priority_map[P_UTHREAD_PRIORITY_HIGHEST]	= THREAD_PRIORITY_HIGHEST;
}

void
__p_uthread_shutdown (void)
{
	_PUThreadDestructor *destr;

	if (__tls_mutex != NULL) {
		p_mutex_free (__tls_mutex);
		__tls_mutex = NULL;
	}

	__p_uthread_win32_thread_detach ();

	destr = __tls_destructors;

	while (destr != NULL) {
		_PUThreadDestructor *next_destr = destr->next;

		TlsFree (destr->key_idx);
		p_free (destr);

		destr = next_destr;
	}
}

void
__p_uthread_win32_thread_detach (void)
{
	pboolean was_called;

	do {
		_PUThreadDestructor *destr;

		was_called = FALSE;

		for (destr = __tls_destructors; destr; destr = destr->next) {
			ppointer value;

			value = TlsGetValue (destr->key_idx);

			if (value != NULL && destr->free_func != NULL) {
				TlsSetValue (destr->key_idx, NULL);
				destr->free_func (value);
				was_called = TRUE;
			}
		}
	} while (was_called);
}

static DWORD
__p_uthread_get_tls_key (PUThreadKey *key)
{
	DWORD tls_key = key->key_idx;

	if (tls_key != TLS_OUT_OF_INDEXES)
		return tls_key;

	p_mutex_lock (__tls_mutex);

	tls_key = key->key_idx;

	if (tls_key == TLS_OUT_OF_INDEXES) {
		_PUThreadDestructor *destr = NULL;

		tls_key = TlsAlloc ();

		if (tls_key == TLS_OUT_OF_INDEXES) {
			P_ERROR ("PUThread: failed to call TlsAlloc()");
			p_mutex_unlock (__tls_mutex);
			return TLS_OUT_OF_INDEXES;
		}

		if (key->free_func != NULL) {
			if ((destr = p_malloc0 (sizeof (_PUThreadDestructor))) == NULL) {
				P_ERROR ("PUThread: failed to allocate memory for a TLS destructor");

				if (TlsFree (tls_key) == 0)
					P_ERROR ("PUThread: failed to call TlsFree()");

				p_mutex_unlock (__tls_mutex);
				return TLS_OUT_OF_INDEXES;
			}

			destr->key_idx   = tls_key;
			destr->free_func = key->free_func;
			destr->next      = __tls_destructors;

			/* At the same time thread exit could be performed at there is no
			 * lock for the global destructor list */
			if (InterlockedCompareExchangePointer ((PVOID volatile *) &__tls_destructors,
							       (PVOID) destr,
							       (PVOID) destr->next) != (PVOID) destr->next) {
				P_ERROR ("PUThread: failed to setup a TLS key destructor");

				if (TlsFree (tls_key) == 0)
					P_ERROR ("PUThread: failed to call(2) TlsFree()");

				p_free (destr);

				p_mutex_unlock (__tls_mutex);
				return TLS_OUT_OF_INDEXES;
			}
		}

		key->key_idx = tls_key;
	}

	p_mutex_unlock (__tls_mutex);

	return tls_key;
}

P_LIB_API PUThread *
p_uthread_create_full (PUThreadFunc	func,
		       ppointer		data,
		       pboolean		joinable,
		       PUThreadPriority	prio)
{
	PUThread	*ret;

	if (!func)
		return NULL;

	if ((ret = p_malloc0 (sizeof (PUThread))) == NULL) {
		P_ERROR ("PUThread: failed to allocate memory");
		return NULL;
	}

	if ((ret->hdl = CreateThread (NULL, 0, (LPTHREAD_START_ROUTINE) func, data, 0, NULL)) == NULL) {
		P_ERROR ("PUThread: failed to call CreateThread()");
		p_free (ret);
		return NULL;
	}

	ret->joinable = joinable;

	p_uthread_set_priority (ret, prio);

	return ret;
}

P_LIB_API PUThread *
p_uthread_create (PUThreadFunc		func,
		  ppointer		data,
		  pboolean		joinable)
{
	/* All checks will be inside */
	return p_uthread_create_full (func, data, joinable, P_UTHREAD_PRIORITY_NORMAL);
}

P_LIB_API void
p_uthread_exit (pint code)
{
	ExitThread ((DWORD) code);
}

P_LIB_API pint
p_uthread_join (PUThread *thread)
{
	DWORD exit_code;

	if (!thread || !thread->joinable)
		return -1;

	if ((WaitForSingleObject (thread->hdl, INFINITE)) != WAIT_OBJECT_0) {
		P_ERROR ("PUThread: failed to call WaitForSingleObject() to join a thread");
		return -1;
	}

	if (!GetExitCodeThread (thread->hdl, &exit_code)) {
		P_ERROR ("PUThread: failed to call GetExitCodeThread()");
		return -1;
	}

	return exit_code;
}

P_LIB_API void
p_uthread_free (PUThread *thread)
{
	if (!thread)
		return;

	CloseHandle (thread->hdl);

	p_free (thread);
}

P_LIB_API void
p_uthread_yield (void)
{
	Sleep (0);
}

P_LIB_API pint
p_uthread_set_priority (PUThread		*thread,
			PUThreadPriority	prio)
{
	if (thread == NULL)
		return -1;

	if (prio > P_UTHREAD_PRIORITY_HIGHEST || prio < P_UTHREAD_PRIORITY_LOWEST) {
		P_WARNING ("PUThread: trying to assign wrong thread priority");
		prio = P_UTHREAD_PRIORITY_NORMAL;
	}

	if (!SetThreadPriority (thread->hdl, p_uthread_priority_map[prio])) {
		P_ERROR ("PUThread: failed to call SetThreadPriority()");
		return -1;
	}

	thread->prio = prio;

	return 0;
}

P_LIB_API P_HANDLE
p_uthread_current_id (void)
{
	return (P_HANDLE) GetCurrentThreadId ();
}

P_LIB_API PUThreadKey *
p_uthread_local_new (PDestroyFunc free_func)
{
	PUThreadKey *ret;

	if ((ret = p_malloc0 (sizeof (PUThreadKey))) == NULL) {
		P_ERROR ("PUThread: failed to allocate memory for PUThreadKey");
		return NULL;
	}

	ret->key_idx   = TLS_OUT_OF_INDEXES;
	ret->free_func = free_func;

	return ret;
}

P_LIB_API void
p_uthread_local_free (PUThreadKey *key)
{
	if (key == NULL)
		return;

	p_free (key);
}

P_LIB_API ppointer
p_uthread_get_local (PUThreadKey *key)
{
	DWORD tls_idx;

	if (key == NULL)
		return NULL;

	tls_idx = __p_uthread_get_tls_key (key);

	return tls_idx == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue (tls_idx);
}

P_LIB_API void
p_uthread_set_local (PUThreadKey	*key,
		     ppointer		value)
{
	DWORD tls_idx;

	if (key == NULL)
		return;

	tls_idx = __p_uthread_get_tls_key (key);

	if (tls_idx != TLS_OUT_OF_INDEXES) {
		if (TlsSetValue (tls_idx, value) == 0)
			P_ERROR ("PUThread: failed to call TlsSetValue()");
	}
}

P_LIB_API void
p_uthread_replace_local	(PUThreadKey	*key,
			 ppointer	value)
{
	DWORD		tls_idx;
	ppointer	old_value;

	if (key == NULL)
		return;

	tls_idx = __p_uthread_get_tls_key (key);

	if (tls_idx == TLS_OUT_OF_INDEXES)
		return;

	old_value = TlsGetValue (tls_idx);

	if (old_value != NULL && key->free_func != NULL)
		key->free_func (old_value);

	if (TlsSetValue (tls_idx, value) == 0)
		P_ERROR ("PUThread: failed to call(2) TlsSetValue()");
}
