/*
 * Copyright (C) 2023 Liberty Global Service B.V.
 * Copyright (C) 2023-2025 Mariusz Kozłowski
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
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include <openssl/evp.h>
#include <openssl/rand.h>


#define IO_SIZE 4096
#define ROUND_UP(n, m) ((n + m) & ~(m - 1))
#define GCM_NONCE_SIZE 12
#define GCM_TAG_SIZE 16
#define GCM_FRAME_MAGIC 0x4d474331U

#define VERBOSE 0

#define log(...) fprintf(stdout, "[x] " __VA_ARGS__)
#if VERBOSE == 1
#define dbg(...) fprintf(stdout, "[x] " __VA_ARGS__)
#else
#define dbg(...)
#endif
#define err(...) fprintf(stderr, "[x] " __VA_ARGS__)


static const EVP_CIPHER *cipher;
static EVP_CIPHER_CTX *ctx;
static int block_size;
static int key_size;
static unsigned char key[EVP_MAX_KEY_LENGTH];
static unsigned char iv[16];
static int gcm_mode;

struct gcm_frame {
	uint32_t magic;
	uint32_t len;
	uint64_t offset;
	unsigned char nonce[GCM_NONCE_SIZE];
} __attribute__((packed));

/*
 * Prototypes matching memcr.
 */
int lib__open(const char *pathname, int flags, mode_t mode);
int lib__close(int fd);
int lib__read(int fd, void *buf, size_t count);
int lib__write(int fd, const void *buf, size_t count);
int lib__random_access(void);
off_t lib__tell(int fd);
int lib__seek(int fd, off_t offset);
int lib__skip(int fd);
int lib__init(int enable, const char *arg);
int lib__fini(void);


int lib__open(const char *pathname, int flags, mode_t mode)
{
	dbg("%s(%s, 0x%x, 0x%x)\n", __func__, pathname, flags, mode);

	if (!cipher)
		return open(pathname, flags, mode);

	if (!gcm_mode) {
		ctx = EVP_CIPHER_CTX_new();
		if (!ctx) {
			err("EVP_CIPHER_CTX_new() failed\n");
			return -1;
		}
	}

	return open(pathname, flags, mode);
}

int lib__close(int fd)
{
	dbg("%s(%d)\n", __func__, fd);

	if (!cipher)
		return close(fd);

	if (!gcm_mode) {
		EVP_CIPHER_CTX_free(ctx);
		ctx = NULL;
	}

	return close(fd);
}

