/*
 * Copyright (c) 2007-2012 Nicira, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/etherdevice.h>
#include <linux/if.h>
#include <linux/if_vlan.h>
#include <linux/jhash.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <linux/compat.h>
#include <linux/version.h>
#include <net/net_namespace.h>

#include "datapath.h"
#include "vport.h"
#include "vport-internal_dev.h"
#include "vport-netdev.h"

/* List of statically compiled vport implementations.  Don't forget to also
 * add yours to the list at the bottom of vport.h. */
static const struct vport_ops *vport_ops_list[] = {
	&ovs_netdev_vport_ops,
	&ovs_internal_vport_ops,
#if IS_ENABLED(CONFIG_NET_IPGRE_DEMUX)
	&ovs_gre_vport_ops,
	&ovs_gre64_vport_ops,
#endif
	&ovs_vxlan_vport_ops,
	&ovs_lisp_vport_ops,
};

/* Protected by RCU read lock for reading, ovs_mutex for writing. */
static struct hlist_head *dev_table;
#define VPORT_HASH_BUCKETS 1024

/**
 *	ovs_vport_init - initialize vport subsystem
 *
 * Called at module load time to initialize the vport subsystem.
 */
int ovs_vport_init(void)
{
	dev_table = kzalloc(VPORT_HASH_BUCKETS * sizeof(struct hlist_head),
			    GFP_KERNEL);
	if (!dev_table)
		return -ENOMEM;

	return 0;
}

/**
 *	ovs_vport_exit - shutdown vport subsystem
 *
 * Called at module exit time to shutdown the vport subsystem.
 */
void ovs_vport_exit(void)
{
	kfree(dev_table);
}

static struct hlist_head *hash_bucket(struct net *net, const char *name)
{
	unsigned int hash = jhash(name, strlen(name), (unsigned long) net);
	return &dev_table[hash & (VPORT_HASH_BUCKETS - 1)];
}

/**
 *	ovs_vport_locate - find a port that has already been created
 *
 * @name: name of port to find
 *
 * Must be called with ovs or RCU read lock.
 */
struct vport *ovs_vport_locate(struct net *net, const char *name)
{
	struct hlist_head *bucket = hash_bucket(net, name);
	struct vport *vport;

	hlist_for_each_entry_rcu(vport, bucket, hash_node)
		if (!strcmp(name, vport->ops->get_name(vport)) &&
		    net_eq(ovs_dp_get_net(vport->dp), net))
			return vport;

	return NULL;
}

/**
 *	ovs_vport_alloc - allocate and initialize new vport
 *
 * @priv_size: Size of private data area to allocate.
 * @ops: vport device ops
 *
 * Allocate and initialize a new vport defined by @ops.  The vport will contain
 * a private data area of size @priv_size that can be accessed using
 * vport_priv().  vports that are no longer needed should be released with
 * ovs_vport_free().
 */
struct vport *ovs_vport_alloc(int priv_size, const struct vport_ops *ops,
			      const struct vport_parms *parms)
{
	struct vport *vport;
	size_t alloc_size;

	alloc_size = sizeof(struct vport);
	if (priv_size) {
		alloc_size = ALIGN(alloc_size, VPORT_ALIGN);
		alloc_size += priv_size;
	}

	vport = kzalloc(alloc_size, GFP_KERNEL);
	if (!vport)
		return ERR_PTR(-ENOMEM);

	vport->dp = parms->dp;
	vport->port_no = parms->port_no;
	vport->upcall_portid = parms->upcall_portid;
	vport->ops = ops;
	vport->ipv4_reasm = parms->ipv4_reasm;
	INIT_HLIST_NODE(&vport->dp_hash_node);

	vport->percpu_stats = alloc_percpu(struct pcpu_tstats);
	if (!vport->percpu_stats) {
		kfree(vport);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock_init(&vport->stats_lock);

	return vport;
}

/**
 *	ovs_vport_free - uninitialize and free vport
 *
 * @vport: vport to free
 *
 * Frees a vport allocated with ovs_vport_alloc() when it is no longer needed.
 *
 * The caller must ensure that an RCU grace period has passed since the last
 * time @vport was in a datapath.
 */
void ovs_vport_free(struct vport *vport)
{
	free_percpu(vport->percpu_stats);
	kfree(vport);
}

/**
 *	ovs_vport_add - add vport device (for kernel callers)
 *
 * @parms: Information about new vport.
 *
 * Creates a new vport with the specified configuration (which is dependent on
 * device type).  ovs_mutex must be held.
 */
struct vport *ovs_vport_add(const struct vport_parms *parms)
{
	struct vport *vport;
	int err = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(vport_ops_list); i++) {
		if (vport_ops_list[i]->type == parms->type) {
			struct hlist_head *bucket;

			vport = vport_ops_list[i]->create(parms);
			if (IS_ERR(vport)) {
				err = PTR_ERR(vport);
				goto out;
			}

			bucket = hash_bucket(ovs_dp_get_net(vport->dp),
					     vport->ops->get_name(vport));
			hlist_add_head_rcu(&vport->hash_node, bucket);
			return vport;
		}
	}

	err = -EAFNOSUPPORT;

out:
	return ERR_PTR(err);
}

