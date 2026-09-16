#include "mdns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// The group and port every mDNS responder on the network listens on.
#define MDNS_GROUP "224.0.0.251"
#define MDNS_PORT 5353

// RFC 6762 recommends 120 seconds for the records that name a host.
#define MDNS_TTL 120

// A query and the single-A-record answer both fit in a fraction of this; 1500
// is one Ethernet frame, so nothing that arrives here can be truncated.
#define MDNS_BUF 1500

#define DNS_TYPE_A 1
#define DNS_TYPE_ANY 255
#define DNS_CLASS_IN 1
// Set on a record whose owner is the only one allowed to publish it: tells
// everyone else's cache to drop what it had for that name.
#define DNS_CACHE_FLUSH 0x8000
// Set by a querier that wants the answer sent straight back to it rather than
// to the whole group.
#define DNS_UNICAST_RESPONSE 0x8000

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static volatile bool running;
static volatile bool stop_requested;

static char host_label[64];	 // "sonix-transfer"
static char host_full[80];	 // "sonix-transfer.local"
static uint16_t host_port;	 // only used in the log line

// ---------------------------------------------------------------------------
// the local address
// ---------------------------------------------------------------------------

// The IPv4 of the wireless interface, or 0. Asked of the kernel rather than of
// wifi.c: this has to be right at the moment a packet is answered, and a cached
// value from another module's worker is one DHCP renewal away from being a lie.
static uint32_t local_address(void) {
	static const char *const interfaces[] = {"wlan0", "wlan1", "eth0"};

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return 0;
	}

	uint32_t address = 0;
	for (unsigned i = 0; i < sizeof(interfaces) / sizeof(interfaces[0]) && !address; i++) {
		struct ifreq req;
		memset(&req, 0, sizeof(req));
		snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", interfaces[i]);

		if (ioctl(fd, SIOCGIFADDR, &req) == 0) {
			struct sockaddr_in *in = (struct sockaddr_in *)&req.ifr_addr;
			address = in->sin_addr.s_addr;
		}
	}

	close(fd);
	return address;
}

// ---------------------------------------------------------------------------
// DNS names
// ---------------------------------------------------------------------------

// Walks a name in `buf` starting at `at`, writing the dotted form into `out`.
// Returns the offset just past the name, or -1.
//
// Compression pointers have to be followed even here: a question is not
// supposed to use one, but "not supposed to" is not a guarantee about what
// arrives on a socket. `budget` stops a pointer loop from spinning forever.
static int read_name(const uint8_t *buf, int len, int at, char *out, int out_size) {
	int written = 0;
	int budget = 32;
	int after = -1;

	out[0] = '\0';

	while (at >= 0 && at < len) {
		uint8_t label = buf[at];

		if (label == 0) {
			return after >= 0 ? after : at + 1;
		}

		if ((label & 0xC0) == 0xC0) { // compression pointer
			if (at + 1 >= len || --budget <= 0) {
				return -1;
			}
			if (after < 0) {
				after = at + 2; // the name ends here, wherever the pointer goes
			}
			at = ((label & 0x3F) << 8) | buf[at + 1];
			continue;
		}

		if ((label & 0xC0) != 0) {
			return -1; // reserved label type
		}
		if (at + 1 + label > len) {
			return -1;
		}

		if (written && written + 1 < out_size) {
			out[written++] = '.';
		}
		for (int i = 0; i < label && written + 1 < out_size; i++) {
			out[written++] = (char)buf[at + 1 + i];
		}
		out[written] = '\0';
		at += 1 + label;
	}

	return -1;
}

// Writes "sonix-transfer.local" as the wire format: one length-prefixed label
// per dotted part, then a zero. No compression -- the saving would be a dozen
// bytes on a packet nothing is counting.
static int write_name(uint8_t *buf, int size, int at, const char *name) {
	const char *part = name;

	while (*part) {
		const char *dot = strchr(part, '.');
		int length = dot ? (int)(dot - part) : (int)strlen(part);

		if (length <= 0 || length > 63 || at + 1 + length >= size) {
			return -1;
		}

		buf[at++] = (uint8_t)length;
		memcpy(buf + at, part, (size_t)length);
		at += length;

		part = dot ? dot + 1 : part + length;
	}

	if (at >= size) {
		return -1;
	}
	buf[at++] = 0;
	return at;
}

