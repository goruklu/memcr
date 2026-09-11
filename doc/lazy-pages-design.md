# Userfaultfd Lazy Page Restore - Design & Implementation

## Overview

This document describes the userfaultfd-based lazy page restoration feature
in memcr, inspired by CRIU's lazy-pages mechanism.

### Problem

Memcr's default restore path is **fully synchronous**: all checkpointed pages
must be uploaded back to the target process before it resumes execution. For
large processes, this means restore latency is proportional to the total
checkpoint size, even if the process only needs a small subset of pages
immediately after resuming.

### Solution

Use Linux's `userfaultfd` mechanism to allow the target process to resume
immediately after checkpoint, with pages delivered **on demand** as the process
accesses them. Pages that were discarded via `MADV_DONTNEED` during checkpoint
are intercepted by a fault handler thread in the memcr daemon and injected back
into the target's address space only when accessed.

### Benefits

- **Reduced freeze time**: Process resumes without waiting for full page upload
  (only the download/checkpoint phase blocks the process)
- **Reduced memory pressure**: Only pages the process actually accesses are
  loaded immediately
- **Background prefetch**: Remaining pages are loaded in the background during
  idle time
- **Transparent**: No changes required to the target process

### Usage

```bash
# Interactive mode with lazy pages
memcr -p <pid> -n --lazy-pages

# With LZ4 compression
memcr -p <pid> -n --lazy-pages --compress lz4

# With encryption
LD_PRELOAD=./libencrypt.so memcr -p <pid> -n --lazy-pages --encrypt

# Service mode with lazy pages
memcr -l 9000 --lazy-pages
```

---

## Architecture

```
+--------------------------------------------------------------+
|                        memcr daemon                           |
|                                                              |
|  +-------------------+    +--------------------------------+ |
|  | setup_target_uffd |    |  lazy-pages handler thread     | |
|  |                   |    |                                | |
|  | - send VMA list   |    |  1. poll(uffd) for faults       | |
|  |   to parasite     |    |  2. lookup page in index       | |
|  | - recv uffd from  |    |  3. read+decompress from dump  | |
|  |   parasite via    |    |     (or from preloaded memory) | |
|  |   recvmsg         |    |  4. UFFDIO_COPY into target    | |
|  |   (SCM_RIGHTS)    |    |  5. background prefetch        | |
|  | - recv status     |    |  6. cleanup when done          | |
|  +-------------------+    +--------------------------------+ |
|           |                            ^                      |
|  +-------------------+                 |                      |
|  | execute_parasite  |                 |                      |
|  | _restore (lazy)   |                 |                      |
|  |                   |                 |                      |
|  | - start handler   |                 |                      |
|  | - mprotect_on     |                 |                      |
|  | - CMD_END         |                 |                      |
|  | - eager excluded  |                 |                      |
|  |   restore via     |                 |                      |
|  |   /proc/pid/mem   |                 |                      |
|  | - ctx_restore     |                 |                      |
|  | - unseize         |                 |                      |
|  +-------------------+                                       |
+--------------------------------------------------------------+
         ^                            |
         | UNIX socket (SCM_RIGHTS)   | userfaultfd
         | parasite sends uffd        | UFFDIO_COPY
         | TO daemon                  |
         |                            v
+--------------------------------------------------------------+
|                      Target Process                           |
|                                                              |
|  Parasite CMD_SETUP_UFFD:                                     |
|  - userfaultfd() -> create uffd (bound to target's mm)        |
|  - UFFDIO_API -> negotiate features                           |
|  - UFFDIO_REGISTER for each VMA (must be in-process)          |
|  - sendmsg(SCM_RIGHTS) -> send uffd to daemon                 |
|  - send status byte back to daemon                            |
|                                                              |
|  +--------+ +--------+ +--------+ +--------+ +--------+     |
|  | stack  | | page 1 | | page 2 | | page 3 | | page 4 |    |
|  |(eager) | | FAULT! | |(prefet)| | FAULT! | |(prefet)|    |
|  +--------+ +--------+ +--------+ +--------+ +--------+     |
|                                                              |
|  Stack/PC VMAs: excluded from uffd, eagerly restored          |
|  Other eligible VMAs: registered with UFFDIO_REGISTER         |
|  (MODE_MISSING)                                               |
+--------------------------------------------------------------+
```

