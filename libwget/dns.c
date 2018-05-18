/*
 * Copyright(c) 2018 Aniketh Gireesh
 * Copyright(c) 2015-2018 Free Software Foundation, Inc.
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
 * DNS query routines, with DNS resolvers of various kinds
 *
 * Changelog
 * 18.05.2018  Ander Juaristi created
 *
 */
#include <config.h>
#include <wget.h>
#include <netdb.h>

/**
 * \file
 * \brief Functions to send DNS queries, supporting DNS resolvers of various kinds
 * \defgroup libwget-dns DNS resolvers
 *
 * @{
 *
 */

struct wget_dns_st {
	int
		resolver,
		family,
		timeout;
	const char *
		doh_hostname;
};

/**
 * \param[in] dns A pointer to an uninitialized `wget_dns_t` structure.
 * \param[in] tcp An initialized `wget_tcp_t` structure (optional, can be NULL).
 *
 * Create a new DNS context.
 *
 * If \p tcp is given, the DNS context will be configured with the values taken
 * from that TCP connection. The DNS context's family and timeout will be those specified
 * in the given TCP connection. These values correspond to the `WGET_DNS_ADDR_FAMILY` and
 * `WGET_DNS_TIMEOUT`.
 *
 * If no \p tcp is given, then the new DNS context will be initialized with default configuration values.
 * These can be changed at any time with wget_dns_set_config_int().
 *
 * The new DNS context will use the standard getaddrinfo(3) resolver by default. This can later be changed
 * with wget_dns_set_config_int() and wget_dns_set_config_string().
 */
void wget_dns_init(wget_dns_t *dns, wget_tcp_t *tcp)
{
	/* TODO implement */
}

/**
 * \param[in] dns A pointer to a `wget_dns_t` structure.
 *
 * Delete a DNS context.
 *
 * Delete the DNS context previously created with wget_dns_init(), and set the given
 * pointer to NULL.
 */
void wget_dns_deinit(wget_dns_t *dns)
{
	/* TODO implement */
}

/**
 * \param[in] dns A DNS context
 * \param[in] key An identifier for the config parameter (starting with `WGET_DNS_`)
 * \param[in] value The value for the config parameter
 *
 * Set a configuration parameter, as an integer.
 *
 * A list of available parameters follows (possible values for \p key).
 *
 *  - WGET_DNS_TIMEOUT: sets the request timeout, in milliseconds. This is the maximum time
 *  wget_dns_resolve() will wait for a DNS query to complete. This might have the value zero (0),
 *  which will cause wget_dns_resolve() to return immediately, or a negative value which will cause it
 *  to wait indefinitely (until the response arrives or the thread is interrupted).
 *  - WGET_DNS_ADDR_FAMILY: sets the preferred address family. This is the address family wget_dns_resolve()
 *  will favor above the others, when more than one address families are returned for the query. This will
 *  typically be `AF_INET` or `AF_INET6`, but it can be any of the values defined in `<socket.h>`. Additionally,
 *  `AF_UNSPEC` means you don't care.
 *  - WGET_DNS_RESOLVER: sets the resolver that will be used. The list that follows describes the available
 *  resolvers.
 *
 * Currently the following DNS resolvers are supported:
 *
 *  - WGET_DNS_RESOLVER_DOH: DNS-over-HTTPS resolver, speaking the protocol defined in draft-ietf-doh-dns-over-https-07*.
 *  This requres a hostname or IP address that must be set with the WGET_DNS_RESOLVER_DOH_HOSTNAME option
 *  by calling wget_dns_set_config_string().
 *  - WGET_DNS_RESOLVER_GETADDRINFO: A standard resolver using the getaddrinfo(3) system call.
 *
 * * https://tools.ietf.org/html/draft-ietf-doh-dns-over-https-07
 */
void wget_dns_set_config_int(wget_dns_t dns, int key int value)
{
	switch (key) {
	case WGET_DNS_ADDR_FAMILY:
		dns->family = value;
		break;
	case WGET_DNS_TIMEOUT:
		dns->timeout = value;
		break;
	case WGET_DNS_RESOLVER:
		if (value == WGET_DNS_RESOLVER_DOH || value == WGET_DNS_RESOLVER_GETADDRINFO)
			dns->value = value;
		else
			error_printf(_("Invalid value for config key WGET_DNS_RESOLVER (%d)\n"), value);
		break;
	default:
		error_printf(_("Unknown config key %d\n"), key);
		break;
	}
}

/**
 * \param[in] dns A DNS context
 * \param[in] key An identifier for the config parameter
 * \oaram[in] value The value for the config parameter
 *
 * The only available parameter currently is WGET_DNS_RESOLVER_DOH_HOSTNAME, which sets
 * the target server's hostname or IP address for a DoH query.
 */
void wget_dns_set_config_string(wget_dns_t dns, int key, const char *value)
{
	if (key == WGET_DNS_RESOLVER_DOH_HOSTNAME)
		dns->doh_hostname = value;
	else
		error_printf(_("Unknown config key %d\n"), key);
}

/**
 * \param[in] dns A DNS context
 * \param[in] host Hostname
 * \param[in] port TCP destination port
 * \param[in] out_addr A pointer to an `addrinfo` structure that will hold the result
 * \return The number of items in `addrinfo` on success; a negative number on error
 *
 * Resolve a host name into its IPv4/IPv6 address.
 *
 * A new `addrinfo` structure will be allocated and the result will be placed there.
 * **The caller is responsible for freeing it.**
 *
 * This function will honor the configuration parameters set in the DNS context
 * previously with wget_dns_set_config_int() and wget_dns_set_config_string().
 *
 * The `addrinfo` structure is a linked list that may contain more than one addresses
 * for the queried host name. The addresses in the list will be sorted according the preferred
 * family that was specified, if any.
 *
 * If the preferred family configuration parameter was set, all addresses with that family
 * will come first in the list, and other families will follow. For example, if `AF_INET` was set
 * as the preferred family, all IPv4 addresses returned by the query will come first in the list,
 * and any IPv6 addresses will come after them, if any were returned.
 */
int wget_dns_resolve(wget_dns_t dns,
		     const char *host, uint16_t port,
		     struct addrinfo **out_addr)
{
	/* TODO implement */
}

/** @} */

