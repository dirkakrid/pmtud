// PMTUD
//
// Copyright (c) 2015 CloudFlare, Inc.

#include <errno.h>
#include <getopt.h>
#include <pcap.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "hashlimit.h"
#include "pmtud.h"
#include "uevent.h"

#define IFACE_RATE_PPS 10.0
#define SRC_RATE_PPS 1.0

static void usage()
{
	fprintf(stderr,
		"Usage:\n"
		"\n"
		"    pmtud [options]\n"
		"\n"
		"Path MTU Daemon is captures and broadcasts ICMP messages "
		"related to\n"
		"MTU detection. It listens on an interface, waiting for ICMP "
		"messages\n"
		"(IPv4 type 3 code 4 or IPv6 type 2 code 0) and it forwards "
		"them\n"
		"verbatim to the broadcast ethernet address.\n"
		"\n"
		"Options:\n"
		"\n"
		"  --iface              Network interface to listen on\n"
		"  --src-rate           Pps limit from single source "
		"(default=%.1f pss)\n"
		"  --iface-rate         Pps limit to send on a single "
		"interface "
		"(default=%.1f pps)\n"
		"  --verbose            Print forwarded packets on screen\n"
		"  --dry-run            Don't inject packets, just dry run\n"
		"  --cpu                Pin to particular cpu\n"
		"  --ports              Forward only ICMP packets with "
		"payload\n"
		"                       containing L4 source port on this "
		"list\n"
		"                       (comma separated)\n"
		"  --help               Print this message\n"
		"\n"
		"Example:\n"
		"\n"
		"    pmtud --iface=eth2 --src-rate=%.1f --iface-rate=%.1f\n"
		"\n",
		SRC_RATE_PPS, IFACE_RATE_PPS, SRC_RATE_PPS, IFACE_RATE_PPS);
	exit(-1);
}

#define SNAPLEN 2048
#define BPF_FILTER                                                             \
	"((icmp and icmp[0] == 3 and icmp[1] == 4) or "                        \
	" (icmp6 and ip6[40+0] == 2 and ip6[40+1] == 0)) and"                  \
	"(ether dst not ff:ff:ff:ff:ff:ff)"

static int on_signal(struct uevent *uevent, int sfd, int mask, void *userdata)
{
	volatile int *done = userdata;
	int buf[512];
	/* Drain. Socket should be NONBLOCK */
	int r = read(sfd, buf, sizeof(buf));
	if (r < 0) {
		PFATAL("read()");
	}

	*done = 1;
	return 0;
}

struct state
{
	pcap_t *pcap;
	int raw_sd;
	struct hashlimit *sources;
	struct hashlimit *ifaces;
	int verbose;
	int dry_run;
	uint64_t *ports_map;
};

