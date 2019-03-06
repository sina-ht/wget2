/* Test of condition variables in multithreaded situations.
	Copyright (C) 2008-2019 Free Software Foundation, Inc.

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wget.h>

static int
	cond_value = 0;
static wget_thread_mutex_t
	lockcond;
static wget_thread_cond_t
	condtest;

static void *cond_routine(G_GNUC_WGET_UNUSED void *arg)
{
	wget_thread_mutex_lock(lockcond);
	while (!cond_value)
		wget_thread_cond_wait(condtest, lockcond, 0);
	wget_thread_mutex_unlock(lockcond);

	cond_value = 2;

	return NULL;
}

static void test_cond(void)
{
	int remain = 1;
	wget_thread_t thread;

	cond_value = 0;

	wget_thread_mutex_init(&lockcond);
	wget_thread_cond_init(&condtest);

	wget_thread_start(&thread, cond_routine, NULL, 0);
	do {
//		yield();
		remain = sleep(remain);
	} while (remain);

	/* signal condition */
	wget_thread_mutex_lock(lockcond);
	cond_value = 1;
	wget_thread_cond_signal(condtest);
	wget_thread_mutex_unlock(lockcond);

	wget_thread_join(&thread);

	wget_thread_cond_destroy(&condtest);
	wget_thread_mutex_destroy(&lockcond);

	if (cond_value != 2)
		exit(1);
}

int main(void)
{
	if (!wget_thread_support())
		return 77;

	test_cond();

	return 0;
}