int lib__read(int fd, void *buf, size_t count)
{
	int ret;
	unsigned char *p;
	unsigned char data[IO_SIZE];
	unsigned char dec_buf[IO_SIZE + EVP_MAX_BLOCK_LENGTH];
	int dec_len;
	int bytes_read = 0;
	int bytes_todo = ROUND_UP(count, block_size);

	dbg("%s(%d, %p, %d)\n", __func__, fd, buf, (int)count);

	if (!cipher)
		return read(fd, buf, count);

	if (gcm_mode) {
		struct gcm_frame frame;
		unsigned char tag[GCM_TAG_SIZE];
		EVP_CIPHER_CTX *gcm_ctx;
		int out_len;
		off_t offset;

		offset = lseek(fd, 0, SEEK_CUR);
		ret = read(fd, &frame, sizeof(frame));
		if (ret != sizeof(frame)) {
			if (!ret)
				return 0;
			err("short GCM frame header at %ld: %d\n", (long)offset, ret);
			return -1;
		}
		if (frame.magic != GCM_FRAME_MAGIC || frame.len != count ||
		    frame.offset != (uint64_t)offset) {
			err("invalid GCM frame at %ld: magic %x, len %u, offset %llu\n",
		    (long)offset, frame.magic, frame.len,
		    (unsigned long long)frame.offset);
			return -1;
		}
		ret = read(fd, buf, count);
		if (ret != (int)count) {
			err("short GCM ciphertext at %ld: %d/%zu\n",
		    (long)offset, ret, count);
			return -1;
		}
		ret = read(fd, tag, sizeof(tag));
		if (ret != sizeof(tag)) {
			err("short GCM tag at %ld: %d/%zu\n", (long)offset,
		    ret, sizeof(tag));
			return -1;
		}

		gcm_ctx = EVP_CIPHER_CTX_new();
		if (!gcm_ctx || EVP_DecryptInit_ex(gcm_ctx, cipher, NULL, NULL, NULL) != 1 ||
		    EVP_CIPHER_CTX_ctrl(gcm_ctx, EVP_CTRL_GCM_SET_IVLEN,
					sizeof(frame.nonce), NULL) != 1 ||
		    EVP_DecryptInit_ex(gcm_ctx, NULL, NULL, key, frame.nonce) != 1 ||
		    EVP_DecryptUpdate(gcm_ctx, NULL, &out_len,
				      (unsigned char *)&frame, sizeof(frame)) != 1 ||
		    EVP_DecryptUpdate(gcm_ctx, buf, &out_len, buf, count) != 1 ||
		    EVP_CIPHER_CTX_ctrl(gcm_ctx, EVP_CTRL_GCM_SET_TAG,
					GCM_TAG_SIZE, tag) != 1 ||
		    EVP_DecryptFinal_ex(gcm_ctx, (unsigned char *)buf + out_len,
					&out_len) != 1) {
			err("GCM authentication failed\n");
			EVP_CIPHER_CTX_free(gcm_ctx);
			return -1;
		}

		EVP_CIPHER_CTX_free(gcm_ctx);
		return count;
	}

	if (!ctx) {
		err("invalid cipher ctx\n");
		return -1;
	}

	ret = EVP_DecryptInit_ex(ctx, cipher, NULL, key, iv);
	if (ret != 1) {
		err("EVP_DecryptInit_ex() failed: %d\n", ret);
		return -1;
	}

	for (p = (unsigned char *)buf; p < (unsigned char *)buf + count; p += dec_len) {
		int size;

		if (bytes_todo - bytes_read < IO_SIZE)
			size = bytes_todo - bytes_read;
		else
			size = IO_SIZE;

		ret = read(fd, data, size);
		if (ret == 0)
			break;

		if (ret < 0) {
			err("read() failed: %m\n");
			return -1;
		}

		bytes_read += ret;

		ret = EVP_DecryptUpdate(ctx, dec_buf, &dec_len, data, ret);
		if (ret != 1) {
			err("EVP_DecryptUpdate() failed: %d\n", ret);
			return -1;
		}

		memcpy(p, dec_buf, dec_len);
	}

	if (!bytes_read)
		return 0;

	ret = EVP_DecryptFinal_ex(ctx, dec_buf, &dec_len);
	if (ret != 1) {
		err("EVP_DecryptFinal_ex() failed: %d\n", ret);
		return -1;
	}

	memcpy(p, dec_buf, dec_len);

	return count;
}

int lib__write(int fd, const void *buf, size_t count)
{
	int ret;
	unsigned char *p;
	unsigned char data[IO_SIZE + EVP_MAX_BLOCK_LENGTH];
	int enc_len;

	dbg("%s(%d, %p, %d)\n", __func__, fd, buf, (int)count);

	if (!cipher)
		return write(fd, buf, count);

	if (gcm_mode) {
		struct gcm_frame frame = {
			.magic = GCM_FRAME_MAGIC,
			.len = count,
		};
		unsigned char tag[GCM_TAG_SIZE];
		unsigned char *ciphertext;
		EVP_CIPHER_CTX *gcm_ctx;
		off_t offset;
		int out_len;
		int final_len;

		offset = lseek(fd, 0, SEEK_CUR);
		if (offset < 0 || RAND_bytes(frame.nonce, sizeof(frame.nonce)) != 1)
			return -1;
		frame.offset = offset;
		ciphertext = malloc(count);
		if (!ciphertext)
			return -1;
		gcm_ctx = EVP_CIPHER_CTX_new();
		if (!gcm_ctx || EVP_EncryptInit_ex(gcm_ctx, cipher, NULL, NULL, NULL) != 1 ||
		    EVP_CIPHER_CTX_ctrl(gcm_ctx, EVP_CTRL_GCM_SET_IVLEN,
					sizeof(frame.nonce), NULL) != 1 ||
		    EVP_EncryptInit_ex(gcm_ctx, NULL, NULL, key, frame.nonce) != 1 ||
		    EVP_EncryptUpdate(gcm_ctx, NULL, &out_len,
				      (unsigned char *)&frame, sizeof(frame)) != 1 ||
		    EVP_EncryptUpdate(gcm_ctx, ciphertext, &out_len, buf, count) != 1 ||
		    EVP_EncryptFinal_ex(gcm_ctx, ciphertext + out_len, &final_len) != 1 ||
		    out_len + final_len != (int)count ||
		    EVP_CIPHER_CTX_ctrl(gcm_ctx, EVP_CTRL_GCM_GET_TAG,
					GCM_TAG_SIZE, tag) != 1 ||
		    write(fd, &frame, sizeof(frame)) != sizeof(frame) ||
		    write(fd, ciphertext, count) != (int)count ||
		    write(fd, tag, sizeof(tag)) != sizeof(tag)) {
			err("GCM frame write failed\n");
			EVP_CIPHER_CTX_free(gcm_ctx);
			free(ciphertext);
			return -1;
		}

		EVP_CIPHER_CTX_free(gcm_ctx);
		free(ciphertext);
		return count;
	}

	if (!ctx) {
		err("invalid cipher ctx\n");
		return -1;
	}

	ret = EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv);
	if (ret != 1) {
		err("EVP_EncryptInit_ex() failed: %d\n", ret);
		return -1;
	}

	for (p = (unsigned char *)buf; p < (unsigned char *)buf + count; p += IO_SIZE) {
		int size;

		if (p > (unsigned char *)buf + count - IO_SIZE)
			size = count % IO_SIZE;
		else
			size = IO_SIZE;

		ret = EVP_EncryptUpdate(ctx, data, &enc_len, p, size);
		if (ret != 1) {
			err("EVP_EncryptUpdate() failed: %d\n", ret);
			return -1;
		}

		ret = write(fd, data, enc_len);
		if (ret < 0) {
			err("write() failed: %m\n");
			return -1;
		}
	}

	ret = EVP_EncryptFinal_ex(ctx, data, &enc_len);
	if (ret != 1) {
		err("EVP_EncryptFinal_ex() failed: %d\n", ret);
		return -1;
	}

	ret = write(fd, data, enc_len);
	if (ret < 0) {
		err("write() failed: %m\n");
		return -1;
	}

	return count;
}

