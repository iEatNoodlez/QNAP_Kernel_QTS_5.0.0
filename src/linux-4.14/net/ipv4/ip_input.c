/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		The Internet Protocol (IP) module.
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Donald Becker, <becker@super.org>
 *		Alan Cox, <alan@lxorguk.ukuu.org.uk>
 *		Richard Underwood
 *		Stefan Becker, <stefanb@yello.ping.de>
 *		Jorge Cwik, <jorge@laser.satlink.net>
 *		Arnt Gulbrandsen, <agulbra@nvg.unit.no>
 *
 *
 * Fixes:
 *		Alan Cox	:	Commented a couple of minor bits of surplus code
 *		Alan Cox	:	Undefining IP_FORWARD doesn't include the code
 *					(just stops a compiler warning).
 *		Alan Cox	:	Frames with >=MAX_ROUTE record routes, strict routes or loose routes
 *					are junked rather than corrupting things.
 *		Alan Cox	:	Frames to bad broadcast subnets are dumped
 *					We used to process them non broadcast and
 *					boy could that cause havoc.
 *		Alan Cox	:	ip_forward sets the free flag on the
 *					new frame it queues. Still crap because
 *					it copies the frame but at least it
 *					doesn't eat memory too.
 *		Alan Cox	:	Generic queue code and memory fixes.
 *		Fred Van Kempen :	IP fragment support (borrowed from NET2E)
 *		Gerhard Koerting:	Forward fragmented frames correctly.
 *		Gerhard Koerting: 	Fixes to my fix of the above 8-).
 *		Gerhard Koerting:	IP interface addressing fix.
 *		Linus Torvalds	:	More robustness checks
 *		Alan Cox	:	Even more checks: Still not as robust as it ought to be
 *		Alan Cox	:	Save IP header pointer for later
 *		Alan Cox	:	ip option setting
 *		Alan Cox	:	Use ip_tos/ip_ttl settings
 *		Alan Cox	:	Fragmentation bogosity removed
 *					(Thanks to Mark.Bush@prg.ox.ac.uk)
 *		Dmitry Gorodchanin :	Send of a raw packet crash fix.
 *		Alan Cox	:	Silly ip bug when an overlength
 *					fragment turns up. Now frees the
 *					queue.
 *		Linus Torvalds/ :	Memory leakage on fragmentation
 *		Alan Cox	:	handling.
 *		Gerhard Koerting:	Forwarding uses IP priority hints
 *		Teemu Rantanen	:	Fragment problems.
 *		Alan Cox	:	General cleanup, comments and reformat
 *		Alan Cox	:	SNMP statistics
 *		Alan Cox	:	BSD address rule semantics. Also see
 *					UDP as there is a nasty checksum issue
 *					if you do things the wrong way.
 *		Alan Cox	:	Always defrag, moved IP_FORWARD to the config.in file
 *		Alan Cox	: 	IP options adjust sk->priority.
 *		Pedro Roque	:	Fix mtu/length error in ip_forward.
 *		Alan Cox	:	Avoid ip_chk_addr when possible.
 *	Richard Underwood	:	IP multicasting.
 *		Alan Cox	:	Cleaned up multicast handlers.
 *		Alan Cox	:	RAW sockets demultiplex in the BSD style.
 *		Gunther Mayer	:	Fix the SNMP reporting typo
 *		Alan Cox	:	Always in group 224.0.0.1
 *	Pauline Middelink	:	Fast ip_checksum update when forwarding
 *					Masquerading support.
 *		Alan Cox	:	Multicast loopback error for 224.0.0.1
 *		Alan Cox	:	IP_MULTICAST_LOOP option.
 *		Alan Cox	:	Use notifiers.
 *		Bjorn Ekwall	:	Removed ip_csum (from slhc.c too)
 *		Bjorn Ekwall	:	Moved ip_fast_csum to ip.h (inline!)
 *		Stefan Becker   :       Send out ICMP HOST REDIRECT
 *	Arnt Gulbrandsen	:	ip_build_xmit
 *		Alan Cox	:	Per socket routing cache
 *		Alan Cox	:	Fixed routing cache, added header cache.
 *		Alan Cox	:	Loopback didn't work right in original ip_build_xmit - fixed it.
 *		Alan Cox	:	Only send ICMP_REDIRECT if src/dest are the same net.
 *		Alan Cox	:	Incoming IP option handling.
 *		Alan Cox	:	Set saddr on raw output frames as per BSD.
 *		Alan Cox	:	Stopped broadcast source route explosions.
 *		Alan Cox	:	Can disable source routing
 *		Takeshi Sone    :	Masquerading didn't work.
 *	Dave Bonn,Alan Cox	:	Faster IP forwarding whenever possible.
 *		Alan Cox	:	Memory leaks, tramples, misc debugging.
 *		Alan Cox	:	Fixed multicast (by popular demand 8))
 *		Alan Cox	:	Fixed forwarding (by even more popular demand 8))
 *		Alan Cox	:	Fixed SNMP statistics [I think]
 *	Gerhard Koerting	:	IP fragmentation forwarding fix
 *		Alan Cox	:	Device lock against page fault.
 *		Alan Cox	:	IP_HDRINCL facility.
 *	Werner Almesberger	:	Zero fragment bug
 *		Alan Cox	:	RAW IP frame length bug
 *		Alan Cox	:	Outgoing firewall on build_xmit
 *		A.N.Kuznetsov	:	IP_OPTIONS support throughout the kernel
 *		Alan Cox	:	Multicast routing hooks
 *		Jos Vos		:	Do accounting *before* call_in_firewall
 *	Willy Konynenberg	:	Transparent proxying support
 *
 *
 *
 * To Fix:
 *		IP fragmentation wants rewriting cleanly. The RFC815 algorithm is much more efficient
 *		and could be made very efficient with the addition of some virtual memory hacks to permit
 *		the allocation of a buffer that can then be 'grown' by twiddling page tables.
 *		Output fragmentation wants updating along with the buffer management to use a single
 *		interleaved copy algorithm so that fragmenting has a one copy overhead. Actual packet
 *		output should probably do its own fragmentation at the UDP/RAW layer. TCP shouldn't cause
 *		fragmentation anyway.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt) "IPv4: " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>

