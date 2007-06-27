#include <linux/kernel.h>
#include <linux/module.h>

#include "mpoe_common.h"
#include "mpoe_hal.h"

/* returns an interface hold matching ifname */
static struct net_device *
mpoe_ifp_find_by_name(const char * ifname)
{
	struct net_device * ifp;

	read_lock(&dev_base_lock);
	mpoe_for_each_netdev(ifp) {
		dev_hold(ifp);
		if (!strcmp(ifp->name, ifname)) {
			read_unlock(&dev_base_lock);
			return ifp;
		}
		dev_put(ifp);
	}
	read_unlock(&dev_base_lock);

	printk(KERN_ERR "MPoE: Failed to find interface '%s'\n", ifname);
	return NULL;
}

/*************
 * Attaching/Detaching interfaces
 */

static struct mpoe_iface ** mpoe_ifaces;
static unsigned int mpoe_iface_nr = 0;
static spinlock_t mpoe_iface_lock = SPIN_LOCK_UNLOCKED;

/* called with ifaces lock hold */
static int
mpoe_iface_attach(struct net_device * ifp)
{
	struct mpoe_iface * iface;
	int ret;
	int i;

	if (mpoe_iface_nr == mpoe_iface_max) {
		printk(KERN_ERR "MPoE: Too many interfaces already attached\n");
		ret = -EBUSY;
		goto out_with_ifp_hold;
	}

	/* FIXME: do not attach twice? */

	for(i=0; i<mpoe_iface_max; i++)
		if (mpoe_ifaces[i] == NULL)
			break;

	iface = kzalloc(sizeof(struct mpoe_iface), GFP_KERNEL);
	if (!iface) {
		printk(KERN_ERR "MPoE: Failed to allocate interface as board %d\n", i);
		ret = -ENOMEM;
		goto out_with_ifp_hold;
	}

	printk(KERN_INFO "MPoE: Attaching interface '%s' as #%i\n", ifp->name, i);

	iface->eth_ifp = ifp;
	iface->endpoint_nr = 0;
	iface->endpoints = kzalloc(mpoe_endpoint_max * sizeof(struct mpoe_endpoint *), GFP_KERNEL);
	if (!iface->endpoints) {
		printk(KERN_ERR "MPoE: Failed to allocate interface endpoint pointers\n");
		ret = -ENOMEM;
		goto out_with_iface;
	}

	spin_lock_init(&iface->endpoint_lock);
	iface->index = i;
	mpoe_iface_nr++;
	mpoe_ifaces[i] = iface;

	return 0;

 out_with_iface:
	kfree(iface);
 out_with_ifp_hold:
	dev_put(ifp);
	return ret;
}

/* called with ifaces lock hold
 * Incoming packets should be disabled (by removing mpoe_pt)
 * to prevent users while detaching the iface
 */
static int
__mpoe_iface_detach(struct mpoe_iface * iface, int force)
{
	int ret;
	int i;

	BUG_ON(mpoe_ifaces[iface->index] == NULL);

	/* mark as closing so that nobody opens a new endpoint,
	 * protected by the ifaces lock
	 */
	iface->status = MPOE_IFACE_STATUS_CLOSING;

	/* if force, close all endpoints
	 * if not force, error if some endpoints are open
	 */
	spin_lock(&iface->endpoint_lock);
	ret = -EBUSY;
	if (!force && iface->endpoint_nr) {
		printk(KERN_INFO "MPoE: cannot detach interface #%d '%s', still %d endpoints open\n",
		       iface->index, iface->eth_ifp->name, iface->endpoint_nr);
		spin_unlock(&iface->endpoint_lock);
		goto out;
	}

	for(i=0; i<mpoe_endpoint_max; i++) {
		struct mpoe_endpoint * endpoint = iface->endpoints[i];
		if (!endpoint)
			continue;

		/* close the endpoint, with the iface lock hold */
		ret = __mpoe_endpoint_close(endpoint, 1);
		if (ret < 0) {
			BUG_ON(ret != -EBUSY);
			/* somebody else is already closing this endpoint
			 * let's forget about it for now, we'll wait later
			 */
		}
	}
	spin_unlock(&iface->endpoint_lock);

	/* release the lock and wait for concurrent endpoint closers to be done */
	while (iface->endpoint_nr)
		schedule_timeout(1); /* FIXME: use msleep (needs a hal)? or waitqueue? */

	printk(KERN_INFO "MPoE: detaching interface #%d '%s'\n", iface->index, iface->eth_ifp->name);

	mpoe_ifaces[iface->index] = NULL;
	mpoe_iface_nr--;
	kfree(iface->endpoints);
	dev_put(iface->eth_ifp);
	kfree(iface);

	return 0;

 out:
	return ret;
}

