/*
 * Copyright(c) 2012 Tim Ruehsen
 * Copyright(c) 2015-2019 Free Software Foundation, Inc.
 *
 * This file is part of libwget.
 *
 * Libwget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Libwget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libwget.  If not, see <https://www.gnu.org/licenses/>.
 *
 *
 * network routines
 *
 * Changelog
 * 25.04.2012  Tim Ruehsen  created
 * 16.11.2012               new functions tcp_set_family() and tcp_set_preferred_family()
 *
 * RFC 7413: TCP Fast Open
 */

#include <config.h>

#include <sys/types.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <c-ctype.h>
#include <time.h>
#include <errno.h>
#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#elif defined HAVE_WS2TCPIP_H
# include <ws2tcpip.h>
#endif
#include <netdb.h>
#include <netinet/in.h>

#ifdef HAVE_NETINET_TCP_H
#	include <netinet/tcp.h>
#endif

#if ((defined _WIN32 || defined __WIN32__) && !defined __CYGWIN__)
# include <sys/ioctl.h>
#else
# include <fcntl.h>
#endif

#if defined __APPLE__ && defined __MACH__ && defined CONNECT_DATA_IDEMPOTENT && defined CONNECT_RESUME_ON_READ_WRITE
# define TCP_FASTOPEN_OSX
#elif defined TCP_FASTOPEN && defined MSG_FASTOPEN
# define TCP_FASTOPEN_LINUX
#endif

#include <wget.h>
#include "private.h"
#include "net.h"

/**
 * \file
 * \brief Functions to work with TCP sockets and SSL/TLS
 * \defgroup libwget-net TCP sockets
 *
 * @{
 *
 * TCP sockets and DNS cache management functions.
 *
 * The following features are supported:
 *
 *  - TCP Fast Open ([RFC 7413](https://tools.ietf.org/html/rfc7413))
 *  - SSL/TLS
 *  - DNS caching
 *
 * Most functions here take a `wget_tcp_t` structure as argument.
 *
 * The `wget_tcp_t` structure represents a TCP connection. You create it with wget_tcp_init()
 * and destroy it with wget_tcp_deinit(). You can connect to a remote host with wget_tcp_connect(),
 * or listen for incoming connections (and accept them) with wget_tcp_listen() and wget_tcp_accept().
 * You end a connection with wget_tcp_close().
 *
 * There are several knobs you can use to customize the behavior of most functions here.
 * The list that follows describes the most important parameters, although you can look at the getter and setter
 * functions here to see them all (`wget_tcp_get_xxx`, `wget_tcp_set_xxx`).
 *
 *  - Timeout: maximum time to wait for an operation to complete. For example, for wget_tcp_read(), it sets the maximum time
 *  to wait until some data is available to read. Most functions here can be non-blocking (with timeout = 0) returning immediately
 *  or they can block indefinitely until something happens (with timeout = -1). For any value greater than zero,
 *  the timeout is taken as milliseconds.
 *  - Caching: whether to use DNS caching or not. The DNS cache is kept internally and is shared among all connections.
 *  You can disable it for a specific `wget_tcp_t`, so that specific connection will not use the DNS cache, or you can
 *  disable it globally.
 *  - Family and preferred family: these are used to determine which address family should be used when resolving a host name or
 *  IP address. You probably use `AF_INET` or `AF_INET6` most of the time. The first one forces the library to use that family,
 *  failing if it cannot find any IP address with it. The second one is just a hint, about which family you would prefer; it will try
 *  to get an address of that family if possible, and will get another one if not.
 *  - SSL/TLS: do you want to use TLS?
 *
 *  When you create a new `wget_tcp_t` with wget_tcp_init(), it is initialized with the following parameters:
 *
 *   - Timeout: -1
 *   - Connection timeout (max. time to wait for a connection to be accepted by the remote host): -1
 *   - DNS timeout (max. time to wait for a DNS query to return): -1
 *   - DNS caching: yes
 *   - Family: `AF_UNSPEC` (basically means "I don't care, pick the first one available").
 */

static struct wget_tcp_st _global_tcp = {
	.sockfd = -1,
	.dns_timeout = -1,
	.connect_timeout = -1,
	.timeout = -1,
	.family = AF_UNSPEC,
	.caching = 1,
#if defined TCP_FASTOPEN_OSX
	.tcp_fastopen = 1,
#elif defined TCP_FASTOPEN_LINUX
	.tcp_fastopen = 1,
	.first_send = 1,
#endif
};

typedef struct
{
	const char
		*hostname,
		*ip;
	uint16_t port;
	long long dns_secs;	// milliseconds
} _stats_data_t;

static wget_stats_callback_t stats_callback;

static wget_thread_mutex_t
	resolve_mutex;
static bool
	initialized;

static void __attribute__ ((constructor)) _wget_net_init(void)
{
	if (!initialized) {
		wget_thread_mutex_init(&resolve_mutex);
		initialized = 1;
	}
}

static void __attribute__ ((destructor)) _wget_net_exit(void)
{
	if (initialized) {
		wget_thread_mutex_destroy(&resolve_mutex);
		initialized = 0;
	}
}

/* for Windows compatibility */
#include "sockets.h"
/**
 * \return 0 for success, else failure
 *
 * Initialize the resources needed for network operations.
 */
int wget_net_init(void)
{
	_wget_net_init();

	int rc = gl_sockets_startup(SOCKETS_2_2);

	return rc ? -1 : 0;
}

/**
 * \return 0 for success, else failure
 *
 * Free the resources allocated by wget_net_init().
 */
