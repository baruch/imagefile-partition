#define _GNU_SOURCE

#include <dlfcn.h>

#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

typedef struct part_t {
	uint8_t status;
	uint8_t start_head;
	uint8_t start_sector; // Two high bits are are high bits of cylinder
	uint8_t start_cylinder;
	uint8_t part_type;
	uint8_t end_head;
	uint8_t end_sector;
	uint8_t end_cylinder;
	uint32_t start_lba; // Little Endian
	uint32_t num_sectors; // Little Endian
} __attribute__((packed)) part_t;

static int initialized;
static dev_t file_dev;
static ino_t file_ino;
static off_t partition_size_bytes;
static off_t partition_base_bytes;
static off_t partition_end_bytes;

static off_t (*real_lseek)(int, off_t, int);
static int (*real_stat)(int ver, const char *path, struct stat *buf);
static int (*real_fstat)(int ver, int fd, struct stat *buf);

static int get_partition(const char *name, int part_num)
{
	int ret;
	int fd;
	uint8_t mbr[512];
	part_t *part;
	int exit_code = -1;

	fd = open(name, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read(fd, mbr, sizeof(mbr));
	if (ret != sizeof(mbr)) {
		fprintf(stderr, "Failed to read MBR, ret=%d: %s\n", ret, strerror(errno));
		goto Exit;
	}

	if (mbr[511] != 0xAA || mbr[510] != 0x55) {
		fprintf(stderr, "MBR signature is missing (%02X %02X)\n", mbr[510], mbr[511]);
		goto Exit;
	}

	if (part_num < 4) {
		part = (part_t*)&mbr[446 + (part_num-1) * 16];
	} else {
		fprintf(stderr, "Don't know how to handle extended partitions yet!\n");
		goto Exit;
	}

	uint32_t start_lba = htole32(part->start_lba);
	uint32_t num_sectors = htole32(part->num_sectors);

	if (start_lba == 0 || num_sectors == 0) {
		fprintf(stderr, "start lba (%u) or num sectors (%u) are invalid, can't handle it\n", start_lba, num_sectors);
		goto Exit;
	}

	partition_base_bytes = start_lba * 512;
	partition_size_bytes = num_sectors * 512;
	partition_end_bytes = partition_base_bytes + partition_size_bytes;

	fprintf(stderr, "Partition detected, starting at %"PRIu64" size %"PRIu64"\n", partition_base_bytes, partition_size_bytes);

	exit_code = 0;
Exit:
	close(fd);
	return exit_code;
}

static void init(void)
{
	if (initialized)
		return;
	initialized = 1;

	/* Fill the file_dev and file_info values, to detect the right file */
	char *filename = getenv("P_FILE");
	if (!filename) {
		fprintf(stderr, "Filename not given in PARTITION_FILE, aborting.\n");
		abort();
	}
	struct stat buf;
	int ret;

	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open file '%s': %s\n", filename, strerror(errno));
		abort();
	}
	ret = fstat(fd, &buf);
	if (ret < 0) {
		fprintf(stderr, "Error in `stat`: %s\n", strerror(errno));
		abort();
	}
	close(fd);

	file_dev = buf.st_dev;
	file_ino = buf.st_ino;

	/* Find the partition inside the file, based on the partition table */
	char *part_num_str = getenv("P_NUM");
	if (!part_num_str) {
		fprintf(stderr, "Partition number not given in PARTITION_NUM, aborting.\n");
		abort();
	}

	int part_num = atoi(part_num_str);
	if (part_num < 1 || part_num > 8) {
		fprintf(stderr, "Invalid partition '%s'\n", part_num_str);
		abort();
	}

	ret = get_partition(filename, part_num);
	if (ret < 0) {
		fprintf(stderr, "Failed to get partition data\n");
		abort();
	}
}

off_t lseek(int fd, off_t offset, int whence)
{
	if (!real_lseek) {
		real_lseek = dlsym(RTLD_NEXT, "lseek");
		if (!real_lseek) {
			fprintf(stderr, "Error in `dlsym` for lseek: %s\n", dlerror());
			abort();
		}

		init();
	}

	switch (whence) {
		case SEEK_SET:
			offset += partition_base_bytes;
			if (offset > partition_end_bytes)
				offset = partition_end_bytes;
			break;
		case SEEK_CUR:
			/* I could verify that we do not go out of bounds, but I'm lazy for now */
			break;
		case SEEK_END:
			/* Go to the partition end */
			whence = SEEK_SET;
			offset = partition_end_bytes;
			break;
		default:
			errno = EINVAL;
			return -1;
	}

	off_t ret = real_lseek(fd, offset, whence);
	if (ret < 0)
		return ret;

	if (ret >= partition_base_bytes)
		return ret - partition_base_bytes;

	fprintf(stderr, "Invalid location seeked into %d\n", ret);
	abort();
	return -1;
}

static int fixup_statbuf(int ret, struct stat *buf)
{
	if (ret == 0 &&
	    S_ISREG(buf->st_mode) &&
		buf->st_dev == file_dev &&
		buf->st_ino == file_ino)
	{
		buf->st_size = partition_size_bytes;
	}

	return ret;
}

int __xstat(int ver, const char *path, struct stat *buf)
{
	if (!real_stat) {
		real_stat = dlsym(RTLD_NEXT, "__xstat");
		if (!real_stat) {
			fprintf(stderr, "Error in `dlsym` for stat: %s\n", dlerror());
			abort();
		}

		init();
	}

	int ret = real_stat(ver, path, buf);
	return fixup_statbuf(ret, buf);
}

int __fxstat (int ver, int fd, struct stat *buf)
{
	if (!real_fstat) {
		real_fstat = dlsym(RTLD_NEXT, "__fxstat");
		if (!real_fstat) {
			fprintf(stderr, "Error in `dlsym` for fstat: %s\n", dlerror());
			abort();
		}

		init();
	}

	int ret = real_fstat(ver, fd, buf);
	return fixup_statbuf(ret, buf);
}

int fallocate(int fd, int mode, off_t offset, off_t len)
{
	/* TODO: Make it work only inside the partition */
	return 0;
}
