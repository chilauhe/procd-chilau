#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/utsname.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "../log.h"

#include "init.h"

#define KB(x) (x * 1024)

#define ZRAM_MOD_PATH "/lib/modules/%s/zram.ko"
#define EXT4_MOD_PATH "/lib/modules/%s/ext4.ko"

static unsigned long
proc_meminfo(void)
{
	FILE *fp;
	char line[256];
	char *key;
	long val = KB(8);

	fp = fopen("/proc/meminfo", "r");
	if (fp == NULL) {
		ERROR("Can't open /proc/meminfo: %m\n");
		return errno;
	}

	while (fgets(line, sizeof(line), fp)) {
		key = strtok(line, ":");
		if (strcasecmp(key, "MemTotal"))
			continue;
		val = atol(strtok(NULL, " kB\n"));
		break;
	}
	fclose(fp);

	return val;
}

static int
early_insmod(char *module)
{
	pid_t pid = fork();
	char *modprobe[] = { "/sbin/modprobe", NULL, NULL };

	if (!pid) {
		char *path;
		struct utsname ver;

		uname(&ver);
		path = alloca(strlen(module) + strlen(ver.release) + 1);
		sprintf(path, module, ver.release);
		modprobe[1] = path;
		execvp(modprobe[0], modprobe);
		ERROR("Can't exec %s: %m\n", modprobe[0]);
		exit(-1);
	}

	if (pid <= 0) {
		ERROR("Can't exec %s: %m\n", modprobe[0]);
		return -1;
	} else {
		waitpid(pid, NULL, 0);
	}

	return 0;
}


int
mount_zram_on_tmp(void)
{
	char *mkfs[] = { "/usr/sbin/mkfs.ext4", "-b", "4096", "-F", "-L", "TEMP", "-m", "0", "-O", "uninit_bg,sparse_super,^has_journal", "/dev/zram0", NULL };
	FILE *fp;
	long zramsize;
	pid_t pid;
	int ret;

	if (early_insmod(ZRAM_MOD_PATH) || early_insmod(EXT4_MOD_PATH)) {
		ERROR("failed to insmod zram support\n");
		return -1;
	}

	mkdev("*", 0600);

	zramsize = proc_meminfo();
	//memory size >= 64M takes more memory, otherwise keep original policy.
	//const value of memsize reduced a little to match real devices.
	if(zramsize > 57UL*1000*1000){	
		zramsize /= 3;
	}else if(zramsize > 28UL*1000*1000){
		zramsize = KB(16);
	}else{
		zramsize /= 2;	
	}

	fp = fopen("/sys/block/zram0/disksize", "r+");
	if (fp == NULL) {
		ERROR("Can't open /sys/block/zram0/disksize: %m\n");
		return errno;
	}
	fprintf(fp, "%ld", KB(zramsize));
	fclose(fp);

	pid = fork();
	if (!pid) {
		execvp(mkfs[0], mkfs);
		ERROR("Can't exec %s: %m\n", mkfs[0]);
		exit(-1);
	} else if (pid <= 0) {
		ERROR("Can't exec %s: %m\n", mkfs[0]);
		return -1;
	} else {
		waitpid(pid, NULL, 0);
	}

	ret = mount("/dev/zram0", "/tmp", "ext4", MS_NOSUID | MS_NODEV | MS_NOATIME, "errors=continue,nobarrier");
	if (ret < 0) {
		ERROR("Can't mount /dev/zram0 on /tmp: %m\n");
		return errno;
	}

	LOG("Using up to %ld kB of RAM as ZRAM storage on /mnt\n", zramsize);

	ret = chmod("/tmp", 01777);
	if (ret < 0) {
		ERROR("Can't set /tmp mode to 1777: %m\n");
		return errno;
	}

	return 0;
}
