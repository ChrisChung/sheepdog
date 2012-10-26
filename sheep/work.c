/*
 * Copyright (C) 2009-2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * This code is based on bs.c from Linux target framework (tgt):
 *   Copyright (C) 2007 FUJITA Tomonori <tomof@acm.org>
 *   Copyright (C) 2007 Mike Christie <michaelc@cs.wisc.edu>
 */
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <syscall.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <linux/types.h>

#include "list.h"
#include "util.h"
#include "work.h"
#include "logger.h"
#include "event.h"
#include "trace/trace.h"

#define NR_MIN_WORKER_THREADS 4
#define NR_MAX_WORKER_THREADS 16384

static int efd;
int total_ordered_workers;
LIST_HEAD(worker_info_list);

enum wq_state {
	WQ_DEAD = (1U << 1),
};

static void *worker_routine(void *arg);

static int create_worker_threads(struct worker_info *wi, size_t nr_threads)
{
	int ret;

	nr_threads = min(nr_threads, (size_t)NR_MAX_WORKER_THREADS);

	pthread_mutex_lock(&wi->startup_lock);
	if (nr_threads <= wi->nr_threads) {
		pthread_mutex_unlock(&wi->startup_lock);
		return 0;
	}
	while (wi->nr_threads < nr_threads) {
		wi->nr_threads++;
		ret = pthread_create(&wi->worker_thread[wi->nr_threads - 1],
				     NULL, worker_routine, wi);
		if (ret != 0) {
			eprintf("failed to create worker thread: %m\n");
			wi->nr_threads--;
			pthread_mutex_unlock(&wi->startup_lock);
			return -1;
		}
		dprintf("create thread %s %zd\n", wi->name, wi->nr_threads);
	}
	pthread_mutex_unlock(&wi->startup_lock);

	return 0;
}

void queue_work(struct work_queue *q, struct work *work)
{
	struct worker_info *wi = container_of(q, struct worker_info, q);

	pthread_mutex_lock(&wi->pending_lock);
	wi->nr_pending++;

	if (!wi->ordered && wi->nr_threads < wi->nr_pending + wi->nr_running)
		create_worker_threads(wi, wi->nr_threads * 2);

	list_add_tail(&work->w_list, &wi->q.pending_list);
	pthread_mutex_unlock(&wi->pending_lock);

	pthread_cond_signal(&wi->pending_cond);
}

static void bs_thread_request_done(int fd, int events, void *data)
{
	int ret;
	struct worker_info *wi;
	struct work *work;
	eventfd_t value;
	LIST_HEAD(list);

	ret = eventfd_read(fd, &value);
	if (ret < 0)
		return;

	list_for_each_entry(wi, &worker_info_list, worker_info_siblings) {
		pthread_mutex_lock(&wi->finished_lock);
		list_splice_init(&wi->finished_list, &list);
		pthread_mutex_unlock(&wi->finished_lock);

		while (!list_empty(&list)) {
			work = list_first_entry(&list, struct work, w_list);
			list_del(&work->w_list);

			work->done(work);
		}
	}
}

static void *worker_routine(void *arg)
{
	struct worker_info *wi = arg;
	struct work *work;
	eventfd_t value = 1;
	int i, uninitialized_var(idx);

	for (i = 0; i < wi->nr_threads; i++) {
		if (wi->worker_thread[i] == pthread_self()) {
			idx = i;
			break;
		}
	}

	if (wi->ordered)
		set_thread_name(wi->name, -1);
	else
		set_thread_name(wi->name, idx);

	pthread_mutex_lock(&wi->startup_lock);
	/* started this thread */
	pthread_mutex_unlock(&wi->startup_lock);

	pthread_mutex_lock(&wi->pending_lock);
	wi->nr_running++;
	pthread_mutex_unlock(&wi->pending_lock);

	while (!(wi->q.wq_state & WQ_DEAD)) {

		pthread_mutex_lock(&wi->pending_lock);
retest:
		if (list_empty(&wi->q.pending_list)) {
			wi->nr_running--;
			pthread_cond_wait(&wi->pending_cond, &wi->pending_lock);
			if (wi->q.wq_state & WQ_DEAD) {
				pthread_mutex_unlock(&wi->pending_lock);
				pthread_exit(NULL);
			}
			wi->nr_running++;
			goto retest;
		}

		wi->nr_pending--;
		work = list_first_entry(&wi->q.pending_list,
				       struct work, w_list);

		list_del(&work->w_list);
		pthread_mutex_unlock(&wi->pending_lock);

		work->fn(work);

		pthread_mutex_lock(&wi->finished_lock);
		list_add_tail(&work->w_list, &wi->finished_list);
		pthread_mutex_unlock(&wi->finished_lock);

		eventfd_write(efd, value);
	}

	pthread_exit(NULL);
}

static int init_eventfd(void)
{
	int ret;
	static bool done;

	if (done)
		return 0;

	efd = eventfd(0, EFD_NONBLOCK);
	if (efd < 0) {
		eprintf("failed to create an event fd: %m\n");
		return 1;
	}

	ret = register_event(efd, bs_thread_request_done, NULL);
	if (ret) {
		eprintf("failed to register event fd %m\n");
		close(efd);
		return 1;
	}

	done = true;

	return 0;
}

struct work_queue *init_work_queue(const char *name, bool ordered)
{
	int i, ret, nr;
	struct worker_info *wi;

	ret = init_eventfd();
	if (ret)
		return NULL;

	nr = ordered ? 1 : NR_MAX_WORKER_THREADS;
	wi = zalloc(sizeof(*wi) + nr * sizeof(pthread_t));
	if (!wi)
		return NULL;

	wi->name = name;
	wi->ordered = ordered;

	INIT_LIST_HEAD(&wi->q.pending_list);
	INIT_LIST_HEAD(&wi->finished_list);

	pthread_cond_init(&wi->pending_cond, NULL);

	pthread_mutex_init(&wi->finished_lock, NULL);
	pthread_mutex_init(&wi->pending_lock, NULL);
	pthread_mutex_init(&wi->startup_lock, NULL);

	nr = ordered ? 1 : NR_MIN_WORKER_THREADS;
	ret = create_worker_threads(wi, nr);
	if (ret < 0)
		goto destroy_threads;

	list_add(&wi->worker_info_siblings, &worker_info_list);

	return &wi->q;
destroy_threads:

	wi->q.wq_state |= WQ_DEAD;
	pthread_mutex_unlock(&wi->startup_lock);
	for (i = wi->nr_threads; i > 0; i--) {
		pthread_join(wi->worker_thread[i - 1], NULL);
		eprintf("stopped worker thread #%d\n", i - 1);
	}

/* destroy_cond_mutex: */
	pthread_cond_destroy(&wi->pending_cond);
	pthread_mutex_destroy(&wi->pending_lock);
	pthread_mutex_destroy(&wi->startup_lock);
	pthread_mutex_destroy(&wi->finished_lock);

	return NULL;
}

#ifdef COMPILE_UNUSED_CODE
static void exit_work_queue(struct work_queue *q)
{
	int i;
	struct worker_info *wi = container_of(q, struct worker_info, q);

	q->wq_state |= WQ_DEAD;
	pthread_cond_broadcast(&wi->pending_cond);

	for (i = 0; wi->worker_thread[i] && i < wi->nr_threads; i++)
		pthread_join(wi->worker_thread[i], NULL);

	pthread_cond_destroy(&wi->pending_cond);
	pthread_mutex_destroy(&wi->pending_lock);
	pthread_mutex_destroy(&wi->startup_lock);
	pthread_mutex_destroy(&wi->finished_lock);
}
#endif