#include <linux/net.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include <net/snmp.h>
#include <net/ip.h>
#include <net/protocol.h>
#include <net/route.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <net/raw.h>
#include <net/checksum.h>
#include <net/inet_ecn.h>
#include <linux/netfilter_ipv4.h>
#include <net/xfrm.h>
#include <linux/mroute.h>
#include <linux/netlink.h>
#include <net/dst_metadata.h>
#include <linux/syscalls.h>

 /* Patch by QNAP: Add IP filter */
#if defined(CONFIG_MACH_QNAPTS) && !defined(QNAP_HAL)
#include <qnap/pic.h>                 /* for deliver message to picd */
#endif

#if defined(CONFIG_MACH_QNAPTS) && defined(QNAP_HAL)
#include <qnap/hal_event.h>

void send_hal_ip_block_event(struct sk_buff *skb)
{
	NETLINK_EVT hal_event;
	unsigned short port;
	struct __netlink_ip_block_cb *ip_block_cb;
	struct iphdr *iph = ip_hdr(skb);
	unsigned char *raw = skb_network_header(skb);

	if (iph->protocol == 0x6 || iph->protocol == 0x11)
		port = ntohs(*(unsigned short *)&raw[22]);
	else
		port = 0;

	hal_event.type = HAL_EVENT_NET;
	hal_event.arg.action = BLOCK_IP_FILTER;
	ip_block_cb = &hal_event.arg.param.netlink_ip_block;
	ip_block_cb->addr = iph->saddr;
	ip_block_cb->protocol = iph->protocol;
	ip_block_cb->port = port;
	send_hal_netlink(&hal_event);
}
#endif

