#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/ethernet.h>
#include <netpacket/packet.h>
#include <arpa/inet.h>

#include "triton.h"
#include "log.h"
#include "mempool.h"

#include "ap_net.h"
#include "pppoe.h"

#include "memdebug.h"

struct tree {
	pthread_mutex_t lock;
	struct rb_root root;
};

#define HASH_BITS 0xff

struct disc_net {
	struct triton_context_t ctx;
	struct list_head entry;
	struct triton_md_handler_t hnd;
	const struct ap_net *net;
	struct tree tree[0];
};

static uint8_t bc_addr[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static mempool_t pkt_pool;

static LIST_HEAD(nets);

static void disc_close(struct triton_context_t *ctx);
static int disc_read(struct triton_md_handler_t *h);

static struct disc_net *init_net(const struct ap_net *net)
{
	struct sockaddr_ll addr;
	int i, f = 1;
	struct disc_net *n;
	struct tree *tree;
	int sock;

	sock = net->socket(PF_PACKET, SOCK_RAW, htons(ETH_P_PPP_DISC));
	if (sock < 0)
		return NULL;

	memset(&addr, 0, sizeof(addr));
	addr.sll_family = AF_PACKET;
	addr.sll_protocol = htons(ETH_P_PPP_DISC);

	net->setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &f, sizeof(f));

	if (net->bind(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		log_error("pppoe: disc: bind: %s\n", strerror(errno));
		close(sock);
		return NULL;
	}

	fcntl(sock, F_SETFD, FD_CLOEXEC);
	net->set_nonblocking(sock, 1);

	n = malloc(sizeof(*net) + (HASH_BITS + 1) * sizeof(struct tree));
	tree = n->tree;

	for (i = 0; i <= HASH_BITS; i++) {
		pthread_mutex_init(&tree[i].lock, NULL);
		tree[i].root = RB_ROOT;
	}

	n->ctx.close = disc_close;
	n->hnd.fd = sock;
	n->hnd.read = disc_read;
	n->net = net;

	triton_context_register(&n->ctx, NULL);
	triton_md_register_handler(&n->ctx, &n->hnd);
	triton_md_enable_handler(&n->hnd, MD_MODE_READ);
	triton_context_wakeup(&n->ctx);

	list_add_tail(&n->entry, &nets);

	return n;
}

static struct disc_net *find_net(const struct ap_net *net)
{
	struct disc_net *n;

	list_for_each_entry(n, &nets, entry) {
		if (n->net == net)
			return n;
	}

	return NULL;
}

int pppoe_disc_start(struct pppoe_serv_t *serv)
{
	struct disc_net *net = find_net(serv->net);
	struct rb_node **p, *parent = NULL;
	struct tree *t;
	int ifindex = serv->ifindex, i;
	struct pppoe_serv_t *n;

	if (!net) {
		net = init_net(serv->net);
		if (!net)
			return -1;
	}

	t = &net->tree[ifindex & HASH_BITS];

	pthread_mutex_lock(&t->lock);

	p = &t->root.rb_node;

	while (*p) {
		parent = *p;
		n = rb_entry(parent, typeof(*n), node);
		i = n->ifindex;

		if (ifindex < i)
			p = &(*p)->rb_left;
		else if (ifindex > i)
			p = &(*p)->rb_right;
		else {
			pthread_mutex_unlock(&t->lock);
			log_error("pppoe: disc: attempt to add duplicate ifindex\n");
			return -1;
		}
	}

	rb_link_node(&serv->node, parent, p);
	rb_insert_color(&serv->node, &t->root);

	pthread_mutex_unlock(&t->lock);

	return net->hnd.fd;
}

void pppoe_disc_stop(struct pppoe_serv_t *serv)
{
	struct disc_net *n = find_net(serv->net);
	struct tree *t = &n->tree[serv->ifindex & HASH_BITS];

	pthread_mutex_lock(&t->lock);
	rb_erase(&serv->node, &t->root);
	pthread_mutex_unlock(&t->lock);
}

static int forward(struct disc_net *net, int ifindex, void *pkt, int len)
{
	struct pppoe_serv_t *n;
	struct tree *t = &net->tree[ifindex & HASH_BITS];
	struct rb_node **p = &t->root.rb_node, *parent = NULL;
	int r = 0;
	struct ethhdr *ethhdr = (struct ethhdr *)(pkt + 4);

	pthread_mutex_lock(&t->lock);

	while (*p) {
		parent = *p;
		n = rb_entry(parent, typeof(*n), node);

		if (ifindex < n->ifindex)
			p = &(*p)->rb_left;
		else if (ifindex > n->ifindex)
			p = &(*p)->rb_right;
		else {
			if (!memcmp(ethhdr->h_dest, bc_addr, ETH_ALEN) || !memcmp(ethhdr->h_dest, n->hwaddr, ETH_ALEN)) {
				*(int *)pkt = len;
				triton_context_call(&n->ctx, (triton_event_func)pppoe_serv_read, pkt);
				r = 1;
			}
			break;
		}
	}

	pthread_mutex_unlock(&t->lock);

	return r;
}

static void notify_down(struct disc_net *net, int ifindex)
{
	struct pppoe_serv_t *n;
	struct tree *t = &net->tree[ifindex & HASH_BITS];
	struct rb_node **p = &t->root.rb_node, *parent = NULL;

	pthread_mutex_lock(&t->lock);

	while (*p) {
		parent = *p;
		n = rb_entry(parent, typeof(*n), node);

		if (ifindex < n->ifindex)
			p = &(*p)->rb_left;
		else if (ifindex > n->ifindex)
			p = &(*p)->rb_right;
		else {
			triton_context_call(&n->ctx, (triton_event_func)_server_stop, n);
			break;
		}
	}

	pthread_mutex_unlock(&t->lock);
}

static int disc_read(struct triton_md_handler_t *h)
{
	struct disc_net *net = container_of(h, typeof(*net), hnd);
	uint8_t *pack = NULL;
	struct ethhdr *ethhdr;
	struct pppoe_hdr *hdr;
	int n;
	struct sockaddr_ll src;
	socklen_t slen = sizeof(src);

	while (1) {
		if (!pack)
			pack = mempool_alloc(pkt_pool);

		n = net->net->recvfrom(h->fd, pack + 4, ETHER_MAX_LEN, MSG_DONTWAIT, (struct sockaddr *)&src, &slen);

		if (n < 0) {
			if (errno == EAGAIN)
				break;

			if (errno == ENETDOWN) {
				notify_down(net, src.sll_ifindex);
				continue;
			}

			log_error("pppoe: disc: read: %s\n", strerror(errno));
			continue;
		}

		ethhdr = (struct ethhdr *)(pack + 4);
		hdr = (struct pppoe_hdr *)(pack + 4 + ETH_HLEN);

		if (n < ETH_HLEN + sizeof(*hdr)) {
			if (conf_verbose)
				log_warn("pppoe: short packet received (%i)\n", n);
			continue;
		}

		if (mac_filter_check(ethhdr->h_source)) {
			__sync_add_and_fetch(&stat_filtered, 1);
			continue;
		}

		//if (memcmp(ethhdr->h_dest, bc_addr, ETH_ALEN) && memcmp(ethhdr->h_dest, serv->hwaddr, ETH_ALEN))
		//	continue;

		if (!memcmp(ethhdr->h_source, bc_addr, ETH_ALEN)) {
			if (conf_verbose)
				log_warn("pppoe: discarding packet (host address is broadcast)\n");
			continue;
		}

		if ((ethhdr->h_source[0] & 1) != 0) {
			if (conf_verbose)
				log_warn("pppoe: discarding packet (host address is not unicast)\n");
			continue;
		}

		if (n < ETH_HLEN + sizeof(*hdr) + ntohs(hdr->length)) {
			if (conf_verbose)
				log_warn("pppoe: short packet received\n");
			continue;
		}

		if (hdr->ver != 1) {
			if (conf_verbose)
				log_warn("pppoe: discarding packet (unsupported version %i)\n", hdr->ver);
			continue;
		}

		if (hdr->type != 1) {
			if (conf_verbose)
				log_warn("pppoe: discarding packet (unsupported type %i)\n", hdr->type);
		}

		if (forward(net, src.sll_ifindex, pack, n))
			pack = NULL;
	}

	mempool_free(pack);

	return 0;
}

static void disc_close(struct triton_context_t *ctx)
{
	struct disc_net *n = container_of(ctx, typeof(*n), ctx);

	triton_md_unregister_handler(&n->hnd, 1);
	triton_context_unregister(ctx);
}

static void init()
{
	pkt_pool = mempool_create(ETHER_MAX_LEN + 4);
}

DEFINE_INIT(1, init);


