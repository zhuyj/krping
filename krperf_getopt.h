/*
 * lifted from fs/ncpfs/getopt.c
 */
#ifndef _KRPERF_GETOPT_H
#define _KRPERF_GETOPT_H

#include "krperf.h"

#define OPT_NOPARAM	BIT(0)
#define OPT_INT		BIT(1)
#define OPT_STRING	BIT(2)

struct krperf_option {
	const char *name;
	unsigned int has_arg;
	int val;
};

static const struct krperf_option krperf_opts[] = {
	{"count", OPT_INT, 'C'},
	{"size", OPT_INT, 'S'},
	{"addr", OPT_STRING, 'a'},
	{"addr6", OPT_STRING, 'A'},
	{"port", OPT_INT, 'p'},
	{"verbose", OPT_NOPARAM, 'v'},
	{"validate", OPT_NOPARAM, 'V'},
	{"server", OPT_NOPARAM, 's'},
	{"client", OPT_NOPARAM, 'c'},
	{"server_inv", OPT_NOPARAM, 'I'},
	{"rlat", OPT_NOPARAM, 'L'},
	{"duplex", OPT_NOPARAM, 'd'},
	{"tos", OPT_INT, 't'},
	{"txdepth", OPT_INT, 'T'},
	{"poll", OPT_NOPARAM, 'P'},
	{"local_dma_lkey", OPT_NOPARAM, 'Z'},
	{"read_inv", OPT_NOPARAM, 'R'},
	{"fr", OPT_NOPARAM, 'f'},
	{"srq", OPT_NOPARAM, 'q'},
	{NULL, 0, 0}
};

int krperf_parse(char *cmd, struct krperf_cb *cb);

#endif /* _KRPERF_GETOPT_H */