static inline int
mpoe_iface_detach(struct mpoe_iface * iface)
{
	return __mpoe_iface_detach(iface, 0);
}

static inline int
mpoe_iface_detach_force(struct mpoe_iface * iface)
{
	return __mpoe_iface_detach(iface, 1);
}

/*************
 * Managing interfaces
 */

/* list attached interfaces */
int
mpoe_ifaces_show(char *buf)
{
	int total = 0;
	int i;

	/* need to lock since we access the internals of the ifaces */
	spin_lock(&mpoe_iface_lock);

	for (i=0; i<mpoe_iface_max; i++) {
		struct mpoe_iface * iface = mpoe_ifaces[i];
		if (iface) {
			char * ifname = iface->eth_ifp->name;
			int length = strlen(ifname);
			/* FIXME: check total+length+2 <= PAGE_SIZE ? */
			strcpy(buf, ifname);
			buf += length;
			strcpy(buf, "\n");
			buf += 1;
			total += length+1;
		}
	}

	spin_unlock(&mpoe_iface_lock);

	return total + 1;
}

/* +name add an interface, -name removes one */
int
mpoe_ifaces_store(const char *buf, size_t size)
{
	char copy[IFNAMSIZ];
	char * ptr;

	/* remove the ending \n if required, so copy first since buf is const */
	strncpy(copy, buf+1, IFNAMSIZ);
	copy[IFNAMSIZ-1] = '\0';
	ptr = strchr(copy, '\n');
	if (ptr)
		*ptr = '\0';

	if (buf[0] == '-') {
		int i, found = 0;
		/* in case none matches, we return -EINVAL. if one matches, it sets ret accordingly */
		int ret = -EINVAL;

		spin_lock(&mpoe_iface_lock);
		for(i=0; i<mpoe_iface_max; i++) {
			struct mpoe_iface * iface = mpoe_ifaces[i];
			if (iface != NULL && !strcmp(iface->eth_ifp->name, copy)) {
				/* Disable incoming packets while removing the iface
				 * to prevent races */
				dev_remove_pack(&mpoe_pt);
				ret = mpoe_iface_detach(iface);
				dev_add_pack(&mpoe_pt);
				if (!ret)
					found = 1;
				break;
			}
		}
		spin_unlock(&mpoe_iface_lock);

		if (!found) {
			printk(KERN_ERR "MPoE: Cannot find any attached interface '%s' to detach\n", copy);
			return -EINVAL;
		}
		return size;

	} else if (buf[0] == '+') {
		struct net_device * ifp;
		int ret;

		ifp = mpoe_ifp_find_by_name(copy);
		if (!ifp)
			return -EINVAL;

		spin_lock(&mpoe_iface_lock);
		ret = mpoe_iface_attach(ifp);
		spin_unlock(&mpoe_iface_lock);
		if (ret < 0)
			return ret;

		return size;

	} else {
		printk(KERN_ERR "MPoE: Unrecognized command passed in the ifaces file, need either +name or -name\n");
		return -EINVAL;
	}
}

/*
 * Used when an incoming packets is to be processed on net_device ifp
 * and returns the corresponding iface.
 *
 * Since iface removal disables incoming packet processing, we don't
 * need to lock the iface array or to hold a reference on the iface.
 */
struct mpoe_iface *
mpoe_iface_find_by_ifp(struct net_device *ifp)
{
	int i;

	for (i=0; i<mpoe_iface_max; i++) {
		struct mpoe_iface * iface = mpoe_ifaces[i];
		if (iface && iface->eth_ifp == ifp)
			return iface;
	}

	return NULL;
}

