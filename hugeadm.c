/***************************************************************************
 *   User front end for using huge pages Copyright (C) 2008, IBM           *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the Lesser GNU General Public License as        *
 *   published by the Free Software Foundation; either version 2.1 of the  *
 *   License, or at your option) any later version.                        *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Lesser General Public License for more details.                   *
 *                                                                         *
 *   You should have received a copy of the Lesser GNU General Public      *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/*
 * hugeadm is designed to make an administrators life simpler, to automate
 * and simplify basic system configuration as it relates to hugepages.  It
 * is designed to help with pool and mount configuration.
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <mntent.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>

#define _GNU_SOURCE /* for getopt_long */
#include <unistd.h>
#include <getopt.h>

#define REPORT_UTIL "hugeadm"
#include "libhugetlbfs_internal.h"
#include "hugetlbfs.h"

extern int optind;
extern char *optarg;

#define OPTION(opts, text)	fprintf(stderr, " %-25s  %s\n", opts, text)
#define CONT(text) 		fprintf(stderr, " %-25s  %s\n", "", text)

#define MOUNT_DIR "/var/lib/hugetlbfs"
#define OPT_MAX 4096

#define PROCMOUNTS "/proc/mounts"
#define FS_NAME "hugetlbfs"
#define MIN_COL 20
#define MAX_SIZE_MNTENT (64 + PATH_MAX + 32 + 128 + 2 * sizeof(int))
#define FORMAT_LEN 20

void print_usage()
{
	fprintf(stderr, "hugeadm [options]\n");
	fprintf(stderr, "options:\n");

	OPTION("--list-all-mounts", "List all current hugetlbfs mount points");
	OPTION("--pool-list", "List all pools");
	OPTION("--pool-pages-min <size>:[+|-]<count>", "");
	CONT("Adjust pool 'size' lower bound");
	OPTION("--pool-pages-max <size>:[+|-]<count>", "");
	CONT("Adjust pool 'size' upper bound");
	OPTION("--create-mounts", "Creates a mount point for each available");
	CONT("huge page size on this system under /var/lib/hugetlbfs");
	OPTION("--create-user-mounts <user>", "");
	CONT("Creates a mount point for each available huge");
	CONT("page size under /var/lib/hugetlbfs/<user>");
	CONT("usable by user <user>");
	OPTION("--create-group-mounts <group>", "");
	CONT("Creates a mount point for each available huge");
	CONT("page size under /var/lib/hugetlbfs/<group>");
	CONT("usable by group <group>");
	OPTION("--create-global-mounts", "");
	CONT("Creates a mount point for each available huge");
	CONT("page size under /var/lib/hugetlbfs/global");
	CONT("for use by anyone");

	OPTION("--page-sizes", "Display page sizes that a configured pool");
	OPTION("--page-sizes-all",
			"Display page sizes support by the hardware");
	OPTION("--dry-run", "Print the equivalent shell commands for what");
	CONT("the specified options would have done without");
	CONT("taking any action");

	OPTION("--help, -h", "Prints this message");
}

int opt_dry_run = 0;

/*
 * getopts return values for options which are long only.
 */
#define LONG_POOL		('p' << 8)
#define LONG_POOL_LIST		(LONG_POOL|'l')
#define LONG_POOL_MIN_ADJ	(LONG_POOL|'m')
#define LONG_POOL_MAX_ADJ	(LONG_POOL|'M')

#define LONG_PAGE	('P' << 8)
#define LONG_PAGE_SIZES	(LONG_PAGE|'s')
#define LONG_PAGE_AVAIL	(LONG_PAGE|'a')

#define LONG_MOUNTS			('m' << 8)
#define LONG_CREATE_MOUNTS		(LONG_MOUNTS|'C')
#define LONG_CREATE_USER_MOUNTS		(LONG_MOUNTS|'U')
#define LONG_CREATE_GROUP_MOUNTS	(LONG_MOUNTS|'g')
#define LONG_CREATE_GLOBAL_MOUNTS	(LONG_MOUNTS|'G')
#define LONG_LIST_ALL_MOUNTS		(LONG_MOUNTS|'A')