static int handle_packet(struct state *state, const uint8_t *p, int data_len)
{
	const char *reason = "unknown";

	/* assumming DLT_EN10MB */

	/* 14 ethernet, 20 ipv4, 8 icmp, 8 IPv4 on payload */
	if (data_len < 14 + 20 + 8 + 8) {
		return -1;
	}

	if (p[0] == 0xff && p[1] == 0xff && p[2] == 0xff && p[3] == 0xff &&
	    p[4] == 0xff && p[5] == 0xff) {
		return -1;
	}

	const uint8_t *hash = NULL;
	int hash_len = 0;

	int l3_offset = 14;
	uint16_t eth_type = (((uint16_t)p[12]) << 8) | (uint16_t)p[13];
	if (eth_type == 0x8100) {
		eth_type = (((uint16_t)p[16]) << 8) | (uint16_t)p[17];
		l3_offset = 18;
	}

	int icmp_offset = -1;
	int valid = 0;
	if (eth_type == 0x0800 && (p[l3_offset] & 0xF0) == 0x40) {
		int l3_hdr_len = (int)(p[l3_offset] & 0x0F) * 4;
		if (l3_hdr_len < 20) {
			reason = "IPv4 header invalid length";
			goto reject;
		}
		icmp_offset = l3_offset + l3_hdr_len;

		uint8_t protocol = p[l3_offset + 9];
		/* header: 20 bytes of IPv4, 8 bytes of ICMP,
		 * payload: 20 bytes of IPv4, 8 bytes of TCP */
		if (protocol == 1 && data_len >= l3_offset + 20 + 8 + 20 + 8) {
			valid = 1;
			hash = &p[l3_offset + 12];
			hash_len = 4;
		}
	}

	if (eth_type == 0x86dd && (p[l3_offset] & 0xF0) == 0x60) {
		icmp_offset = l3_offset + 40;

		uint8_t protocol = p[l3_offset + 6];
		/* header, 40 bytes of IPv6, 8 bytes of ICMP
		 * payload: 32 bytes of IPv6 payload */
		if (protocol == 58 && data_len >= l3_offset + 40 + 8 + 32) {
			valid = 1;
			hash = &p[l3_offset + 8];
			hash_len = 16;
		}
	}

	if (valid == 0 || hash == NULL || hash_len == 0 || icmp_offset < 0) {
		reason = "Invalid protocol or too short";
		goto reject;
	}

	if (state->ports_map) {
		int payload_offset = icmp_offset + 8;
		if (data_len < payload_offset + 9) {
			reason = "Payload too short";
			goto reject;
		}

		/* Optimistic parsing: ignore protocol field in ICMP
		 * payload, ignore IP length, etc. */
		int l4_offset = -1;
		switch (p[payload_offset + 8] & 0xF0) {
		case 0x40:
			l4_offset = payload_offset +
				    (int)(p[payload_offset] & 0x0F) * 4;
			break;
		case 0x60:
			l4_offset = payload_offset + 40;
			break;
		default:
			reason = "Invalid ICMP payload";
			goto reject;
		}

		if (data_len < l4_offset + 2) {
			reason = "Too short to read L4 source port";
			goto reject;
		}
		uint16_t l4_sport = ((uint16_t)p[l4_offset]) << 8 |
				    ((uint16_t)p[l4_offset + 1]);
		if (bitmap_get(state->ports_map, l4_sport) == 0) {
			reason = "L4 source port not on whitelist";
			goto reject;
		}
	}

	uint8_t dst_mac[6];
	memcpy(dst_mac, p, 6);

	/* alright, write there anyway */
	uint8_t *pp = (uint8_t *)p;

	int i;
	for (i = 0; i < 6; i++) {
		pp[i] = 0xff;
	}

	for (i = 0; i < 6; i++) {
		pp[6 + i] = dst_mac[i];
	}

	if (!hashlimit_touch_hash(state->sources, hash, hash_len)) {
		reason = "Ratelimited on source IP";
		goto reject;
	}
	if (!hashlimit_touch(state->ifaces, 0)) {
		reason = "Ratelimited on outgoing interface";
		goto reject;
	}

	reason = "transmitting";
	if (state->verbose > 2) {
		printf("%s %s  %s\n", ip_to_string(hash, hash_len), reason,
		       to_hex(p, data_len));
	} else if (state->verbose == 1) {
		printf("%s %s\n", ip_to_string(hash, hash_len), reason);
	}

	if (state->dry_run == 0) {
		int r = send(state->raw_sd, pp, data_len, 0);
		/* ENOBUFS happens during IRQ storms okay to ignore */
		if (r < 0 && errno != ENOBUFS) {
			PFATAL("send()");
		}
	}
	return 1;

reject:
	if (state->verbose > 2) {
		printf("%s %s  %s\n", ip_to_string(hash, hash_len), reason,
		       to_hex(p, data_len));
	} else if (state->verbose > 1) {
		printf("%s %s\n", ip_to_string(hash, hash_len), reason);
	}

	return -1;
}

static int handle_pcap(struct uevent *uevent, int sfd, int mask, void *userdata)
{
	struct state *state = userdata;

	while (1) {
		struct pcap_pkthdr *hdr;
		const uint8_t *data;

		int r = pcap_next_ex(state->pcap, &hdr, &data);

		switch (r) {
		case 1:
			if (hdr->len == hdr->caplen) {
				handle_packet(state, data, hdr->caplen);
			} else {
				/* Partial caputre */
			}
			break;

		case 0:
			/* Timeout */
			return 0;

		case -1:
			FATAL("pcap_next_ex(): %s", pcap_geterr(state->pcap));
			break;

		case -2:
			return 0;
		}
	}
}