#ifdef CONFIG_MACH_QNAPTS
#include <linux/syscalls.h>

/* IP filter (ip security or ipsec) */
#define MAX_IPSEC_RULE_NUM    2048
#define MAX_PRIV_NUM          256
#define MAX_IPSEC_RULE_SIZE   (2+MAX_IPSEC_RULE_NUM*3)
#define IPSEC_RULE_NUM                ipsec_rules[1]
#define IPSEC_RULE_TYPE               ipsec_rules[0]
#define IPSEC_ACCEPT_ALL      0
#define IPSEC_DEF_ALLOWED     1
#define IPSEC_DEF_DENIED      2
#define IPSEC_DEF_PRIV_ALLOWED        3

static int ipsec_init;
static spinlock_t ipsec_lock;
static unsigned int ipsec_rules[MAX_IPSEC_RULE_SIZE] = { 0, 0};
static unsigned int privileged_list[MAX_PRIV_NUM];
/* added by Jeff on 2007/3/28 for ipsec control */

#define REVERSE(x) ((((x)<<24)&0xff000000) | \
		(((x)<<8) & 0x00ff0000) | \
		(((x)>>8) & 0x0000ff00) | \
		(((x)>>24) & 0x000000ff))

SYSCALL_DEFINE2(set_ipsec_rules, const char __user *, arg, int, len)
{
	unsigned int type;
	unsigned int *tmp_buf;

	if (!ipsec_init) {
		spin_lock_init(&ipsec_lock);
		memset(privileged_list, 0, sizeof(privileged_list));
		ipsec_init = 1;
	}
	if (len > sizeof(ipsec_rules)) {
		pr_warn("sys_set_ipsec_rules: len %d is over %lu\n",
			len, sizeof(ipsec_rules));
		/* we only handle the first MAX_IPSEC_RULE_NUM rules */
		len = sizeof(ipsec_rules);
	}
	if (copy_from_user(&type, arg, 4)) {
		pr_err("sys_set_ipsec_rules: copy_from_user failed!\n");
		return -EFAULT;
	}

	if (type == IPSEC_DEF_PRIV_ALLOWED) {
		unsigned int a[2];

		if (len != 12) {
			return -EFAULT;
		}
		if (copy_from_user(a, arg + 4, 8)) {
			return -EFAULT;
		}
		if (a[0] >= MAX_PRIV_NUM) {
			return -EFAULT;
		}
		spin_lock_irq(&ipsec_lock);
		privileged_list[a[0]] = a[1];
		spin_unlock_irq(&ipsec_lock);
		return 0;
	}

	tmp_buf = kmalloc(len, GFP_KERNEL);
	if(!tmp_buf)
		return -ENOMEM;

	if (copy_from_user(tmp_buf, arg, len)) {
		pr_err("sys_set_ipsec_rules: copy_from_user failed!\n");
		kfree(tmp_buf);
		return -EFAULT;
	}

	spin_lock_irq(&ipsec_lock);
	memcpy(ipsec_rules, tmp_buf, len);
	kfree(tmp_buf);

	IPSEC_RULE_NUM = ((len / 4) - 2) / 3;
#if 1 /* dump ipsec rules */
	{
		int i;
		unsigned int *p = ipsec_rules;

		pr_debug("rule type=%u, num=%u\n", *p, IPSEC_RULE_NUM);
		p += 2;
		for (i = 0; i < IPSEC_RULE_NUM; i++) {
			/* pr_debug("rule %u=[%lX,%08lX, %lX]\n", i, *p, *(p+1), *(p+2)); */
			p += 3;
		}
	}
#endif
	spin_unlock_irq(&ipsec_lock);
	return 0;
}

