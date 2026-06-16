# Userfaultfd Lazy Page Restore - Design Document

## Overview

This document describes the design and implementation plan for integrating
`userfaultfd`-based lazy page restoration into memcr, inspired by CRIU's
lazy-pages mechanism.

### Problem

Currently, memcr's restore path is **fully synchronous**: all checkpointed
pages must be uploaded back to the target process before it resumes execution.
For large processes, this means restore latency is proportional to the total
checkpoint size, even if the process only needs a small subset of pages
immediately after resuming.

### Solution

Use Linux's `userfaultfd` mechanism to allow the target process to resume
immediately after checkpoint, with pages delivered **on demand** as the process
accesses them. Pages that were discarded via `MADV_DONTNEED` during checkpoint
will be intercepted by a fault handler in the memcr daemon and injected back
into the target's address space only when accessed.

### Benefits

- **Reduced restore latency**: Process resumes near-instantly without waiting
  for full page upload
- **Reduced memory pressure**: Only pages the process actually needs are loaded
- **Background prefetch**: Remaining pages are loaded in the background during
  idle time
- **Transparent**: No changes required to the target process

---

## Architecture

```
+--------------------------------------------------------------+
|                        memcr daemon                           |
|                                                              |
|  +------------------+     +--------------------------------+ |
|  | execute_parasite |     |  lazy-pages handler thread     | |
|  | _restore_lazy()  |---->|                                | |
|  |                  |     |  1. poll(uffd) for faults       | |
|  | - CMD_SETUP_UFFD |     |  2. lookup page in index       | |
|  | - recv uffd      |     |  3. read+decompress from dump  | |
|  | - mprotect_on    |     |  4. UFFDIO_COPY into target    | |
|  | - CMD_END        |     |  5. background prefetch        | |
|  | - ctx_restore    |     |  6. cleanup when done          | |
|  | - unseize        |     +--------------------------------+ |
|  +------------------+                                        |
+--------------------------------------------------------------+
         |                            |
         | UNIX socket (SCM_RIGHTS)   | userfaultfd
         | to receive uffd            | UFFDIO_COPY
         v                            v
+--------------------------------------------------------------+
|                      Target Process                           |
|                                                              |
|  +--------+ +--------+ +--------+ +--------+ +--------+     |
|  | page 0 | | page 1 | | page 2 | | page 3 | | page 4 |    |
|  |(loaded) | | FAULT! | |(loaded) | | FAULT! | |(prefet)|    |
|  +--------+ +--------+ +--------+ +--------+ +--------+     |
|                                                              |
|  VMAs registered with UFFDIO_REGISTER (MODE_MISSING)         |
|  Process blocked on fault until daemon resolves it           |
+--------------------------------------------------------------+
```

### How It Differs from CRIU

| Aspect | CRIU | memcr |
|--------|------|-------|
| Process state | Fully recreated from scratch | Existing process, pages discarded via MADV_DONTNEED |
| VMA creation | Restored by CRIU | Already exist (only page content is missing) |
| Handler | Separate `criu lazy-pages` daemon process | Thread in the memcr daemon |
| UFFD creation | During process restore | Parasite creates inside target before resuming |
| Page source | Local images, remote page-server, or network | Local dump file only (initially) |

### Sequence of Operations

```
CHECKPOINT (unchanged):
  1. seize_target()
  2. parasite: scan VMAs, save pages to dump file
  3. parasite: MADV_DONTNEED on saved regions
  4. process stays frozen, waiting for restore

RESTORE (with --lazy-pages):
  1. parasite CMD_SETUP_UFFD:
     a. userfaultfd() -> create uffd in target
     b. UFFDIO_API -> negotiate features
     c. UFFDIO_REGISTER for each eligible VMA -> MODE_MISSING
     d. sendmsg(SCM_RIGHTS) -> send uffd to daemon
  2. daemon recvmsg() -> receives uffd
  3. target_mprotect_on() -> restore original page protections
  4. CMD_END -> parasite exits
  5. build page_index from dump file (addr -> file offset mapping)
  6. ctx_restore() -> restore registers/code
  7. unseize_target() -> PROCESS RESUMES IMMEDIATELY
  8. spawn lazy_pages_handler_thread(uffd, page_index, dump_fd)

LAZY-PAGES HANDLER (runs asynchronously):
  loop:
    poll(uffd, timeout=100ms)
    if page fault received:
      addr = fault_address & PAGE_MASK
      entry = page_index_lookup(addr)
      buf = read_and_decompress(dump_fd, entry.offset, entry.comp_len)
      ioctl(uffd, UFFDIO_COPY, {dst=addr, src=buf, len=PAGE_SIZE})
      mark page as served
    else if timeout (idle):
      prefetch next batch of unserved pages via UFFDIO_COPY
    if all pages served:
      UFFDIO_UNREGISTER all ranges
      close(uffd)
      cleanup and exit thread
```

