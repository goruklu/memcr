/*
 * Copyright 2023 Comcast Cable Communications Management, LLC
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
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

#include "memcr.h"
#include "memcrclient_proto.h"
#include "libmemcrclient.h"

#define CMD_LEN_MAX		(sizeof(struct service_command) + (2*sizeof(memcr_svc_checkpoint_options)) \
						 + MEMCR_DUMPDIR_LEN_MAX + sizeof(memcr_compress_alg))

static void print_version(void)
{
	fprintf(stdout, "[i] memcr-client version %s\n", GIT_VERSION);
}

static int send_cmd_v2(int cd, memcr_svc_cmd cmd, pid_t pid, const struct service_options *opt)
{
	int ret;
	struct service_response resp = {0};

	size_t cmd_size = 0;
	unsigned char cmd_buf[CMD_LEN_MAX];

	struct service_command cmd_cr = {.cmd = cmd, .pid = pid};
	memcpy(cmd_buf, &cmd_cr, sizeof(struct service_command));
	cmd_size += sizeof(struct service_command);

	if (opt && opt->is_dump_dir) {
		memcr_svc_checkpoint_options opt_id = MEMCR_CHECKPOINT_DUMPDIR;
		memcpy(cmd_buf + cmd_size, &opt_id, sizeof(memcr_svc_checkpoint_options));
		cmd_size += sizeof(memcr_svc_checkpoint_options);
		strncpy((char *)cmd_buf + cmd_size, opt->dump_dir, MEMCR_DUMPDIR_LEN_MAX);
		cmd_size += strlen(opt->dump_dir) + 1;
	}

	if (opt && opt->is_compress_alg) {
		memcr_svc_checkpoint_options opt_id = MEMCR_CHECKPOINT_COMPRESS_ALG;
		memcpy(cmd_buf + cmd_size, &opt_id, sizeof(memcr_svc_checkpoint_options));
		cmd_size += sizeof(memcr_svc_checkpoint_options);
		memcpy(cmd_buf + cmd_size, &opt->compress_alg, sizeof(memcr_compress_alg));
		cmd_size += sizeof(memcr_compress_alg);
	}

	struct service_command cmd_v2 = {.cmd = MEMCR_CMDS_V2, .pid = cmd_size};

	ret = write(cd, &cmd_v2, sizeof(cmd_v2));
	if (ret != sizeof(cmd_v2)) {
		fprintf(stderr, "%s() write v2 header failed: ret %d, errno %m\n", __func__, ret);
		return -1;
	}

	ret = write(cd, cmd_buf, cmd_size);
	if ((size_t)ret != cmd_size) {
		fprintf(stderr, "%s() write v2 payload failed: ret %d, errno %m\n", __func__, ret);
		return -1;
	}

	ret = read(cd, &resp, sizeof(struct service_response));
	if (ret != sizeof(struct service_response)) {
		fprintf(stderr, "%s() read response failed: ret %d, errno %m\n", __func__, ret);
		return -1;
	}

	fprintf(stdout, "Procedure finished with %s status.\n", MEMCR_OK == resp.resp_code ? "OK" : "ERROR");

	return resp.resp_code;
}

static void usage(const char *name, int status)
{
	fprintf(status ? stderr : stdout,
		"%s -l PORT|PATH -p PID [-c -r [-d DIR] [-z ALG]] [-v] [-V]\n" \
		"options: \n" \
		"  -h --help\t\thelp\n" \
		"  -l --location\t\tTCP port number of localhost memcr service\n" \
		"\t\t\t or filesystem path to memcr service UNIX socket\n" \
		"  -p --pid\t\tprocess ID to be checkpointed / restored\n" \
		"  -c --checkpoint\tsend checkpoint command to memcr service\n" \
		"  -r --restore\t\tsend restore command to memcr service\n" \
		"  -d --dir\t\tdir where memory dump is stored (max length %d)\n" \
		"  -z --compress\t\tcompress memory dump with selected algorithm: 'lz4', 'zstd' or disable with 'none'\n" \
		"  -v --v1\t\tforce using old (v1) protocol\n" \
		"  -V --version\t\tprint version and exit\n",
		name, MEMCR_DUMPDIR_LEN_MAX);
	exit(status);
}

int main(int argc, char *argv[])
{
	int ret = 0, cd, opt;
	int checkpoint = 0;
	int restore = 0;
	int option_index;
	char *comm_location = NULL;
	int pid = 0;
	const char *dump_dir_s = NULL;
	const char *compress_alg_s = NULL;
	struct service_options checkpoint_options = {0};
	int v1 = 0;

	struct option long_options[] = {
		{ "help",       0,  0,  'h'},
		{ "location",   1,  0,  'l'},
		{ "pid",        1,  0,  'p'},
		{ "checkpoint", 0,  0,  'c'},
		{ "dir",        1,  0,  'd'},
		{ "compress",   1,  0,  'z'},
		{ "restore",    0,  0,  'r'},
		{ "version",    0,  0,  'V'},
		{ "v1",         0,  0,  'v'},
		{ NULL,         0,  0,  0  }
	};

	while ((opt = getopt_long(argc, argv, "hl:p:crVd:z:v", long_options, &option_index)) != -1) {
		switch (opt) {
			case 'h':
				usage(argv[0], 0);
				break;
			case 'l':
				comm_location = optarg;
				break;
			case 'p':
				pid = atoi(optarg);
				break;
			case 'c':
				checkpoint = 1;
				break;
			case 'd':
				dump_dir_s = optarg;
				break;
			case 'z':
				compress_alg_s = optarg;
				break;
			case 'r':
				restore = 1;
				break;
			case 'V':
				print_version();
				exit(0);
			case 'v':
				v1 = 1;
				break;
			default: /* '?' */
				usage(argv[0], 1);
				break;
		}
	}

	if (!pid || !comm_location) {
		fprintf(stderr, "Incorrect arguments provided!\n");
		usage(argv[0], 1);
		return -1;
	}

	if (!checkpoint && !restore) {
		fprintf(stderr, "You have to provide a command (checkpoint or restore or both)!\n");
		usage(argv[0], 1);
		return -1;
	}

	if (!checkpoint && (dump_dir_s || compress_alg_s)) {
		fprintf(stderr, "Dir dump and compression options are available only for checkpoint!\n");
		usage(argv[0], 1);
		return -1;
	}

	if (dump_dir_s) {
		if (strlen(dump_dir_s) >= MEMCR_DUMPDIR_LEN_MAX) {
			fprintf(stderr, "Dir dump path too long!\n");
			usage(argv[0], 1);
			return -1;
		}

		strcpy(checkpoint_options.dump_dir, dump_dir_s);
		checkpoint_options.is_dump_dir = 1;
	}

	if (compress_alg_s) {
		checkpoint_options.is_compress_alg = 1;
		if (strcmp(compress_alg_s, "none") == 0)
			checkpoint_options.compress_alg = MEMCR_COMPRESS_NONE;
		else if (strcmp(compress_alg_s, "lz4") == 0)
			checkpoint_options.compress_alg = MEMCR_COMPRESS_LZ4;
		else if (strcmp(compress_alg_s, "zstd") == 0)
			checkpoint_options.compress_alg = MEMCR_COMPRESS_ZSTD;
		else {
			fprintf(stderr, "Incorrect compression algorithm provided!\n");
			usage(argv[0], 1);
			return -1;
		}
	}

	if (checkpoint) {
		fprintf(stdout, "Will checkpoint %d.\n", pid);

		cd = memcr_client_connect(comm_location);
		if (cd < 0) {
			fprintf(stderr, "Connection creation failed!\n");
			return cd;
		}

		if (v1)
			ret = memcr_client_checkpoint(cd, pid);
		else
			ret = send_cmd_v2(cd, MEMCR_CHECKPOINT, pid, &checkpoint_options);

		memcr_client_disconnect(cd);
	}

	if (restore) {
		fprintf(stdout, "Will restore %d.\n", pid);

		cd = memcr_client_connect(comm_location);
		if (cd < 0) {
			fprintf(stderr, "Connection creation failed!\n");
			return cd;
		}

		if (v1)
			ret = memcr_client_restore(cd, pid);
		else
			ret = send_cmd_v2(cd, MEMCR_RESTORE, pid, NULL);

		memcr_client_disconnect(cd);
	}

	fprintf(stdout, "Command executed, exiting.\n");
	return ret;
}