### How It Differs from CRIU

| Aspect | CRIU | memcr |
|--------|------|-------|
| Process state | Fully recreated from scratch | Existing process, pages discarded via MADV_DONTNEED |
| VMA creation | Restored by CRIU | Already exist (only page content is missing) |
| Handler | Separate `criu lazy-pages` daemon process | Thread in the memcr daemon |
| UFFD creation | During process restore | Parasite creates inside target, sends to daemon |
| Page source | Local images, remote page-server, or network | Local dump file (with preload for encrypted) |
| Stack handling | N/A (full restore) | Stack VMA excluded from uffd, eagerly restored |

---

## Detailed Sequence of Operations

```
CHECKPOINT (unchanged):
  1. seize_target()
  2. parasite: scan VMAs, save pages to dump file
  3. parasite: MADV_DONTNEED on saved regions (releases RSS)
  4. process stays frozen, waiting for restore

RESTORE (with --lazy-pages):
  setup_target_uffd() [in daemon]:
    1. connect to parasite, send CMD_SETUP_UFFD
    2. send VMA ranges (excluding stack/PC VMAs)
    3. shutdown(SHUT_WR) -> signal end of VMA list

  parasite cmd_setup_uffd():
    4. userfaultfd(O_NONBLOCK | O_CLOEXEC) -> create uffd (bound to target's mm)
    5. UFFDIO_API -> negotiate features
    6. For each VMA range received:
       UFFDIO_REGISTER -> MODE_MISSING
    7. sendmsg(SCM_RIGHTS) -> send uffd fd to daemon
    8. send status byte back (0 = success, 1 = failure)
    9. close uffd copy (daemon retains the received copy)

  cmd_restore_lazy() [back in daemon]:
   10. switch socket to blocking mode
   11. recvmsg(SCM_RIGHTS) -> receive uffd from parasite
   12. read status byte from parasite (verify registration success)
   13. target_mprotect_on() -> restore original page protections
   14. CMD_END -> parasite exits

   15. Build page_index from dump file:
       - Non-encrypted: record file offsets for lseek+read later
       - Encrypted: read+decrypt+decompress all data into memory (preload)
    16. Mark page-index entries in PC/SP VMAs as excluded from prefetch.
    17. Set up lazy_pages_ctx (buffers, dump path, function pointers)
    18. lazy_pages_start() -> spawn handler thread before any further
        ptrace/blob work. This prevents a deadlock if that work accesses a
        discarded page in a uffd-registered VMA.

   execute_parasite_restore():
    19. parasite_status_wait() -> wait for parasite to exit
    20. munmap parasite_blob area
    21. Eagerly restore PC/SP-excluded VMA pages via /proc/pid/mem:
        - Read preloaded data or use a separate dump fd and buffer
        - The handler owns its own dump fd and buffers; sharing either would
          race with its asynchronous reads and could restore corrupt data
        - pwrite() directly into target's address space
        - Fall back to ptrace POKEDATA when /proc/<pid>/mem is unavailable
    22. signals_unblock()
    23. ctx_restore() -> restore original code and stack via ptrace
    24. unseize_target() -> PROCESS RESUMES IMMEDIATELY

LAZY-PAGES HANDLER THREAD (runs asynchronously):
  loop:
    poll(uffd, timeout=100ms)
    if page fault received:
      addr = fault_address & PAGE_MASK
      entry = page_index_lookup(addr)
      if no entry:
        UFFDIO_ZEROPAGE to unblock the faulting thread
        count a zero fallback; log the first five, then every 500th
      else if entry->data (preloaded):
        copy from memory
      else:
        lseek + compress_read from dump file
      ioctl(uffd, UFFDIO_COPY, {dst=region_addr, src=buf, len=region_len})
      mark region as served
    else if timeout (idle):
      prefetch next batch of unserved pages via UFFDIO_COPY
    else if POLLHUP/POLLERR:
      target died, exit
    if all pages served:
      close(uffd)
      cleanup and exit thread
```

