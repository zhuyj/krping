#ifndef _KRPERF_PROC_H
#define _KRPERF_PROC_H
#include <linux/types.h>

ssize_t krperf_write_proc(struct file * file, const char __user * buffer,
						  size_t count, loff_t *ppos);

int krperf_read_open(struct inode *inode, struct file *file);
#endif /* _KRPERF_PROC_H */
