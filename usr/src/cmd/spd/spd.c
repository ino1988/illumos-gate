/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 *
 * Copyright (c) 2014, Tegile Systems, Inc. All rights reserved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>
#include <stropts.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <smbios.h>

#include <sys/i2c/clients/spd.h>

#define	DEV_SPD		"/dev/spd"

#define	OPTIONS		":anv"

static struct smb_snum {
	uint32_t	serial_num;
	const char	*tag;
} *serials;
static int num_serials;

static void smbios_get_dimm_serials(void);
static void print_spds(int, int);
static void usage(const char *);

int
main(int argc, char **argv)
{
	int	v, nv, c, a;
	extern int optind, opterr;

	v = nv = 1;
	a = 0;
	opterr = 0;
	while ((c = getopt(argc, argv, OPTIONS)) != -1) {
		switch (c) {
		case 'a':
			a++;
			break;

		case 'n':
			v = 0;
			break;

		case 'v':
			nv = 0;
			break;

		default:
			usage(argv[0]);
		}
	}

	if (a != 0 && (v == 0 || nv == 0)) {
		fprintf(stderr, "-a is mutually exclusive with -n or -v\n");
		usage(argv[0]);
	}

	if (optind != argc) {
		fprintf(stderr, "Too many options\n");
		usage(argv[0]);
	}

	if (v == 0 && nv == 0) {
		/* both specified */
		v = nv = 1;
	}

	smbios_get_dimm_serials();

	print_spds(v, nv);

	exit(EXIT_SUCCESS);
}

static int
memdev(smbios_hdl_t *hdl, const smbios_struct_t *sp, void *arg)
{
	smbios_info_t		cmn;

	if (sp->smbstr_type != SMB_TYPE_MEMDEVICE)
		return (0);

	if (smbios_info_common(hdl, sp->smbstr_id, &cmn) == SMB_ERR)
		return (0);

	if (cmn.smbi_serial == NULL || cmn.smbi_location == NULL)
		return (0);

	serials = realloc(serials, sizeof (*serials) * ++num_serials);

	serials[num_serials - 1].serial_num = strtol(cmn.smbi_serial, NULL, 16);
	serials[num_serials - 1].tag = strdup(cmn.smbi_location);

	return (0);
}

static void
smbios_get_dimm_serials(void)
{
	smbios_hdl_t	*smb_hdl;
	int		err;

	if ((smb_hdl = smbios_open(NULL, SMB_VERSION, 0, &err)) == NULL) {
		fprintf(stderr, "Failed to open smbios: %s\n",
		    smbios_errmsg(err));
		return;
	}

	smbios_iter(smb_hdl, memdev, NULL);

	smbios_close(smb_hdl);
}

static const char *
get_location(uint32_t ser)
{
	int	i;

	for (i = 0; i < num_serials; i++)
		if (serials[i].serial_num == ser)
			return (serials[i].tag);

	return (NULL);
}

static int
dimms_only(const struct dirent *d)
{
	return (strncmp(d->d_name, "dimm", 4) == 0 ? 1 : 0);
}

static void
print_spd(int num, spd_dimm_t *spd)
{
	double	ftb, mtb, clock_time;
	char	rev[8];
	char	slot[12];
	const char *tag;
	int	clk_hz, i;

	if (spd->ftb_divisor != 0 && spd->mtb_divisor != 0) {
		ftb = (double)spd->ftb_dividend / spd->ftb_divisor;
		mtb = (double)spd->mtb_dividend / spd->mtb_divisor;

		clock_time = (double)spd->min_ctime_mtb * mtb +
		    (double)spd->min_ctime_ftb * ftb / 1000.0;

		clk_hz = (int)(2.0 * 1000 / clock_time + 0.5);
	} else {
		clk_hz = 0;
	}

	tag = get_location(spd->serial_no);

	if (tag)
		strlcpy(slot, tag, sizeof (slot));
	else if (spd->slot == (uint16_t)-1u)
		snprintf(slot, sizeof (slot), "   %2d", num);
	else
		snprintf(slot, sizeof (slot), "  %2d-%-2d", spd->socket,
		    spd->slot);

	memset(rev, ' ', sizeof (rev));
	rev[2] = spd->revision >> 8;
	rev[3] = spd->revision & 0xff;
	rev[7] = 0;
	if (!isprint(rev[2]) && !isprint(rev[3]))
		snprintf(rev, sizeof (rev), "0x%04x", spd->revision);

	for (i = PARTNO_SIZE - 1; i > 0; i--) {
		if (isspace(spd->part_no[i]))
			spd->part_no[i] = 0;
		else
			break;
	}

	printf("%9s %c %6d  %5d %18s %-6s %010d %4d-%02d  0x%04X\n",
	    slot, spd->non_volatile ? 'N' : 'V', spd->size, clk_hz,
	    spd->part_no, rev, spd->serial_no,
	    spd->year, spd->week, spd->manu_id);
}

static void
print_spds(int v, int nv)
{
	struct dirent	**dirlist;
	spd_dimm_t 	spd_info;
	int		fd, n, i, num;
	char		name[PATH_MAX];

	if ((n = scandir(DEV_SPD, &dirlist, dimms_only, alphasort)) < 0) {
		fprintf(stderr, "scandir(%s) failed: %s\n", DEV_SPD,
		    strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (n == 0)
		return;

	printf("  Dimm    T  MBytes   MHz       Part No       Rev     "
	    "Serial No   Date    Manuf\n");

	for (i = 0; i < n; i++) {
		if (sscanf(dirlist[i]->d_name, "dimm%d", &num) != 1)
			continue;

		snprintf(name, sizeof (name), "%s/%s", DEV_SPD,
		    dirlist[i]->d_name);

		if ((fd = open(name, O_RDONLY)) < 0) {
			fprintf(stderr, "Failed to open %s: %s\n", name,
			    strerror(errno));
			continue;
		}

		if (ioctl(fd, SPD_IOCTL_GET_SPD, &spd_info) < 0) {
			fprintf(stderr, "SPD_IOCTL_GET_SPD(%s) failed: %s\n",
			    name, strerror(errno));
			close(fd);
			continue;
		}

		close(fd);

		if ((v && spd_info.non_volatile == 0) ||
		    (nv && spd_info.non_volatile == 1))
			print_spd(num, &spd_info);
	}
}

static void
usage(const char *cmd)
{
	fprintf(stderr, "Usage:\t%s\t[ -a | -n | -v ]\n", cmd);
	fprintf(stderr, "\t-a\tdisplay all DIMMS\n");
	fprintf(stderr, "\t-n\tdisplay non-volatile DIMMS only\n");
	fprintf(stderr, "\t-v\tdisplay volatile DIMMS only\n");

	exit(EXIT_FAILURE);
}