---

## Graceful Fallback

When userfaultfd is not available (kernel lacks `CONFIG_USERFAULTFD`, or the
syscall returns `ENOSYS`/`EPERM`), the lazy restore path falls back to the
standard eager restore automatically:

1. The parasite attempts `userfaultfd()` inside the target process
2. If it fails, the parasite returns -1 (no crash, no SIGILL)
3. The parasite drains the VMA list (keeps protocol in sync) and stays alive
   for the `CMD_SET_PAGES` eager restore fallback
4. `cmd_restore_lazy()` detects the failure and calls `cmd_restore()` instead
5. Returns a special code (`-2`) to `execute_parasite_restore()`
6. `execute_parasite_restore()` recognizes the eager fallback code and enters
   the shared eager completion path, including parasite wait, blob unmap,
   signal restoration, and `ctx_restore()`

This ensures:
- No parasite crash (no `__builtin_trap()` / SIGILL)
- The parasite remains alive for the eager `CMD_SET_PAGES` path
- The eager restore completes the full lifecycle including parasite termination
- The target PC, stack pointer, and overwritten code are restored before it
  resumes

If uffd creation succeeds but `UFFDIO_REGISTER` fails in the parasite, the
parasite drains the VMA list (keeps protocol in sync), sends a failure status
byte, stays alive, and the daemon falls back to eager restore via the same
mechanism.

---

## Key Implementation Details

### Stack/PC VMA Exclusion

VMAs containing the original program counter (`ctx.pc`) or stack pointer
(`ctx.sp`) are **excluded** from userfaultfd registration. This is necessary
because:

1. `ctx_restore()` uses `ptrace(PTRACE_POKEDATA)` to write back the original
   code and stack after the parasite exits
2. Ptrace cannot resolve userfaultfd-registered faults on a stopped process
3. The stack must be fully present when the process resumes execution

Index entries in these excluded VMAs are marked `excluded`, so the handler's
background prefetch skips them. They are restored eagerly via `/proc/<pid>/mem`
using `pwrite()` before `ctx_restore()` runs, with `PTRACE_POKEDATA` as the
fallback when `/proc/<pid>/mem` is unavailable.

### Socket Blocking for SCM_RIGHTS

The parasite socket is created with `SOCK_NONBLOCK` for the normal command
protocol. When receiving the uffd from the parasite via `recvmsg(SCM_RIGHTS)`
and reading the parasite's status response, the daemon switches the socket to
blocking mode (`fcntl(F_SETFL, ~O_NONBLOCK)`).

### Handler Thread Lifecycle and I/O Ownership

The handler thread starts **before** waiting for the parasite and before later
ptrace/blob operations. Those operations can touch discarded pages in a
registered VMA; starting the handler first ensures it can resolve the resulting
userfaultfd event instead of leaving the target blocked.

The handler exclusively owns `lazy_ctx.dump_fd`, `page_buf`, and `decomp_buf`.
The eager restore of excluded VMAs uses a separate buffer and independently
opens the dump file when data was not preloaded. This permits early handler
startup without sharing a file position or buffer between threads.

The thread is detached (`pthread_detach`) and the main thread polls
`lazy_ctx.active` to wait for completion in interactive and service modes.

### Page-Index Synchronization and Zero Fallbacks

The handler thread and the eager excluded-VMA restore can both mark indexed
regions served. `page_index_mark_served()` serializes updates to `served` and
`served_pages` with a mutex.