/*
 * Return the number of mpoe ifaces.
 * No need to lock since the array of iface is always coherent
 * and we don't access the internals of the ifaces
 */
int
mpoe_ifaces_get_count(void)
{
	int i, count = 0;

	for (i=0; i<mpoe_iface_max; i++)
		if (mpoe_ifaces[i] != NULL)
			count++;

	return count;
}

int
mpoe_iface_get_id(uint8_t board_index, struct mpoe_mac_addr * board_addr, char * board_name)
{
	struct net_device * ifp;
	int ret;

	/* need to lock since we access the internals of the iface */
	spin_lock(&mpoe_iface_lock);

	ret = -EINVAL;
	if (board_index >= mpoe_iface_max
	    || mpoe_ifaces[board_index] == NULL)
		goto out_with_lock;

	ifp = mpoe_ifaces[board_index]->eth_ifp;

	mpoe_mac_addr_of_netdevice(ifp, board_addr);
	strncpy(board_name, ifp->name, MPOE_IF_NAMESIZE);

	spin_unlock(&mpoe_iface_lock);

	return 0;

 out_with_lock:
	spin_unlock(&mpoe_iface_lock);
	return ret;
}

/**********
 * Attaching endpoints to ifaces
 */

int
mpoe_iface_attach_endpoint(struct mpoe_endpoint * endpoint)
{
	struct mpoe_iface * iface;
	int ret;

	ret = -EINVAL;
	if (endpoint->endpoint_index >= mpoe_endpoint_max)
		goto out;

	/* lock the list of ifaces */
	spin_lock(&mpoe_iface_lock);

	/* find the iface */
	ret = -EINVAL;
	if (endpoint->board_index >= mpoe_iface_max
	    || (iface = mpoe_ifaces[endpoint->board_index]) == NULL
	    || iface->status != MPOE_IFACE_STATUS_OK) {
		printk(KERN_ERR "MPoE: Cannot open endpoint on unexisting board %d\n",
		       endpoint->board_index);
		goto out_with_ifaces_locked;
	}
	iface = mpoe_ifaces[endpoint->board_index];

	/* lock the list of endpoints in the iface */
	spin_lock(&iface->endpoint_lock);

	/* add the endpoint */
	if (iface->endpoints[endpoint->endpoint_index] != NULL) {
		printk(KERN_ERR "MPoE: endpoint already open\n");
		goto out_with_endpoints_locked;
	}

	iface->endpoints[endpoint->endpoint_index] = endpoint ;
	iface->endpoint_nr++;
	endpoint->iface = iface;

	/* mark the endpoint as open here so that anybody removing this
	 * iface never sees any endpoint in status INIT in the iface list
	 * (only OK and CLOSING are allowed there)
	 */
	endpoint->status = MPOE_ENDPOINT_STATUS_OK;

	spin_unlock(&iface->endpoint_lock);
	spin_unlock(&mpoe_iface_lock);

	return 0;

 out_with_endpoints_locked:
	spin_unlock(&iface->endpoint_lock);
 out_with_ifaces_locked:
	spin_unlock(&mpoe_iface_lock);
 out:
	return ret;
}

/* called while endpoint is status CLOSING.
 * either without holding the iface lock (when closing from the application)
 * or holding it (when detaching an iface and thus removing all endpoints)
 */
void
mpoe_iface_detach_endpoint(struct mpoe_endpoint * endpoint,
			   int ifacelocked)
{
	struct mpoe_iface * iface = endpoint->iface;

	BUG_ON(endpoint->status != MPOE_ENDPOINT_STATUS_CLOSING);

	/* lock the list of endpoints in the iface, if needed */
	if (!ifacelocked)
		spin_lock(&iface->endpoint_lock);

	BUG_ON(iface->endpoints[endpoint->endpoint_index] != endpoint);
	iface->endpoints[endpoint->endpoint_index] = NULL;
	iface->endpoint_nr--;

	if (!ifacelocked)
		spin_unlock(&iface->endpoint_lock);
}

/*************
 * Netdevice notifier
 */