int lib__init(int enable, const char *arg)
{
	int ret;
	const char *description;

	dbg("%s(%d, %s)\n", __func__, enable, arg);

	if (!enable) {
		gcm_mode = 0;
		log("encryption not enabled\n");
		return 0;
	}
	if (!arg)
		arg = getenv("MEMCR_ENCRYPT_CIPHER");
	if (!arg)
		arg = "aes-128-cbc";

	if (!strcmp(arg, "aes-128-cbc"))
		cipher = EVP_aes_128_cbc();
	else if (!strcmp(arg, "aes-192-cbc"))
		cipher = EVP_aes_192_cbc();
	else if (!strcmp(arg, "aes-256-cbc"))
		cipher = EVP_aes_256_cbc();
	else if (!strcmp(arg, "aes-256-gcm")) {
		cipher = EVP_aes_256_gcm();
		gcm_mode = 1;
	}
	else {
		err("supported ciphers are:\n" \
		    "\taes-128-cbc\n" \
		    "\taes-192-cbc\n" \
		    "\taes-256-cbc\n" \
		    "\taes-256-gcm\n"
		);
		return -1;
	}

	if (!cipher) {
		err("EVP_aes_*_cbc() failed\n");
		return -1;
	}

	if (!strcmp(arg, "aes-192-cbc"))
		key_size = 24;
	else if (!strcmp(arg, "aes-256-cbc") ||
		 !strcmp(arg, "aes-256-gcm"))
		key_size = 32;
	else
		key_size = 16;

	ret = RAND_bytes(key, key_size);
	if (ret != 1) {
		err("RAND_bytes() for key failed: %d\n", ret);
		return -1;
	}

	ret = RAND_bytes(iv, sizeof(iv));
	if (ret != 1) {
		err("RAND_bytes() for iv failed: %d\n", ret);
		return -1;
	}

	description = arg;
	block_size = gcm_mode ? 1 : 16;

	log("encrypt: %s, block size %d\n", description, block_size);

	return 0;
}

int lib__random_access(void)
{
	return gcm_mode;
}

off_t lib__tell(int fd)
{
	return lseek(fd, 0, SEEK_CUR);
}

int lib__seek(int fd, off_t offset)
{
	return lseek(fd, offset, SEEK_SET) == offset ? 0 : -1;
}

int lib__skip(int fd)
{
	struct gcm_frame frame;

	if (!gcm_mode)
		return -1;
	if (read(fd, &frame, sizeof(frame)) != sizeof(frame) ||
	    frame.magic != GCM_FRAME_MAGIC ||
	    frame.offset != (uint64_t)(lseek(fd, 0, SEEK_CUR) - sizeof(frame)))
		return -1;
	return lseek(fd, frame.len + GCM_TAG_SIZE, SEEK_CUR) < 0 ? -1 : 0;
}

int lib__fini(void)
{
	dbg("%s()\n", __func__);

	return 0;
}
