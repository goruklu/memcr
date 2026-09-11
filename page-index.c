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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "memcr.h"
#include "page-index.h"

#define PAGE_INDEX_INITIAL_CAPACITY 256

#define err(...) fprintf(stderr, "[!] " __VA_ARGS__)

struct page_index *page_index_create(void)
{
	struct page_index *idx;

	idx = calloc(1, sizeof(*idx));
	if (!idx)
		return NULL;

	idx->entries = calloc(PAGE_INDEX_INITIAL_CAPACITY, sizeof(struct page_index_entry));
	if (!idx->entries) {
		free(idx);
		return NULL;
	}

	idx->capacity = PAGE_INDEX_INITIAL_CAPACITY;
	idx->nr_entries = 0;
	idx->total_pages = 0;
	idx->served_pages = 0;
	pthread_mutex_init(&idx->lock, NULL);

	return idx;
}

void page_index_destroy(struct page_index *idx)
{
	if (!idx)
		return;

	if (idx->preloaded) {
		int i;
		for (i = 0; i < idx->nr_entries; i++) {
			free(idx->entries[i].data);
		}
	}

	pthread_mutex_destroy(&idx->lock);
	free(idx->entries);
	free(idx);
}

int page_index_add(struct page_index *idx, unsigned long addr, unsigned long len,
		   off_t file_offset, unsigned long on_disk_len)
{
	struct page_index_entry *entry;

	if (idx->nr_entries >= idx->capacity) {
		int new_capacity = idx->capacity * 2;
		struct page_index_entry *new_entries;

		new_entries = realloc(idx->entries,
				      new_capacity * sizeof(struct page_index_entry));
		if (!new_entries)
			return -1;

		idx->entries = new_entries;
		idx->capacity = new_capacity;
	}

	entry = &idx->entries[idx->nr_entries];
	entry->addr = addr;
	entry->len = len;
	entry->file_offset = file_offset;
	entry->on_disk_len = on_disk_len;
	entry->data = NULL;
	entry->served = 0;

	idx->nr_entries++;
	idx->total_pages += len / PAGE_SIZE;

	return 0;
}

static int cmp_entries_by_addr(const void *a, const void *b)
{
	const struct page_index_entry *ea = a;
	const struct page_index_entry *eb = b;

	if (ea->addr < eb->addr)
		return -1;
	if (ea->addr > eb->addr)
		return 1;
	return 0;
}

/*
 * Build the page index by scanning the dump file sequentially.
 *
 * Dump file format:
 *   [vm_region header (16 bytes)][data payload]...
 *
 * When compressed=1, data payload format is:
 *   [uint32_t compressed_len][compressed_data (compressed_len bytes)]
 *
 * When compressed=0 (plain):
 *   [raw page data (vm_region.len bytes)]
 *
 * When encrypted=1, the file must be read strictly sequentially via read_fn
 * (no lseek allowed). Page data is decompressed and stored in memory for
 * later retrieval (preloaded mode).
 *
 * When encrypted=0, file_offset is recorded for later lseek+read during
 * fault resolution.
 */
int page_index_build(struct page_index *idx, int dump_fd,
		     int (*read_fn)(int fd, void *buf, size_t count),
		     int compressed, int encrypted,
		     int (*decompress_fn)(char *dst, const size_t len,
					  int (*xread)(int fd, void *buf, size_t count),
					  int fd))
{
	struct vm_region vmr;
	off_t data_offset;
	unsigned long on_disk_len;
	int ret;
	/* Scratch buffer for skipping compressed data in non-encrypted mode */
	char skip_buf[4096];

	/* Seek to beginning of dump file (works even for encrypted files at start) */
	lseek(dump_fd, 0, SEEK_SET);

	while (1) {
		/* Read vm_region header */
		ret = read_fn(dump_fd, &vmr, sizeof(vmr));
		if (ret == 0)
			break; /* EOF */
		if (ret != (int)sizeof(vmr)) {
			if (ret > 0)
				err("page_index_build: short read on vm_region header (%d)\n", ret);
			return -1;
		}

		if (encrypted) {
			/*
			 * Encrypted mode: read and decompress data now,
			 * store it in memory for later use.
			 */
			char *page_data = malloc(vmr.len);
			if (!page_data) {
				err("page_index_build: malloc(%lu) failed\n", vmr.len);
				return -1;
			}

			ret = decompress_fn(page_data, vmr.len, read_fn, dump_fd);
			if (ret < 0) {
				err("page_index_build: decompress failed for region %lx\n", vmr.addr);
				free(page_data);
				return -1;
			}

			ret = page_index_add(idx, vmr.addr, vmr.len, 0, 0);
			if (ret < 0) {
				free(page_data);
				return -1;
			}

			/* Store the decompressed data pointer in the entry */
			idx->entries[idx->nr_entries - 1].data = page_data;
		} else {
			/* Non-encrypted: record file offset, skip data */
			data_offset = lseek(dump_fd, 0, SEEK_CUR);
			if (data_offset < 0) {
				err("page_index_build: lseek for data offset failed\n");
				return -1;
			}

			if (compressed) {
				uint32_t comp_len;
				ret = read_fn(dump_fd, &comp_len, sizeof(comp_len));
				if (ret != (int)sizeof(comp_len)) {
					err("page_index_build: failed to read compressed length\n");
					return -1;
				}
				on_disk_len = sizeof(comp_len) + comp_len;

				/* Skip past compressed data */
				if (lseek(dump_fd, comp_len, SEEK_CUR) < 0) {
					err("page_index_build: lseek past compressed data failed\n");
					return -1;
				}
			} else {
				on_disk_len = vmr.len;
				if (lseek(dump_fd, vmr.len, SEEK_CUR) < 0) {
					err("page_index_build: lseek past raw data failed\n");
					return -1;
				}
			}

			ret = page_index_add(idx, vmr.addr, vmr.len, data_offset, on_disk_len);
			if (ret < 0) {
				err("page_index_build: page_index_add failed\n");
				return -1;
			}
		}
	}

	if (encrypted)
		idx->preloaded = 1;

	/* Sort entries by address for binary search */
	qsort(idx->entries, idx->nr_entries, sizeof(struct page_index_entry),
	      cmp_entries_by_addr);

	(void)skip_buf; /* suppress unused warning */
	return 0;
}

/*
 * Look up the page index entry containing the given address.
 * Uses binary search on the sorted entries array.
 */
struct page_index_entry *page_index_lookup(struct page_index *idx, unsigned long addr)
{
	int lo = 0;
	int hi = idx->nr_entries - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		struct page_index_entry *entry = &idx->entries[mid];

		if (addr < entry->addr) {
			hi = mid - 1;
		} else if (addr >= entry->addr + entry->len) {
			lo = mid + 1;
		} else {
			return entry;
		}
	}

	return NULL;
}

/*
 * Find the next unserved entry for background prefetching.
 * Simple linear scan from the beginning.
 */
struct page_index_entry *page_index_next_unserved(struct page_index *idx)
{
	int i;

	for (i = 0; i < idx->nr_entries; i++) {
		if (!idx->entries[i].served && !idx->entries[i].excluded)
			return &idx->entries[i];
	}
	return NULL;
}

void page_index_mark_served(struct page_index *idx, struct page_index_entry *entry)
{
	pthread_mutex_lock(&idx->lock);
	if (!entry->served) {
		entry->served = 1;
		idx->served_pages += entry->len / PAGE_SIZE;
	}
	pthread_mutex_unlock(&idx->lock);
}
