/*
 * tune.c
 *
 * ocfs2 tune utility
 *
 * Copyright (C) 2004 Oracle Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 * Authors: Sunil Mushran
 */

#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE /* Because libc really doesn't want us using O_DIRECT? */

#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fd.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <malloc.h>
#include <time.h>
#include <libgen.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <ctype.h>

#include <ocfs2.h>
#include <ocfs2_fs.h>
#include <ocfs2_disk_dlm.h>
#include <ocfs1_fs_compat.h>
#include <kernel-list.h>

#define SYSTEM_FILE_NAME_MAX   40

typedef struct _ocfs2_tune_opts {
	uint16_t num_nodes;
	uint64_t vol_size;
	uint64_t jrnl_size;
	char *vol_label;
	char *progname;
	char *device;
	int verbose;
	int quiet;
	time_t tune_time;
	int fd;
} ocfs2_tune_opts;

ocfs2_tune_opts opts;

/*
 * usage()
 *
 */
static void usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [-L volume-label] [-n number-of-nodes]\n"
			"\t\t[-j journal-size] [-S volume-size] [-qvV] device\n",
			progname);
	exit(0);
}

/*
 * version()
 *
 */
static void version(const char *progname)
{
	fprintf(stderr, "%s %s\n", progname, VERSION);
}

/*
 * get_number()
 *
 */
static int get_number(char *arg, uint64_t *res)
{
	char *ptr = NULL;
	uint64_t num;

	num = strtoull(arg, &ptr, 0);

	if ((ptr == arg) || (num == UINT64_MAX))
		return(-EINVAL);

	switch (*ptr) {
	case '\0':
		break;

	case 'g':
	case 'G':
		num *= 1024;
		/* FALL THROUGH */

	case 'm':
	case 'M':
		num *= 1024;
		/* FALL THROUGH */

	case 'k':
	case 'K':
		num *= 1024;
		/* FALL THROUGH */

	case 'b':
	case 'B':
		break;

	default:
		return -EINVAL;
	}

	*res = num;

	return 0;
}

/*
 * get_options()
 *
 */
static void get_options(int argc, char **argv)
{
	int c;
	int show_version = 0;
	int ret;
	char *dummy;
	uint64_t val;
	uint64_t max_journal_size = 500 * ONE_MEGA_BYTE;

	static struct option long_options[] = {
		{ "label", 1, 0, 'L' },
		{ "nodes", 1, 0, 'n' },
		{ "verbose", 0, 0, 'v' },
		{ "quiet", 0, 0, 'q' },
		{ "version", 0, 0, 'V' },
		{ "journalsize", 0, 0, 'j'},
		{ "volumesize", 0, 0, 'S'},
		{ 0, 0, 0, 0}
	};

	if (argc && *argv)
		opts.progname = basename(argv[0]);
	else
		opts.progname = strdup("tunefs.ocfs2");

	while (1) {
		c = getopt_long(argc, argv, "L:n:j:S:vqV", long_options, 
				NULL);

		if (c == -1)
			break;

		switch (c) {
		case 'L':
			opts.vol_label = strdup(optarg);

			if (strlen(opts.vol_label) >= MAX_VOL_LABEL_LEN) {
				com_err(opts.progname, 0,
					"Volume label too long: must be less "
					"than %d characters",
					MAX_VOL_LABEL_LEN);
				exit(1);
			}
			break;

		case 'n':
			opts.num_nodes = strtoul(optarg, &dummy, 0);

			if (opts.num_nodes > OCFS2_MAX_NODES ||
			    *dummy != '\0') {
				com_err(opts.progname, 0,
					"Number of nodes must be no more than %d",
					OCFS2_MAX_NODES);
				exit(1);
			} else if (opts.num_nodes < 2) {
				com_err(opts.progname, 0,
					"Initial nodes must be at least 2");
				exit(1);
			}
			break;

		case 'j':
			ret = get_number(optarg, &val);

			if (ret || 
			    val < OCFS2_MIN_JOURNAL_SIZE ||
			    val > max_journal_size) {
				com_err(opts.progname, 0,
					"Invalid journal size %s: must be "
					"between %d and %"PRIu64" bytes",
					optarg,
					OCFS2_MIN_JOURNAL_SIZE,
					max_journal_size);
				exit(1);
			}

			opts.jrnl_size = val;
			break;

		case 'S':
			ret = get_number(optarg, &val);
			if (ret)
				exit(1);
			opts.vol_size = val;
			break;

		case 'v':
			opts.verbose = 1;
			break;

		case 'q':
			opts.quiet = 1;
			break;

		case 'V':
			show_version = 1;
			break;

		default:
			usage(opts.progname);
			break;
		}
	}

	if (!opts.quiet || show_version)
		version(opts.progname);

	if (show_version)
		exit(0);

	if (optind == argc)
		usage(opts.progname);

	opts.device = strdup(argv[optind]);

	opts.tune_time = time(NULL);

	return ;
}

/*
 * add_nodes()
 *
 */
