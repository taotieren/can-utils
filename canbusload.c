/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * canbusload.c - monitor CAN bus load
 *
 * Copyright (c) 2002-2008 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <linux-can@vger.kernel.org>
 *
 */

#include <ctype.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include "lib.h"
#include "terminal.h"
#include "canframelen.h"

#define MAXSOCK 16    /* max. number of CAN interfaces given on the cmdline */

#define PERCENTRES 5 /* resolution in percent for bargraph */
#define NUMBAR (100 / PERCENTRES) /* number of bargraph elements */

extern int optind, opterr, optopt;

static struct {
	char devname[IFNAMSIZ + 1];
	unsigned int bitrate;
	unsigned int dbitrate;
	unsigned int recv_frames;
	unsigned int recv_bits_total;
	unsigned int recv_bits_payload;
	unsigned int recv_bits_dbitrate;
} stat[MAXSOCK + 1];

static volatile int running = 1;
static volatile sig_atomic_t signal_num;
static int max_devname_len; /* to prevent frazzled device name output */
static int max_bitrate_len;
static int currmax;
static unsigned char redraw;
static unsigned char timestamp;
static unsigned char color;
static unsigned char bargraph;
static enum cfl_mode mode = CFL_WORSTCASE;
static char *prg;

void print_usage(char *prg)
{
	fprintf(stderr, "%s - monitor CAN bus load.\n", prg);
	fprintf(stderr, "\nUsage: %s [options] <CAN interface>+\n", prg);
	fprintf(stderr, "  (use CTRL-C to terminate %s)\n\n", prg);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "         -t  (show current time on the first line)\n");
	fprintf(stderr, "         -c  (colorize lines)\n");
	fprintf(stderr, "         -b  (show bargraph in %d%% resolution)\n", PERCENTRES);
	fprintf(stderr, "         -r  (redraw the terminal - similar to top)\n");
	fprintf(stderr, "         -i  (ignore bitstuffing in bandwidth calculation)\n");
	fprintf(stderr, "         -e  (exact calculation of stuffed bits)\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Up to %d CAN interfaces with mandatory bitrate can be specified on the \n", MAXSOCK);
	fprintf(stderr, "commandline in the form: <ifname>@<bitrate>[,<dbitrate>]\n\n");
	fprintf(stderr, "The bitrate is mandatory as it is needed to know the CAN bus bitrate to\n");
	fprintf(stderr, "calculate the bus load percentage based on the received CAN frames.\n");
	fprintf(stderr, "Due to the bitstuffing estimation the calculated busload may exceed 100%%.\n");
	fprintf(stderr, "For each given interface the data is presented in one line which contains:\n\n");
	fprintf(stderr, "(interface) (received CAN frames) (used bits total) (used bits for payload)\n");
	fprintf(stderr, "\nExamples:\n");
	fprintf(stderr, "\nuser$> canbusload can0@100000 can1@500000 can2@500000 can3@500000 -r -t -b -c\n\n");
	fprintf(stderr, "%s 2014-02-01 21:13:16 (worst case bitstuffing)\n", prg);
	fprintf(stderr, " can0@100000   805   74491  36656  74%% |XXXXXXXXXXXXXX......|\n");
	fprintf(stderr, " can1@500000   796   75140  37728  15%% |XXX.................|\n");
	fprintf(stderr, " can2@500000     0       0      0   0%% |....................|\n");
	fprintf(stderr, " can3@500000    47    4633   2424   0%% |....................|\n");
	fprintf(stderr, "\n");
}

void sigterm(int signo)
{
	running = 0;
	signal_num = signo;
}

void printstats(int signo)
{
	int i, j, percent;

	if (redraw)
		printf("%s", CSR_HOME);

	if (timestamp) {
		time_t currtime;
		struct tm now;

		if (time(&currtime) == (time_t)-1) {
			perror("time");
			exit(1);
		}

		localtime_r(&currtime, &now);

		printf("%s %04d-%02d-%02d %02d:%02d:%02d ",
		       prg,
		       now.tm_year + 1900,
		       now.tm_mon + 1,
		       now.tm_mday,
		       now.tm_hour,
		       now.tm_min,
		       now.tm_sec);

		switch (mode) {

		case CFL_NO_BITSTUFFING:
			/* plain bit calculation without bitstuffing */
			printf("(ignore bitstuffing)\n");
			break;

		case CFL_WORSTCASE:
			/* worst case estimation - see above */
			printf("(worst case bitstuffing)\n");
			break;

		case CFL_EXACT:
			/* exact calculation of stuffed bits based on frame content and CRC */
			printf("(exact bitstuffing)\n");
			break;

		default:
			printf("(unknown bitstuffing)\n");
			break;
		}
	}

	for (i = 0; i < currmax; i++) {
		if (color) {
			if (i % 2)
				printf("%s", FGRED);
			else
				printf("%s", FGBLUE);
		}

		if (stat[i].bitrate)
			percent = ((stat[i].recv_bits_total - stat[i].recv_bits_dbitrate) * 100) / stat[i].bitrate +
				(stat[i].recv_bits_dbitrate * 100) / stat[i].dbitrate;
		else
			percent = 0;

		printf(" %*s@%-*d %5d %7d %6d %6d %3d%%",
		       max_devname_len, stat[i].devname,
		       max_bitrate_len, stat[i].bitrate,
		       stat[i].recv_frames,
		       stat[i].recv_bits_total,
		       stat[i].recv_bits_payload,
		       stat[i].recv_bits_dbitrate,
		       percent);

		if (bargraph) {

			printf(" |");

			if (percent > 100)
				percent = 100;

			for (j = 0; j < NUMBAR; j++) {
				if (j < percent / PERCENTRES)
					printf("X");
				else
					printf(".");
			}

			printf("|");
		}

		if (color)
			printf("%s", ATTRESET);

		if (!redraw || (i < currmax - 1))
			printf("\n");

		stat[i].recv_frames = 0;
		stat[i].recv_bits_total = 0;
		stat[i].recv_bits_dbitrate = 0;
		stat[i].recv_bits_payload = 0;
	}

	if (!redraw)
		printf("\n");

	fflush(stdout);

	alarm(1);
}