static int put16(uint8_t *buf, int size, int at, uint16_t value) {
	if (at + 2 > size) {
		return -1;
	}
	buf[at++] = (uint8_t)(value >> 8);
	buf[at++] = (uint8_t)(value & 0xFF);
	return at;
}

static int put32(uint8_t *buf, int size, int at, uint32_t value) {
	if (at + 4 > size) {
		return -1;
	}
	buf[at++] = (uint8_t)(value >> 24);
	buf[at++] = (uint8_t)((value >> 16) & 0xFF);
	buf[at++] = (uint8_t)((value >> 8) & 0xFF);
	buf[at++] = (uint8_t)(value & 0xFF);
	return at;
}

static uint16_t get16(const uint8_t *buf, int at) { return (uint16_t)((buf[at] << 8) | buf[at + 1]); }

// One answer: this host's name, an A record, this host's address.
static int build_answer(uint8_t *out, int size, uint16_t id, const char *name, uint32_t address) {
	int at = 0;

	at = put16(out, size, at, id);
	at = put16(out, size, at, 0x8400); // response, authoritative
	at = put16(out, size, at, 0);	   // no questions echoed back
	at = put16(out, size, at, 1);	   // one answer
	at = put16(out, size, at, 0);	   // no authority records
	at = put16(out, size, at, 0);	   // no additional records
	if (at < 0) {
		return -1;
	}

	at = write_name(out, size, at, name);
	if (at < 0) {
		return -1;
	}

	at = put16(out, size, at, DNS_TYPE_A);
	at = put16(out, size, at, DNS_CLASS_IN | DNS_CACHE_FLUSH);
	at = put32(out, size, at, MDNS_TTL);
	at = put16(out, size, at, 4);
	if (at < 0 || at + 4 > size) {
		return -1;
	}

	memcpy(out + at, &address, 4); // already network order, straight from the ioctl
	return at + 4;
}

// ---------------------------------------------------------------------------
// the socket
// ---------------------------------------------------------------------------

static int open_socket(uint32_t address) {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("mdns: socket");
		return -1;
	}

	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
	// Every responder on the machine shares 5353. Without this, starting this
	// one while shairport's is up would simply fail to bind.
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(MDNS_PORT);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		printf("mdns: cannot bind :%d: %s\n", MDNS_PORT, strerror(errno));
		close(fd);
		return -1;
	}

	// Join on this interface's address, not on INADDR_ANY: letting the kernel
	// choose the interface is how a responder ends up listening on the wrong
	// one.
	struct ip_mreq group;
	memset(&group, 0, sizeof(group));
	group.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
	group.imr_interface.s_addr = address;

	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) != 0) {
		printf("mdns: cannot join " MDNS_GROUP ": %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	// 255 is what RFC 6762 requires, so a responder behind a router that
	// decrements is still heard on its own link.
	unsigned char ttl = 255;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

	struct in_addr out_if;
	out_if.s_addr = address;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &out_if, sizeof(out_if));

	return fd;
}

static void send_to_group(int fd, const uint8_t *packet, int length) {
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = inet_addr(MDNS_GROUP);
	to.sin_port = htons(MDNS_PORT);

	sendto(fd, packet, (size_t)length, 0, (struct sockaddr *)&to, sizeof(to));
}

// ---------------------------------------------------------------------------
// answering
// ---------------------------------------------------------------------------

// Everything this responder knows how to say. Returns the reply length, or 0
// when the query was not about this host.
static int handle_query(const uint8_t *in, int len, uint32_t address, const char *name, uint8_t *out, int out_size,
						bool *unicast) {
	if (len < 12) {
		return 0;
	}

	uint16_t flags = get16(in, 2);
	if (flags & 0x8000) {
		return 0; // a response, not a question
	}

	uint16_t id = get16(in, 0);
	int questions = get16(in, 4);
	int at = 12;

	for (int i = 0; i < questions; i++) {
		char asked[256];
		at = read_name(in, len, at, asked, sizeof(asked));
		if (at < 0 || at + 4 > len) {
			return 0;
		}

		uint16_t qtype = get16(in, at);
		uint16_t qclass = get16(in, at + 2);
		at += 4;

		if ((qclass & 0x7FFF) != DNS_CLASS_IN) {
			continue;
		}
		if (qtype != DNS_TYPE_A && qtype != DNS_TYPE_ANY) {
			continue;
		}
		if (strcasecmp(asked, name) != 0) {
			continue;
		}

		*unicast = (qclass & DNS_UNICAST_RESPONSE) != 0;
		return build_answer(out, out_size, id, name, address);
	}

	return 0;
}