int wget_net_deinit(void)
{
	_wget_net_exit();

	int rc = gl_sockets_cleanup();

	return rc ? -1 : 0;
}

/*
 * Reorder address list so that addresses of the preferred family will come first.
 */
static struct addrinfo *_wget_sort_preferred(struct addrinfo *addrinfo, int preferred_family)
{
	struct addrinfo *preferred = NULL, *preferred_tail = NULL;
	struct addrinfo *unpreferred = NULL, *unpreferred_tail = NULL;

	for (struct addrinfo *ai = addrinfo; ai;) {
		if (ai->ai_family == preferred_family) {
			if (preferred_tail)
				preferred_tail->ai_next = ai;
			else
				preferred = ai; // remember the head of the list

			preferred_tail = ai;
			ai = ai->ai_next;
			preferred_tail->ai_next = NULL;
		} else {
			if (unpreferred_tail)
				unpreferred_tail->ai_next = ai;
			else
				unpreferred = ai; // remember the head of the list

			unpreferred_tail = ai;
			ai = ai->ai_next;
			unpreferred_tail->ai_next = NULL;
		}
	}

	/* Merge preferred + not preferred */
	if (preferred) {
		preferred_tail->ai_next = unpreferred;
		return preferred;
	} else {
		return unpreferred;
	}
}

// we can't provide a portable way of respecting a DNS timeout
static int _wget_tcp_resolve(int family, int flags, const char *host, uint16_t port, struct addrinfo **out_addr)
{
	struct addrinfo hints = {
		.ai_family = family,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_ADDRCONFIG | flags
	};

	if (port) {
		char s_port[NI_MAXSERV];

		hints.ai_flags |= AI_NUMERICSERV;

		wget_snprintf(s_port, sizeof(s_port), "%hu", port);
		debug_printf("resolving %s:%s...\n", host ? host : "", s_port);
		return getaddrinfo(host, s_port, &hints, out_addr);
	} else {
		debug_printf("resolving %s...\n", host);
		return getaddrinfo(host, NULL, &hints, out_addr);
	}
}

/**
 *
 * \param[in] ip IP address of name
 * \param[in] name Domain name, part of the cache's lookup key
 * \param[in] port Port number, part of the cache's lookup key
 * \return 0 on success, < 0 on error
 *
 * Assign an IP address to the name+port key in the DNS cache.
 * The \p name should be lowercase.
 */