#define MAX_POOLS	32

static int cmpsizes(const void *p1, const void *p2)
{
	return ((struct hpage_pool *)p1)->pagesize >
			((struct hpage_pool *)p2)->pagesize;
}

void pool_list(void)
{
	struct hpage_pool pools[MAX_POOLS];
	int pos;
	int cnt;

	cnt = hpool_sizes(pools, MAX_POOLS);
	if (cnt < 0) {
		ERROR("unable to obtain pools list");
		exit(EXIT_FAILURE);
	}
	qsort(pools, cnt, sizeof(pools[0]), cmpsizes);

	printf("%10s %8s %8s %8s %8s\n",
		"Size", "Minimum", "Current", "Maximum", "Default");
	for (pos = 0; cnt--; pos++) {
		printf("%10ld %8ld %8ld %8ld %8s\n", pools[pos].pagesize,
			pools[pos].minimum, pools[pos].size,
			pools[pos].maximum, (pools[pos].is_default) ? "*" : "");
	}
}

struct mount_list
{
	struct mntent entry;
	char data[MAX_SIZE_MNTENT];
	struct mount_list *next;
};

void print_mounts(struct mount_list *current, int longest)
{
	char format_str[FORMAT_LEN];

	snprintf(format_str, FORMAT_LEN, "%%-%ds %%s\n", longest);
	printf(format_str, "Mount Point", "Options");
	while (current) {
		printf(format_str, current->entry.mnt_dir,
				   current->entry.mnt_opts);
		current = current->next;
	}
}

void mounts_list_all(void)
{
	FILE *mounts;
	struct mount_list *list, *current, *previous = NULL;
	int length, longest = MIN_COL;

	/* First try /proc/mounts, then /etc/mtab */
	mounts = setmntent(PROCMOUNTS, "r");
	if (!mounts) {
		mounts = setmntent(MOUNTED, "r");
		if (!mounts) {
			ERROR("unable to open %s or %s for reading",
				PROCMOUNTS, MOUNTED);
			exit(EXIT_FAILURE);
		}
	}

	list = malloc(sizeof(struct mount_list));
	if (!list) {
		ERROR("out of memory");
		exit(EXIT_FAILURE);
	}

	list->next = NULL;
	current = list;
	while (getmntent_r(mounts, &(current->entry), current->data, MAX_SIZE_MNTENT)) {
		if (strcasecmp(current->entry.mnt_type, FS_NAME) == 0) {
			length = strlen(current->entry.mnt_dir);
			if (length > longest)
				longest = length;

			current->next = malloc(sizeof(struct mount_list));
			if (!current->next) {
				ERROR("out of memory");
				exit(EXIT_FAILURE);
			}
			previous = current;
			current = current->next;
			current->next = NULL;
		}
	}

	endmntent(mounts);

	if (previous) {
		free(previous->next);
		previous->next = NULL;
		print_mounts(list, longest);
	} else {
		/* No hugetlbfs mounts were found */
		printf("No hugetlbfs mount point found.\n");
	}

	current = list;
	while (current) {
		previous = current;
		current = current->next;
		free(previous);
	}
}

int make_dir(char *path, mode_t mode, uid_t uid, gid_t gid)
{
	struct passwd *pwd;
	struct group *grp;

	if (opt_dry_run) {
		pwd = getpwuid(uid);
		grp = getgrgid(gid);
		printf("if [ ! -e %s ]\n", path);
		printf("then\n");
		printf(" mkdir %s\n", path);
		printf(" chown %s:%s %s\n", pwd->pw_name, grp->gr_name, path);
		printf(" chmod %o %s\n", mode, path);
		printf("fi\n");
		return 0;
	}

	if (mkdir(path, mode)) {
		if (errno != EEXIST) {
			ERROR("Unable to create dir %s, error: %s\n",
				path, strerror(errno));
			return 1;
		}
	} else {
		if (chown(path, uid, gid)) {
			ERROR("Unable to change ownership of %s, error: %s\n",
				path, strerror(errno));
			return 1;
		}

		if (chmod(path, mode)) {
			ERROR("Unable to change permission on %s, error: %s\n",
				path, strerror(errno));
			return 1;
		}
	}

	return 0;
}

