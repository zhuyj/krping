#ifndef _KRPERF_PROC_H
#define _KRPERF_PROC_H
#include <linux/types.h>

ssize_t krperf_write_proc(struct file * file, const char __user * buffer,
						  size_t count, loff_t *ppos);

#endif /* _KRPERF_PROC_H */