---

## Implementation Phases

### Phase 1: CLI Option and Plumbing

**Files:** `memcr.c`, `memcr.h`, `memcrclient_proto.h`

**Changes:**
- Add `--lazy-pages` / `-L` command-line option
- Add `static int lazy_pages;` global flag in `memcr.c`
- Add option to `usage()` help text
- Wire through `struct service_command` so the client can request lazy restore
  in service mode
- Add runtime kernel version check (require >= 4.11)

**Complexity:** Low

---

### Phase 2: Syscall Wrappers for userfaultfd

**Files:** `arch/syscall.h`, `arch/syscall.c` (for each architecture)

**Changes:**
- Add `sys_userfaultfd(int flags)` wrapper
- Add `sys_ioctl(int fd, unsigned long cmd, unsigned long arg)` wrapper
- Add `sys_sendmsg(int fd, struct msghdr *msg, int flags)` wrapper (for
  SCM_RIGHTS)

These use the architecture-specific raw syscall interfaces already established
in the codebase (e.g., `arch/x86_64/linux-abi.h` provides `__syscall0` through
`__syscall6`).

**Syscall numbers:**

| Arch | userfaultfd | ioctl | sendmsg |
|------|-------------|-------|---------|
| x86_64 | 323 | 16 | 46 |
| arm64 | 282 | 29 | 211 |
| arm | 388 | 54 | 296 |
| riscv64 | 282 | 29 | 211 |

**Complexity:** Low

---

### Phase 3: Parasite CMD_SETUP_UFFD Command

**Files:** `parasite.c`, `memcr.h`

**Changes to `memcr.h`:**
- Add `CMD_SETUP_UFFD = 5` to `memcr_cmd` enum
- Add request structure:
  ```c
  struct uffd_register_req {
      unsigned long addr;   // VMA start address
      unsigned long len;    // VMA length
  };
  ```

**Changes to `parasite.c`:**
- Add `cmd_setup_uffd(const int cd)` function:
  ```c
  static int cmd_setup_uffd(const int cd)
  {
      int uffd, ret;
      struct uffdio_api api;
      struct uffdio_register reg;
      struct uffd_register_req req;

      // 1. Create userfaultfd
      uffd = sys_userfaultfd(O_NONBLOCK | O_CLOEXEC);

      // 2. Negotiate API
      api.api = UFFD_API;
      api.features = UFFD_FEATURE_EVENT_UNMAP | UFFD_FEATURE_EVENT_REMOVE;
      sys_ioctl(uffd, UFFDIO_API, &api);

      // 3. Register each VMA range (sent by daemon)
      while (read(cd, &req, sizeof(req)) > 0) {
          reg.range.start = req.addr;
          reg.range.len = req.len;
          reg.mode = UFFDIO_REGISTER_MODE_MISSING;
          sys_ioctl(uffd, UFFDIO_REGISTER, &reg);
      }

      // 4. Send uffd back to daemon via SCM_RIGHTS
      send_fd(cd, uffd);
      sys_close(uffd);

      return 0;
  }
  ```
- Add `send_fd()` helper using `sys_sendmsg()` with `SCM_RIGHTS` ancillary data
- Wire `CMD_SETUP_UFFD` into `handle_connection()` switch

**Complexity:** Medium

---

### Phase 4: Page Index for Random-Access Lookup

**Files:** New `page-index.c` / `page-index.h`

**Purpose:** Currently the dump file is sequential `(vm_region, data)` pairs.
The lazy handler needs O(1) lookup: given a faulting address, find the page data
in the dump file.

**Data structures:**
```c
struct page_index_entry {
    unsigned long addr;          // page-aligned virtual address
    unsigned long len;           // length of contiguous region
    off_t file_offset;           // offset to compressed data in dump file
    unsigned long compressed_len; // size of compressed data
};

struct page_index {
    struct page_index_entry *entries;
    int nr_entries;
    int capacity;
    unsigned char *served;       // bitmap: which pages have been served
};
```

