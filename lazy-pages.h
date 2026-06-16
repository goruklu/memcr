/*
 * Copyright (C) 2025 memcr contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef __LAZY_PAGES_H__
#define __LAZY_PAGES_H__

#include <pthread.h>
#include "page-index.h"

#define LAZY_PAGES_PREFETCH_TIMEOUT_MS	100
#define LAZY_PAGES_PREFETCH_BATCH	8

struct lazy_pages_ctx {
	int uffd;			/* userfaultfd file descriptor */
	int dump_fd;			/* dump file descriptor */
	struct page_index *index;	/* page index for lookups */
	pid_t target_pid;		/* target process pid */
	char *decomp_buf;		/* buffer for reading compressed data */
	char *page_buf;			/* page-aligned buffer for decompressed data */
	size_t buf_size;		/* size of page_buf / decomp_buf */
	pthread_t thread;		/* handler thread id */
	int active;			/* 1 if handler is running */

	/* Dump file I/O functions (support encryption layer) */
	int (*dump_read)(int fd, void *buf, size_t count);
	int (*dump_close)(int fd);
};

/*
 * Start the lazy-pages handler thread.
 * Takes ownership of uffd and dump_fd (will close them on cleanup).
 * Returns 0 on success, -1 on error.
 */
int lazy_pages_start(struct lazy_pages_ctx *ctx);

/*
 * Wait for the lazy-pages handler to complete.
 * Blocks until all pages have been served or the target dies.
 */
void lazy_pages_wait(struct lazy_pages_ctx *ctx);

/*
 * Stop the lazy-pages handler if still running and free resources.
 */
void lazy_pages_stop(struct lazy_pages_ctx *ctx);

/*
 * Eagerly serve a single page fault at the given address.
 * Used to inject critical pages (PC/SP) before ctx_restore.
 * Returns 0 on success, -1 on error (e.g., address not in index).
 */
int lazy_pages_serve_page(struct lazy_pages_ctx *ctx, unsigned long addr);

#endif /* __LAZY_PAGES_H__ */
