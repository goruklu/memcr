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

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <signal.h>

#include "memcr.h"
#include "lazy-pages.h"
#include "page-index.h"
#include "compress.h"

#define err(...) fprintf(stderr, "[!] " __VA_ARGS__)
#define msg(...) fprintf(stdout, "[*] " __VA_ARGS__)

#if defined(LOG_LEVEL) && LOG_LEVEL >= 2
#define log(...) fprintf(stdout, "[l] " __VA_ARGS__)
#else
#define log(...)
#endif

/*
 * Read a region's data from the dump file at the given offset,
 * decompress it into page_buf.
 *
 * Returns 0 on success, -1 on error.
 */
static int read_region_data(struct lazy_pages_ctx *ctx, struct page_index_entry *entry)
{
	int ret;

	/* Seek to the data payload in the dump file */
	if (lseek(ctx->dump_fd, entry->file_offset, SEEK_SET) < 0) {
		err("lazy-pages: lseek to offset %ld failed: %m\n",
		    (long)entry->file_offset);
		return -1;
	}

	/* Use compress_read which handles both plain and compressed data */
	ret = compress_read(ctx->page_buf, entry->len, ctx->dump_read, ctx->dump_fd);
	if (ret < 0) {
		err("lazy-pages: compress_read failed for region %lx len %lu\n",
		    entry->addr, entry->len);
		return -1;
	}

	return 0;
}

/*
 * Handle a page fault by reading the faulted page from the dump file
 * and injecting it into the target process via UFFDIO_COPY.
 *
 * Returns 0 on success, -1 on error.
 */
static int handle_page_fault(struct lazy_pages_ctx *ctx, unsigned long fault_addr)
{
	struct page_index_entry *entry;
	struct uffdio_copy copy;
	size_t page_offset;
	int ret;

	/* Page-align the fault address */
	fault_addr &= ~((unsigned long)PAGE_SIZE - 1);

	/* Find the region containing this address */
	entry = page_index_lookup(ctx->index, fault_addr);
	if (!entry) {
		err("lazy-pages: no index entry for fault at %lx\n", fault_addr);
		return -1;
	}

	/* If already served (e.g. by prefetch), this is a spurious wake */
	if (entry->served) {
		log("lazy-pages: region %lx already served\n", fault_addr);
		return 0;
	}

	/* Read and decompress the region */
	ret = read_region_data(ctx, entry);
	if (ret < 0)
		return ret;

	/*
	 * Inject the entire region via UFFDIO_COPY.
	 * Even though we only faulted on one page, we inject the whole region
	 * to avoid repeated decompressions for adjacent pages.
	 */
	copy.dst = entry->addr;
	copy.src = (unsigned long)ctx->page_buf;
	copy.len = entry->len;
	copy.mode = 0; /* wake the faulting thread */

	ret = ioctl(ctx->uffd, UFFDIO_COPY, &copy);
	if (ret < 0) {
		if (errno == EEXIST) {
			/* Page was already resolved (race with prefetch) */
			log("lazy-pages: UFFDIO_COPY EEXIST for %lx\n", entry->addr);
		} else if (errno == ENOSPC) {
			/*
			 * Partial copy - some pages in the range were already
			 * present. Try page-by-page for the remaining.
			 */
			page_offset = 0;
			while (page_offset < entry->len) {
				copy.dst = entry->addr + page_offset;
				copy.src = (unsigned long)(ctx->page_buf + page_offset);
				copy.len = PAGE_SIZE;
				copy.mode = 0;

				ret = ioctl(ctx->uffd, UFFDIO_COPY, &copy);
				if (ret < 0 && errno != EEXIST) {
					err("lazy-pages: UFFDIO_COPY page %lx failed: %m\n",
					    entry->addr + page_offset);
				}
				page_offset += PAGE_SIZE;
			}
		} else {
			err("lazy-pages: UFFDIO_COPY %lx len %lu failed: %m\n",
			    entry->addr, entry->len);
			return -1;
		}
	}

	page_index_mark_served(ctx->index, entry);

	log("lazy-pages: served region %lx-%lx (%lu pages)\n",
	    entry->addr, entry->addr + entry->len, entry->len / PAGE_SIZE);

	return 0;
}

/*
 * Prefetch a batch of pages in the background.
 * Uses UFFDIO_COPY with MODE_DONTWAKE since no thread is waiting.
 */
static void prefetch_pages(struct lazy_pages_ctx *ctx, int batch_size)
{
	struct page_index_entry *entry;
	struct uffdio_copy copy;
	int i, ret;

	for (i = 0; i < batch_size; i++) {
		entry = page_index_next_unserved(ctx->index);
		if (!entry)
			return; /* All pages served */

		ret = read_region_data(ctx, entry);
		if (ret < 0) {
			/* Skip this region on error */
			page_index_mark_served(ctx->index, entry);
			continue;
		}

		copy.dst = entry->addr;
		copy.src = (unsigned long)ctx->page_buf;
		copy.len = entry->len;
		copy.mode = UFFDIO_COPY_MODE_DONTWAKE;

		ret = ioctl(ctx->uffd, UFFDIO_COPY, &copy);
		if (ret < 0 && errno != EEXIST) {
			/*
			 * If the target unmapped this region or the copy failed
			 * for any reason, just mark it served and continue.
			 */
			log("lazy-pages: prefetch UFFDIO_COPY %lx failed: %m\n",
			    entry->addr);
		}

		page_index_mark_served(ctx->index, entry);
		log("lazy-pages: prefetched region %lx-%lx\n",
		    entry->addr, entry->addr + entry->len);
	}
}