#ifdef __BIG_ENDIAN
#define ADDR_TYPE_BIT_MASK      0x80000000
#else
#define ADDR_TYPE_BIT_MASK      0x00000080
#endif
/* return 1 if violate ipsec rules */
static int filter_by_ipsec_rules(struct sk_buff *skb)
{
	struct iphdr *iph = ip_hdr(skb);
	int i, ret = 0;
	unsigned int *p = &ipsec_rules[2];

	if (!ipsec_init || iph->saddr == iph->daddr)
		return ret;

	if (skb->dev) {
		struct in_device *in_dev = __in_dev_get_rcu(skb->dev);

		if (in_dev) {
			struct in_ifaddr **ifap = NULL;
			struct in_ifaddr *ifa = NULL;

			for (ifap = &in_dev->ifa_list; (ifa = *ifap) != NULL; ifap = &ifa->ifa_next) {
				if (iph->saddr == ifa->ifa_address)
					return ret;
			}
		}
	}

	spin_lock_irq(&ipsec_lock);
	if (IPSEC_RULE_TYPE == IPSEC_ACCEPT_ALL) {
		spin_unlock_irq(&ipsec_lock);
		return ret;
	}

	/* apply privileged rules here */
	for (i = 0; i < MAX_PRIV_NUM; i++) {
		if (privileged_list[i] != 0 && privileged_list[i] == iph->saddr) {
			spin_unlock_irq(&ipsec_lock);
			return ret;
		}
	}

	for (i = 0; i < IPSEC_RULE_NUM; i++, p += 3) {
		unsigned int t = *p;
		unsigned int v = *(p+1);
		unsigned int type = *(p+2);
		unsigned int max = 0, min = 0, addr = 0;

		if (type == 2) {
			min = REVERSE(v);
			max = REVERSE(t);
			addr = REVERSE(iph->saddr);
		}

		if (((type == 0 && (t&ADDR_TYPE_BIT_MASK) == 0) && iph->saddr == v) ||
		    ((type == 1 && (t&ADDR_TYPE_BIT_MASK) == ADDR_TYPE_BIT_MASK) && (iph->saddr&t) == (v&t)) ||
				(type == 2 && addr >= min && addr <= max)) {
			if (type == 2 && iph->saddr >= min && iph->saddr <= max) {
				/* do nothing */
				/* pr_debug("Between ip range\n"); */
			}

			if (IPSEC_RULE_TYPE == IPSEC_DEF_DENIED)
				ret = 1;

			spin_unlock_irq(&ipsec_lock);
			/* pr_debug("In the list....and the rule type is [%d], ret = [%d]\n", IPSEC_RULE_TYPE, ret); */
			return ret;
		}
	}

	if (IPSEC_RULE_TYPE == IPSEC_DEF_ALLOWED)
		ret = 1;

	spin_unlock_irq(&ipsec_lock);
	return ret;
}

#define MAX_VIOLIST_NUM	256
static struct violist_st { unsigned int addr; int protocol; int port; u64 tick; } violist[MAX_VIOLIST_NUM];
static int violist_head = -1, violist_tail = -1;

static void add_to_violated_list(struct sk_buff *skb)
{
	struct iphdr *iph = ip_hdr(skb);
	int port = 0;
	int num = 0;
	unsigned char *raw = skb_network_header(skb);

	if (iph->protocol == 0x6 || iph->protocol == 0x11)
		port = (int)*((short *)&raw[22]);

	spin_lock_irq(&ipsec_lock);
	if (violist_head == -1) { /* empty */
		violist_head = 0;
		violist_tail = 1;
		violist[0].addr = iph->saddr;
		violist[0].protocol = iph->protocol;
		violist[0].tick = jiffies;
		violist[0].port = port;
		spin_unlock_irq(&ipsec_lock);
		return;
	}

	num = (violist_tail - violist_head + MAX_VIOLIST_NUM) % MAX_VIOLIST_NUM;
	if (num == MAX_VIOLIST_NUM - 1) /* full */
		violist_head = (violist_head+1) % MAX_VIOLIST_NUM;  /* advance head */

	violist[violist_tail].addr = iph->saddr;
	violist[violist_tail].protocol = iph->protocol;
	violist[violist_tail].tick = jiffies;
	violist[violist_tail].port = port;
	violist_tail = (violist_tail + 1) % MAX_VIOLIST_NUM;  /* advance tail */
	spin_unlock_irq(&ipsec_lock);
}