**Operations:**
- `page_index_build(int dump_fd)` - scan dump file, record offsets for each
  `vm_region`
- `page_index_lookup(struct page_index *idx, unsigned long addr)` - binary
  search by address
- `page_index_mark_served(struct page_index *idx, unsigned long addr, unsigned long len)` -
  mark pages as served
- `page_index_next_unserved(struct page_index *idx)` - get next unserved entry
  for prefetch
- `page_index_destroy(struct page_index *idx)` - free memory

**Building the index:**
```c
// Scan dump file: read vm_region headers, record data positions, skip data
while (read(dump_fd, &vmr, sizeof(vmr)) == sizeof(vmr)) {
    entry.addr = vmr.addr;
    entry.len = vmr.len;
    entry.file_offset = lseek(dump_fd, 0, SEEK_CUR);
    entry.compressed_len = get_compressed_size(dump_fd);
    page_index_add(idx, &entry);
    lseek(dump_fd, entry.compressed_len, SEEK_CUR);  // skip data
}
// Sort by address for binary search
qsort(idx->entries, idx->nr_entries, sizeof(*idx->entries), cmp_by_addr);
```

**Note:** Regions in the dump file may span multiple pages. When a fault occurs
for a single page within a larger region, the handler decompresses the whole
region and extracts the relevant page. This is a tradeoff for implementation
simplicity; per-page storage can be added later.

**Complexity:** Medium

---

### Phase 5: Lazy-Pages Handler Thread

**Files:** New `lazy-pages.c` / `lazy-pages.h`

This is the core of the implementation.

**Handler context:**
```c
struct lazy_pages_ctx {
    int uffd;                    // userfaultfd file descriptor
    int dump_fd;                 // dump file descriptor
    struct page_index *index;    // page index for lookups
    pid_t target_pid;            // for monitoring target liveness
    unsigned long pages_total;   // total pages to restore
    unsigned long pages_served;  // pages already served
    char *decomp_buf;            // decompression buffer
    char *page_buf;              // page-aligned buffer for UFFDIO_COPY
};
```

**Main loop:**
```c
void *lazy_pages_handler(void *arg)
{
    struct lazy_pages_ctx *ctx = arg;
    struct pollfd pfd = { .fd = ctx->uffd, .events = POLLIN };
    struct uffd_msg msg;

    while (ctx->pages_served < ctx->pages_total) {
        int ret = poll(&pfd, 1, PREFETCH_TIMEOUT_MS);

        if (ret > 0 && (pfd.revents & POLLIN)) {
            // Handle page fault
            read(ctx->uffd, &msg, sizeof(msg));

            if (msg.event == UFFD_EVENT_PAGEFAULT) {
                unsigned long addr = msg.arg.pagefault.address & ~(PAGE_SIZE - 1);
                handle_page_fault(ctx, addr);
            } else if (msg.event == UFFD_EVENT_UNMAP ||
                       msg.event == UFFD_EVENT_REMOVE) {
                // Target unmapped these pages, skip them
                mark_range_done(ctx, msg.arg.remove.start,
                               msg.arg.remove.end - msg.arg.remove.start);
            }
        } else if (ret == 0) {
            // Timeout: prefetch pages in background
            prefetch_pages(ctx, PREFETCH_BATCH_SIZE);
        } else if (pfd.revents & POLLHUP) {
            // Target died
            break;
        }
    }

    // Cleanup
    cleanup_lazy_pages(ctx);
    return NULL;
}
```

**Fault resolution:**
```c
static int handle_page_fault(struct lazy_pages_ctx *ctx, unsigned long addr)
{
    struct page_index_entry *entry = page_index_lookup(ctx->index, addr);
    if (!entry)
        return -1;  // unknown address

    // Read and decompress region from dump file
    // Uses dump_read() + compress_read() to handle encryption and compression
    // transparently (see "Compression and Encryption Compatibility" section)
    if (ctx->preloaded_dump) {
        // Encrypted: read from preloaded memory buffer
        memcpy(ctx->decomp_buf, ctx->preloaded_dump + entry->file_offset,
               entry->compressed_len);
    } else {
        // Non-encrypted: seek and read from file
        lseek(ctx->dump_fd, entry->file_offset, SEEK_SET);
    }
    compress_read(ctx->page_buf, entry->len, dump_read, ctx->dump_fd);

    // Calculate offset within the region for this specific page
    size_t page_offset = addr - entry->addr;

    // Inject page into target via userfaultfd
    struct uffdio_copy copy = {
        .dst = addr,
        .src = (unsigned long)(ctx->page_buf + page_offset),
        .len = PAGE_SIZE,
        .mode = 0,  // wake the faulting thread
    };
    ioctl(ctx->uffd, UFFDIO_COPY, &copy);

    ctx->pages_served++;
    return 0;
}
```

