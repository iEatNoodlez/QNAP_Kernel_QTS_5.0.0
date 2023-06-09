/*
 * linux/net/sunrpc/stats.c
 *
 * procfs-based user access to generic RPC statistics. The stats files
 * reside in /proc/net/rpc.
 *
 * The read routines assume that the buffer passed in is just big enough.
 * If you implement an RPC service that has its own stats routine which
 * appends the generic RPC stats, make sure you don't exceed the PAGE_SIZE
 * limit.
 *
 * Copyright (C) 1995, 1996, 1997 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/module.h>
#include <linux/slab.h>

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/metrics.h>
#include <linux/rcupdate.h>

#include "netns.h"

#define RPCDBG_FACILITY	RPCDBG_MISC

/* Patch by QNAP: Setup a interface to rewrite payload size */
#ifdef CONFIG_MACH_QNAPTS
#include <linux/pagemap.h>
#define TCP_PAYLOAD_NAME "tcp_payload_size"
#define UDP_PAYLOAD_NAME "udp_payload_size"
static unsigned int paylod_perm = 0644;
struct qnap_nfs_payload {
	char name[128];
	u32 payload_size;
};

static int payload_show(struct seq_file *seq, void *v)
{
	const struct qnap_nfs_payload *payload = seq->private;

	if(!strcmp(payload->name, TCP_PAYLOAD_NAME))
	    seq_printf(seq, "%u\n", svc_tcp_read_payload());
	else if(!strcmp(payload->name, UDP_PAYLOAD_NAME))
	    seq_printf(seq, "%u\n", svc_udp_read_payload());

	return 0;
}

static uint32_t my_atou(const char *name)
{
	uint32_t val = 0;

	for (;; name++) {
		switch (*name) {
		case '0' ... '9':
			val = 10*val+(*name-'0');
			break;
		default:
			return val;
		}
	}
}

static int payload_open(struct inode *inode, struct file *file)
{
	return single_open(file, payload_show, PDE_DATA(inode));
}


static ssize_t tcp_payload_write(struct file *file, const char __user *input,
        size_t size, loff_t *loff)
{
        char buf[128];
        uint32_t payload_size;
        if (size == 0)
                return 0;
        memset(buf, 0, sizeof(buf));
        if (size > sizeof(buf))
                size = sizeof(buf);
        if (copy_from_user(buf, input, size) != 0)
                return -EFAULT;
        if (*loff != 0)
                return -ESPIPE;
        payload_size = my_atou(buf);
	if(     (payload_size >= PAGE_SIZE) &&
                (payload_size <= RPCSVC_MAXPAYLOAD ) &&
                (payload_size % PAGE_SIZE == 0))
        {
                svc_tcp_set_payload(payload_size);
                
        }else{
                printk("invalid payload size %u (should be multiples of %lu between %lu - %u)\n",
                        payload_size, PAGE_SIZE, PAGE_SIZE, RPCSVC_MAXPAYLOAD);
        }

        return size;
}

static ssize_t udp_payload_write(struct file *file, const char __user *input,
        size_t size, loff_t *loff)
{
        char buf[128];
        uint32_t payload_size;
        if (size == 0)
                return 0;
        memset(buf, 0, sizeof(buf));
        if (size > sizeof(buf))
                size = sizeof(buf);
        if (copy_from_user(buf, input, size) != 0)
                return -EFAULT;
        if (*loff != 0)
                return -ESPIPE;
        payload_size = my_atou(buf);
	if(     (payload_size >= PAGE_SIZE) &&
                (payload_size <= RPCSVC_MAXPAYLOAD ) &&
                (payload_size % PAGE_SIZE == 0))
        {
                svc_udp_set_payload(payload_size);
                

        }else{
                printk("invalid payload size %s (should be multiples of %lu between %lu - %u)\n",
                        buf, PAGE_SIZE, PAGE_SIZE, RPCSVC_MAXPAYLOAD);
                
        }

	return size;
}