/**
 * ensure_dir will build the entire directory structure up to and
 * including path, all directories built will be owned by
 * user:group and permissions will be set to mode.
 */
int ensure_dir(char *path, mode_t mode, uid_t uid, gid_t gid)
{
	char *idx;

	if (!path || strlen(path) == 0)
		return 0;

	idx = strchr(path + 1, '/');

	do {
		if (idx)
			*idx = '\0';

		if (make_dir(path, mode, uid, gid))
			return 1;

		if (idx) {
			*idx = '/';
			idx++;
		}
	} while ((idx = strchr(idx, '/')) != NULL);

	if (make_dir(path, mode, uid, gid))
		return 1;

	return 0;
}

int mount_dir(char *path, char *options, mode_t mode, uid_t uid, gid_t gid)
{
	struct passwd *pwd;
	struct group *grp;

	if (opt_dry_run) {
		pwd = getpwuid(uid);
		grp = getgrgid(gid);
		printf("mount -t %s none %s -o %s\n", FS_NAME,
			path, options);
		printf("chown %s:%s %s\n", pwd->pw_name, grp->gr_name,
			path);
		printf("chmod %o %s\n", mode, path);
	} else {
		if (mount("none", path, FS_NAME, 0, options)) {
			ERROR("Unable to mount %s, error: %s\n",
				path, strerror(errno));
			return 1;
		}

		if (chown(path, uid, gid)) {
			ERROR("Unable to change ownership of %s, error: %s\n",
				path, strerror(errno));
			return 1;
		}

		if (chmod(path, mode)) {
			ERROR("Unable to set permissions on %s, error: %s\n",
				path, strerror(errno));
			return 1;
		}
	}
	return 0;
}


void create_mounts(char *user, char *group, char *base, mode_t mode)
{
	struct hpage_pool pools[MAX_POOLS];
	char path[PATH_MAX];
	char options[OPT_MAX];
	int cnt, pos;
	struct passwd *pwd;
	struct group *grp;
	uid_t uid = 0;
	gid_t gid = 0;

	if (geteuid() != 0) {
		ERROR("Mounts can only be created by root\n");
		exit(EXIT_FAILURE);
	}

	if (user) {
		pwd = getpwnam(user);
		if (!pwd) {
			ERROR("Could not find specified user %s\n", user);
			exit(EXIT_FAILURE);
		}
		uid = pwd->pw_uid;
	} else if (group) {
		grp = getgrnam(group);
		if (!grp) {
			ERROR("Could not find specified group %s\n", group);
			exit(EXIT_FAILURE);
		}
		gid = grp->gr_gid;
	}

	if (ensure_dir(base,
		S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, 0, 0))
		exit(EXIT_FAILURE);

	cnt = hpool_sizes(pools, MAX_POOLS);
	if (cnt < 0) {
		ERROR("Unable to obtain pools list\n");
		exit(EXIT_FAILURE);
	}

	for (pos=0; cnt--; pos++) {
		if (user)
			snprintf(path, PATH_MAX, "%s/%s/pagesize-%ld",
				base, user, pools[pos].pagesize);
		else if (group)
			snprintf(path, PATH_MAX, "%s/%s/pagesize-%ld",
				base, group, pools[pos].pagesize);
		else
			snprintf(path, PATH_MAX, "%s/pagesize-%ld",
				base, pools[pos].pagesize);
		snprintf(options, OPT_MAX, "pagesize=%ld",
				pools[pos].pagesize);
		if (ensure_dir(path, mode, uid, gid))
			exit(EXIT_FAILURE);

		if (mount_dir(path, options, mode, uid, gid))
			exit(EXIT_FAILURE);
	}
}

enum {
	POOL_MIN,
	POOL_MAX,
	POOL_BOTH,
};