static errcode_t add_nodes(ocfs2_filesys *fs)
{
	errcode_t ret = 0;
	uint16_t old_num = OCFS2_RAW_SB(fs->fs_super)->s_max_nodes;
	char fname[SYSTEM_FILE_NAME_MAX];
	uint64_t blkno;
	int mode;
	int i, j;

	for (i = OCFS2_LAST_GLOBAL_SYSTEM_INODE + 1; i < NUM_SYSTEM_INODES; ++i) {
		for (j = old_num; j < opts.num_nodes; ++j) {
			sprintf(fname, sysfile_info[i].name, j);
			printf("Adding %s...  ", fname);

			/* create inode for system file */
			mode = sysfile_info[i].dir ? 0755 | S_IFDIR : 0644 | S_IFREG;
			ret =  ocfs2_new_system_inode(fs, &blkno, mode, sysfile_info[i].flags);
			if (ret)
				goto bail;

			/* Add the inode to the system dir */
			ret = ocfs2_link(fs, fs->fs_sysdir_blkno, fname,
					 blkno, OCFS2_FT_REG_FILE);
			if (ret) {
				if (ret == OCFS2_ET_DIR_NO_SPACE) {
					ret = ocfs2_expand_dir(fs, fs->fs_sysdir_blkno);
					if (!ret)
						ret = ocfs2_link(fs, fs->fs_sysdir_blkno,
								 fname, blkno, OCFS2_FT_REG_FILE);
				}
				if (ret)
					goto bail;
			}
			printf("\r                                                     \r");
		}
	}
bail:
	if (ret)
		printf("\n");

	return ret;
}

/*
 * get_default_journal_size()
 *
 */
static errcode_t get_default_journal_size(ocfs2_filesys *fs, uint64_t *jrnl_size)
{
	errcode_t ret;
	char jrnl_node0[40];
	uint64_t blkno;
	char *buf = NULL;
	ocfs2_dinode *di;

	snprintf (jrnl_node0, sizeof(jrnl_node0),
		  sysfile_info[JOURNAL_SYSTEM_INODE].name, 0);

	ret = ocfs2_lookup(fs, fs->fs_sysdir_blkno, jrnl_node0,
			   strlen(jrnl_node0), NULL, &blkno);
	if (ret)
		goto bail;


	ret = ocfs2_malloc_block(fs->fs_io, &buf);
	if (ret)
		return ret;

	ret = ocfs2_read_inode(fs, blkno, buf);
	if (ret)
		goto bail;

	di = (ocfs2_dinode *)buf;
	*jrnl_size = (di->i_clusters <<
		      OCFS2_RAW_SB(fs->fs_super)->s_clustersize_bits);
bail:
	if (buf)
		ocfs2_free(&buf);

	return ret;
}

/*
 * update_volume_label()
 *
 */
static void update_volume_label(ocfs2_filesys *fs, int *changed)
{
	strncpy (OCFS2_RAW_SB(fs->fs_super)->s_label, opts.vol_label,
		 MAX_VOL_LABEL_LEN);

	*changed = 1;

	return ;
}

/*
 * update_nodes()
 *
 */
static errcode_t update_nodes(ocfs2_filesys *fs, int *changed)
{
	errcode_t ret = 0;

	ret = add_nodes(fs);
	if (ret)
		return ret;

	OCFS2_RAW_SB(fs->fs_super)->s_max_nodes = opts.num_nodes;
	*changed = 1;

	return ret;
}

/*
 * update_journal_size()
 *
 */
static errcode_t update_journal_size(ocfs2_filesys *fs, int *changed)
{
	errcode_t ret = 0;
	char jrnl_file[40];
	uint64_t blkno;
	int i;
	uint16_t max_nodes = OCFS2_RAW_SB(fs->fs_super)->s_max_nodes;
	uint64_t num_clusters;
	char *buf = NULL;
	ocfs2_dinode *di;

	num_clusters = opts.jrnl_size >>
			OCFS2_RAW_SB(fs->fs_super)->s_clustersize_bits;

	ret = ocfs2_malloc_block(fs->fs_io, &buf);
	if (ret)
		return ret;

	for (i = 0; i < max_nodes; ++i) {
		snprintf (jrnl_file, sizeof(jrnl_file),
			  sysfile_info[JOURNAL_SYSTEM_INODE].name, i);

		ret = ocfs2_lookup(fs, fs->fs_sysdir_blkno, jrnl_file,
				   strlen(jrnl_file), NULL, &blkno);
		if (ret)
			goto bail;


		ret = ocfs2_read_inode(fs, blkno, buf);
		if (ret)
			goto bail;

		di = (ocfs2_dinode *)buf;
		if (num_clusters <= di->i_clusters)
			continue;

		ret = ocfs2_extend_allocation(fs, blkno,
					      (num_clusters - di->i_clusters));
		if (ret)
			goto bail;

		ret = ocfs2_read_inode(fs, blkno, buf);
		if (ret)
			goto bail;

		di = (ocfs2_dinode *)buf;
		di->i_size = di->i_clusters <<
				OCFS2_RAW_SB(fs->fs_super)->s_clustersize_bits;

		ret = ocfs2_write_inode(fs, blkno, buf);
		if (ret)
			goto bail;

		break;
	}

bail:
	if (buf)
		ocfs2_free(&buf);

	return ret;
}