**Background prefetch:**
```c
static void prefetch_pages(struct lazy_pages_ctx *ctx, int batch_size)
{
    for (int i = 0; i < batch_size; i++) {
        struct page_index_entry *entry = page_index_next_unserved(ctx->index);
        if (!entry)
            break;

        // Read and decompress (same logic as fault resolution)
        if (ctx->preloaded_dump) {
            memcpy(ctx->decomp_buf, ctx->preloaded_dump + entry->file_offset,
                   entry->compressed_len);
        } else {
            lseek(ctx->dump_fd, entry->file_offset, SEEK_SET);
        }
        compress_read(ctx->page_buf, entry->len, dump_read, ctx->dump_fd);

        struct uffdio_copy copy = {
            .dst = entry->addr,
            .src = (unsigned long)ctx->page_buf,
            .len = entry->len,
            .mode = UFFDIO_COPY_MODE_DONTWAKE,  // no thread to wake
        };

        int ret = ioctl(ctx->uffd, UFFDIO_COPY, &copy);
        if (ret == 0) {
            page_index_mark_served(ctx->index, entry->addr, entry->len);
            ctx->pages_served += entry->len / PAGE_SIZE;
        }
        // EEXIST means page was already faulted in -- that's fine
    }
}
```

**Complexity:** High

---

### Phase 6: Modified Restore Flow

**Files:** `memcr.c`

**New function `execute_parasite_restore_lazy()`:**
```c
static int execute_parasite_restore_lazy(pid_t pid)
{
    int uffd;
    int status;
    struct page_index *index;

    // 1. Tell parasite to set up userfaultfd and register VMAs
    uffd = setup_target_uffd(pid, vmas, nr_vmas);
    if (uffd < 0) {
        err("setup_target_uffd() failed, falling back to eager restore\n");
        return execute_parasite_restore(pid);  // fallback
    }

    // 2. Restore memory protections
    target_mprotect_on(pid);

    // 3. Tell parasite to exit
    target_cmd_end(pid);
    parasite_status_wait(&status);

    if (WIFSIGNALED(status))
        return 1;

    // 4. Clean up parasite blob
    execute_blob(&ctx, munmap_blob, munmap_blob_size,
                 (unsigned long)ctx.blob, sizeof(parasite_blob));

    // 5. Restore signals and context
    signals_unblock(pid);
    ctx_restore(pid);

    // 6. Build page index from dump file
    index = page_index_build(pid);

    // 7. Resume target process -- IT RUNS NOW
    unseize_target(pid);

    // 8. Launch lazy pages handler thread
    launch_lazy_pages_handler(uffd, index, pid);

    return 0;
}
```

**Helper for uffd setup:**
```c
static int setup_target_uffd(pid_t pid, struct vm_area *vmas, int nr_vmas)
{
    int cd = parasite_connect(pid);
    if (cd < 0)
        return -1;

    parasite_write(cd, &(char){CMD_SETUP_UFFD}, 1);

    // Send VMA ranges to register (only MAP_PRIVATE|MAP_ANONYMOUS)
    for (int i = 0; i < nr_vmas; i++) {
        if (vmas[i].flags == FLAG_ANON || vmas[i].flags == FLAG_STACK
            || vmas[i].flags == FLAG_HEAP) {
            struct uffd_register_req req = {
                .addr = vmas[i].start,
                .len = vmas[i].end - vmas[i].start,
            };
            parasite_write(cd, &req, sizeof(req));
        }
    }

    // Signal end of VMA list
    shutdown(cd, SHUT_WR);

    // Receive the uffd via SCM_RIGHTS
    int uffd = recv_fd(cd);
    close(cd);
    return uffd;
}
```