static long value_adjust(char *adjust_str, long base)
{
	long adjust;
	char *iter;

	/* Convert and validate the adjust. */
	adjust = strtol(adjust_str, &iter, 0);
	if (*iter) {
		ERROR("%s: invalid adjustment\n", adjust_str);
		exit(EXIT_FAILURE);
	}

	if (adjust_str[0] != '+' && adjust_str[0] != '-')
		base = 0;

	/* Ensure we neither go negative nor exceed LONG_MAX. */
	if (adjust < 0 && -adjust > base) {
		adjust = -base;
	}
	if (adjust > 0 && (base + adjust) < base) {
		adjust = LONG_MAX - base;
	}
	base += adjust;

	return base;
}


void pool_adjust(char *cmd, unsigned int counter)
{
	struct hpage_pool pools[MAX_POOLS];
	int pos;
	int cnt;

	char *iter = NULL;
	char *page_size_str = NULL;
	char *adjust_str = NULL;
	long page_size;

	unsigned long min;
	unsigned long max;

	/* Extract the pagesize and adjustment. */
	page_size_str = strtok_r(cmd, ":", &iter);
	if (page_size_str)
		adjust_str = strtok_r(NULL, ":", &iter);

	if (!page_size_str || !adjust_str) {
		ERROR("%s: invalid resize specificiation\n", cmd);
		exit(EXIT_FAILURE);
	}
	INFO("page_size<%s> adjust<%s> counter<%d>\n",
					page_size_str, adjust_str, counter);

	/* Convert and validate the page_size. */
	page_size = parse_page_size(page_size_str);

	cnt = hpool_sizes(pools, MAX_POOLS);
	if (cnt < 0) {
		ERROR("unable to obtain pools list");
		exit(EXIT_FAILURE);
	}
	for (pos = 0; cnt--; pos++) {
		if (pools[pos].pagesize == page_size)
			break;
	}
	if (cnt < 0) {
		ERROR("%s: unknown page size\n", page_size_str);
		exit(EXIT_FAILURE);
	}

	min = pools[pos].minimum;
	max = pools[pos].maximum;

	if (counter == POOL_BOTH) {
		min = value_adjust(adjust_str, min);
		max = min;
	} else if (counter == POOL_MIN) {
		min = value_adjust(adjust_str, min);
		if (min > max)
			max = min;
	} else {
		max = value_adjust(adjust_str, max);
		if (max < min)
			min = max;
	}

	INFO("%ld, %ld -> %ld, %ld\n", pools[pos].minimum, pools[pos].maximum,
		min, max);

	if ((pools[pos].maximum - pools[pos].minimum) < (max - min)) {
		INFO("setting HUGEPAGES_OC to %ld\n", (max - min));
		set_huge_page_counter(page_size, HUGEPAGES_OC, (max - min));
	}
	if (pools[pos].minimum != min) {
		INFO("setting HUGEPAGES_TOTAL to %ld\n", min);
		set_huge_page_counter(page_size, HUGEPAGES_TOTAL, min);
	}
	/*
	 * HUGEPAGES_TOTAL is not guarenteed to check to exactly the figure
	 * requested should there be insufficient pages.  Check the new
	 * value and adjust HUGEPAGES_OC accordingly.
	 */
	get_pool_size(page_size, &pools[pos]);
	if (pools[pos].minimum != min) {
		WARNING("failed to set pool minimum to %ld became %ld\n",
			min, pools[pos].minimum);
		min = pools[pos].minimum;
	}
	if (pools[pos].maximum != max) {
		INFO("setting HUGEPAGES_OC to %ld\n", (max - min));
		set_huge_page_counter(page_size, HUGEPAGES_OC, (max - min));
	}
}

void page_sizes(int all)
{
	struct hpage_pool pools[MAX_POOLS];
	int pos;
	int cnt;

	cnt = hpool_sizes(pools, MAX_POOLS);
	if (cnt < 0) {
		ERROR("unable to obtain pools list");
		exit(EXIT_FAILURE);
	}
	qsort(pools, cnt, sizeof(pools[0]), cmpsizes);

	for (pos = 0; cnt--; pos++) {
		if (all || (pools[pos].maximum &&
		    hugetlbfs_find_path_for_size(pools[pos].pagesize)))
			printf("%ld\n", pools[pos].pagesize);
	}
}

