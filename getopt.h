/*
 * lifted from fs/ncpfs/getopt.c
 */
#ifndef _KRPING_GETOPT_H
#define _KRPING_GETOPT_H

#define OPT_NOPARAM	1
#define OPT_INT		2
#define OPT_STRING	4
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
	{"wlat", OPT_NOPARAM, 'l'},
	{"rlat", OPT_NOPARAM, 'L'},
	{"bw", OPT_NOPARAM, 'B'},
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

extern int krperf_getopt(const char *caller, char **options, const struct krperf_option *opts,
		      char **optopt, char **optarg, unsigned long *value);

#endif /* _KRPING_GETOPT_H */