static ssize_t payload_write(struct file *file, const char __user *input,
			     size_t size, loff_t *loff)
{
	const struct qnap_proc_dir_entry *pde =
		PDE_DATA(file->f_path.dentry->d_inode);
	struct qnap_nfs_payload *payload = pde->data;
	char buf[128];
	uint32_t payload_size;

	if (size == 0)
		return 0;
	memset(buf, 0, sizeof(buf));
	if (size > sizeof(buf))
		size = sizeof(buf);
	if (copy_from_user(buf, input, size) != 0)
		return -EFAULT;

	if (*loff != 0)
		return -ESPIPE;
	payload_size = my_atou(buf);

	if ((!strcmp(payload->name, TCP_PAYLOAD_NAME)) &&
	    (payload_size >= PAGE_SIZE) &&
	    (payload_size <= RPCSVC_MAXPAYLOAD) &&
	    (payload_size % PAGE_SIZE == 0)) {
		payload->payload_size = payload_size;
		svc_tcp_set_payload(payload->payload_size);
		return size;

	} else if ((!strcmp(payload->name, UDP_PAYLOAD_NAME)) &&
		   (payload_size >= PAGE_SIZE) &&
		   (payload_size <= RPCSVC_MAXPAYLOAD) &&
		   (payload_size % PAGE_SIZE == 0)) {
		payload->payload_size = payload_size;
		svc_udp_set_payload(payload->payload_size);
		return size;
	}else{
		printk("invalid payload size %s (should be multiples of %lu between %lu - %u)\n",
			buf, PAGE_SIZE, PAGE_SIZE, RPCSVC_MAXPAYLOAD);
		return size;
	}
}

static const struct file_operations payload_fops = {
	.owner = THIS_MODULE,
	.open = payload_open,
	.read  = seq_read,
	.write = payload_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations tcp_payload_fops = {
        .owner = THIS_MODULE,
        .open = payload_open,
        .read  = seq_read,
        .write = tcp_payload_write,
        .llseek = seq_lseek,
        .release = single_release,
};
static const struct file_operations udp_payload_fops = {
        .owner = THIS_MODULE,
        .open = payload_open,
        .read  = seq_read,
        .write = udp_payload_write,
        .llseek = seq_lseek,
        .release = single_release,
};


#endif /* CONFIG_MACH_QNAPTS */

/*
 * Get RPC client stats
 */
static int rpc_proc_show(struct seq_file *seq, void *v) {
	const struct rpc_stat	*statp = seq->private;
	const struct rpc_program *prog = statp->program;
	unsigned int i, j;

	seq_printf(seq,
		"net %u %u %u %u\n",
			statp->netcnt,
			statp->netudpcnt,
			statp->nettcpcnt,
			statp->nettcpconn);
	seq_printf(seq,
		"rpc %u %u %u\n",
			statp->rpccnt,
			statp->rpcretrans,
			statp->rpcauthrefresh);

	for (i = 0; i < prog->nrvers; i++) {
		const struct rpc_version *vers = prog->version[i];
		if (!vers)
			continue;
		seq_printf(seq, "proc%u %u",
					vers->number, vers->nrprocs);
		for (j = 0; j < vers->nrprocs; j++)
			seq_printf(seq, " %u", vers->counts[j]);
		seq_putc(seq, '\n');
	}
	return 0;
}

static int rpc_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, rpc_proc_show, PDE_DATA(inode));
}