**Modify restore dispatch:**
```c
static int cmd_restore(pid_t pid)
{
    if (lazy_pages)
        return cmd_restore_lazy(pid);

    // ... existing eager restore code unchanged ...
}
```

**Complexity:** Medium

---

### Phase 7: Cleanup, Edge Cases, and Testing

**Files:** `memcr.c`, `lazy-pages.c`, `tests/`

**Tasks:**

1. **Kernel capability check:** At startup, verify `userfaultfd` is available
   by attempting to create one. Handle `EPERM` (unprivileged userfaultfd
   disabled) and `ENOSYS` (kernel too old).

2. **Graceful fallback:** If uffd setup fails at any point, fall back to eager
   restore with a warning message.

3. **Target death handling:** If the target process dies while the handler is
   active, detect via `POLLHUP` on the uffd or check `/proc/<pid>` and clean
   up gracefully.

4. **Signal safety:** Ensure the handler thread masks signals properly and
   doesn't interfere with the daemon's signal handling.

5. **Service mode integration:** In service mode, the handler thread must work
   correctly within the worker process model. The worker should not exit until
   the handler completes or the target dies.

6. **Compression compatibility:** Ensure the page index and fault handler work
   with all compression backends (none, lz4, zstd).

7. **Encryption compatibility:** Ensure compatibility with the optional
   encryption layer (`libencrypt.so`).

8. **Test additions:**
   - Modify `tests/run.sh` to run tests with `--lazy-pages`
   - New test: verify process correctness after lazy restore
   - New test: verify background prefetch completes all pages
   - New test: verify graceful fallback when uffd unavailable

**Complexity:** Medium

---

## Compression and Encryption Compatibility

The lazy-pages handler must work correctly with compressed and encrypted dump
files. This section describes how the existing layers interact with random-access
page resolution.

### Current Dump File Format

The dump file is a sequential stream of records:

```
[vm_region header][compressed/encrypted data][vm_region header][compressed/encrypted data]...
```

When compression is enabled (lz4 or zstd), each region's data is prefixed with
a `uint32_t` compressed length, followed by the compressed payload:

```
[vm_region (16 bytes)][uint32_t comp_len][compressed data (comp_len bytes)]...
```

When encryption is enabled via `libencrypt.so`, the `dump_read()`/`dump_write()`
functions are overridden by the preloaded library, which wraps all I/O in a
transparent encryption layer.

### How Lazy-Pages Handles Compression

The lazy-pages handler uses the **same** `compress_read()` function pointer and
`dump_read()` wrapper that the eager restore path uses. The difference is that
instead of reading the entire dump sequentially in `target_set_pages()`, the
handler seeks to specific offsets to service individual page faults.

**Page index building (Phase 4):**

During index construction, the handler reads the dump file sequentially once
(using `dump_read()`), recording for each `vm_region`:
- The file offset to the start of its compressed payload (i.e., the position
  of the `uint32_t comp_len` prefix for lz4/zstd, or the raw data for plain)
- The decompressed length (from `vm_region.len`)

This sequential scan is compatible with both compression and encryption since it
reads the file in order.

**Fault resolution:**

When a page fault occurs, the handler must read a specific region from the dump.
The approach depends on the encryption mode:

1. **No encryption (most common):** Use `lseek()` + `dump_read()` to seek to
   the recorded file offset and read the compressed payload. Then call
   `compress_read()` to decompress into a page buffer.

2. **With encryption:** Encryption libraries may not support random access
   (many use stream ciphers or block-chaining modes). Two strategies:

   a. **Pre-read into memory:** At index-build time, read and decrypt the
      entire dump file into an in-memory buffer. The page index then points
      into this buffer. This uses more memory but avoids random-access issues
      with the encryption layer.

   b. **Sequential read with caching:** Open the dump file once, read it
      sequentially through the encryption layer, and cache all decompressed
      regions in memory (keyed by address). Faults are then served from the
      cache without further file I/O.

   Strategy (a) is simpler and recommended for the initial implementation since
   encrypted dumps are typically smaller (encryption implies sensitive data,
   which tends to be constrained in size). A large dump would need strategy (b)
   or a seekable encryption scheme.

### Implementation Detail

The `lazy_pages_ctx` structure will hold:

```c
struct lazy_pages_ctx {
    int dump_fd;                  // dump file descriptor (via dump_open)
    struct page_index *index;     // address -> offset mapping
    char *decomp_buf;            // buffer for compressed data
    char *page_buf;              // buffer for decompressed page data

    // For encrypted dumps: pre-loaded memory buffer
    char *preloaded_dump;        // NULL if not encrypted
    size_t preloaded_dump_size;  // size of preloaded buffer
};
```

When encryption is active (`lib__read != NULL`), the handler preloads the
entire dump into `preloaded_dump` during initialization. The page index file
offsets then serve as offsets into this buffer rather than requiring `lseek()`
on the encrypted file descriptor.

When only compression is active (no encryption), standard `lseek()` +
`dump_read()` works because the underlying file is a regular file that supports
seeking, and the compression layer operates on individual records (not a
continuous stream).

### Summary

| Configuration | Random Access Strategy |
|---------------|----------------------|
| Plain (no compression, no encryption) | `lseek()` + `read()` directly |
| Compressed only (lz4/zstd) | `lseek()` to offset, read `uint32_t` + payload, decompress |
| Encrypted only | Preload entire decrypted dump into memory at init |
| Compressed + Encrypted | Preload + decompress individual regions from memory buffer |

---

## File Summary

| File | Action | Purpose |
|------|--------|---------|
| `memcr.c` | Modify | CLI option, restore flow dispatch, uffd setup helper |
| `memcr.h` | Modify | New command enum, uffd request struct |
| `memcrclient_proto.h` | Modify | Add lazy-pages flag to service protocol |
| `parasite.c` | Modify | CMD_SETUP_UFFD handler, send_fd helper |
| `arch/syscall.h` | Modify | Declare new syscall wrappers |
| `arch/syscall.c` | Modify | Implement sys_userfaultfd, sys_ioctl, sys_sendmsg |
| `lazy-pages.c` | **New** | Lazy-pages handler thread implementation |
| `lazy-pages.h` | **New** | Lazy-pages public interface |
| `page-index.c` | **New** | Page dump index for random-access lookups |
| `page-index.h` | **New** | Page index public interface |
| `Makefile` | Modify | Add new source files to build |
| `tests/run.sh` | Modify | Add lazy-pages test runs |

---

## Limitations (Initial Implementation)

- **VMA types:** Only `MAP_PRIVATE | MAP_ANONYMOUS` (stack, heap, anonymous
  mappings). File-backed mappings are restored eagerly.
- **Kernel version:** Requires Linux >= 4.11 (non-cooperative userfaultfd).
- **Granularity:** Page faults are resolved at region granularity (entire
  compressed region decompressed per fault). Single-page extraction within a
  multi-page region may cause extra decompression overhead.
- **No remote pages:** All pages served from local dump file only.
- **Single target:** One target process per lazy-pages handler instance.
- **Privileges:** May require `CAP_SYS_PTRACE` or
  `/proc/sys/vm/unprivileged_userfaultfd = 1` depending on kernel
  configuration.

## Future Enhancements

- **Hybrid eager+lazy mode:** Eagerly restore stack/heap entry pages, lazy for
  the rest, to reduce immediate stall probability on resume.
- **Page-granular dumps:** Store pages individually in the dump file for
  efficient single-page resolution without decompressing entire regions.
- **Network page server:** Serve pages from a remote host for lazy live
  migration across machines.
- **File-backed VMA support:** Extend userfaultfd registration to file-mapped
  memory (requires kernel >= 4.11 for shmem, hugetlbfs).
- **Access-pattern prefetch:** Track page fault patterns to predict and
  prefetch intelligently (sequential, strided, etc.).
- **Huge page support:** Handle 2MB/1GB transparent huge pages.
- **Multi-page fault resolution:** When a fault occurs, proactively inject
  nearby pages from the same region to reduce future faults (spatial locality).

---

## References

- [CRIU Userfaultfd documentation](https://criu.org/Userfaultfd)
- [userfaultfd(2) man page](https://man7.org/linux/man-pages/man2/userfaultfd.2.html)
- [Linux kernel userfaultfd docs](https://www.kernel.org/doc/html/latest/admin-guide/mm/userfaultfd.html)
- [ioctl_userfaultfd(2)](https://man7.org/linux/man-pages/man2/ioctl_userfaultfd.2.html)
- CRIU source: `criu/uffd.c`, `criu/page-xfer.c`