int wget_tcp_dns_cache_add(const char *ip, const char *name, uint16_t port)
{
	int rc, family;
	struct addrinfo *ai;

	if (wget_ip_is_family(ip, WGET_NET_FAMILY_IPV4)) {
		family = AF_INET;
	} else if (wget_ip_is_family(ip, WGET_NET_FAMILY_IPV6)) {
		family = AF_INET6;
	} else
		return -1;

	if ((rc = _wget_tcp_resolve(family, AI_NUMERICHOST, ip, port, &ai)) != 0) {
		error_printf(_("Failed to resolve %s:%d: %s\n"), ip, port, gai_strerror(rc));
		return -1;
	}

	wget_dns_cache_add(name, port, ai);

	return 0;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure, obtained with a previous call to wget_tcp_init().
 * \param[in] host Hostname
 * \param[in] port TCP destination port
 * \return A `struct addrinfo` structure (defined in libc's `<netdb.h>`). Must be freed by the caller with `freeaddrinfo(3)`.
 *
 * Resolve a host name into its IPv4/IPv6 address.
 *
 * The **caching** parameter tells wget_tcp_resolve() to use the DNS cache as long as possible. This means that if
 * the queried hostname is found in the cache, that will be returned without querying any actual DNS server. If no such
 * entry is found, a DNS query is performed, and the result stored in the cache. You can enable caching with wget_tcp_set_dns_caching().
 *
 * Note that if **caching** is false, the DNS cache will not be used at all. Not only it won't be used for looking up the hostname,
 * but the addresses returned by the DNS server will not be stored in it either.
 *
 * This function uses the following `wget_tcp_t` parameters:
 *
 *  - DNS caching: Use the internal DNS cache. If the hostname is found there, return it immediately.
 *    Otherwise continue and do a normal DNS query, and store the result in the cache. You can enable this
 *    with wget_tcp_set_dns_cache().
 *  - Address family: Desired address family for the returned addresses. This will typically be `AF_INET` or `AF_INET6`,
 *    but it can be any of the values defined in `<socket.h>`. Additionally, `AF_UNSPEC` means you don't care: it will
 *    return any address family that can be used with the specified \p host and \p port. If **family** is different
 *    than `AF_UNSPEC` and the specified family is not found, _that's an error condition_ and thus wget_tcp_resolve() will return NULL.
 *    You can set this with wget_tcp_set_family().
 *  - Preferred address family: Tries to resolve addresses of this family if possible. This is only honored if **family**
 *    (see point above) is `AF_UNSPEC`.
 *
 *  The parameter \p tcp might be NULL. In that case, the aforementioned behavior is governed by global options: those set by
 *  previous calls to wget_tcp_set_dns_caching(), wget_tcp_set_family() and wget_tcp_set_preferred_family(), etc.
 *
 *  The returned `addrinfo` structure must be freed with `freeaddrinfo(3)`. Note that if you call wget_tcp_connect(),
 *  this will be done for you when you call wget_tcp_close(). But if you call this function alone, you must take care of it.
 */
struct addrinfo *wget_tcp_resolve(wget_tcp_t *tcp, const char *host, uint16_t port)
{
	struct addrinfo *addrinfo = NULL;
	int rc = 0;
	char adr[NI_MAXHOST], sport[NI_MAXSERV];
	long long before_millisecs = 0;
	_stats_data_t stats;

	if (!tcp)
		tcp = &_global_tcp;

	if (stats_callback)
		before_millisecs = wget_get_timemillis();
	// get the IP address for the server
	for (int tries = 0, max = 3; tries < max; tries++) {
		if (tcp->caching) {
			if ((addrinfo = wget_dns_cache_get(host, port)))
				return addrinfo;

			// prevent multiple address resolutions of the same host
			wget_thread_mutex_lock(resolve_mutex);

			// now try again
			if ((addrinfo = wget_dns_cache_get(host, port))) {
				wget_thread_mutex_unlock(resolve_mutex);
				return addrinfo;
			}
		}

		addrinfo = NULL;

		rc = _wget_tcp_resolve(tcp->family, 0, host, port, &addrinfo);
		if (rc == 0 || rc != EAI_AGAIN)
			break;

		if (tries < max - 1) {
			if (tcp->caching)
				wget_thread_mutex_unlock(resolve_mutex);
			wget_millisleep(100);
		}
	}

	if (stats_callback) {
		long long after_millisecs = wget_get_timemillis();
		stats.dns_secs = after_millisecs - before_millisecs;
		stats.hostname = host;
		stats.port = port;
	}

	if (rc) {
		error_printf(_("Failed to resolve %s (%s)\n"),
				(host ? host : ""), gai_strerror(rc));

		if (tcp->caching)
			wget_thread_mutex_unlock(resolve_mutex);

		if (stats_callback) {
			stats.ip = NULL;
			stats_callback(&stats);
		}

		return NULL;
	}

	if (tcp->family == AF_UNSPEC && tcp->preferred_family != AF_UNSPEC)
		addrinfo = _wget_sort_preferred(addrinfo, tcp->preferred_family);

	if (stats_callback) {
		if ((rc = getnameinfo(addrinfo->ai_addr, addrinfo->ai_addrlen, adr, sizeof(adr), sport, sizeof(sport), NI_NUMERICHOST | NI_NUMERICSERV)) == 0)
			stats.ip = adr;
		else
			stats.ip = "???";

		stats_callback(&stats);
	}

	/* Finally, print the address list to the debug pipe if enabled */
	if (wget_logger_is_active(wget_get_logger(WGET_LOGGER_DEBUG))) {
		for (struct addrinfo *ai = addrinfo; ai; ai = ai->ai_next) {
			if ((rc = getnameinfo(ai->ai_addr, ai->ai_addrlen, adr, sizeof(adr), sport, sizeof(sport), NI_NUMERICHOST | NI_NUMERICSERV)) == 0)
				debug_printf("has %s:%s\n", adr, sport);
			else
				debug_printf("has ??? (%s)\n", gai_strerror(rc));
		}
	}

	if (tcp->caching) {
		/*
		 * In case of a race condition the already existing addrinfo is returned.
		 * The addrinfo argument given to wget_dns_cache_add() will be freed in this case.
		 */
		addrinfo = wget_dns_cache_add(host, port, addrinfo);
		wget_thread_mutex_unlock(resolve_mutex);
	}

	return addrinfo;
}

static int G_GNUC_WGET_CONST _value_to_family(int value)
{
	switch (value) {
	case WGET_NET_FAMILY_IPV4:
		return AF_INET;
	case WGET_NET_FAMILY_IPV6:
		return AF_INET6;
	default:
		return AF_UNSPEC;
	}
}

static int G_GNUC_WGET_CONST _family_to_value(int family)
{
	switch (family) {
	case AF_INET:
		return WGET_NET_FAMILY_IPV4;
	case AF_INET6:
		return WGET_NET_FAMILY_IPV6;
	default:
		return WGET_NET_FAMILY_ANY;
	}
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \param[in] tcp_fastopen 1 or 0, whether to enable or disable TCP Fast Open.
 *
 * Enable or disable TCP Fast Open ([RFC 7413](https://tools.ietf.org/html/rfc7413)), if available.
 *
 * This function is a no-op on systems where TCP Fast Open is not supported.
 *
 * If \p tcp is NULL, TCP Fast Open is enabled or disabled globally.
 */
#if defined TCP_FASTOPEN_OSX || defined TCP_FASTOPEN_LINUX
void wget_tcp_set_tcp_fastopen(wget_tcp_t *tcp, int tcp_fastopen)
{
	(tcp ? tcp : &_global_tcp)->tcp_fastopen = !!tcp_fastopen;
}
#else
void wget_tcp_set_tcp_fastopen(wget_tcp_t G_GNUC_WGET_UNUSED *tcp, int G_GNUC_WGET_UNUSED tcp_fastopen)
{
}
#endif

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \return 1 if TCP Fast Open is enabled, 0 otherwise.
 *
 * Tells whether TCP Fast Open is enabled or not.
 *
 * You can enable and disable it with wget_tcp_set_tcp_fastopen().
 */
char wget_tcp_get_tcp_fastopen(wget_tcp_t *tcp)
{
	return (tcp ? tcp : &_global_tcp)->tcp_fastopen;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \param[in] false_start 1 or 0, whether to enable or disable TLS False Start.
 *
 * Enable or disable TLS False Start ([RFC 7918](https://tools.ietf.org/html/rfc7413)).
 *
 * If \p tcp is NULL, TLS False Start is enabled or disabled globally.
 */
void wget_tcp_set_tls_false_start(wget_tcp_t *tcp, int false_start)
{
	(tcp ? tcp : &_global_tcp)->tls_false_start = !!false_start;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \return 1 if TLS False Start is enabled, 0 otherwise.
 *
 * Tells whether TLS False Start is enabled or not.
 *
 * You can enable and disable it with wget_tcp_set_tls_false_start().
 */
char wget_tcp_get_tls_false_start(wget_tcp_t *tcp)
{
	return (tcp ? tcp : &_global_tcp)->tls_false_start;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \param[in] caching 1 or 0, whether to enable or disable DNS caching
 *
 * Enable or disable DNS caching for the connection provided, or globally.
 *
 * The DNS cache is kept internally in memory, and is used in wget_tcp_resolve() to speed up DNS queries.
 *
 * If \p tcp is NULL, DNS caching is enabled or disabled globally.
 */
void wget_tcp_set_dns_caching(wget_tcp_t *tcp, int caching)
{
	(tcp ? tcp : &_global_tcp)->caching = !!caching;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \return 1 if DNS caching is enabled, 0 otherwise.
 *
 * Tells whether DNS caching is enabled or not.
 *
 * You can enable and disable it with wget_tcp_set_dns_caching().
 */
int wget_tcp_get_dns_caching(wget_tcp_t *tcp)
{
	return (tcp ? tcp : &_global_tcp)->caching;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init().
 * \param[in] protocol The protocol, either WGET_PROTOCOL_HTTP_2_0 or WGET_PROTOCOL_HTTP_1_1.
 *
 * Set the protocol for the connection provided, or globally.
 *
 * If \p tcp is NULL, theprotocol will be set globally (for all connections). Otherwise,
 * only for the provided connection (\p tcp).
 */
void wget_tcp_set_protocol(wget_tcp_t *tcp, int protocol)
{
	(tcp ? tcp : &_global_tcp)->protocol = protocol;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init().
 * \return The protocol with this connection, currently WGET_PROTOCOL_HTTP_2_0 or WGET_PROTOCOL_HTTP_1_1.
 *
 * Get protocol used with the provided connection, or globally (if \p tcp is NULL).
 */
int wget_tcp_get_protocol(wget_tcp_t *tcp)
{
	return (tcp ? tcp : &_global_tcp)->protocol;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \param[in] family One of the socket families defined in `<socket.h>`, such as `AF_INET` or `AF_INET6`.
 *
 * Tells the preferred address family that should be used when establishing a TCP connection.
 *
 * wget_tcp_resolve() will favor that and pick an address of that family if possible.
 *
 * If \p tcp is NULL, the preferred address family will be set globally.
 */
void wget_tcp_set_preferred_family(wget_tcp_t *tcp, int family)
{
	(tcp ? tcp : &_global_tcp)->preferred_family = _value_to_family(family);
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \return One of the socket families defined in `<socket.h>`, such as `AF_INET` or `AF_INET6`.
 *
 * Get the preferred address family that was previously set with wget_tcp_set_preferred_family().
 */
int wget_tcp_get_preferred_family(wget_tcp_t *tcp)
{
	return _family_to_value((tcp ? tcp : &_global_tcp)->preferred_family);
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \param[in] family One of the socket families defined in `<socket.h>`, such as `AF_INET` or `AF_INET6`.
 *
 * Tell the address family that will be used when establishing a TCP connection.
 *
 * wget_tcp_resolve() will pick an address of that family, or fail if it cannot find one.
 *
 * If \p tcp is NULL, the address family will be set globally.
 */
void wget_tcp_set_family(wget_tcp_t *tcp, int family)
{
	(tcp ? tcp : &_global_tcp)->family = _value_to_family(family);
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \return One of the socket families defined in `<socket.h>`, such as `AF_INET` or `AF_INET6`.
 *
 * Get the address family that was previously set with wget_tcp_set_family().
 */
int wget_tcp_get_family(wget_tcp_t *tcp)
{
	return _family_to_value((tcp ? tcp : &_global_tcp)->family);
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \return The local port.
 *
 * Get the port number the TCP connection \p tcp is bound to on the local machine.
 */
int wget_tcp_get_local_port(wget_tcp_t *tcp)
{
	if (unlikely(!tcp))
		return 0;

	struct sockaddr_storage addr_store;
	struct sockaddr *addr = (struct sockaddr *)&addr_store;
	socklen_t addr_len = sizeof(addr_store);

	/* Get automatically retrieved port number */
	if (getsockname(tcp->sockfd, addr, &addr_len) == 0) {
		char s_port[NI_MAXSERV];

		if (getnameinfo(addr, addr_len, NULL, 0, s_port, sizeof(s_port), NI_NUMERICSERV) == 0)
			return atoi(s_port);
	}

	return 0;
}

/**
 * \param[in] fn A `wget_stats_callback_t` callback function used to collect DNS statistics
 *
 * Set callback function to be called once DNS statistics for a host are collected
 */
void wget_tcp_set_stats_dns(wget_stats_callback_t fn)
{
	stats_callback = fn;
}

/**
 * \param[in] type A `wget_dns_stats_t` constant representing DNS statistical info to return
 * \param[in] _stats An internal  pointer sent to callback function
 * \return DNS statistical info in question
 *
 * Get the specific DNS statistics information
 */
const void *wget_tcp_get_stats_dns(const wget_dns_stats_t type, const void *_stats)
{
	const _stats_data_t *stats = (_stats_data_t *) _stats;

	switch(type) {
	case WGET_STATS_DNS_HOST:
		return stats->hostname;
	case WGET_STATS_DNS_IP:
		return stats->ip;
	case WGET_STATS_DNS_PORT:
		return &(stats->port);
	case WGET_STATS_DNS_SECS:
		return &(stats->dns_secs);
	default:
		return NULL;
	}
}

/**
 * \param[in] tcp A TCP connection.
 * \param[in] timeout The timeout value.
 *
 * Set the timeout (in milliseconds) for the DNS queries.
 *
 * This is the maximum time to wait until we get a response from the server.
 *
 * The following two values are special:
 *
 *  - `0`: No timeout, immediate.
 *  - `-1`: Infinite timeout. Wait indefinitely.
 */
void wget_tcp_set_dns_timeout(wget_tcp_t *tcp, int timeout)
{
	(tcp ? tcp : &_global_tcp)->dns_timeout = timeout;
}

/**
 * \param[in] tcp A TCP connection.
 * \param[in] timeout The timeout value.
 *
 * Set the timeout for the TCP connection.
 *
 * This is the maximum time to wait until the remote host accepts our connection.
 *
 * The following two values are special:
 *
 *  - `0`: No timeout, immediate.
 *  - `-1`: Infinite timeout. Wait indefinitely.
 */
void wget_tcp_set_connect_timeout(wget_tcp_t *tcp, int timeout)
{
	(tcp ? tcp : &_global_tcp)->connect_timeout = timeout;
}

/**
 * \param[in] tcp A TCP connection.
 * \param[in] timeout The timeout value.
 *
 * Set the timeout (in milliseconds) for wget_tcp_read(), wget_tcp_write() and wget_tcp_accept().
 *
 * The following two values are special:
 *
 *  - `0`: No timeout, immediate.
 *  - `-1`: Infinite timeout. Wait indefinitely.
 */
void wget_tcp_set_timeout(wget_tcp_t *tcp, int timeout)
{
	(tcp ? tcp : &_global_tcp)->timeout = timeout;
}

/**
 * \param[in] tcp A TCP connection.
 * \return The timeout value that was set with wget_tcp_set_timeout().
 *
 * Get the timeout value that was set with wget_tcp_set_timeout().
 */
int wget_tcp_get_timeout(wget_tcp_t *tcp)
{
	return (tcp ? tcp : &_global_tcp)->timeout;
}

/**
 * \param[in] tcp A TCP connection. Might be NULL.
 * \param[in] bind_address An IP address or host name.
 *
 * Set the IP address/hostname the socket \p tcp will bind to on the local machine
 * when connecting to a remote host.
 *
 * The hostname can explicitly set the port after a colon (':').
 *
 * This is mainly relevant to wget_tcp_connect().
 */
void wget_tcp_set_bind_address(wget_tcp_t *tcp, const char *bind_address)
{
	if (!tcp)
		tcp = &_global_tcp;

	if (tcp->bind_addrinfo_allocated) {
		freeaddrinfo(tcp->bind_addrinfo);
		tcp->bind_addrinfo = NULL;
	}

	if (bind_address) {
		char copy[strlen(bind_address) + 1], *s = copy;
		const char *host;

		memcpy(copy, bind_address, sizeof(copy));

		if (*s == '[') {
			/* IPv6 address within brackets */
			char *p = strrchr(s, ']');
			if (p) {
				host = s + 1;
				s = p + 1;
			} else {
				/* Something is broken */
				host = s + 1;
				while (*s)
					s++;
			}
		} else {
			host = s;
			while (*s && *s != ':')
				s++;
		}

		if (*s == ':') {
			*s++ = 0;
			if (c_isdigit(*s))
				tcp->bind_addrinfo = wget_tcp_resolve(tcp, host, (uint16_t) atoi(s));
		} else {
			tcp->bind_addrinfo = wget_tcp_resolve(tcp, host, 0);
		}

		tcp->bind_addrinfo_allocated = !tcp->caching && tcp->bind_addrinfo;
	}
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init().
 * \param[in] ssl Flag to enable or disable SSL/TLS on the given connection.
 *
 * Enable or disable SSL/TLS.
 *
 * If \p tcp is NULL, TLS will be enabled globally. Otherwise, TLS will be enabled only for the provided connection.
 */
void wget_tcp_set_ssl(wget_tcp_t *tcp, int ssl)
{
	(tcp ? tcp : &_global_tcp)->ssl = !!ssl;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init().
 * \return 1 if TLs is enabled, 0 otherwise.
 *
 * Tells whether TLS is enabled or not.
 */
int wget_tcp_get_ssl(wget_tcp_t *tcp)
{
	return (tcp ? tcp : &_global_tcp)->ssl;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \param[in] hostname A hostname. The value of the SNI field.
 *
 * Sets the TLS Server Name Indication (SNI). For more info see [RFC 6066, sect. 3](https://tools.ietf.org/html/rfc6066#section-3).
 *
 * SNI basically does at the TLS layer what the `Host:` header field does at the application (HTTP) layer.
 * The server might use this information to locate an appropriate X.509 certificate from a pool of certificates, or to direct
 * the request to a specific virtual host, for instance.
 */
void wget_tcp_set_ssl_hostname(wget_tcp_t *tcp, const char *hostname)
{
	if (!tcp)
		tcp = &_global_tcp;

	xfree(tcp->ssl_hostname);
	tcp->ssl_hostname = wget_strdup(hostname);
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 * \return A hostname. The value of the SNI field.
 *
 * Returns the value that was set to SNI with a previous call to wget_tcp_set_ssl_hostname().
 */
const char *wget_tcp_get_ssl_hostname(wget_tcp_t *tcp)
{
	return (tcp ? tcp : &_global_tcp)->ssl_hostname;
}

/**
 * \return A new `wget_tcp_t` structure, with pre-defined parameters.
 *
 * Create a new `wget_tcp_t` structure, that represents a TCP connection.
 * It can be destroyed with wget_tcp_deinit().
 *
 * This function does not establish or modify a TCP connection in any way.
 * That can be done with the other functions in this file, such as
 * wget_tcp_connect() or wget_tcp_listen() and wget_tcp_accept().
 */
wget_tcp_t *wget_tcp_init(void)
{
	wget_tcp_t *tcp = xmalloc(sizeof(wget_tcp_t));

	*tcp = _global_tcp;
	tcp->ssl_hostname = wget_strdup(_global_tcp.ssl_hostname);

	return tcp;
}

/**
 * \param[in] _tcp A **pointer** to a `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init(). Might be NULL.
 *
 * Release a TCP connection (created with wget_tcp_init()).
 *
 * The `wget_tcp_t` structure will be freed and \p _tcp will be set to NULL.
 *
 * If \p _tcp is NULL, the SNI field will be cleared.
 *
 * Does not free the internal DNS cache, so that other connections can re-use it.
 * Call wget_dns_cache_free() if you want to free it.
 */
void wget_tcp_deinit(wget_tcp_t **_tcp)
{
	wget_tcp_t *tcp;

	if (!_tcp) {
		xfree(_global_tcp.ssl_hostname);
		return;
	}

	if ((tcp = *_tcp)) {
		wget_tcp_close(tcp);

		if (tcp->bind_addrinfo_allocated) {
			freeaddrinfo(tcp->bind_addrinfo);
			tcp->bind_addrinfo = NULL;
		}

		xfree(tcp->ssl_hostname);
		xfree(tcp->ip);
		xfree(tcp);

		*_tcp = NULL;
	}
}

static void _set_async(int fd)
{
#if ((defined _WIN32 || defined __WIN32__) && !defined __CYGWIN__)
	unsigned long blocking = 0;

	if (ioctl(fd, FIONBIO, &blocking))
		error_printf_exit(_("Failed to set socket to non-blocking\n"));
#else
	int flags;

	if ((flags = fcntl(fd, F_GETFL)) < 0)
		error_printf_exit(_("Failed to get socket flags\n"));

	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		error_printf_exit(_("Failed to set socket to non-blocking\n"));
#endif
}

static void _set_socket_options(int fd)
{
	int on = 1;

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on)) == -1)
		error_printf(_("Failed to set socket option REUSEADDR\n"));

	on = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on)) == -1)
		error_printf(_("Failed to set socket option NODELAY\n"));
}

/**
 * Test whether the given connection (\p tcp) is ready to read or write.
 *
 * The parameter \p flags can have one or both (with bitwise OR) of the following values:
 *
 *  - `WGET_IO_READABLE`: Is data available for reading?
 *  - `WGET_IO_WRITABLE`: Can we write immediately (without having to wait until the TCP buffer frees)?
 */
int wget_tcp_ready_2_transfer(wget_tcp_t *tcp, int flags)
{
	if (likely(tcp))
		return wget_ready_2_transfer(tcp->sockfd, tcp->timeout, flags);
	else
		return -1;
}

/**
 * \param[in] tcp A `wget_tcp_t` structure representing a TCP connection, returned by wget_tcp_init().
 * \param[in] host Hostname or IP address to connect to.
 * \param[in] port port number
 * \return WGET_E_SUCCESS (0) on success, or a negative integer on error (some of WGET_E_XXX defined in `<wget.h>`).
 *
 * Open a TCP connection with a remote host.
 *
 * This function will use TLS if it has been enabled for this `wget_tcp_t`. You can enable it
 * with wget_tcp_set_ssl(). Additionally, you can also use wget_tcp_set_ssl_hostname() to set the
 * Server Name Indication (SNI).
 *
 * You can set which IP address and port on the local machine will the socket be bound to
 * with wget_tcp_set_bind_address(). Otherwise the socket will bind to any address and port
 * chosen by the operating system.
 *
 * This function will try to use TCP Fast Open if enabled and available. If TCP Fast Open fails,
 * it will fall back to the normal TCP handshake, without raising an error. You can enable TCP Fast Open
 * with wget_tcp_set_tcp_fastopen().
 *
 * If the connection fails, `WGET_E_CONNECT` is returned.
 */
int wget_tcp_connect(wget_tcp_t *tcp, const char *host, uint16_t port)
{
	struct addrinfo *ai;
	int rc, ret = WGET_E_UNKNOWN;
	char adr[NI_MAXHOST], s_port[NI_MAXSERV];
	int debug = wget_logger_is_active(wget_get_logger(WGET_LOGGER_DEBUG));

	if (unlikely(!tcp))
		return WGET_E_INVALID;

	if (tcp->addrinfo_allocated)
		freeaddrinfo(tcp->addrinfo);

	tcp->addrinfo = wget_tcp_resolve(tcp, host, port);

	tcp->addrinfo_allocated = !tcp->caching;

	for (ai = tcp->addrinfo; ai; ai = ai->ai_next) {
		if (debug) {
			rc = getnameinfo(ai->ai_addr, ai->ai_addrlen,
					adr, sizeof(adr),
					s_port, sizeof(s_port),
					NI_NUMERICHOST | NI_NUMERICSERV);
			if (rc == 0)
				debug_printf("trying %s:%s...\n", adr, s_port);
			else
				debug_printf("trying ???:%s (%s)...\n", s_port, gai_strerror(rc));
		}

		int sockfd;
		if ((sockfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) != -1) {
			_set_async(sockfd);
			_set_socket_options(sockfd);

			if (tcp->bind_addrinfo) {
				if (debug) {
					rc = getnameinfo(tcp->bind_addrinfo->ai_addr,
							tcp->bind_addrinfo->ai_addrlen,
							adr, sizeof(adr),
							s_port, sizeof(s_port),
							NI_NUMERICHOST | NI_NUMERICSERV);
					if (rc == 0)
						debug_printf("binding to %s:%s...\n", adr, s_port);
					else
						debug_printf("binding to ???:%s (%s)...\n", s_port, gai_strerror(rc));
				}

				if (bind(sockfd, tcp->bind_addrinfo->ai_addr, tcp->bind_addrinfo->ai_addrlen) != 0) {
					error_printf(_("Failed to bind (%d)\n"), errno);
					close(sockfd);

					return WGET_E_UNKNOWN;
				}
			}

			/* Enable TCP Fast Open, if required by the user and available */
			if (tcp->tcp_fastopen) {
#ifdef TCP_FASTOPEN_OSX
				sa_endpoints_t endpoints = { .sae_dstaddr = ai->ai_addr, .sae_dstaddrlen = ai->ai_addrlen };
				rc = connectx(sockfd, &endpoints, SAE_ASSOCID_ANY, CONNECT_RESUME_ON_READ_WRITE | CONNECT_DATA_IDEMPOTENT, NULL, 0, NULL, NULL);
				tcp->first_send = 0;
#else
				rc = 0;
				errno = 0;
				tcp->connect_addrinfo = ai;
				tcp->first_send = 1;
#endif
			} else {
				rc = connect(sockfd, ai->ai_addr, ai->ai_addrlen);
				tcp->first_send = 0;
			}

			if (rc < 0
				&& errno != EAGAIN
				&& errno != EINPROGRESS
			) {
				error_printf(_("Failed to connect (%d)\n"), errno);
				ret = WGET_E_CONNECT;
				close(sockfd);
			} else {
				tcp->sockfd = sockfd;
				if (tcp->ssl) {
					if ((ret = wget_ssl_open(tcp))) {
						if (ret == WGET_E_CERTIFICATE) {
							wget_tcp_close(tcp);
							break; /* stop here - the server cert couldn't be validated */
						}

						/* do not free tcp->addrinfo when calling wget_tcp_close() */
						struct addrinfo *ai_tmp = tcp->addrinfo;

						tcp->addrinfo = NULL;
						wget_tcp_close(tcp);
						tcp->addrinfo = ai_tmp;

						continue;
					}
				}

				if ((rc = getnameinfo(ai->ai_addr, ai->ai_addrlen, adr, sizeof(adr), s_port, sizeof(s_port), NI_NUMERICHOST | NI_NUMERICSERV)) == 0)
					tcp->ip = wget_strdup(adr);
				else
					tcp->ip = wget_strdup("???");

				return WGET_E_SUCCESS;
			}
		} else
			error_printf(_("Failed to create socket (%d)\n"), errno);
	}

	return ret;
}

/**
 * \param[in] tcp An active connection.
 * \return WGET_E_SUCCESS (0) on success, or a negative integer on error (one of WGET_E_XXX, defined in `<wget.h>`).
 * Start TLS for this connection.
 *
 * This will typically be called by wget_tcp_accept().
 *
 * If the socket is listening (e.g. wget_tcp_listen(), wget_tcp_accept()), it will expect the client to perform a TLS handshake,
 * and fail if it doesn't.
 *
 * If this is a client connection (e.g. wget_tcp_connect()), it will try perform a TLS handshake with the server.
 */
int wget_tcp_tls_start(wget_tcp_t *tcp)
{
	return wget_ssl_open(tcp);
}

/**
 * \param[in] tcp An active connection.
 *
 * Stops TLS, but does not close the connection. Data will be transmitted in the clear from now on.
 */
void wget_tcp_tls_stop(wget_tcp_t *tcp)
{
	if (tcp)
		wget_ssl_close(&tcp->ssl_session);
}

/**
 * \param[in] tcp An active TCP connection.
 * \param[in] buf Destination buffer, at least \p count bytes long.
 * \param[in] count Length of the buffer \p buf.
 * \return Number of bytes read
 *
 * Read \p count bytes of data from the TCP connection represented by \p tcp
 * and store them in the buffer \p buf.
 *
 * This function knows whether the provided connection is over TLS or not
 * and it will do the right thing.
 *
 * The `tcp->timeout` parameter is taken into account by this function as well.
 * It specifies how long should this function wait until there's data available
 * to read (in milliseconds). The default timeout is -1, which means to wait indefinitely.
 *
 * The following two values are special:
 *
 *  - `0`: No timeout, immediate.
 *  - `-1`: Infinite timeout. Wait indefinitely until a new connection comes.
 *
 * You can set the timeout with wget_tcp_set_timeout().
 *
 * In particular, the returned value will be zero if no data was available for reading
 * before the timeout elapsed.
 */
ssize_t wget_tcp_read(wget_tcp_t *tcp, char *buf, size_t count)
{
	ssize_t rc;

	if (unlikely(!tcp || !buf))
		return 0;

	if (tcp->ssl_session) {
		rc = wget_ssl_read_timeout(tcp->ssl_session, buf, count, tcp->timeout);
	} else {
		if (tcp->timeout) {
			if ((rc = wget_ready_2_read(tcp->sockfd, tcp->timeout)) <= 0)
				return rc;
		}

		rc = recvfrom(tcp->sockfd, buf, count, 0, NULL, NULL);
	}

	if (rc < 0)
		error_printf(_("Failed to read %zu bytes (%d)\n"), count, errno);

	return rc;
}

/**
 * \param[in] tcp An active TCP connection.
 * \param[in] buf A buffer, at least \p count bytes long.
 * \param[in] count Number of bytes from \p buf to send through \p tcp.
 * \return The number of bytes written, or -1 on error.
 *
 * Write \p count bytes of data from the buffer \p buf to the TCP connection
 * represented by \p tcp.
 *
 * This function knows whether the provided connection is over TLS or not
 * and it will do the right thing.
 *
 * TCP Fast Open will be used if it's available and enabled. You can enable TCP Fast Open
 * with wget_tcp_set_tcp_fastopen().
 *
 * This function honors the `timeout` parameter. If the write operation fails because the socket buffer is full,
 * then it will wait at most that amount of milliseconds. If after the timeout the socket is still unavailable
 * for writing, this function returns zero.
 *
 * The following two values are special:
 *
 *  - `0`: No timeout. The socket must be available immediately.
 *  - `-1`: Infinite timeout. Wait indefinitely until the socket becomes available.
 *
 * You can set the timeout with wget_tcp_set_timeout().
 */
ssize_t wget_tcp_write(wget_tcp_t *tcp, const char *buf, size_t count)
{
	ssize_t nwritten = 0;

	if (unlikely(!tcp || !buf))
		return -1;

	if (tcp->ssl_session)
		return wget_ssl_write_timeout(tcp->ssl_session, buf, count, tcp->timeout);

	while (count) {
		ssize_t n;

#ifdef TCP_FASTOPEN_LINUX
		if (tcp->tcp_fastopen && tcp->first_send) {
			n = sendto(tcp->sockfd, buf, count, MSG_FASTOPEN,
				tcp->connect_addrinfo->ai_addr, tcp->connect_addrinfo->ai_addrlen);
			tcp->first_send = 0;

			if (n < 0 && errno == EOPNOTSUPP) {
				/* fallback from fastopen, e.g. when fastopen is disabled in system */
				tcp->tcp_fastopen = 0;

				int rc = connect(tcp->sockfd, tcp->connect_addrinfo->ai_addr, tcp->connect_addrinfo->ai_addrlen);
				if (rc < 0
					&& errno != EAGAIN
					&& errno != ENOTCONN
					&& errno != EINPROGRESS)
				{
					error_printf(_("Failed to connect (%d)\n"), errno);
					return -1;
				}
				errno = EAGAIN;
			}
		} else
#endif
			n = send(tcp->sockfd, buf, count, 0);

		if (n >= 0) {
			nwritten += n;

			if ((size_t)n >= count)
				return nwritten;

			count -= n;
			buf += n;
		} else {
			if (errno != EAGAIN
				&& errno != ENOTCONN
				&& errno != EINPROGRESS
			) {
				error_printf(_("Failed to write %zu bytes (%d: %s)\n"), count, errno, strerror(errno));
				return -1;
			}

			if (tcp->timeout) {
				int rc = wget_ready_2_write(tcp->sockfd, tcp->timeout);
				if (rc <= 0)
					return rc;
			}
		}
	}

	return 0;
}

/**
 * \param[in] tcp An active TCP connection.
 * \param[in] fmt Format string (like in `printf(3)`).
 * \param[in] args `va_args` argument list (like in `vprintf(3)`)
 *
 * Write data in vprintf-style format, to the connection \p tcp.
 *
 * It uses wget_tcp_write().
 */
ssize_t wget_tcp_vprintf(wget_tcp_t *tcp, const char *fmt, va_list args)
{
	char sbuf[4096];
	wget_buffer_t buf;
	ssize_t len2;

	wget_buffer_init(&buf, sbuf, sizeof(sbuf));
	wget_buffer_vprintf(&buf, fmt, args);

	len2 = wget_tcp_write(tcp, buf.data, buf.length);

	wget_buffer_deinit(&buf);

	if (len2 > 0)
		debug_write(buf.data, len2);

	if (len2 > 0 && (ssize_t) buf.length != len2)
		error_printf(_("%s: internal error: length mismatch %zu != %zd\n"), __func__, buf.length, len2);

	return len2;
}

/**
 * \param[in] tcp An active TCP connection.
 * \param[in] fmt Format string (like in `printf(3)`).
 *
 * Write data in printf-style format, to the connection \p tcp.
 *
 * It uses wget_tcp_vprintf(), which in turn uses wget_tcp_write().
 */
ssize_t wget_tcp_printf(wget_tcp_t *tcp, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	ssize_t len = wget_tcp_vprintf(tcp, fmt, args);
	va_end(args);

	return len;
}

/**
 * \param[in] tcp An active TCP connection
 *
 * Close a TCP connection.
 */
void wget_tcp_close(wget_tcp_t *tcp)
{
	if (likely(tcp)) {
		wget_tcp_tls_stop(tcp);
		if (tcp->sockfd != -1) {
			close(tcp->sockfd);
			tcp->sockfd = -1;
		}
		if (tcp->addrinfo_allocated) {
			freeaddrinfo(tcp->addrinfo);
		}
		tcp->addrinfo = NULL;
	}
}
/** @} */