static const struct file_operations rpc_proc_fops = {
	.owner = THIS_MODULE,
	.open = rpc_proc_open,
	.read  = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/*
 * Get RPC server stats
 */
void svc_seq_show(struct seq_file *seq, const struct svc_stat *statp)
{
	const struct svc_program *prog = statp->program;
	const struct svc_version *vers;
	unsigned int i, j;

	seq_printf(seq,
		"net %u %u %u %u\n",
			statp->netcnt,
			statp->netudpcnt,
			statp->nettcpcnt,
			statp->nettcpconn);
	seq_printf(seq,
		"rpc %u %u %u %u %u\n",
			statp->rpccnt,
			statp->rpcbadfmt+statp->rpcbadauth+statp->rpcbadclnt,
			statp->rpcbadfmt,
			statp->rpcbadauth,
			statp->rpcbadclnt);

	for (i = 0; i < prog->pg_nvers; i++) {
		vers = prog->pg_vers[i];
		if (!vers)
			continue;
		seq_printf(seq, "proc%d %u", i, vers->vs_nproc);
		for (j = 0; j < vers->vs_nproc; j++)
			seq_printf(seq, " %u", vers->vs_count[j]);
		seq_putc(seq, '\n');
	}
}
EXPORT_SYMBOL_GPL(svc_seq_show);

/**
 * rpc_alloc_iostats - allocate an rpc_iostats structure
 * @clnt: RPC program, version, and xprt
 *
 */
struct rpc_iostats *rpc_alloc_iostats(struct rpc_clnt *clnt)
{
	struct rpc_iostats *stats;
	int i;

	stats = kcalloc(clnt->cl_maxproc, sizeof(*stats), GFP_KERNEL);
	if (stats) {
		for (i = 0; i < clnt->cl_maxproc; i++)
			spin_lock_init(&stats[i].om_lock);
	}
	return stats;
}
EXPORT_SYMBOL_GPL(rpc_alloc_iostats);

/**
 * rpc_free_iostats - release an rpc_iostats structure
 * @stats: doomed rpc_iostats structure
 *
 */
void rpc_free_iostats(struct rpc_iostats *stats)
{
	kfree(stats);
}
EXPORT_SYMBOL_GPL(rpc_free_iostats);

/**
 * rpc_count_iostats_metrics - tally up per-task stats
 * @task: completed rpc_task
 * @op_metrics: stat structure for OP that will accumulate stats from @task
 */
void rpc_count_iostats_metrics(const struct rpc_task *task,
			       struct rpc_iostats *op_metrics)
{
	struct rpc_rqst *req = task->tk_rqstp;
	ktime_t delta, now;

	if (!op_metrics || !req)
		return;

	now = ktime_get();
	spin_lock(&op_metrics->om_lock);

	op_metrics->om_ops++;
	/* kernel API: om_ops must never become larger than om_ntrans */
	op_metrics->om_ntrans += max(req->rq_ntrans, 1);
	op_metrics->om_timeouts += task->tk_timeouts;

	op_metrics->om_bytes_sent += req->rq_xmit_bytes_sent;
	op_metrics->om_bytes_recv += req->rq_reply_bytes_recvd;

	if (ktime_to_ns(req->rq_xtime)) {
		delta = ktime_sub(req->rq_xtime, task->tk_start);
		op_metrics->om_queue = ktime_add(op_metrics->om_queue, delta);
	}
	op_metrics->om_rtt = ktime_add(op_metrics->om_rtt, req->rq_rtt);

	delta = ktime_sub(now, task->tk_start);
	op_metrics->om_execute = ktime_add(op_metrics->om_execute, delta);

	spin_unlock(&op_metrics->om_lock);
}
EXPORT_SYMBOL_GPL(rpc_count_iostats_metrics);

/**
 * rpc_count_iostats - tally up per-task stats
 * @task: completed rpc_task
 * @stats: array of stat structures
 *
 * Uses the statidx from @task
 */
void rpc_count_iostats(const struct rpc_task *task, struct rpc_iostats *stats)
{
	rpc_count_iostats_metrics(task,
				  &stats[task->tk_msg.rpc_proc->p_statidx]);
}
EXPORT_SYMBOL_GPL(rpc_count_iostats);

static void _print_name(struct seq_file *seq, unsigned int op,
			const struct rpc_procinfo *procs)
{
	if (procs[op].p_name)
		seq_printf(seq, "\t%12s: ", procs[op].p_name);
	else if (op == 0)
		seq_printf(seq, "\t        NULL: ");
	else
		seq_printf(seq, "\t%12u: ", op);
}

void rpc_print_iostats(struct seq_file *seq, struct rpc_clnt *clnt)
{
	struct rpc_iostats *stats = clnt->cl_metrics;
	struct rpc_xprt *xprt;
	unsigned int op, maxproc = clnt->cl_maxproc;

	if (!stats)
		return;

	seq_printf(seq, "\tRPC iostats version: %s  ", RPC_IOSTATS_VERS);
	seq_printf(seq, "p/v: %u/%u (%s)\n",
			clnt->cl_prog, clnt->cl_vers, clnt->cl_program->name);

	rcu_read_lock();
	xprt = rcu_dereference(clnt->cl_xprt);
	if (xprt)
		xprt->ops->print_stats(xprt, seq);
	rcu_read_unlock();

	seq_printf(seq, "\tper-op statistics\n");
	for (op = 0; op < maxproc; op++) {
		struct rpc_iostats *metrics = &stats[op];
		_print_name(seq, op, clnt->cl_procinfo);
		seq_printf(seq, "%lu %lu %lu %Lu %Lu %Lu %Lu %Lu\n",
				metrics->om_ops,
				metrics->om_ntrans,
				metrics->om_timeouts,
				metrics->om_bytes_sent,
				metrics->om_bytes_recv,
				ktime_to_ms(metrics->om_queue),
				ktime_to_ms(metrics->om_rtt),
				ktime_to_ms(metrics->om_execute));
	}
}
EXPORT_SYMBOL_GPL(rpc_print_iostats);

/*
 * Register/unregister RPC proc files
 */
static inline struct proc_dir_entry *
do_register(struct net *net, const char *name, void *data,
	    const struct file_operations *fops)
{
	struct sunrpc_net *sn;

	dprintk("RPC:       registering /proc/net/rpc/%s\n", name);
	sn = net_generic(net, sunrpc_net_id);
	return proc_create_data(name, 0, sn->proc_net_rpc, fops, data);
}

struct proc_dir_entry *
rpc_proc_register(struct net *net, struct rpc_stat *statp)
{
	return do_register(net, statp->program->name, statp, &rpc_proc_fops);
}
EXPORT_SYMBOL_GPL(rpc_proc_register);

void
rpc_proc_unregister(struct net *net, const char *name)
{
	struct sunrpc_net *sn;

	sn = net_generic(net, sunrpc_net_id);
	remove_proc_entry(name, sn->proc_net_rpc);
}
EXPORT_SYMBOL_GPL(rpc_proc_unregister);

struct proc_dir_entry *
svc_proc_register(struct net *net, struct svc_stat *statp, const struct file_operations *fops)
{
	return do_register(net, statp->program->pg_name, statp, fops);
}
EXPORT_SYMBOL_GPL(svc_proc_register);

void
svc_proc_unregister(struct net *net, const char *name)
{
	struct sunrpc_net *sn;

	sn = net_generic(net, sunrpc_net_id);
	remove_proc_entry(name, sn->proc_net_rpc);
}
EXPORT_SYMBOL_GPL(svc_proc_unregister);

int rpc_proc_init(struct net *net)
{
	struct sunrpc_net *sn;
#ifdef CONFIG_MACH_QNAPTS
	struct qnap_nfs_payload *tcp_payload, *udp_payload;
#endif

	dprintk("RPC:       registering /proc/net/rpc\n");
	sn = net_generic(net, sunrpc_net_id);
	sn->proc_net_rpc = proc_mkdir("rpc", net->proc_net);
	if (sn->proc_net_rpc == NULL)
		return -ENOMEM;

	/* Patch by QNAP Setup a interface to rewrite payload size */
#ifdef CONFIG_MACH_QNAPTS
	/* TCP */
	tcp_payload = kmalloc(sizeof(*tcp_payload), GFP_ATOMIC);
	if (!tcp_payload) {
		sn->proc_net_rpc_tcp_payload = NULL;
		return -ENOMEM;
	}
	snprintf(tcp_payload->name, sizeof(tcp_payload->name),
		 TCP_PAYLOAD_NAME);
	sn->proc_net_rpc_tcp_payload = proc_create_data(tcp_payload->name,
							paylod_perm,
							sn->proc_net_rpc,
							&tcp_payload_fops,
							tcp_payload);

	if (!sn->proc_net_rpc_tcp_payload) {
		kfree(tcp_payload);
		return -ENOMEM;
	}
	/* UDP */
	udp_payload = kmalloc(sizeof(*udp_payload), GFP_ATOMIC);
	if (!udp_payload) {
		sn->proc_net_rpc_udp_payload = NULL;
		kfree(tcp_payload);
		return -ENOMEM;
	}
	snprintf(udp_payload->name, sizeof(udp_payload->name),
		 UDP_PAYLOAD_NAME);
	sn->proc_net_rpc_udp_payload = proc_create_data(udp_payload->name,
							paylod_perm,
							sn->proc_net_rpc,
							&udp_payload_fops,
							udp_payload);
	if (!sn->proc_net_rpc_udp_payload) {
		kfree(udp_payload);
		kfree(tcp_payload);
		return -ENOMEM;
	}

#endif /* CONFIG_MACH_QNAPTS */
	return 0;
}

void rpc_proc_exit(struct net *net)
{
	dprintk("RPC:       unregistering /proc/net/rpc\n");
#ifdef CONFIG_MACH_QNAPTS
	remove_proc_entry("rpc/tcp_payload_size", net->proc_net);
	remove_proc_entry("rpc/udp_payload_size", net->proc_net);
#endif
	remove_proc_entry("rpc", net->proc_net);
}