A fault address with no page-index entry cannot be restored from the dump.
Memcr supplies a zero page with `UFFDIO_ZEROPAGE` to release the faulting
thread. This commonly represents anonymous memory that was never resident at
checkpoint time and was therefore correctly absent from the dump. The handler
counts these zero fallbacks, logs the first five and each 500th thereafter, and
includes the total in its completion message. High counts should be correlated
with application behavior before being treated as missing checkpoint data.

### Parasite Exit Reaping

The generic SIGCHLD handlers can reap the parasite before the dedicated watcher
calls `wait4()`. When this race produces `ECHILD`, the watcher consumes the
status saved by the signal handler and signals normal restore completion.

---

## Compression and Encryption Compatibility

### Dump File Format

```
[vm_region (16 bytes)][data payload][vm_region (16 bytes)][data payload]...

Data payload (plain):     [raw data, vmr.len bytes]
Data payload (compressed):[uint32_t comp_len][compressed data, comp_len bytes]
Data payload (encrypted): [encrypted stream of above]
```

### Strategy by Configuration

| Configuration | Index Build | Fault Resolution |
|---------------|-------------|-----------------|
| Plain | `lseek` + `read` to record offsets, skip data | `lseek` + `read` directly |
| Compressed (lz4/zstd) | `lseek` + `read` comp_len header, skip | `lseek` + `compress_read()` |
| Encrypted | Sequential `read_fn` + `decompress_fn`, store in memory | Serve from `entry->data` |
| Compressed + Encrypted | Sequential read + decompress, store in memory | Serve from `entry->data` |

### Encrypted Mode (Preloading)

AES-CBC (used by `libencrypt.so`) is a stream cipher that maintains internal
state. It does not support random-access reads. Therefore, when encryption is
active:

1. During `page_index_build()`, all regions are read sequentially through the
   encryption layer and decompressed via `compress_read()`
2. Decompressed page data is stored in `entry->data` (heap-allocated per region)
3. The `page_index.preloaded` flag is set to indicate memory-backed mode
4. During fault resolution, `read_region_data()` copies from `entry->data`
   instead of seeking in the file
5. On cleanup, `page_index_destroy()` frees all `entry->data` allocations

This uses more memory (~working set size) but is necessary for correctness.

---

## File Summary

| File | Action | Purpose |
|------|--------|---------|
| `memcr.c` | Modified | CLI option, lazy restore flow, `recv_fd()`, eager stack restore, fallback handling |
| `memcr.h` | Modified | `CMD_SETUP_UFFD` enum, `struct uffd_region_req` |
| `parasite.c` | Modified | `cmd_setup_uffd()` creates uffd + does UFFDIO_API + UFFDIO_REGISTER, `send_fd()` helper |
| `arch/syscall.h` | Modified | Declare `sys_userfaultfd`, `sys_ioctl`, `sys_sendmsg` |
| `arch/syscall.c` | Modified | Implement syscall wrappers including `sys_sendmsg` |
| `lazy-pages.c` | **New** | Handler thread, fault resolution, prefetch, `lazy_pages_serve_page()` |
| `lazy-pages.h` | **New** | `struct lazy_pages_ctx`, public API |
| `page-index.c` | **New** | Index build (plain/compressed/encrypted), binary search lookup |
| `page-index.h` | **New** | `struct page_index`, `struct page_index_entry` |
| `Makefile` | Modified | Added `page-index.o`, `lazy-pages.o` to memcr link |
| `tests/run_lazy_test.sh` | **New** | Functional test suite for lazy-pages |
| `tests/bench_lazy_pages.sh` | **New** | Performance benchmark (eager vs lazy) |

---

## Testing

### Functional Tests (`tests/run_lazy_test.sh`)

```bash
sudo ./tests/run_lazy_test.sh /path/to/memcr
```