int main(int argc, char** argv)
{
	int ops;
	int has_hugepages = kernel_has_hugepages();

	char opts[] = "+hd";
	char base[PATH_MAX];
	int ret = 0, index = 0;
	struct option long_opts[] = {
		{"help",       no_argument, NULL, 'h'},

		{"list-all-mounts", no_argument, NULL, LONG_LIST_ALL_MOUNTS},
		{"pool-list", no_argument, NULL, LONG_POOL_LIST},
		{"pool-pages-min", required_argument, NULL, LONG_POOL_MIN_ADJ},
		{"pool-pages-max", required_argument, NULL, LONG_POOL_MAX_ADJ},
		{"create-mounts", no_argument, NULL, LONG_CREATE_MOUNTS},
		{"create-user-mounts", required_argument, NULL, LONG_CREATE_USER_MOUNTS},
		{"create-group-mounts", required_argument, NULL, LONG_CREATE_GROUP_MOUNTS},
		{"create-global-mounts", no_argument, NULL, LONG_CREATE_GLOBAL_MOUNTS},

		{"page-sizes", no_argument, NULL, LONG_PAGE_SIZES},
		{"page-sizes-all", no_argument, NULL, LONG_PAGE_AVAIL},
		{"dry-run", no_argument, NULL, 'd'},

		{0},
	};

	hugetlbfs_setup_debug();
	setup_mounts();

	ops = 0;
	while (ret != -1) {
		ret = getopt_long(argc, argv, opts, long_opts, &index);
		switch (ret) {
		case -1:
			break;

		case '?':
			print_usage();
			exit(EXIT_FAILURE);

		case 'h':
			print_usage();
			exit(EXIT_SUCCESS);

		case 'd':
			opt_dry_run = 1;
			continue;

		default:
			/* All other commands require hugepage support. */
			if (! has_hugepages) {
				ERROR("kernel does not support huge pages\n");
				exit(EXIT_FAILURE);
			}
		}
		switch (ret) {
		case -1:
			break;

		case LONG_LIST_ALL_MOUNTS:
			mounts_list_all();
			break;

		case LONG_POOL_LIST:
			pool_list();
			break;

		case LONG_POOL_MIN_ADJ:
			if (! kernel_has_overcommit())
				pool_adjust(optarg, POOL_BOTH);
			else
				pool_adjust(optarg, POOL_MIN);
			break;

		case LONG_POOL_MAX_ADJ:
			if (! kernel_has_overcommit()) {
				ERROR("kernel does not support overcommit, "
					"max cannot be adjusted\n");
				exit(EXIT_FAILURE);
			}
			pool_adjust(optarg, POOL_MAX);
			break;

		case LONG_CREATE_MOUNTS:
			snprintf(base, PATH_MAX, "%s", MOUNT_DIR);
			create_mounts(NULL, NULL, base, S_IRWXU | S_IRWXG);
			break;

		case LONG_CREATE_USER_MOUNTS:
			snprintf(base, PATH_MAX, "%s/user", MOUNT_DIR);
			create_mounts(optarg, NULL, base, S_IRWXU);
			break;

		case LONG_CREATE_GROUP_MOUNTS:
			snprintf(base, PATH_MAX, "%s/group", MOUNT_DIR);
			create_mounts(NULL, optarg, base, S_IRWXG);
			break;

		case LONG_CREATE_GLOBAL_MOUNTS:
			snprintf(base, PATH_MAX, "%s/global", MOUNT_DIR);
			create_mounts(NULL, NULL, base, S_IRWXU | S_IRWXG | S_IRWXO);
			break;

		case LONG_PAGE_SIZES:
			page_sizes(0);
			break;

		case LONG_PAGE_AVAIL:
			page_sizes(1);
			break;

		default:
			WARNING("unparsed option %08x\n", ret);
			ret = -1;
			break;
		}
		if (ret != -1)
			ops++;
	}
	index = optind;

	if ((argc - index) != 0 || ops == 0) {
		print_usage();
		exit(EXIT_FAILURE);
	}

	exit(EXIT_SUCCESS);
}