/**
 *	ovs_vport_set_options - modify existing vport device (for kernel callers)
 *
 * @vport: vport to modify.
 * @options: New configuration.
 *
 * Modifies an existing device with the specified configuration (which is
 * dependent on device type).  ovs_mutex must be held.
 */
int ovs_vport_set_options(struct vport *vport, struct nlattr *options)
{
	if (!vport->ops->set_options)
		return -EOPNOTSUPP;
	return vport->ops->set_options(vport, options);
}

/**
 *	ovs_vport_del - delete existing vport device
 *
 * @vport: vport to delete.
 *
 * Detaches @vport from its datapath and destroys it.  It is possible to fail
 * for reasons such as lack of memory.  ovs_mutex must be held.
 */
void ovs_vport_del(struct vport *vport)
{
	ASSERT_OVSL();

	hlist_del_rcu(&vport->hash_node);
	vport->ops->destroy(vport);
}

/**
 *	ovs_vport_set_stats - sets offset device stats
 *
 * @vport: vport on which to set stats
 * @stats: stats to set
 *
 * Provides a set of transmit, receive, and error stats to be added as an
 * offset to the collected data when stats are retrieved.  Some devices may not
 * support setting the stats, in which case the result will always be
 * -EOPNOTSUPP.
 *
 * Must be called with ovs_mutex.
 */
void ovs_vport_set_stats(struct vport *vport, struct ovs_vport_stats *stats)
{
	spin_lock_bh(&vport->stats_lock);
	vport->offset_stats = *stats;
	spin_unlock_bh(&vport->stats_lock);
}

/**
 *	ovs_vport_get_stats - retrieve device stats
 *
 * @vport: vport from which to retrieve the stats
 * @stats: location to store stats
 *
 * Retrieves transmit, receive, and error stats for the given device.
 *
 * Must be called with ovs_mutex or rcu_read_lock.
 */
void ovs_vport_get_stats(struct vport *vport, struct ovs_vport_stats *stats)
{
	int i;

	/* We potentially have 3 sources of stats that need to be
	 * combined: those we have collected (split into err_stats and
	 * percpu_stats), offset_stats from set_stats(), and device
	 * error stats from netdev->get_stats() (for errors that happen
	 * downstream and therefore aren't reported through our
	 * vport_record_error() function).
	 * Stats from first two sources are merged and reported by ovs over
	 * OVS_VPORT_ATTR_STATS.
	 * netdev-stats can be directly read over netlink-ioctl.
	 */

	spin_lock_bh(&vport->stats_lock);

	*stats = vport->offset_stats;

	stats->rx_errors	+= vport->err_stats.rx_errors;
	stats->tx_errors	+= vport->err_stats.tx_errors;
	stats->tx_dropped	+= vport->err_stats.tx_dropped;
	stats->rx_dropped	+= vport->err_stats.rx_dropped;

	spin_unlock_bh(&vport->stats_lock);

	for_each_possible_cpu(i) {
		const struct pcpu_tstats *percpu_stats;
		struct pcpu_tstats local_stats;
		unsigned int start;

		percpu_stats = per_cpu_ptr(vport->percpu_stats, i);

		do {
			start = u64_stats_fetch_begin_bh(&percpu_stats->syncp);
			local_stats = *percpu_stats;
		} while (u64_stats_fetch_retry_bh(&percpu_stats->syncp, start));

		stats->rx_bytes		+= local_stats.rx_bytes;
		stats->rx_packets	+= local_stats.rx_packets;
		stats->tx_bytes		+= local_stats.tx_bytes;
		stats->tx_packets	+= local_stats.tx_packets;
	}
}

/**
 *	ovs_vport_get_options - retrieve device options
 *
 * @vport: vport from which to retrieve the options.
 * @skb: sk_buff where options should be appended.
 *
 * Retrieves the configuration of the given device, appending an
 * %OVS_VPORT_ATTR_OPTIONS attribute that in turn contains nested
 * vport-specific attributes to @skb.
 *
 * Returns 0 if successful, -EMSGSIZE if @skb has insufficient room, or another
 * negative error code if a real error occurred.  If an error occurs, @skb is
 * left unmodified.
 *
 * Must be called with ovs_mutex or rcu_read_lock.
 */