/*
 * update_volume_size()
 *
 */
static errcode_t update_volume_size(ocfs2_filesys *fs, int *changed)
{
	printf("TODO: update_volume_size\n");
	return 0;
}

/*
 * main()
 *
 */
int main(int argc, char **argv)
{
	errcode_t ret = 0;
	ocfs2_filesys *fs = NULL;
	int upd_label = 0;
	int upd_nodes = 0;
	int upd_jrnls = 0;
	int upd_vsize = 0;
	uint16_t tmp;
	uint64_t def_jrnl_size = 0;
	uint64_t num_clusters;
	uint64_t vol_size = 0;

	initialize_ocfs_error_table();

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	memset (&opts, 0, sizeof(opts));

	get_options(argc, argv);

	ret = ocfs2_open(opts.device, OCFS2_FLAG_RW, 0, 0, &fs);
	if (ret) {
		if (ret == OCFS2_ET_OCFS_REV)
			printf("ERROR: %s is an ocfs (and not ocfs2) volume. ", opts.device);
		else
			printf("ERROR: %s is not an ocfs2 volume. ", opts.device);
		printf("Aborting.\n");
		goto bail;
	}

//	check_32bit_blocks (s);

	/* validate volume label */
	if (opts.vol_label) {
		printf("Changing volume label from %s to %s\n",
		       OCFS2_RAW_SB(fs->fs_super)->s_label, opts.vol_label);
	}

	/* validate num nodes */
	if (opts.num_nodes) {
		tmp = OCFS2_RAW_SB(fs->fs_super)->s_max_nodes;
		if (opts.num_nodes > tmp) {
			printf("Changing number of nodes from %d to %d\n",
			       tmp, opts.num_nodes);
		} else {
			printf("ERROR: Nodes (%d) has to be larger than "
			       "configured nodes (%d)\n", opts.num_nodes, tmp);
			goto bail;
		}
	}

	/* validate journal size */
	if (opts.jrnl_size) {
		ret = get_default_journal_size(fs, &def_jrnl_size);
		if (ret)
			goto bail;

		num_clusters = (opts.jrnl_size + fs->fs_clustersize - 1) >>
				OCFS2_RAW_SB(fs->fs_super)->s_clustersize_bits;

		opts.jrnl_size = num_clusters <<
				OCFS2_RAW_SB(fs->fs_super)->s_clustersize_bits;

		if (opts.jrnl_size > def_jrnl_size)
			printf("Changing journal size %"PRIu64" to %"PRIu64"\n",
			       def_jrnl_size, opts.jrnl_size);
		else {
			printf("ERROR: Journal size %"PRIu64" has to be larger "
			       "than %"PRIu64"\n", opts.jrnl_size, def_jrnl_size);
			goto bail;
		}
	}

	/* validate volume size */
	if (opts.vol_size) {
		num_clusters = (opts.vol_size + fs->fs_clustersize - 1) >>
				OCFS2_RAW_SB(fs->fs_super)->s_clustersize_bits;
		opts.vol_size = num_clusters <<
				OCFS2_RAW_SB(fs->fs_super)->s_clustersize_bits;

		vol_size = fs->fs_clusters <<
			OCFS2_RAW_SB(fs->fs_super)->s_clustersize_bits;

		printf("Changing volume size %"PRIu64" to %"PRIu64"\n",
		       vol_size, opts.vol_size);
	}

	/* Abort? */
	printf("Proceed (y/N): ");
	if (toupper(getchar()) != 'Y') {
		printf("Aborting operation.\n");
		goto bail;
	}

	/* update volume label */
	if (opts.vol_label) {
		update_volume_label(fs, &upd_label);
		if (upd_label)
			printf("Changed volume label\n");
	}

	/* update number of nodes */
	if (opts.num_nodes) {
		ret = update_nodes(fs, &upd_nodes);
		if (ret) {
			com_err(opts.progname, ret, "while updating nodes");
			goto bail;
		}
		if (upd_nodes)
			printf("Added nodes\n");
	}

	/* update journal size */
	if (opts.jrnl_size) {
		ret = update_journal_size(fs, &upd_jrnls);
		if (ret) {
			com_err(opts.progname, ret, "while updating journal size");
			goto bail;
		}
		if (upd_jrnls)
			printf("Resized journals\n");
	}

	/* update volume size */
	if (opts.vol_size) {
		ret = update_volume_size(fs, &upd_vsize);
		if (ret) {
			com_err(opts.progname, ret, "while updating volume size");
			goto bail;
		}
		if (upd_vsize)
			printf("Resized volume\n");
	}


	/* write superblock */
	if (upd_label || upd_nodes || upd_vsize) {
		ret = ocfs2_write_super(fs);
		if (ret) {
			com_err(opts.progname, ret, "while writing superblock");
			goto bail;
		}
		printf("Wrote Superblock\n");
	}

bail:
	if (fs)
		ocfs2_close(fs);

	return ret;
}