Tests lazy-pages with:
- Basic mode (no extra options)
- `--proc-mem`
- `--rss-file`
- `--proc-mem --rss-file`
- LZ4 compression (`--compress lz4`)
- Encryption (AES-128-CBC, AES-256-CBC)
- Combined: LZ4 + encryption
- Fallback behavior (uffd unavailable)

All tests verify memory integrity after lazy restore using `test-malloc`
(16 MB static + 16 MB heap = ~32 MB working set).

### Performance Benchmark (`tests/bench_lazy_pages.sh`)

```bash
sudo ./tests/bench_lazy_pages.sh /path/to/memcr [ITERATIONS]
```

Measures and compares:
- **Freeze time**: How long the process is stopped (download + upload for eager,
  download-only for lazy)
- **Wall time**: Total time including on-demand page serving after resume

Example results (arm64, 32 MB workload, kernel 5.4):

| Mode | Freeze (ms) | Wall (ms) |
|------|-------------|-----------|
| Eager (baseline) | 35 | 43 |
| Lazy-pages | 27 | 659 |
| Lazy-pages + --proc-mem | 26 | 664 |

Lazy-pages eliminates the upload phase from the freeze time. Wall time is
higher because `test-malloc` touches all 32 MB after resume, triggering all
page faults. Real-world workloads that only access a subset of pages would
show much better wall time.

---

## Limitations

- **VMA types:** Only `MAP_PRIVATE | MAP_ANONYMOUS` (stack, heap, anonymous
  mappings) registered with uffd. File-backed mappings are handled by the
  lazy handler but not registered for fault interception.
- **Kernel config:** Requires `CONFIG_USERFAULTFD=y` in the kernel. If not
  present, the syscall returns `ENOSYS` and memcr falls back to eager restore
  automatically.
- **Kernel version:** Requires Linux >= 4.11 (non-cooperative userfaultfd).
- **Granularity:** Faults are resolved at region granularity (entire compressed
  region decompressed per fault, typically up to 1 MB).
- **Memory overhead (encrypted):** Encrypted dumps require preloading all page
  data into memory (~working set size of additional RAM).
- **No remote pages:** All pages served from local dump file only.
- **Single target:** One target process per lazy-pages handler instance.
- **Privileges:** The target process (and therefore the parasite) must be able
  to call `userfaultfd()`. This requires either `vm.unprivileged_userfaultfd=1`
  (sysctl) or the target having `CAP_SYS_PTRACE`. If neither is satisfied,
  `userfaultfd()` returns `-EPERM` and memcr falls back to eager restore.
- **Stack VMA:** Always eagerly restored (not lazy). This is typically small
  (8-132 KB) and required for correct process resumption.

## Future Enhancements

- **Page-granular dumps:** Store pages individually for efficient single-page
  resolution without decompressing entire regions.
- **Network page server:** Serve pages from a remote host for lazy live
  migration across machines.
- **File-backed VMA support:** Extend userfaultfd registration to file-mapped
  memory (requires kernel >= 4.11 for shmem, hugetlbfs).
- **Access-pattern prefetch:** Track page fault patterns to predict and
  prefetch intelligently (sequential, strided, etc.).
- **Huge page support:** Handle 2MB/1GB transparent huge pages.
- **Multi-page fault resolution:** When a fault occurs, proactively inject
  nearby pages from the same region to reduce future faults.
- **Seekable encryption:** Support encryption modes that allow random access
  (e.g., AES-CTR) to avoid the memory preload overhead.

---

## References

- [CRIU Userfaultfd documentation](https://criu.org/Userfaultfd)
- [userfaultfd(2) man page](https://man7.org/linux/man-pages/man2/userfaultfd.2.html)
- [Linux kernel userfaultfd docs](https://www.kernel.org/doc/html/latest/admin-guide/mm/userfaultfd.html)
- [ioctl_userfaultfd(2)](https://man7.org/linux/man-pages/man2/ioctl_userfaultfd.2.html)
- CRIU source: `criu/uffd.c`, `criu/page-xfer.c`