int ovs_vport_get_options(const struct vport *vport, struct sk_buff *skb)
{
	struct nlattr *nla;
	int err;

	if (!vport->ops->get_options)
		return 0;

	nla = nla_nest_start(skb, OVS_VPORT_ATTR_OPTIONS);
	if (!nla)
		return -EMSGSIZE;

	err = vport->ops->get_options(vport, skb);
	if (err) {
		nla_nest_cancel(skb, nla);
		return err;
	}

	nla_nest_end(skb, nla);
	return 0;
}

/**
 *	ovs_vport_receive - pass up received packet to the datapath for processing
 *
 * @vport: vport that received the packet
 * @skb: skb that was received
 * @tun_key: tunnel (if any) that carried packet
 *
 * Must be called with rcu_read_lock.  The packet cannot be shared and
 * skb->data should point to the Ethernet header.  The caller must have already
 * called compute_ip_summed() to initialize the checksumming fields.
 */
void ovs_vport_receive(struct vport *vport, struct sk_buff *skb,
		       struct ovs_key_ipv4_tunnel *tun_key)
{
	struct pcpu_tstats *stats;

	stats = this_cpu_ptr(vport->percpu_stats);
	u64_stats_update_begin(&stats->syncp);
	stats->rx_packets++;
	stats->rx_bytes += skb->len;
	u64_stats_update_end(&stats->syncp);

	OVS_CB(skb)->tun_key = tun_key;
	ovs_dp_process_received_packet(vport, skb);
}

static int __ovs_vport_send(struct vport *vport, struct sk_buff *skb)
{
	int sent = vport->ops->send(vport, skb);

	if (likely(sent > 0)) {
		struct pcpu_tstats *stats;

		stats = this_cpu_ptr(vport->percpu_stats);

		u64_stats_update_begin(&stats->syncp);
		stats->tx_packets++;
		stats->tx_bytes += sent;
		u64_stats_update_end(&stats->syncp);
	} else if (sent < 0) {
		ovs_vport_record_error(vport, VPORT_E_TX_ERROR);
		kfree_skb(skb);
	} else
		ovs_vport_record_error(vport, VPORT_E_TX_DROPPED);

	return sent;
}

static struct sk_buff *
ovs_vlan_untag(struct sk_buff *skb)
{
	u16 proto, tci;

	if (unlikely(eth_hdr(skb)->h_proto != htons(ETH_P_8021Q)))
		return NULL;

	tci = vlan_eth_hdr(skb)->h_vlan_TCI;
	proto = vlan_eth_hdr(skb)->h_vlan_proto;

	if (unlikely(skb_cow_head(skb, 0) != 0))
		return NULL;

	memmove((void*)vlan_eth_hdr(skb) + VLAN_HLEN,
		(void*)skb->data, ETH_ALEN * 2);
	skb_pull(skb, VLAN_HLEN);
	skb_reset_mac_header(skb);

        __vlan_hwaccel_put_tag(skb, proto, ntohs(tci));
	return skb;
}