SYSCALL_DEFINE2(get_ipsec_vio_acc_list, char __user *, arg, int, len)
{
	int num, i;
	u64 tick = jiffies;
	struct violist_st *vbuf;

	if (!ipsec_init)
		return 0;

	vbuf = kmalloc(sizeof(violist), GFP_KERNEL);
	if(!vbuf)
		return -ENOMEM;

	spin_lock_irq(&ipsec_lock);
	if (violist_head == -1) { /* empty */
		spin_unlock_irq(&ipsec_lock);
		kfree(vbuf);
		return 0;
	}

	num = (violist_tail - violist_head + MAX_VIOLIST_NUM) % MAX_VIOLIST_NUM;
	if (len < sizeof(struct violist_st)*num) {
		spin_unlock_irq(&ipsec_lock);
		kfree(vbuf);
		return -EFAULT;
	}

	for (i = 0; i < num; i++, arg += sizeof(struct violist_st)) {
		violist[violist_head].tick = jiffies_to_msecs(tick - violist[violist_head].tick);
		memcpy(vbuf + i, &violist[violist_head], sizeof(struct violist_st));
		violist_head = (violist_head + 1) % MAX_VIOLIST_NUM; /* advance head */
	}

	violist_head = -1;
	violist_tail = -1;
	spin_unlock_irq(&ipsec_lock);

	if(copy_to_user(arg, vbuf, num * sizeof(struct violist_st))) {
		kfree(vbuf);
		return -EFAULT;
	}
	kfree(vbuf);
	return num;
}

int is_filtered_by_ipsec_rules(struct sk_buff *skb)
{
	if (filter_by_ipsec_rules(skb)) {
		add_to_violated_list(skb);
		/* Hugo add. notify picd */
#if !defined(QNAP_HAL)
		send_message_to_app(QNAP_BLOCK_IP_EVENT);
#else
		send_hal_ip_block_event(skb);
#endif
		return 1;
	}

	return 0;
}

#endif /* End IP filter (ip security or ipsec) */

/*
 *	Process Router Attention IP option (RFC 2113)
 */
bool ip_call_ra_chain(struct sk_buff *skb)
{
	struct ip_ra_chain *ra;
	u8 protocol = ip_hdr(skb)->protocol;
	struct sock *last = NULL;
	struct net_device *dev = skb->dev;
	struct net *net = dev_net(dev);

	for (ra = rcu_dereference(net->ipv4.ra_chain); ra; ra = rcu_dereference(ra->next)) {
		struct sock *sk = ra->sk;

		/* If socket is bound to an interface, only report
		 * the packet if it came  from that interface.
		 */
		if (sk && inet_sk(sk)->inet_num == protocol &&
		    (!sk->sk_bound_dev_if ||
		     sk->sk_bound_dev_if == dev->ifindex)) {
			if (ip_is_fragment(ip_hdr(skb))) {
				if (ip_defrag(net, skb, IP_DEFRAG_CALL_RA_CHAIN))
					return true;
			}
			if (last) {
				struct sk_buff *skb2 = skb_clone(skb, GFP_ATOMIC);
				if (skb2)
					raw_rcv(last, skb2);
			}
			last = sk;
		}
	}

	if (last) {
		raw_rcv(last, skb);
		return true;
	}
	return false;
}