int main(int argc, char **argv)
{
	fd_set rdfs;
	int s[MAXSOCK];

	int opt;
	char *ptr, *nptr;
	struct sockaddr_can addr;
	struct canfd_frame frame;
	int nbytes, i;
	struct ifreq ifr;

	signal(SIGTERM, sigterm);
	signal(SIGHUP, sigterm);
	signal(SIGINT, sigterm);

	signal(SIGALRM, printstats);

	prg = basename(argv[0]);

	while ((opt = getopt(argc, argv, "rtbcieh?")) != -1) {
		switch (opt) {
		case 'r':
			redraw = 1;
			break;

		case 't':
			timestamp = 1;
			break;

		case 'b':
			bargraph = 1;
			break;

		case 'c':
			color = 1;
			break;

		case 'i':
			mode = CFL_NO_BITSTUFFING;
			break;

		case 'e':
			mode = CFL_EXACT;
			break;

		default:
			print_usage(prg);
			exit(1);
			break;
		}
	}

	if (optind == argc) {
		print_usage(prg);
		exit(0);
	}

	currmax = argc - optind; /* find real number of CAN devices */

	if (currmax > MAXSOCK) {
		printf("More than %d CAN devices given on commandline!\n", MAXSOCK);
		return 1;
	}

	for (i = 0; i < currmax; i++) {
		ptr = argv[optind + i];

		nbytes = strlen(ptr);
		if (nbytes >= (int)(IFNAMSIZ + sizeof("@1000000") + 1)) {
			printf("name of CAN device '%s' is too long!\n", ptr);
			return 1;
		}

		pr_debug("open %d '%s'.\n", i, ptr);

		s[i] = socket(PF_CAN, SOCK_RAW, CAN_RAW);
		if (s[i] < 0) {
			perror("socket");
			return 1;
		}

		nptr = strchr(ptr, '@');

		if (!nptr) {
			fprintf(stderr, "Specify CAN interfaces in the form <CAN interface>@<bitrate>, e.g. can0@500000\n");
			print_usage(prg);
			return 1;
		}

		nbytes = nptr - ptr;  /* interface name is up the first '@' */

		if (nbytes >= (int)IFNAMSIZ) {
			printf("name of CAN device '%s' is too long!\n", ptr);
			return 1;
		}

		strncpy(stat[i].devname, ptr, nbytes);
		memset(&ifr.ifr_name, 0, sizeof(ifr.ifr_name));
		strncpy(ifr.ifr_name, ptr, nbytes);

		if (nbytes > max_devname_len)
			max_devname_len = nbytes; /* for nice printing */

		char *endp;
		stat[i].bitrate = strtol(nptr + 1, &endp, 0); /* bitrate is placed behind the '@' */
		if (*endp == ',')
			/* data bitrate is placed behind the ',' */
			stat[i].dbitrate = strtol(endp + 1, &endp, 0);
		else
			stat[i].dbitrate = stat[i].bitrate;

		if (!stat[i].bitrate || stat[i].bitrate > 1000000) {
			printf("invalid bitrate for CAN device '%s'!\n", ptr);
			return 1;
		}

		nbytes = strlen(nptr + 1);
		if (nbytes > max_bitrate_len)
			max_bitrate_len = nbytes; /* for nice printing */

		pr_debug("using interface name '%s'.\n", ifr.ifr_name);

		/* try to switch the socket into CAN FD mode */
		const int canfd_on = 1;
		setsockopt(s[i], SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on));

		if (ioctl(s[i], SIOCGIFINDEX, &ifr) < 0) {
			perror("SIOCGIFINDEX");
			exit(1);
		}

		addr.can_family = AF_CAN;
		addr.can_ifindex = ifr.ifr_ifindex;

		if (bind(s[i], (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			perror("bind");
			return 1;
		}
	}

	alarm(1);

	if (redraw)
		printf("%s", CLR_SCREEN);

	while (running) {
		FD_ZERO(&rdfs);
		for (i = 0; i < currmax; i++)
			FD_SET(s[i], &rdfs);

		if (select(s[currmax - 1] + 1, &rdfs, NULL, NULL, NULL) < 0) {
			//perror("pselect");
			continue;
		}

		for (i = 0; i < currmax; i++) { /* check all CAN RAW sockets */

			if (FD_ISSET(s[i], &rdfs)) {
				nbytes = read(s[i], &frame, sizeof(frame));

				if (nbytes < 0) {
					perror("read");
					return 1;
				}

				if (nbytes < (int)sizeof(struct can_frame)) {
					fprintf(stderr, "read: incomplete CAN frame\n");
					return 1;
				}

				stat[i].recv_frames++;
				stat[i].recv_bits_payload += frame.len * 8;
				stat[i].recv_bits_dbitrate += can_frame_dbitrate_length(
						&frame, mode, sizeof(frame));
				stat[i].recv_bits_total += can_frame_length(&frame,
									    mode, nbytes);
			}
		}
	}

	for (i = 0; i < currmax; i++)
		close(s[i]);

	if (signal_num)
		return 128 + signal_num;

	return 0;
}