static int
ovs_vport_fragment(struct vport *vport, struct sk_buff *skb,
		   unsigned int frag_max_size, unsigned int mtu)
{
	unsigned int ip_hlen = ip_hdrlen(skb);
	unsigned int flag = ntohs(ip_hdr(skb)->frag_off) & IP_DF;
	unsigned int left = ntohs(ip_hdr(skb)->tot_len) - ip_hlen;
	unsigned int frag_max = ((frag_max_size > 0 ? frag_max_size : mtu)
				 - ip_hlen) & ~7;
	unsigned int frag_off = 0;
	unsigned int frag_len;
	struct sk_buff *frag;
	int sent = 0;

	net_info_ratelimited("FRAG: net=%p dp=%s port=%s(%u) %pI4 -> %pI4 proto=%u tot_len=%u frag_max_size=%u mtu=%u\n",
			     vport->dp->net,
			     ovs_dp_name(vport->dp),
			     vport->ops->get_name(vport),
			     vport->port_no,
			     &ip_hdr(skb)->saddr,
			     &ip_hdr(skb)->daddr,
			     ip_hdr(skb)->protocol,
			     ntohs(ip_hdr(skb)->tot_len),
			     frag_max_size,
			     mtu);

	while (left > 0) {
		unsigned int len;

		if (left > frag_max) {
			flag |= IP_MF;
			frag_len = frag_max;
		} else {
			flag &= ~IP_MF;
			frag_len = left;
		}

		len = ETH_HLEN + NET_IP_ALIGN + ip_hlen + frag_len;
		frag = alloc_skb(len, GFP_KERNEL);
		if (unlikely(!frag))
			return sent;

		skb_reserve(frag, len);

		skb_push(frag, frag_len);
		skb_copy_bits(skb, ETH_HLEN + ip_hlen + frag_off, frag->data,
			      frag_len);

		skb_push(frag, ip_hlen);
		skb_copy_bits(skb, ETH_HLEN, frag->data, ip_hlen);
		skb_reset_network_header(frag);

		ip_hdr(frag)->tot_len = htons(ip_hlen + frag_len);
		ip_hdr(frag)->frag_off = htons((frag_off >> 3) & IP_OFFSET);
		ip_hdr(frag)->frag_off |= htons(flag);
		ip_send_check(ip_hdr(frag));

		skb_push(frag, ETH_HLEN);
		skb_copy_bits(skb, 0, frag->data, ETH_HLEN);
		skb_reset_mac_header(frag);

		memcpy(frag->cb, skb->cb, sizeof(skb->cb));

		if (vlan_tx_tag_present(skb)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
			__vlan_hwaccel_put_tag(frag, skb->vlan_proto,
                                               vlan_tx_tag_get(skb));
#else
			__vlan_hwaccel_put_tag(frag, htons(ETH_P_8021Q),
                                               vlan_tx_tag_get(skb));
#endif
		}

		sent = __ovs_vport_send(vport, frag);

		left -= frag_len;
		frag_off += frag_len;
	}

	kfree_skb(skb);
	return sent;
}

/**
 *	ovs_vport_send - send a packet on a device
 *
 * @vport: vport on which to send the packet
 * @skb: skb to send
 *
 * Sends the given packet and returns the length of data sent.  Either ovs
 * lock or rcu_read_lock must be held.
 */
int ovs_vport_send(struct vport *vport, struct sk_buff *skb)
{
	unsigned int frag_max_size = 0, mtu = 0;
	u16 encap;

	BUG_ON(!OVS_CB(skb));

	if (vport->ops->type == OVS_VPORT_TYPE_NETDEV ||
	    vport->ops->type == OVS_VPORT_TYPE_INTERNAL)
		mtu = netdev_vport_priv(vport)->dev->mtu;

	frag_max_size = OVS_CB(skb)->pkt_key->phy.frag_max_size;

	if (frag_max_size > 0)
		goto fragment;

	if (!mtu)
		goto send;

	if (eth_hdr(skb)->h_proto == htons(ETH_P_IP)) {
		if (ip_hdr(skb)->frag_off & htons(IP_DF))
			goto send;
		else if (ntohs(ip_hdr(skb)->tot_len) > mtu)
			goto fragment;
	} else if (eth_hdr(skb)->h_proto == htons(ETH_P_8021Q)) {
		encap = vlan_eth_hdr(skb)->h_vlan_encapsulated_proto;
		if (encap == ntohs(ETH_P_IP)) {
			if (ip_hdr(skb)->frag_off & htons(IP_DF)) {
				goto send;
			} else if (ntohs(ip_hdr(skb)->tot_len) > mtu) {
				if (!ovs_vlan_untag(skb))
					return 0;
				goto fragment;
			}
		}
	}

send:
	return __ovs_vport_send(vport, skb);

fragment:
	return ovs_vport_fragment(vport, skb, frag_max_size, mtu);
}


/**
 *	ovs_vport_record_error - indicate device error to generic stats layer
 *
 * @vport: vport that encountered the error
 * @err_type: one of enum vport_err_type types to indicate the error type
 *
 * If using the vport generic stats layer indicate that an error of the given
 * type has occurred.
 */
void ovs_vport_record_error(struct vport *vport, enum vport_err_type err_type)
{
	spin_lock(&vport->stats_lock);

	switch (err_type) {
	case VPORT_E_RX_DROPPED:
		vport->err_stats.rx_dropped++;
		break;

	case VPORT_E_RX_ERROR:
		vport->err_stats.rx_errors++;
		break;

	case VPORT_E_TX_DROPPED:
		vport->err_stats.tx_dropped++;
		break;

	case VPORT_E_TX_ERROR:
		vport->err_stats.tx_errors++;
		break;
	}

	spin_unlock(&vport->stats_lock);
}

static void free_vport_rcu(struct rcu_head *rcu)
{
	struct vport *vport = container_of(rcu, struct vport, rcu);

	ovs_vport_free(vport);
}

void ovs_vport_deferred_free(struct vport *vport)
{
	if (!vport)
		return;

	call_rcu(&vport->rcu, free_vport_rcu);
}
