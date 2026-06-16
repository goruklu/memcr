/*
 * Copyright 2023 Comcast Cable Communications Management, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef __MEMCR_CLIENT_PROTO_H__
#define __MEMCR_CLIENT_PROTO_H__

typedef enum {
  MEMCR_CHECKPOINT = 100,
  MEMCR_RESTORE,
  MEMCR_CMDS_V2
} memcr_svc_cmd;

typedef enum {
  MEMCR_CHECKPOINT_DUMPDIR = 200,
  MEMCR_CHECKPOINT_COMPRESS_ALG,
} memcr_svc_checkpoint_options;

#define MEMCR_DUMPDIR_LEN_MAX	1024

typedef enum {
  MEMCR_COMPRESS_NONE = 0,
  MEMCR_COMPRESS_LZ4,
  MEMCR_COMPRESS_ZSTD
} memcr_compress_alg;

struct service_command {
  memcr_svc_cmd cmd;
  pid_t pid;
} __attribute__((packed));

struct service_options {
  int is_dump_dir;
  char dump_dir[MEMCR_DUMPDIR_LEN_MAX];
  int is_compress_alg;
  memcr_compress_alg compress_alg;
};

typedef enum {
  MEMCR_OK = 0,
  MEMCR_ERROR_GENERAL = -1,
  MEMCR_INVALID_PID = -2
} memcr_svc_response;

struct service_response {
  memcr_svc_response resp_code;
} __attribute__((packed));

#endif /* __MEMCR_CLIENT_PROTO_H__ */