static int ip_local_deliver_finish(struct net *net, struct sock *sk, struct sk_buff *skb)
{
	__skb_pull(skb, skb_network_header_len(skb));

	rcu_read_lock();
	{
		int protocol = ip_hdr(skb)->protocol;
		const struct net_protocol *ipprot;
		int raw;

	resubmit:
		raw = raw_local_deliver(skb, protocol);

		ipprot = rcu_dereference(inet_protos[protocol]);
		if (ipprot) {
			int ret;

			if (!ipprot->no_policy) {
				if (!xfrm4_policy_check(NULL, XFRM_POLICY_IN, skb)) {
					kfree_skb(skb);
					goto out;
				}
				nf_reset(skb);
			}
			ret = ipprot->handler(skb);
			if (ret < 0) {
				protocol = -ret;
				goto resubmit;
			}
			__IP_INC_STATS(net, IPSTATS_MIB_INDELIVERS);
		} else {
			if (!raw) {
				if (xfrm4_policy_check(NULL, XFRM_POLICY_IN, skb)) {
					__IP_INC_STATS(net, IPSTATS_MIB_INUNKNOWNPROTOS);
					icmp_send(skb, ICMP_DEST_UNREACH,
						  ICMP_PROT_UNREACH, 0);
				}
				kfree_skb(skb);
			} else {
				__IP_INC_STATS(net, IPSTATS_MIB_INDELIVERS);
				consume_skb(skb);
			}
		}
	}
 out:
	rcu_read_unlock();

	return 0;
}

/*
 * 	Deliver IP Packets to the higher protocol layers.
 */
int ip_local_deliver(struct sk_buff *skb)
{
	/*
	 *	Reassemble IP fragments.
	 */
	struct net *net = dev_net(skb->dev);

	if (ip_is_fragment(ip_hdr(skb))) {
		if (ip_defrag(net, skb, IP_DEFRAG_LOCAL_DELIVER))
			return 0;
	}

	return NF_HOOK(NFPROTO_IPV4, NF_INET_LOCAL_IN,
		       net, NULL, skb, skb->dev, NULL,
		       ip_local_deliver_finish);
}

static inline bool ip_rcv_options(struct sk_buff *skb)
{
	struct ip_options *opt;
	const struct iphdr *iph;
	struct net_device *dev = skb->dev;

	/* It looks as overkill, because not all
	   IP options require packet mangling.
	   But it is the easiest for now, especially taking
	   into account that combination of IP options
	   and running sniffer is extremely rare condition.
					      --ANK (980813)
	*/
	if (skb_cow(skb, skb_headroom(skb))) {
		__IP_INC_STATS(dev_net(dev), IPSTATS_MIB_INDISCARDS);
		goto drop;
	}

	iph = ip_hdr(skb);
	opt = &(IPCB(skb)->opt);
	opt->optlen = iph->ihl*4 - sizeof(struct iphdr);

	if (ip_options_compile(dev_net(dev), opt, skb)) {
		__IP_INC_STATS(dev_net(dev), IPSTATS_MIB_INHDRERRORS);
		goto drop;
	}

	if (unlikely(opt->srr)) {
		struct in_device *in_dev = __in_dev_get_rcu(dev);

		if (in_dev) {
			if (!IN_DEV_SOURCE_ROUTE(in_dev)) {
				if (IN_DEV_LOG_MARTIANS(in_dev))
					net_info_ratelimited("source route option %pI4 -> %pI4\n",
							     &iph->saddr,
							     &iph->daddr);
				goto drop;
			}
		}

		if (ip_options_rcv_srr(skb))
			goto drop;
	}

	return false;
drop:
	return true;
}