/*
 * Check if the target process is still alive.
 */
static int target_alive(pid_t pid)
{
	return (kill(pid, 0) == 0 || errno != ESRCH);
}

/*
 * Main lazy-pages handler loop.
 * Runs as a thread in the daemon process.
 */
static void *lazy_pages_thread(void *arg)
{
	struct lazy_pages_ctx *ctx = arg;
	struct pollfd pfd;
	struct uffd_msg uffd_msg;
	ssize_t nread;
	int ret;

	/* Block signals in this thread - let main thread handle them */
	sigset_t mask;
	sigfillset(&mask);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

	pfd.fd = ctx->uffd;
	pfd.events = POLLIN;

	msg("lazy-pages: handler started for pid %d (%lu pages in %d regions)\n",
	    ctx->target_pid, ctx->index->total_pages, ctx->index->nr_entries);

	while (ctx->index->served_pages < ctx->index->total_pages) {
		/* Check target is still alive */
		if (!target_alive(ctx->target_pid)) {
			msg("lazy-pages: target %d died, stopping handler\n",
			    ctx->target_pid);
			break;
		}

		ret = poll(&pfd, 1, LAZY_PAGES_PREFETCH_TIMEOUT_MS);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			err("lazy-pages: poll failed: %m\n");
			break;
		}

		if (ret > 0) {
			if (pfd.revents & POLLERR) {
				err("lazy-pages: POLLERR on uffd\n");
				break;
			}
			if (pfd.revents & POLLHUP) {
				msg("lazy-pages: uffd closed (target died?)\n");
				break;
			}
			if (pfd.revents & POLLIN) {
				nread = read(ctx->uffd, &uffd_msg, sizeof(uffd_msg));
				if (nread <= 0) {
					if (errno == EAGAIN)
						continue;
					err("lazy-pages: read uffd failed: %m\n");
					break;
				}

				if (uffd_msg.event == UFFD_EVENT_PAGEFAULT) {
					unsigned long addr = uffd_msg.arg.pagefault.address;
					log("lazy-pages: page fault at %lx\n", addr);
					ret = handle_page_fault(ctx, addr);
					if (ret < 0) {
						err("lazy-pages: failed to handle fault at %lx\n",
						    addr);
						/*
						 * We can't leave the thread stuck.
						 * Try to wake it with a zero page.
						 */
						struct uffdio_zeropage zero = {
							.range = {
								.start = addr & ~((unsigned long)PAGE_SIZE - 1),
								.len = PAGE_SIZE,
							},
							.mode = 0,
						};
						ioctl(ctx->uffd, UFFDIO_ZEROPAGE, &zero);
					}
				} else if (uffd_msg.event == UFFD_EVENT_UNMAP ||
					   uffd_msg.event == UFFD_EVENT_REMOVE) {
					/*
					 * Target unmapped or removed pages.
					 * Mark any overlapping regions as served.
					 */
					unsigned long start = uffd_msg.arg.remove.start;
					unsigned long end = uffd_msg.arg.remove.end;
					struct page_index_entry *entry;

					log("lazy-pages: unmap/remove event %lx-%lx\n",
					    start, end);

					while (start < end) {
						entry = page_index_lookup(ctx->index, start);
						if (entry) {
							page_index_mark_served(ctx->index, entry);
							start = entry->addr + entry->len;
						} else {
							start += PAGE_SIZE;
						}
					}
				}
			}
		} else {
			/* Timeout: prefetch in background */
			prefetch_pages(ctx, LAZY_PAGES_PREFETCH_BATCH);
		}
	}

	msg("lazy-pages: handler finished (%lu/%lu pages served)\n",
	    ctx->index->served_pages, ctx->index->total_pages);

	/* Cleanup */
	close(ctx->uffd);
	ctx->uffd = -1;
	ctx->active = 0;

	return NULL;
}

int lazy_pages_start(struct lazy_pages_ctx *ctx)
{
	int ret;

	ctx->active = 1;

	ret = pthread_create(&ctx->thread, NULL, lazy_pages_thread, ctx);
	if (ret) {
		err("lazy-pages: pthread_create failed: %s\n", strerror(ret));
		ctx->active = 0;
		return -1;
	}

	/* Detach the thread so it cleans up automatically if not joined */
	pthread_detach(ctx->thread);

	return 0;
}

void lazy_pages_wait(struct lazy_pages_ctx *ctx)
{
	/*
	 * Since the thread is detached, we poll the active flag.
	 * In a more sophisticated implementation, we'd use a condition variable.
	 */
	while (ctx->active) {
		usleep(100000); /* 100ms */
	}
}

void lazy_pages_stop(struct lazy_pages_ctx *ctx)
{
	if (ctx->active && ctx->uffd >= 0) {
		/* Closing the uffd will cause poll() to return POLLHUP */
		close(ctx->uffd);
		ctx->uffd = -1;
	}

	/* Wait for thread to exit */
	while (ctx->active) {
		usleep(10000); /* 10ms */
	}

	/* Free resources */
	if (ctx->page_buf) {
		free(ctx->page_buf);
		ctx->page_buf = NULL;
	}
	if (ctx->decomp_buf) {
		free(ctx->decomp_buf);
		ctx->decomp_buf = NULL;
	}
	if (ctx->index) {
		page_index_destroy(ctx->index);
		ctx->index = NULL;
	}
	if (ctx->dump_fd >= 0) {
		if (ctx->dump_close)
			ctx->dump_close(ctx->dump_fd);
		else
			close(ctx->dump_fd);
		ctx->dump_fd = -1;
	}
}