int main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"iface", required_argument, 0, 'i'},
		{"src-rate", required_argument, 0, 's'},
		{"iface-rate", required_argument, 0, 'r'},
		{"verbose", no_argument, 0, 'v'},
		{"dry-run", no_argument, 0, 'd'},
		{"cpu", required_argument, 0, 'c'},
		{"help", no_argument, 0, 'h'},
		{"ports", required_argument, 0, 'p'},
		{NULL, 0, 0, 0}};

	const char *optstring = optstring_from_long_options(long_options);
	const char *iface = NULL;

	double src_rate = SRC_RATE_PPS;
	double iface_rate = IFACE_RATE_PPS;
	int verbose = 0;
	int dry_run = 0;
	int taskset_cpu = -1;
	uint64_t *ports_map = NULL;

	optind = 1;
	while (1) {
		int option_index = 0;
		int arg = getopt_long(argc, argv, optstring, long_options,
				      &option_index);
		if (arg == -1) {
			break;
		}

		switch (arg) {
		case 0:
			FATAL("Unknown option: %s", argv[optind]);
			break;

		case 'h':
			usage();
			break;

		case '?':
			exit(-1);
			break;

		case 'i':
			iface = optarg;
			break;

		case 's':
			src_rate = atof(optarg);
			if (src_rate <= 0.0) {
				FATAL("Rates must be greater than zero");
			}
			break;

		case 'r':
			iface_rate = atof(optarg);
			if (iface_rate <= 0.0) {
				FATAL("Rates must be greater than zero");
			}
			break;

		case 'p': {
			if (ports_map == NULL) {
				ports_map = bitmap_alloc(65536);
			}
			const char **org_ports = parse_argv(optarg, ',');
			const char **ports = org_ports;
			for (; ports[0] != NULL; ports++) {
				errno = 0;
				char *eptr = NULL;
				int port = strtol(ports[0], &eptr, 10);
				if (port < 0 || port > 65535 || errno != 0 ||
				    (unsigned)(eptr - ports[0]) !=
					    strlen(ports[0])) {
					FATAL("Malformed port number value "
					      "\"%s\".",
					      ports[0]);
				}
				bitmap_set(ports_map, port);
			}
			free(org_ports);
		} break;

		case 'v':
			verbose++;
			break;

		case 'd':
			dry_run = 1;
			break;

		case 'c':
			taskset_cpu = atoi(optarg);
			break;

		default:
			FATAL("Unknown option %c: %s", arg,
			      str_quote(argv[optind]));
		}
	}

	if (argv[optind]) {
		FATAL("Not sure what you mean by %s", str_quote(argv[optind]));
	}

	if (iface == NULL) {
		FATAL("Specify interface with --iface option");
	}

	if (set_core_dump(1) < 0) {
		ERRORF("[ ] Failed to enable core dumps\n");
	}

	if (taskset_cpu > -1) {
		if (taskset(taskset_cpu)) {
			ERRORF("[ ] sched_setaffinity(%i): %s\n", taskset_cpu,
			       strerror(errno));
		}
	}

	struct pcap_stat stats = {0, 0, 0};
	struct state state;
	state.pcap = setup_pcap(iface, BPF_FILTER, SNAPLEN, &stats);
	state.raw_sd = setup_raw(iface);
	state.sources = hashlimit_alloc(8191, src_rate, src_rate * 1.9);
	state.ifaces = hashlimit_alloc(1, iface_rate, iface_rate * 1.9);
	state.verbose = verbose;
	state.dry_run = dry_run;
	state.ports_map = ports_map;

	int pcap_fd = pcap_get_selectable_fd(state.pcap);
	if (pcap_fd < 0) {
		PFATAL("pcap_get_selectable_fd()");
	}

	volatile int done = 0;
	struct uevent uevent;
	uevent_new(&uevent);
	uevent_yield(&uevent, signal_desc(SIGINT), UEVENT_READ, on_signal,
		     (void *)&done);
	uevent_yield(&uevent, signal_desc(SIGTERM), UEVENT_READ, on_signal,
		     (void *)&done);
	uevent_yield(&uevent, pcap_fd, UEVENT_READ, handle_pcap, &state);

	fprintf(stderr,
		"[*] #%i Started pmtud on %s rates={iface=%.1f pps source=%.1f "
		"pps}, "
		"verbose=%i, dry_run=%i\n",
		getpid(), str_quote(iface), iface_rate, src_rate, verbose,
		dry_run);

	while (done == 0) {
		struct timeval timeout =
			NSEC_TIMEVAL(MSEC_NSEC(24 * 60 * 60 * 1000UL));
		int r = uevent_select(&uevent, &timeout);
		if (r != 0) {
			continue;
		}
	}
	fprintf(stderr, "[*] #%i Quitting\n", getpid());

	unsetup_pcap(state.pcap, iface, &stats);
	fprintf(stderr, "[*] #%i recv=%i drop=%i ifdrop=%i\n", getpid(),
		stats.ps_recv, stats.ps_drop, stats.ps_ifdrop);

	close(state.raw_sd);
	hashlimit_free(state.sources);
	hashlimit_free(state.ifaces);
	if (state.ports_map) {
		bitmap_free(state.ports_map);
	}

	return 0;
}