static int ip_rcv_finish(struct net *net, struct sock *sk, struct sk_buff *skb)
{
	const struct iphdr *iph = ip_hdr(skb);
	int (*edemux)(struct sk_buff *skb);
	struct net_device *dev = skb->dev;
	struct rtable *rt;
	int err;

	/* if ingress device is enslaved to an L3 master device pass the
	 * skb to its handler for processing
	 */
	skb = l3mdev_ip_rcv(skb);
	if (!skb)
		return NET_RX_SUCCESS;

	if (net->ipv4.sysctl_ip_early_demux &&
	    !skb_dst(skb) &&
	    !skb->sk &&
	    !ip_is_fragment(iph)) {
		const struct net_protocol *ipprot;
		int protocol = iph->protocol;

		ipprot = rcu_dereference(inet_protos[protocol]);
		if (ipprot && (edemux = READ_ONCE(ipprot->early_demux))) {
			err = edemux(skb);
			if (unlikely(err))
				goto drop_error;
			/* must reload iph, skb->head might have changed */
			iph = ip_hdr(skb);
		}
	}

	/*
	 *	Initialise the virtual path cache for the packet. It describes
	 *	how the packet travels inside Linux networking.
	 */
	if (!skb_valid_dst(skb)) {
		err = ip_route_input_noref(skb, iph->daddr, iph->saddr,
					   iph->tos, dev);
		if (unlikely(err))
			goto drop_error;
	}

#ifdef CONFIG_IP_ROUTE_CLASSID
	if (unlikely(skb_dst(skb)->tclassid)) {
		struct ip_rt_acct *st = this_cpu_ptr(ip_rt_acct);
		u32 idx = skb_dst(skb)->tclassid;
		st[idx&0xFF].o_packets++;
		st[idx&0xFF].o_bytes += skb->len;
		st[(idx>>16)&0xFF].i_packets++;
		st[(idx>>16)&0xFF].i_bytes += skb->len;
	}
#endif

	if (iph->ihl > 5 && ip_rcv_options(skb))
		goto drop;

	rt = skb_rtable(skb);
	if (rt->rt_type == RTN_MULTICAST) {
		__IP_UPD_PO_STATS(net, IPSTATS_MIB_INMCAST, skb->len);
	} else if (rt->rt_type == RTN_BROADCAST) {
		__IP_UPD_PO_STATS(net, IPSTATS_MIB_INBCAST, skb->len);
	} else if (skb->pkt_type == PACKET_BROADCAST ||
		   skb->pkt_type == PACKET_MULTICAST) {
		struct in_device *in_dev = __in_dev_get_rcu(dev);

		/* RFC 1122 3.3.6:
		 *
		 *   When a host sends a datagram to a link-layer broadcast
		 *   address, the IP destination address MUST be a legal IP
		 *   broadcast or IP multicast address.
		 *
		 *   A host SHOULD silently discard a datagram that is received
		 *   via a link-layer broadcast (see Section 2.4) but does not
		 *   specify an IP multicast or broadcast destination address.
		 *
		 * This doesn't explicitly say L2 *broadcast*, but broadcast is
		 * in a way a form of multicast and the most common use case for
		 * this is 802.11 protecting against cross-station spoofing (the
		 * so-called "hole-196" attack) so do it for both.
		 */
		if (in_dev &&
		    IN_DEV_ORCONF(in_dev, DROP_UNICAST_IN_L2_MULTICAST))
			goto drop;
	}

	return dst_input(skb);

drop:
	kfree_skb(skb);
	return NET_RX_DROP;

drop_error:
	if (err == -EXDEV)
		__NET_INC_STATS(net, LINUX_MIB_IPRPFILTER);
	goto drop;
}

/*
 * 	Main IP Receive routine.
 */