static void *mdns_thread(void *unused) {
	(void)unused;

	int fd = -1;
	uint32_t bound_address = 0;

	while (!stop_requested) {
		uint32_t address = local_address();

		// No address yet, or it moved: start again on the new one. An mDNS
		// responder answering with an address it no longer has is worse than
		// one that says nothing.
		if (address == 0) {
			if (fd >= 0) {
				close(fd);
				fd = -1;
				bound_address = 0;
			}
			usleep(500 * 1000);
			continue;
		}

		if (fd < 0 || address != bound_address) {
			if (fd >= 0) {
				close(fd);
			}
			fd = open_socket(address);
			if (fd < 0) {
				usleep(1000 * 1000);
				continue;
			}
			bound_address = address;

			char shown[INET_ADDRSTRLEN] = "?";
			inet_ntop(AF_INET, &address, shown, sizeof(shown));

			pthread_mutex_lock(&lock);
			char name[sizeof(host_full)];
			snprintf(name, sizeof(name), "%s", host_full);
			uint16_t port = host_port;
			pthread_mutex_unlock(&lock);

			printf("mdns: answering for %s at %s (port %u)\n", name, shown, (unsigned)port);

			// Say it before being asked, twice: a browser that already had the
			// page open when the address changed picks the new one up without
			// having to time out first.
			uint8_t packet[MDNS_BUF];
			int length = build_answer(packet, sizeof(packet), 0, name, address);
			if (length > 0) {
				send_to_group(fd, packet, length);
				usleep(150 * 1000);
				send_to_group(fd, packet, length);
			}
		}

		struct pollfd waiting = {.fd = fd, .events = POLLIN, .revents = 0};
		int ready = poll(&waiting, 1, 1000);
		if (ready <= 0) {
			continue; // timeout, or a signal: go round and re-check the address
		}

		uint8_t in[MDNS_BUF];
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		ssize_t got = recvfrom(fd, in, sizeof(in), 0, (struct sockaddr *)&from, &from_len);
		if (got <= 0) {
			continue;
		}

		pthread_mutex_lock(&lock);
		char name[sizeof(host_full)];
		snprintf(name, sizeof(name), "%s", host_full);
		pthread_mutex_unlock(&lock);

		if (!name[0]) {
			continue;
		}

		uint8_t out[MDNS_BUF];
		bool unicast = false;
		int length = handle_query(in, (int)got, address, name, out, sizeof(out), &unicast);
		if (length <= 0) {
			continue;
		}

		if (unicast) {
			sendto(fd, out, (size_t)length, 0, (struct sockaddr *)&from, from_len);
		} else {
			send_to_group(fd, out, length);
		}
	}

	if (fd >= 0) {
		close(fd);
	}

	running = false;
	return NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

bool mdns_start(const char *name, uint16_t port) {
	if (!name || !name[0]) {
		return false;
	}

	pthread_mutex_lock(&lock);
	snprintf(host_label, sizeof(host_label), "%s", name);
	snprintf(host_full, sizeof(host_full), "%s.local", name);
	host_port = port;
	pthread_mutex_unlock(&lock);

	if (running) {
		return true; // already up; it just answers for the new name now
	}

	stop_requested = false;
	running = true;

	if (pthread_create(&thread, NULL, mdns_thread, NULL) != 0) {
		perror("mdns: pthread_create");
		running = false;
		return false;
	}
	pthread_detach(thread);

	return true;
}

void mdns_stop(void) {
	if (!running) {
		return;
	}

	stop_requested = true;

	// The thread wakes at least once a second, so this is a short wait. Bounded
	// anyway: nothing here is worth hanging the caller over.
	for (int i = 0; i < 30 && running; i++) {
		usleep(100 * 1000);
	}

	pthread_mutex_lock(&lock);
	host_full[0] = '\0';
	host_label[0] = '\0';
	pthread_mutex_unlock(&lock);
}

bool mdns_running(void) { return running; }

void mdns_hostname(char *out, int size) {
	if (!out || size <= 0) {
		return;
	}
	pthread_mutex_lock(&lock);
	snprintf(out, (size_t)size, "%s", host_full);
	pthread_mutex_unlock(&lock);
}
