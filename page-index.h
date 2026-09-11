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

#ifndef __PAGE_INDEX_H__
#define __PAGE_INDEX_H__

#include <sys/types.h>
#include <pthread.h>

struct page_index_entry {
	unsigned long addr;		/* page-aligned virtual address */
	unsigned long len;		/* decompressed length of region */
	off_t file_offset;		/* offset to data payload in dump file */
	unsigned long on_disk_len;	/* size of data on disk (compressed or raw) */
	char *data;			/* preloaded page data (encrypted mode only) */
	int served;			/* 1 if this region has been served */
	int excluded;			/* 1 if this region falls within a VMA
					 * excluded from uffd registration
					 * (e.g. contains PC/SP). Such entries
					 * must never be touched by the lazy
					 * handler (UFFDIO_COPY would fail
					 * since the range was never
					 * registered) -- they are restored
					 * eagerly by the caller instead. */
};

struct page_index {
	struct page_index_entry *entries;
	int nr_entries;
	int capacity;
	unsigned long total_pages;	/* total number of pages across all entries */
	unsigned long served_pages;	/* number of pages already served */
	int preloaded;			/* 1 if data is preloaded in memory */
	pthread_mutex_t lock;		/* protects served/served_pages updates,
					 * since the main thread (eager stack
					 * restore) and the lazy-pages handler
					 * thread may both call
					 * page_index_mark_served() concurrently */
};

struct page_index *page_index_create(void);
void page_index_destroy(struct page_index *idx);
int page_index_add(struct page_index *idx, unsigned long addr, unsigned long len,
		   off_t file_offset, unsigned long on_disk_len);
int page_index_build(struct page_index *idx, int dump_fd,
		     int (*read_fn)(int fd, void *buf, size_t count),
		     int compressed, int encrypted,
		     int (*decompress_fn)(char *dst, const size_t len,
					  int (*xread)(int fd, void *buf, size_t count),
					  int fd));
struct page_index_entry *page_index_lookup(struct page_index *idx, unsigned long addr);
struct page_index_entry *page_index_next_unserved(struct page_index *idx);
void page_index_mark_served(struct page_index *idx, struct page_index_entry *entry);

#endif /* __PAGE_INDEX_H__ */