int ip_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt, struct net_device *orig_dev)
{
	const struct iphdr *iph;
	struct net *net;
	u32 len;

	/* When the interface is in promisc. mode, drop all the crap
	 * that it receives, do not try to analyse it.
	 */
	if (skb->pkt_type == PACKET_OTHERHOST)
		goto drop;


	net = dev_net(dev);
	__IP_UPD_PO_STATS(net, IPSTATS_MIB_IN, skb->len);

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb) {
		__IP_INC_STATS(net, IPSTATS_MIB_INDISCARDS);
		goto out;
	}

	if (!pskb_may_pull(skb, sizeof(struct iphdr)))
		goto inhdr_error;

	iph = ip_hdr(skb);

	/*
	 *	RFC1122: 3.2.1.2 MUST silently discard any IP frame that fails the checksum.
	 *
	 *	Is the datagram acceptable?
	 *
	 *	1.	Length at least the size of an ip header
	 *	2.	Version of 4
	 *	3.	Checksums correctly. [Speed optimisation for later, skip loopback checksums]
	 *	4.	Doesn't have a bogus length
	 */

	if (iph->ihl < 5 || iph->version != 4)
		goto inhdr_error;

	BUILD_BUG_ON(IPSTATS_MIB_ECT1PKTS != IPSTATS_MIB_NOECTPKTS + INET_ECN_ECT_1);
	BUILD_BUG_ON(IPSTATS_MIB_ECT0PKTS != IPSTATS_MIB_NOECTPKTS + INET_ECN_ECT_0);
	BUILD_BUG_ON(IPSTATS_MIB_CEPKTS != IPSTATS_MIB_NOECTPKTS + INET_ECN_CE);
	__IP_ADD_STATS(net,
		       IPSTATS_MIB_NOECTPKTS + (iph->tos & INET_ECN_MASK),
		       max_t(unsigned short, 1, skb_shinfo(skb)->gso_segs));

	if (!pskb_may_pull(skb, iph->ihl*4))
		goto inhdr_error;

	iph = ip_hdr(skb);

	if (unlikely(ip_fast_csum((u8 *)iph, iph->ihl)))
		goto csum_error;

	len = ntohs(iph->tot_len);
	if (skb->len < len) {
		__IP_INC_STATS(net, IPSTATS_MIB_INTRUNCATEDPKTS);
		goto drop;
	} else if (len < (iph->ihl*4))
		goto inhdr_error;

	/* Our transport medium may have padded the buffer out. Now we know it
	 * is IP we can trim to the true length of the frame.
	 * Note this now means skb->len holds ntohs(iph->tot_len).
	 */
	if (pskb_trim_rcsum(skb, len)) {
		__IP_INC_STATS(net, IPSTATS_MIB_INDISCARDS);
		goto drop;
	}

	skb->transport_header = skb->network_header + iph->ihl*4;

	/* Remove any debris in the socket control block */
	memset(IPCB(skb), 0, sizeof(struct inet_skb_parm));
	IPCB(skb)->iif = skb->skb_iif;

/* Patch by QNAP: Add IP filter */
#ifdef CONFIG_MACH_QNAPTS
	if (iph->protocol == 0x11) {
		unsigned char *raw = skb_network_header(skb);
#ifdef __BIG_ENDIAN
		int port = (int)*((short *)&raw[22]);
#else
		int port = (int)(raw[22] << 8 | raw[23]);
#endif

		if (port == 9500 || port == 1900 || port == 8097) { /* bcclient(9500, 8097) ssdp(1900) */
			if (is_filtered_by_ipsec_rules(skb)) {
				kfree_skb(skb);
				return NET_RX_DROP;
			}
		}
	}
#endif /* CONFIG_MACH_QNAPTS */

	/* Must drop socket now because of tproxy. */
	skb_orphan(skb);

	return NF_HOOK(NFPROTO_IPV4, NF_INET_PRE_ROUTING,
		       net, NULL, skb, dev, NULL,
		       ip_rcv_finish);

csum_error:
	__IP_INC_STATS(net, IPSTATS_MIB_CSUMERRORS);
inhdr_error:
	__IP_INC_STATS(net, IPSTATS_MIB_INHDRERRORS);
drop:
	kfree_skb(skb);
out:
	return NET_RX_DROP;
}
