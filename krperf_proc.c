#include <linux/types.h>
#include <linux/fs.h>
#include <linux/module.h>

int krperf_doit(char *cmd);
/*
 * Write proc is used to start a ping client or server.
 */
ssize_t krperf_write_proc(struct file * file, const char __user * buffer,
		size_t count, loff_t *ppos)
{
	char *cmd;
	int rc;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	cmd = kzalloc(count, GFP_KERNEL);
	if (cmd == NULL) {
		pr_err("kmalloc failure\n");
		return -ENOMEM;
	}
	if (copy_from_user(cmd, buffer, count)) {
		kfree(cmd);
		return -EFAULT;
	}

	/*
	 * remove the \n.
	 */
	cmd[count - 1] = 0;
	pr_info("proc write |%s|\n", cmd);
	rc = krperf_doit(cmd);
	kfree(cmd);
	module_put(THIS_MODULE);
	if (rc)
		return rc;
	else
		return (int) count;
}