static int
mpoe_netdevice_notifier_cb(struct notifier_block *unused,
			   unsigned long event, void *ptr)
{
	struct net_device *ifp = (struct net_device *) ptr;

	if (event == NETDEV_UNREGISTER) {
		struct mpoe_iface * iface;

		spin_lock(&mpoe_iface_lock);
		iface = mpoe_iface_find_by_ifp(ifp);
		if (iface) {
			int ret;
			printk(KERN_INFO "MPoE: interface '%s' being unregistered, forcing closing of endpoints...\n",
			       ifp->name);
			/* There is no need to disable incoming packets since
			 * the ethernet ifp is already disabled before the notifier is called
			 */
			ret = mpoe_iface_detach_force(iface);
			BUG_ON(ret);
		}
		spin_unlock(&mpoe_iface_lock);
	}

	return NOTIFY_DONE;
}

/*************
 * Initialization and termination
 */

static struct notifier_block mpoe_netdevice_notifier = {
	.notifier_call = mpoe_netdevice_notifier_cb,
};

int
mpoe_net_init(const char * ifnames)
{
	int ret = 0;

	ret = mpoe_init_pull();
	if (ret < 0)
		goto abort;

	dev_add_pack(&mpoe_pt);

	ret = register_netdevice_notifier(&mpoe_netdevice_notifier);
	if (ret < 0) {
		printk(KERN_ERR "MPoE: failed to register netdevice notifier\n");
		goto abort_with_pack;
	}

	mpoe_ifaces = kzalloc(mpoe_iface_max * sizeof(struct mpoe_iface *), GFP_KERNEL);
	if (!mpoe_ifaces) {
		printk(KERN_ERR "MPoE: failed to allocate interface array\n");
		ret = -ENOMEM;
		goto abort_with_notifier;
	}

	if (ifnames) {
		/* attach ifaces whose name are in ifnames (limited to mpoe_iface_max) */
		char * copy = kstrdup(ifnames, GFP_KERNEL);
		char * ifname;

		while ((ifname = strsep(&copy, ",")) != NULL) {
			struct net_device * ifp;
			ifp = mpoe_ifp_find_by_name(ifname);
			if (ifp)
				if (mpoe_iface_attach(ifp) < 0)
					break;
		}

		kfree(copy);

	} else {
		/* attach everything (limited to mpoe_iface_max) */
		struct net_device * ifp;

		read_lock(&dev_base_lock);
		mpoe_for_each_netdev(ifp) {
			dev_hold(ifp);
			if (mpoe_iface_attach(ifp) < 0)
				break;
		}
		read_unlock(&dev_base_lock);
	}

	printk(KERN_INFO "MPoE: attached %d interfaces\n", mpoe_iface_nr);
	return 0;

 abort_with_notifier:
	unregister_netdevice_notifier(&mpoe_netdevice_notifier);
 abort_with_pack:
	dev_remove_pack(&mpoe_pt);
	mpoe_exit_pull();
 abort:
	return ret;
}

void
mpoe_net_exit(void)
{
	int i, nr = 0;

	/* Module unloading cannot happen before all users exit
	 * since they hold a reference on the chardev.
	 * So _all_ endpoint are closed once we arrive here.
	 */

	dev_remove_pack(&mpoe_pt);
	/* Now, no iface may be used by any incoming packet */

	/* Prevent mpoe_netdevice_notifier from removing an iface now */
	spin_lock(&mpoe_iface_lock);

	for (i=0; i<mpoe_iface_max; i++) {
		struct mpoe_iface * iface = mpoe_ifaces[i];
		if (iface != NULL) {
			/* Detach the iface now
			 * All endpoints are closed, no need to force */
			BUG_ON(mpoe_iface_detach(iface) < 0);
			nr++;
		}
	}
	printk(KERN_INFO "MPoE: detached %d interfaces\n", nr);

	/* Release the lock and let mpoe_netdevice_notifier finish in case
	 * it got called during our loop.
	 * And unregister the notifier then */
	spin_unlock(&mpoe_iface_lock);
	unregister_netdevice_notifier(&mpoe_netdevice_notifier);

	/* Free structures now that the notifier is gone */
	kfree(mpoe_ifaces);

	mpoe_exit_pull();
}

/*
 * Local variables:
 *  tab-width: 8
 *  c-basic-offset: 8
 *  c-indent-level: 8
 * End:
 */
