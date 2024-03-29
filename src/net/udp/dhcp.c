/*
 * Copyright (C) 2006 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <byteswap.h>
#include <ipxe/if_ether.h>
#include <ipxe/iobuf.h>
#include <ipxe/netdevice.h>
#include <ipxe/device.h>
#include <ipxe/xfer.h>
#include <ipxe/open.h>
#include <ipxe/job.h>
#include <ipxe/retry.h>
#include <ipxe/tcpip.h>
#include <ipxe/ip.h>
#include <ipxe/uuid.h>
#include <ipxe/timer.h>
#include <ipxe/settings.h>
#include <ipxe/dhcp.h>
#include <ipxe/dhcpopts.h>
#include <ipxe/dhcppkt.h>
#include <ipxe/dhcp_arch.h>
#include <ipxe/features.h>

/** @file
 *
 * Dynamic Host Configuration Protocol
 *
 */

struct dhcp_session;
static int dhcp_tx ( struct dhcp_session *dhcp );

/**
 * DHCP operation types
 *
 * This table maps from DHCP message types (i.e. values of the @c
 * DHCP_MESSAGE_TYPE option) to values of the "op" field within a DHCP
 * packet.
 */
static const uint8_t dhcp_op[] = {
	[DHCPDISCOVER]	= BOOTP_REQUEST,
	[DHCPOFFER]	= BOOTP_REPLY,
	[DHCPREQUEST]	= BOOTP_REQUEST,
	[DHCPDECLINE]	= BOOTP_REQUEST,
	[DHCPACK]	= BOOTP_REPLY,
	[DHCPNAK]	= BOOTP_REPLY,
	[DHCPRELEASE]	= BOOTP_REQUEST,
	[DHCPINFORM]	= BOOTP_REQUEST,
};

/** Raw option data for options common to all DHCP requests */
static uint8_t dhcp_request_options_data[] = {
	DHCP_MESSAGE_TYPE, DHCP_BYTE ( 0 ),
	DHCP_MAX_MESSAGE_SIZE,
	DHCP_WORD ( ETH_MAX_MTU - 20 /* IP header */ - 8 /* UDP header */ ),
	DHCP_CLIENT_ARCHITECTURE, DHCP_ARCH_CLIENT_ARCHITECTURE,
	DHCP_CLIENT_NDI, DHCP_ARCH_CLIENT_NDI,
	DHCP_VENDOR_CLASS_ID, DHCP_ARCH_VENDOR_CLASS_ID,
	DHCP_USER_CLASS_ID, DHCP_STRING ( 'x', 'N', 'B', 'A' ),
	DHCP_PARAMETER_REQUEST_LIST,
	DHCP_OPTION ( DHCP_SUBNET_MASK, DHCP_ROUTERS, DHCP_DNS_SERVERS,
		      DHCP_LOG_SERVERS, DHCP_HOST_NAME, DHCP_DOMAIN_NAME,
		      DHCP_ROOT_PATH, DHCP_VENDOR_ENCAP, DHCP_VENDOR_CLASS_ID,
		      DHCP_TFTP_SERVER_NAME, DHCP_BOOTFILE_NAME,
		      DHCP_EB_ENCAP, DHCP_ISCSI_INITIATOR_IQN ),
	DHCP_END
};

/** Version number feature */
FEATURE_VERSION ( VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH );

/** DHCP server address setting */
struct setting dhcp_server_setting __setting ( SETTING_MISC ) = {
	.name = "dhcp-server",
	.description = "DHCP server",
	.tag = DHCP_SERVER_IDENTIFIER,
	.type = &setting_type_ipv4,
};

/** DHCP user class setting */
struct setting user_class_setting __setting ( SETTING_HOST_EXTRA ) = {
	.name = "user-class",
	.description = "DHCP user class",
	.tag = DHCP_USER_CLASS_ID,
	.type = &setting_type_string,
};

/** Use cached network settings */
struct setting use_cached_setting __setting ( SETTING_MISC ) = {
	.name = "use-cached",
	.description = "Use cached settings",
	.tag = DHCP_EB_USE_CACHED,
	.type = &setting_type_uint8,
};

/**
 * Most recent DHCP transaction ID
 *
 * This is exposed for use by the fakedhcp code when reconstructing
 * DHCP packets for PXE NBPs.
 */
uint32_t dhcp_last_xid;

/**
 * Name a DHCP packet type
 *
 * @v msgtype		DHCP message type
 * @ret string		DHCP mesasge type name
 */
static inline const char * dhcp_msgtype_name ( unsigned int msgtype ) {
	switch ( msgtype ) {
	case DHCPNONE:		return "BOOTP"; /* Non-DHCP packet */
	case DHCPDISCOVER:	return "DHCPDISCOVER";
	case DHCPOFFER:		return "DHCPOFFER";
	case DHCPREQUEST:	return "DHCPREQUEST";
	case DHCPDECLINE:	return "DHCPDECLINE";
	case DHCPACK:		return "DHCPACK";
	case DHCPNAK:		return "DHCPNAK";
	case DHCPRELEASE:	return "DHCPRELEASE";
	case DHCPINFORM:	return "DHCPINFORM";
	default:		return "DHCP<invalid>";
	}
}

/****************************************************************************
 *
 * DHCP session
 *
 */

struct dhcp_session;

/** DHCP session state operations */
struct dhcp_session_state {
	/** State name */
	const char *name;
	/**
	 * Construct transmitted packet
	 *
	 * @v dhcp		DHCP session
	 * @v dhcppkt		DHCP packet
	 * @v peer		Destination address
	 */
	int ( * tx ) ( struct dhcp_session *dhcp,
		       struct dhcp_packet *dhcppkt,
		       struct sockaddr_in *peer );
	/** Handle received packet
	 *
	 * @v dhcp		DHCP session
	 * @v dhcppkt		DHCP packet
	 * @v peer		DHCP server address
	 * @v msgtype		DHCP message type
	 * @v server_id		DHCP server ID
	 */
	void ( * rx ) ( struct dhcp_session *dhcp,
			struct dhcp_packet *dhcppkt,
			struct sockaddr_in *peer,
			uint8_t msgtype, struct in_addr server_id );
	/** Handle timer expiry
	 *
	 * @v dhcp		DHCP session
	 */
	void ( * expired ) ( struct dhcp_session *dhcp );
	/** Transmitted message type */
	uint8_t tx_msgtype;
	/** Apply minimum timeout */
	uint8_t apply_min_timeout;
};

static struct dhcp_session_state dhcp_state_discover;
static struct dhcp_session_state dhcp_state_request;
static struct dhcp_session_state dhcp_state_proxy;
static struct dhcp_session_state dhcp_state_pxebs;

/** A DHCP session */
struct dhcp_session {
	/** Reference counter */
	struct refcnt refcnt;
	/** Job control interface */
	struct interface job;
	/** Data transfer interface */
	struct interface xfer;

	/** Network device being configured */
	struct net_device *netdev;
	/** Local socket address */
	struct sockaddr_in local;
	/** State of the session */
	struct dhcp_session_state *state;
	/** Transaction ID (in network-endian order) */
	uint32_t xid;

	/** Offered IP address */
	struct in_addr offer;
	/** DHCP server */
	struct in_addr server;
	/** DHCP offer priority */
	int priority;

	/** ProxyDHCP protocol extensions should be ignored */
	int no_pxedhcp;
	/** ProxyDHCP server */
	struct in_addr proxy_server;
	/** ProxyDHCP offer */
	struct dhcp_packet *proxy_offer;
	/** ProxyDHCP offer priority */
	int proxy_priority;

	/** PXE Boot Server type */
	uint16_t pxe_type;
	/** List of PXE Boot Servers to attempt */
	struct in_addr *pxe_attempt;
	/** List of PXE Boot Servers to accept */
	struct in_addr *pxe_accept;

	/** Retransmission timer */
	struct retry_timer timer;
	/** Transmission counter */
	unsigned int count;
	/** Start time of the current state (in ticks) */
	unsigned long start;
};

/**
 * Free DHCP session
 *
 * @v refcnt		Reference counter
 */
static void dhcp_free ( struct refcnt *refcnt ) {
	struct dhcp_session *dhcp =
		container_of ( refcnt, struct dhcp_session, refcnt );

	netdev_put ( dhcp->netdev );
	dhcppkt_put ( dhcp->proxy_offer );
	free ( dhcp );
}

/**
 * Mark DHCP session as complete
 *
 * @v dhcp		DHCP session
 * @v rc		Return status code
 */
static void dhcp_finished ( struct dhcp_session *dhcp, int rc ) {

	/* Stop retry timer */
	stop_timer ( &dhcp->timer );

	/* Shut down interfaces */
	intf_shutdown ( &dhcp->xfer, rc );
	intf_shutdown ( &dhcp->job, rc );
}

/**
 * Transition to new DHCP session state
 *
 * @v dhcp		DHCP session
 * @v state		New session state
 */
static void dhcp_set_state ( struct dhcp_session *dhcp,
			     struct dhcp_session_state *state ) {

	DBGC ( dhcp, "DHCP %p entering %s state\n", dhcp, state->name );
	dhcp->state = state;
	dhcp->start = currticks();
	stop_timer ( &dhcp->timer );
	dhcp->timer.min_timeout =
		( state->apply_min_timeout ? DHCP_MIN_TIMEOUT : 0 );
	dhcp->timer.max_timeout = DHCP_MAX_TIMEOUT;
	start_timer_nodelay ( &dhcp->timer );
}

/**
 * Check if DHCP packet contains PXE options
 *
 * @v dhcppkt		DHCP packet
 * @ret has_pxeopts	DHCP packet contains PXE options
 *
 * It is assumed that the packet is already known to contain option 60
 * set to "PXEClient".
 */
static int dhcp_has_pxeopts ( struct dhcp_packet *dhcppkt ) {

	/* Check for a boot filename */
	if ( dhcppkt_fetch ( dhcppkt, DHCP_BOOTFILE_NAME, NULL, 0 ) > 0 )
		return 1;

	/* Check for a PXE boot menu */
	if ( dhcppkt_fetch ( dhcppkt, DHCP_PXE_BOOT_MENU, NULL, 0 ) > 0 )
		return 1;

	return 0;
}

/****************************************************************************
 *
 * DHCP state machine
 *
 */

/**
 * Construct transmitted packet for DHCP discovery
 *
 * @v dhcp		DHCP session
 * @v dhcppkt		DHCP packet
 * @v peer		Destination address
 */
static int dhcp_discovery_tx ( struct dhcp_session *dhcp,
			       struct dhcp_packet *dhcppkt __unused,
			       struct sockaddr_in *peer ) {

	DBGC ( dhcp, "DHCP %p DHCPDISCOVER\n", dhcp );

	/* Set server address */
	peer->sin_addr.s_addr = INADDR_BROADCAST;
	peer->sin_port = htons ( BOOTPS_PORT );

	return 0;
}

/**
 * Handle received packet during DHCP discovery
 *
 * @v dhcp		DHCP session
 * @v dhcppkt		DHCP packet
 * @v peer		DHCP server address
 * @v msgtype		DHCP message type
 * @v server_id		DHCP server ID
 */
static void dhcp_discovery_rx ( struct dhcp_session *dhcp,
				struct dhcp_packet *dhcppkt,
				struct sockaddr_in *peer, uint8_t msgtype,
				struct in_addr server_id ) {
	struct in_addr ip;
	char vci[9]; /* "PXEClient" */
	int vci_len;
	int has_pxeclient;
	int8_t priority = 0;
	uint8_t no_pxedhcp = 0;
	unsigned long elapsed;

	DBGC ( dhcp, "DHCP %p %s from %s:%d", dhcp,
	       dhcp_msgtype_name ( msgtype ), inet_ntoa ( peer->sin_addr ),
	       ntohs ( peer->sin_port ) );
	if ( server_id.s_addr != peer->sin_addr.s_addr )
		DBGC ( dhcp, " (%s)", inet_ntoa ( server_id ) );

	/* Identify offered IP address */
	ip = dhcppkt->dhcphdr->yiaddr;
	if ( ip.s_addr )
		DBGC ( dhcp, " for %s", inet_ntoa ( ip ) );

	/* Identify "PXEClient" vendor class */
	vci_len = dhcppkt_fetch ( dhcppkt, DHCP_VENDOR_CLASS_ID,
				  vci, sizeof ( vci ) );
	has_pxeclient = ( ( vci_len >= ( int ) sizeof ( vci ) ) &&
			  ( strncmp ( "PXEClient", vci, sizeof (vci) ) == 0 ));
	if ( has_pxeclient ) {
		DBGC ( dhcp, "%s",
		       ( dhcp_has_pxeopts ( dhcppkt ) ? " pxe" : " proxy" ) );
	}

	/* Identify priority */
	dhcppkt_fetch ( dhcppkt, DHCP_EB_PRIORITY, &priority,
			sizeof ( priority ) );
	if ( priority )
		DBGC ( dhcp, " pri %d", priority );

	/* Identify ignore-PXE flag */
	dhcppkt_fetch ( dhcppkt, DHCP_EB_NO_PXEDHCP, &no_pxedhcp,
			sizeof ( no_pxedhcp ) );
	if ( no_pxedhcp )
		DBGC ( dhcp, " nopxe" );
	DBGC ( dhcp, "\n" );

	/* Select as DHCP offer, if applicable */
	if ( ip.s_addr && ( peer->sin_port == htons ( BOOTPS_PORT ) ) &&
	     ( ( msgtype == DHCPOFFER ) || ( ! msgtype /* BOOTP */ ) ) &&
	     ( priority >= dhcp->priority ) ) {
		dhcp->offer = ip;
		dhcp->server = server_id;
		dhcp->priority = priority;
		dhcp->no_pxedhcp = no_pxedhcp;
	}

	/* Select as ProxyDHCP offer, if applicable */
	if ( server_id.s_addr && has_pxeclient &&
	     ( priority >= dhcp->proxy_priority ) ) {
		dhcppkt_put ( dhcp->proxy_offer );
		dhcp->proxy_server = server_id;
		dhcp->proxy_offer = dhcppkt_get ( dhcppkt );
		dhcp->proxy_priority = priority;
	}

	/* We can exit the discovery state when we have a valid
	 * DHCPOFFER, and either:
	 *
	 *  o  The DHCPOFFER instructs us to ignore ProxyDHCPOFFERs, or
	 *  o  We have a valid ProxyDHCPOFFER, or
	 *  o  We have allowed sufficient time for ProxyDHCPOFFERs.
	 */

	/* If we don't yet have a DHCPOFFER, do nothing */
	if ( ! dhcp->offer.s_addr )
		return;

	/* If we can't yet transition to DHCPREQUEST, do nothing */
	elapsed = ( currticks() - dhcp->start );
	if ( ! ( dhcp->no_pxedhcp || dhcp->proxy_offer ||
		 ( elapsed > PROXYDHCP_MAX_TIMEOUT ) ) )
		return;

	/* Transition to DHCPREQUEST */
	dhcp_set_state ( dhcp, &dhcp_state_request );
}

/**
 * Handle timer expiry during DHCP discovery
 *
 * @v dhcp		DHCP session
 */
static void dhcp_discovery_expired ( struct dhcp_session *dhcp ) {
	unsigned long elapsed = ( currticks() - dhcp->start );

	/* Give up waiting for ProxyDHCP before we reach the failure point */
	if ( dhcp->offer.s_addr && ( elapsed > PROXYDHCP_MAX_TIMEOUT ) ) {
		dhcp_set_state ( dhcp, &dhcp_state_request );
		return;
	}

	/* Otherwise, retransmit current packet */
	dhcp_tx ( dhcp );
}

/** DHCP discovery state operations */
static struct dhcp_session_state dhcp_state_discover = {
	.name			= "discovery",
	.tx			= dhcp_discovery_tx,
	.rx			= dhcp_discovery_rx,
	.expired		= dhcp_discovery_expired,
	.tx_msgtype		= DHCPDISCOVER,
	.apply_min_timeout	= 1,
};

/**
 * Construct transmitted packet for DHCP request
 *
 * @v dhcp		DHCP session
 * @v dhcppkt		DHCP packet
 * @v peer		Destination address
 */
static int dhcp_request_tx ( struct dhcp_session *dhcp,
			     struct dhcp_packet *dhcppkt,
			     struct sockaddr_in *peer ) {
	int rc;

	DBGC ( dhcp, "DHCP %p DHCPREQUEST to %s:%d",
	       dhcp, inet_ntoa ( dhcp->server ), BOOTPS_PORT );
	DBGC ( dhcp, " for %s\n", inet_ntoa ( dhcp->offer ) );

	/* Set server ID */
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_SERVER_IDENTIFIER,
				    &dhcp->server,
				    sizeof ( dhcp->server ) ) ) != 0 )
		return rc;

	/* Set requested IP address */
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_REQUESTED_ADDRESS,
				    &dhcp->offer,
				    sizeof ( dhcp->offer ) ) ) != 0 )
		return rc;

	/* Set server address */
	peer->sin_addr.s_addr = INADDR_BROADCAST;
	peer->sin_port = htons ( BOOTPS_PORT );

	return 0;
}

/**
 * Handle received packet during DHCP request
 *
 * @v dhcp		DHCP session
 * @v dhcppkt		DHCP packet
 * @v peer		DHCP server address
 * @v msgtype		DHCP message type
 * @v server_id		DHCP server ID
 */
static void dhcp_request_rx ( struct dhcp_session *dhcp,
			      struct dhcp_packet *dhcppkt,
			      struct sockaddr_in *peer, uint8_t msgtype,
			      struct in_addr server_id ) {
	struct in_addr ip;
	struct settings *parent;
	struct settings *settings;
	int rc;

	DBGC ( dhcp, "DHCP %p %s from %s:%d", dhcp,
	       dhcp_msgtype_name ( msgtype ), inet_ntoa ( peer->sin_addr ),
	       ntohs ( peer->sin_port ) );
	if ( server_id.s_addr != peer->sin_addr.s_addr )
		DBGC ( dhcp, " (%s)", inet_ntoa ( server_id ) );

	/* Identify leased IP address */
	ip = dhcppkt->dhcphdr->yiaddr;
	if ( ip.s_addr )
		DBGC ( dhcp, " for %s", inet_ntoa ( ip ) );
	DBGC ( dhcp, "\n" );

	/* Filter out unacceptable responses */
	if ( peer->sin_port != htons ( BOOTPS_PORT ) )
		return;
	if ( msgtype /* BOOTP */ && ( msgtype != DHCPACK ) )
		return;
	if ( server_id.s_addr != dhcp->server.s_addr )
		return;
	if ( ip.s_addr != dhcp->offer.s_addr )
		return;

	/* Record assigned address */
	dhcp->local.sin_addr = ip;

	/* Register settings */
	parent = netdev_settings ( dhcp->netdev );
	settings = &dhcppkt->settings;
	if ( ( rc = register_settings ( settings, parent,
					DHCP_SETTINGS_NAME ) ) != 0 ) {
		DBGC ( dhcp, "DHCP %p could not register settings: %s\n",
		       dhcp, strerror ( rc ) );
		dhcp_finished ( dhcp, rc );
		return;
	}

	/* Perform ProxyDHCP if applicable */
	if ( dhcp->proxy_offer /* Have ProxyDHCP offer */ &&
	     ( ! dhcp->no_pxedhcp ) /* ProxyDHCP not disabled */ ) {
		if ( dhcp_has_pxeopts ( dhcp->proxy_offer ) ) {
			/* PXE options already present; register settings
			 * without performing a ProxyDHCPREQUEST
			 */
			settings = &dhcp->proxy_offer->settings;
			if ( ( rc = register_settings ( settings, NULL,
					   PROXYDHCP_SETTINGS_NAME ) ) != 0 ) {
				DBGC ( dhcp, "DHCP %p could not register "
				       "proxy settings: %s\n",
				       dhcp, strerror ( rc ) );
				dhcp_finished ( dhcp, rc );
				return;
			}
		} else {
			/* PXE options not present; use a ProxyDHCPREQUEST */
			dhcp_set_state ( dhcp, &dhcp_state_proxy );
			return;
		}
	}

	/* Terminate DHCP */
	dhcp_finished ( dhcp, 0 );
}

/**
 * Handle timer expiry during DHCP discovery
 *
 * @v dhcp		DHCP session
 */
static void dhcp_request_expired ( struct dhcp_session *dhcp ) {

	/* Retransmit current packet */
	dhcp_tx ( dhcp );
}

/** DHCP request state operations */
static struct dhcp_session_state dhcp_state_request = {
	.name			= "request",
	.tx			= dhcp_request_tx,
	.rx			= dhcp_request_rx,
	.expired		= dhcp_request_expired,
	.tx_msgtype		= DHCPREQUEST,
	.apply_min_timeout	= 0,
};

/**
 * Construct transmitted packet for ProxyDHCP request
 *
 * @v dhcp		DHCP session
 * @v dhcppkt		DHCP packet
 * @v peer		Destination address
 */
static int dhcp_proxy_tx ( struct dhcp_session *dhcp,
			   struct dhcp_packet *dhcppkt,
			   struct sockaddr_in *peer ) {
	int rc;

	DBGC ( dhcp, "DHCP %p ProxyDHCP REQUEST to %s\n", dhcp,
	       inet_ntoa ( dhcp->proxy_server ) );

	/* Set server ID */
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_SERVER_IDENTIFIER,
				    &dhcp->proxy_server,
				    sizeof ( dhcp->proxy_server ) ) ) != 0 )
		return rc;

	/* Set server address */
	peer->sin_addr = dhcp->proxy_server;
	peer->sin_port = htons ( PXE_PORT );

	return 0;
}

/**
 * Handle received packet during ProxyDHCP request
 *
 * @v dhcp		DHCP session
 * @v dhcppkt		DHCP packet
 * @v peer		DHCP server address
 * @v msgtype		DHCP message type
 * @v server_id		DHCP server ID
 */
static void dhcp_proxy_rx ( struct dhcp_session *dhcp,
			    struct dhcp_packet *dhcppkt,
			    struct sockaddr_in *peer, uint8_t msgtype,
			    struct in_addr server_id ) {
	struct settings *settings = &dhcppkt->settings;
	int rc;

	DBGC ( dhcp, "DHCP %p %s from %s:%d", dhcp,
	       dhcp_msgtype_name ( msgtype ), inet_ntoa ( peer->sin_addr ),
	       ntohs ( peer->sin_port ) );
	if ( server_id.s_addr != peer->sin_addr.s_addr )
		DBGC ( dhcp, " (%s)", inet_ntoa ( server_id ) );
	DBGC ( dhcp, "\n" );

	/* Filter out unacceptable responses */
	if ( peer->sin_port != ntohs ( PXE_PORT ) )
		return;
	if ( ( msgtype != DHCPOFFER ) && ( msgtype != DHCPACK ) )
		return;
	if ( server_id.s_addr /* Linux PXE server omits server ID */ &&
	     ( server_id.s_addr != dhcp->proxy_server.s_addr ) )
		return;

	/* Register settings */
	if ( ( rc = register_settings ( settings, NULL,
					PROXYDHCP_SETTINGS_NAME ) ) != 0 ) {
		DBGC ( dhcp, "DHCP %p could not register proxy settings: %s\n",
		       dhcp, strerror ( rc ) );
		dhcp_finished ( dhcp, rc );
		return;
	}

	/* Terminate DHCP */
	dhcp_finished ( dhcp, 0 );
}

/**
 * Handle timer expiry during ProxyDHCP request
 *
 * @v dhcp		DHCP session
 */
static void dhcp_proxy_expired ( struct dhcp_session *dhcp ) {
	unsigned long elapsed = ( currticks() - dhcp->start );

	/* Give up waiting for ProxyDHCP before we reach the failure point */
	if ( elapsed > PROXYDHCP_MAX_TIMEOUT ) {
		dhcp_finished ( dhcp, 0 );
		return;
	}

	/* Retransmit current packet */
	dhcp_tx ( dhcp );
}

/** ProxyDHCP request state operations */
static struct dhcp_session_state dhcp_state_proxy = {
	.name			= "ProxyDHCP",
	.tx			= dhcp_proxy_tx,
	.rx			= dhcp_proxy_rx,
	.expired		= dhcp_proxy_expired,
	.tx_msgtype		= DHCPREQUEST,
	.apply_min_timeout	= 0,
};

/**
 * Construct transmitted packet for PXE Boot Server Discovery
 *
 * @v dhcp		DHCP session
 * @v dhcppkt		DHCP packet
 * @v peer		Destination address
 */
static int dhcp_pxebs_tx ( struct dhcp_session *dhcp,
			   struct dhcp_packet *dhcppkt,
			   struct sockaddr_in *peer ) {
	struct dhcp_pxe_boot_menu_item menu_item = { 0, 0 };
	int rc;

	/* Set server address */
	peer->sin_addr = *(dhcp->pxe_attempt);
	peer->sin_port = ( ( peer->sin_addr.s_addr == INADDR_BROADCAST ) ?
			   htons ( BOOTPS_PORT ) : htons ( PXE_PORT ) );

	DBGC ( dhcp, "DHCP %p PXEBS REQUEST to %s:%d for type %d\n",
	       dhcp, inet_ntoa ( peer->sin_addr ), ntohs ( peer->sin_port ),
	       le16_to_cpu ( dhcp->pxe_type ) );

	/* Set boot menu item */
	menu_item.type = dhcp->pxe_type;
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_PXE_BOOT_MENU_ITEM,
				    &menu_item, sizeof ( menu_item ) ) ) != 0 )
		return rc;

	return 0;
}

/**
 * Check to see if PXE Boot Server address is acceptable
 *
 * @v dhcp		DHCP session
 * @v bs		Boot Server address
 * @ret accept		Boot Server is acceptable
 */
static int dhcp_pxebs_accept ( struct dhcp_session *dhcp,
			       struct in_addr bs ) {
	struct in_addr *accept;

	/* Accept if we have no acceptance filter */
	if ( ! dhcp->pxe_accept )
		return 1;

	/* Scan through acceptance list */
	for ( accept = dhcp->pxe_accept ; accept->s_addr ; accept++ ) {
		if ( accept->s_addr == bs.s_addr )
			return 1;
	}

	DBGC ( dhcp, "DHCP %p rejecting server %s\n",
	       dhcp, inet_ntoa ( bs ) );
	return 0;
}

/**
 * Handle received packet during PXE Boot Server Discovery
 *
 * @v dhcp		DHCP session
 * @v dhcppkt		DHCP packet
 * @v peer		DHCP server address
 * @v msgtype		DHCP message type
 * @v server_id		DHCP server ID
 */
static void dhcp_pxebs_rx ( struct dhcp_session *dhcp,
			    struct dhcp_packet *dhcppkt,
			    struct sockaddr_in *peer, uint8_t msgtype,
			    struct in_addr server_id ) {
	struct dhcp_pxe_boot_menu_item menu_item = { 0, 0 };
	int rc;

	DBGC ( dhcp, "DHCP %p %s from %s:%d", dhcp,
	       dhcp_msgtype_name ( msgtype ), inet_ntoa ( peer->sin_addr ),
	       ntohs ( peer->sin_port ) );
	if ( server_id.s_addr != peer->sin_addr.s_addr )
		DBGC ( dhcp, " (%s)", inet_ntoa ( server_id ) );

	/* Identify boot menu item */
	dhcppkt_fetch ( dhcppkt, DHCP_PXE_BOOT_MENU_ITEM,
			&menu_item, sizeof ( menu_item ) );
	if ( menu_item.type )
		DBGC ( dhcp, " for type %d", ntohs ( menu_item.type ) );
	DBGC ( dhcp, "\n" );

	/* Filter out unacceptable responses */
	if ( ( peer->sin_port != htons ( BOOTPS_PORT ) ) &&
	     ( peer->sin_port != htons ( PXE_PORT ) ) )
		return;
	if ( msgtype != DHCPACK )
		return;
	if ( menu_item.type != dhcp->pxe_type )
		return;
	if ( ! dhcp_pxebs_accept ( dhcp, ( server_id.s_addr ?
					   server_id : peer->sin_addr ) ) )
		return;

	/* Register settings */
	if ( ( rc = register_settings ( &dhcppkt->settings, NULL,
					PXEBS_SETTINGS_NAME ) ) != 0 ) {
		DBGC ( dhcp, "DHCP %p could not register settings: %s\n",
		       dhcp, strerror ( rc ) );
		dhcp_finished ( dhcp, rc );
		return;
	}

	/* Terminate DHCP */
	dhcp_finished ( dhcp, 0 );
}

/**
 * Handle timer expiry during PXE Boot Server Discovery
 *
 * @v dhcp		DHCP session
 */
static void dhcp_pxebs_expired ( struct dhcp_session *dhcp ) {
	unsigned long elapsed = ( currticks() - dhcp->start );

	/* Give up waiting before we reach the failure point, and fail
	 * over to the next server in the attempt list
	 */
	if ( elapsed > PXEBS_MAX_TIMEOUT ) {
		dhcp->pxe_attempt++;
		if ( dhcp->pxe_attempt->s_addr ) {
			dhcp_set_state ( dhcp, &dhcp_state_pxebs );
			return;
		} else {
			dhcp_finished ( dhcp, -ETIMEDOUT );
			return;
		}
	}

	/* Retransmit current packet */
	dhcp_tx ( dhcp );
}

/** PXE Boot Server Discovery state operations */
static struct dhcp_session_state dhcp_state_pxebs = {
	.name			= "PXEBS",
	.tx			= dhcp_pxebs_tx,
	.rx			= dhcp_pxebs_rx,
	.expired		= dhcp_pxebs_expired,
	.tx_msgtype		= DHCPREQUEST,
	.apply_min_timeout	= 1,
};

/****************************************************************************
 *
 * Packet construction
 *
 */

/**
 * Construct DHCP client hardware address field and broadcast flag
 *
 * @v netdev		Network device
 * @v chaddr		Hardware address buffer
 * @v flags		Flags to set (or NULL)
 * @ret hlen		Hardware address length
 */
unsigned int dhcp_chaddr ( struct net_device *netdev, void *chaddr,
			   uint16_t *flags ) {
	struct ll_protocol *ll_protocol = netdev->ll_protocol;
	struct dhcphdr *dhcphdr;
	int rc;

	/* If the link-layer address cannot fit into the chaddr field
	 * (as is the case for IPoIB) then try using the Ethernet-
	 * compatible link-layer address.  If we do this, set the
	 * broadcast flag, since chaddr then does not represent a
	 * valid link-layer address for the return path.
	 *
	 * If we cannot produce an Ethernet-compatible link-layer
	 * address, try using the hardware address.
	 *
	 * If even the hardware address is too large, use an empty
	 * chaddr field and set the broadcast flag.
	 *
	 * This goes against RFC4390, but RFC4390 mandates that we use
	 * a DHCP client identifier that conforms with RFC4361, which
	 * we cannot do without either persistent (NIC-independent)
	 * storage, or by eliminating the hardware address completely
	 * from the DHCP packet, which seems unfriendly to users.
	 */
	if ( ll_protocol->ll_addr_len <= sizeof ( dhcphdr->chaddr ) ) {
		memcpy ( chaddr, netdev->ll_addr, ll_protocol->ll_addr_len );
		return ll_protocol->ll_addr_len;
	}
	if ( flags )
		*flags |= htons ( BOOTP_FL_BROADCAST );
	if ( ( rc = ll_protocol->eth_addr ( netdev->ll_addr, chaddr ) ) == 0 )
		return ETH_ALEN;
	if ( ll_protocol->hw_addr_len <= sizeof ( dhcphdr->chaddr ) ) {
		memcpy ( chaddr, netdev->hw_addr, ll_protocol->hw_addr_len );
		return ll_protocol->hw_addr_len;
	}
	return 0;
}

/**
 * Create a DHCP packet
 *
 * @v dhcppkt		DHCP packet structure to fill in
 * @v netdev		Network device
 * @v msgtype		DHCP message type
 * @v xid		Transaction ID (in network-endian order)
 * @v options		Initial options to include (or NULL)
 * @v options_len	Length of initial options
 * @v data		Buffer for DHCP packet
 * @v max_len		Size of DHCP packet buffer
 * @ret rc		Return status code
 *
 * Creates a DHCP packet in the specified buffer, and initialise a
 * DHCP packet structure.
 */
int dhcp_create_packet ( struct dhcp_packet *dhcppkt,
			 struct net_device *netdev, uint8_t msgtype,
			 uint32_t xid, const void *options, size_t options_len,
			 void *data, size_t max_len ) {
	struct dhcphdr *dhcphdr = data;
	int rc;

	/* Sanity check */
	if ( max_len < ( sizeof ( *dhcphdr ) + options_len ) )
		return -ENOSPC;

	/* Initialise DHCP packet content */
	memset ( dhcphdr, 0, max_len );
	dhcphdr->xid = xid;
	dhcphdr->magic = htonl ( DHCP_MAGIC_COOKIE );
	dhcphdr->htype = ntohs ( netdev->ll_protocol->ll_proto );
	dhcphdr->op = dhcp_op[msgtype];
	dhcphdr->hlen = dhcp_chaddr ( netdev, dhcphdr->chaddr,
				      &dhcphdr->flags );
	memcpy ( dhcphdr->options, options, options_len );

	/* Initialise DHCP packet structure */
	memset ( dhcppkt, 0, sizeof ( *dhcppkt ) );
	dhcppkt_init ( dhcppkt, data, max_len );
	
	/* Set DHCP_MESSAGE_TYPE option */
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_MESSAGE_TYPE,
				    &msgtype, sizeof ( msgtype ) ) ) != 0 )
		return rc;

	return 0;
}

/**
 * Create DHCP request packet
 *
 * @v dhcppkt		DHCP packet structure to fill in
 * @v netdev		Network device
 * @v msgtype		DHCP message type
 * @v xid		Transaction ID (in network-endian order)
 * @v ciaddr		Client IP address
 * @v data		Buffer for DHCP packet
 * @v max_len		Size of DHCP packet buffer
 * @ret rc		Return status code
 *
 * Creates a DHCP request packet in the specified buffer, and
 * initialise a DHCP packet structure.
 */
int dhcp_create_request ( struct dhcp_packet *dhcppkt,
			  struct net_device *netdev, unsigned int msgtype,
			  uint32_t xid, struct in_addr ciaddr,
			  void *data, size_t max_len ) {
	struct dhcp_netdev_desc dhcp_desc;
	struct dhcp_client_id client_id;
	struct dhcp_client_uuid client_uuid;
	uint8_t *dhcp_features;
	size_t dhcp_features_len;
	size_t ll_addr_len;
	ssize_t len;
	int rc;

	/* Create DHCP packet */
	if ( ( rc = dhcp_create_packet ( dhcppkt, netdev, msgtype, xid,
					 dhcp_request_options_data,
					 sizeof ( dhcp_request_options_data ),
					 data, max_len ) ) != 0 ) {
		DBG ( "DHCP could not create DHCP packet: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	/* Set client IP address */
	dhcppkt->dhcphdr->ciaddr = ciaddr;

	/* Add options to identify the feature list */
	dhcp_features = table_start ( DHCP_FEATURES );
	dhcp_features_len = table_num_entries ( DHCP_FEATURES );
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_EB_ENCAP, dhcp_features,
				    dhcp_features_len ) ) != 0 ) {
		DBG ( "DHCP could not set features list option: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	/* Add options to identify the network device */
	fetch_setting ( &netdev->settings.settings, &busid_setting, &dhcp_desc,
		sizeof ( dhcp_desc ) );
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_EB_BUS_ID, &dhcp_desc,
				    sizeof ( dhcp_desc ) ) ) != 0 ) {
		DBG ( "DHCP could not set bus ID option: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	/* Add DHCP client identifier.  Required for Infiniband, and
	 * doesn't hurt other link layers.
	 */
	client_id.ll_proto = ntohs ( netdev->ll_protocol->ll_proto );
	ll_addr_len = netdev->ll_protocol->ll_addr_len;
	assert ( ll_addr_len <= sizeof ( client_id.ll_addr ) );
	memcpy ( client_id.ll_addr, netdev->ll_addr, ll_addr_len );
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_CLIENT_ID, &client_id,
				    ( ll_addr_len + 1 ) ) ) != 0 ) {
		DBG ( "DHCP could not set client ID: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	/* Add client UUID, if we have one.  Required for PXE. */
	client_uuid.type = DHCP_CLIENT_UUID_TYPE;
	if ( ( len = fetch_uuid_setting ( NULL, &uuid_setting,
					  &client_uuid.uuid ) ) >= 0 ) {
		if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_CLIENT_UUID,
					    &client_uuid,
					    sizeof ( client_uuid ) ) ) != 0 ) {
			DBG ( "DHCP could not set client UUID: %s\n",
			      strerror ( rc ) );
			return rc;
		}
	}

	/* Add user class, if we have one. */
	if ( ( len = fetch_setting_len ( NULL, &user_class_setting ) ) >= 0 ) {
		char user_class[len];
		fetch_setting ( NULL, &user_class_setting, user_class,
				sizeof ( user_class ) );
		if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_USER_CLASS_ID,
					    &user_class,
					    sizeof ( user_class ) ) ) != 0 ) {
			DBG ( "DHCP could not set user class: %s\n",
			      strerror ( rc ) );
			return rc;
		}
	}

	return 0;
}

/****************************************************************************
 *
 * Data transfer interface
 *
 */

/**
 * Transmit DHCP request
 *
 * @v dhcp		DHCP session
 * @ret rc		Return status code
 */
static int dhcp_tx ( struct dhcp_session *dhcp ) {
	static struct sockaddr_in peer = {
		.sin_family = AF_INET,
	};
	struct xfer_metadata meta = {
		.netdev = dhcp->netdev,
		.src = ( struct sockaddr * ) &dhcp->local,
		.dest = ( struct sockaddr * ) &peer,
	};
	struct io_buffer *iobuf;
	uint8_t msgtype = dhcp->state->tx_msgtype;
	struct dhcp_packet dhcppkt;
	int rc;

	/* Start retry timer.  Do this first so that failures to
	 * transmit will be retried.
	 */
	start_timer ( &dhcp->timer );

	/* Allocate buffer for packet */
	iobuf = xfer_alloc_iob ( &dhcp->xfer, DHCP_MIN_LEN );
	if ( ! iobuf )
		return -ENOMEM;

	/* Create basic DHCP packet in temporary buffer */
	if ( ( rc = dhcp_create_request ( &dhcppkt, dhcp->netdev, msgtype,
					  dhcp->xid, dhcp->local.sin_addr,
					  iobuf->data,
					  iob_tailroom ( iobuf ) ) ) != 0 ) {
		DBGC ( dhcp, "DHCP %p could not construct DHCP request: %s\n",
		       dhcp, strerror ( rc ) );
		goto done;
	}

	/* (Ab)use the "secs" field to convey metadata about the DHCP
	 * session state into packet traces.  Useful for extracting
	 * debug information from non-debug builds.
	 */
	dhcppkt.dhcphdr->secs = htons ( ( ++(dhcp->count) << 2 ) |
					( dhcp->offer.s_addr ? 0x02 : 0 ) |
					( dhcp->proxy_offer ? 0x01 : 0 ) );

	/* Fill in packet based on current state */
	if ( ( rc = dhcp->state->tx ( dhcp, &dhcppkt, &peer ) ) != 0 ) {
		DBGC ( dhcp, "DHCP %p could not fill DHCP request: %s\n",
		       dhcp, strerror ( rc ) );
		goto done;
	}

	/* Transmit the packet */
	iob_put ( iobuf, dhcppkt_len ( &dhcppkt ) );
	if ( ( rc = xfer_deliver ( &dhcp->xfer, iob_disown ( iobuf ),
				   &meta ) ) != 0 ) {
		DBGC ( dhcp, "DHCP %p could not transmit UDP packet: %s\n",
		       dhcp, strerror ( rc ) );
		goto done;
	}

 done:
	free_iob ( iobuf );
	return rc;
}

/**
 * Receive new data
 *
 * @v dhcp		DHCP session
 * @v iobuf		I/O buffer
 * @v meta		Transfer metadata
 * @ret rc		Return status code
 */
static int dhcp_deliver ( struct dhcp_session *dhcp,
			  struct io_buffer *iobuf,
			  struct xfer_metadata *meta ) {
	struct sockaddr_in *peer;
	size_t data_len;
	struct dhcp_packet *dhcppkt;
	struct dhcphdr *dhcphdr;
	uint8_t msgtype = 0;
	struct in_addr server_id = { 0 };
	int rc = 0;

	/* Sanity checks */
	if ( ! meta->src ) {
		DBGC ( dhcp, "DHCP %p received packet without source port\n",
		       dhcp );
		rc = -EINVAL;
		goto err_no_src;
	}
	peer = ( struct sockaddr_in * ) meta->src;

	/* Create a DHCP packet containing the I/O buffer contents.
	 * Whilst we could just use the original buffer in situ, that
	 * would waste the unused space in the packet buffer, and also
	 * waste a relatively scarce fully-aligned I/O buffer.
	 */
	data_len = iob_len ( iobuf );
	dhcppkt = zalloc ( sizeof ( *dhcppkt ) + data_len );
	if ( ! dhcppkt ) {
		rc = -ENOMEM;
		goto err_alloc_dhcppkt;
	}
	dhcphdr = ( ( ( void * ) dhcppkt ) + sizeof ( *dhcppkt ) );
	memcpy ( dhcphdr, iobuf->data, data_len );
	dhcppkt_init ( dhcppkt, dhcphdr, data_len );

	/* Identify message type */
	dhcppkt_fetch ( dhcppkt, DHCP_MESSAGE_TYPE, &msgtype,
			sizeof ( msgtype ) );

	/* Identify server ID */
	dhcppkt_fetch ( dhcppkt, DHCP_SERVER_IDENTIFIER,
			&server_id, sizeof ( server_id ) );

	/* Check for matching transaction ID */
	if ( dhcphdr->xid != dhcp->xid ) {
		DBGC ( dhcp, "DHCP %p %s from %s:%d has bad transaction "
		       "ID\n", dhcp, dhcp_msgtype_name ( msgtype ),
		       inet_ntoa ( peer->sin_addr ),
		       ntohs ( peer->sin_port ) );
		rc = -EINVAL;
		goto err_xid;
	};

	/* Handle packet based on current state */
	dhcp->state->rx ( dhcp, dhcppkt, peer, msgtype, server_id );

 err_xid:
	dhcppkt_put ( dhcppkt );
 err_alloc_dhcppkt:
 err_no_src:
	free_iob ( iobuf );
	return rc;
}

/** DHCP data transfer interface operations */
static struct interface_operation dhcp_xfer_operations[] = {
	INTF_OP ( xfer_deliver, struct dhcp_session *, dhcp_deliver ),
};

/** DHCP data transfer interface descriptor */
static struct interface_descriptor dhcp_xfer_desc =
	INTF_DESC ( struct dhcp_session, xfer, dhcp_xfer_operations );

/**
 * Handle DHCP retry timer expiry
 *
 * @v timer		DHCP retry timer
 * @v fail		Failure indicator
 */
static void dhcp_timer_expired ( struct retry_timer *timer, int fail ) {
	struct dhcp_session *dhcp =
		container_of ( timer, struct dhcp_session, timer );

	/* If we have failed, terminate DHCP */
	if ( fail ) {
		dhcp_finished ( dhcp, -ETIMEDOUT );
		return;
	}

	/* Handle timer expiry based on current state */
	dhcp->state->expired ( dhcp );
}

/****************************************************************************
 *
 * Job control interface
 *
 */

/** DHCP job control interface operations */
static struct interface_operation dhcp_job_op[] = {
	INTF_OP ( intf_close, struct dhcp_session *, dhcp_finished ),
};

/** DHCP job control interface descriptor */
static struct interface_descriptor dhcp_job_desc =
	INTF_DESC ( struct dhcp_session, job, dhcp_job_op );

/****************************************************************************
 *
 * Instantiators
 *
 */

/**
 * DHCP peer address for socket opening
 *
 * This is a dummy address; the only useful portion is the socket
 * family (so that we get a UDP connection).  The DHCP client will set
 * the IP address and source port explicitly on each transmission.
 */
static struct sockaddr dhcp_peer = {
	.sa_family = AF_INET,
};

/**
 * Get cached DHCPACK where none exists
 */
__weak void get_cached_dhcpack ( void ) { __keepme }

/**
 * Start DHCP state machine on a network device
 *
 * @v job		Job control interface
 * @v netdev		Network device
 * @ret rc		Return status code, or positive if cached
 *
 * Starts DHCP on the specified network device.  If successful, the
 * DHCPACK (and ProxyDHCPACK, if applicable) will be registered as
 * option sources.
 *
 * On a return of 0, a background job has been started to perform the
 * DHCP request. Any nonzero return means the job has not been
 * started; a positive return value indicates the success condition of
 * having fetched the appropriate data from cached information.
 */
int start_dhcp ( struct interface *job, struct net_device *netdev ) {
	struct dhcp_session *dhcp;
	int rc;

	/* Check for cached DHCP information */
	get_cached_dhcpack();
	if ( fetch_uintz_setting ( NULL, &use_cached_setting ) ) {
		DBG ( "DHCP using cached network settings\n" );
		return 1;
	}

	/* Allocate and initialise structure */
	dhcp = zalloc ( sizeof ( *dhcp ) );
	if ( ! dhcp )
		return -ENOMEM;
	ref_init ( &dhcp->refcnt, dhcp_free );
	intf_init ( &dhcp->job, &dhcp_job_desc, &dhcp->refcnt );
	intf_init ( &dhcp->xfer, &dhcp_xfer_desc, &dhcp->refcnt );
	timer_init ( &dhcp->timer, dhcp_timer_expired, &dhcp->refcnt );
	dhcp->netdev = netdev_get ( netdev );
	dhcp->local.sin_family = AF_INET;
	dhcp->local.sin_port = htons ( BOOTPC_PORT );
	dhcp->xid = random();

	/* Store DHCP transaction ID for fakedhcp code */
	dhcp_last_xid = dhcp->xid;

	/* Instantiate child objects and attach to our interfaces */
	if ( ( rc = xfer_open_socket ( &dhcp->xfer, SOCK_DGRAM, &dhcp_peer,
				  ( struct sockaddr * ) &dhcp->local ) ) != 0 )
		goto err;

	/* Enter DHCPDISCOVER state */
	dhcp_set_state ( dhcp, &dhcp_state_discover );

	/* Attach parent interface, mortalise self, and return */
	intf_plug_plug ( &dhcp->job, job );
	ref_put ( &dhcp->refcnt );
	return 0;

 err:
	dhcp_finished ( dhcp, rc );
	ref_put ( &dhcp->refcnt );
	return rc;
}

/**
 * Retrieve list of PXE boot servers for a given server type
 *
 * @v dhcp		DHCP session
 * @v raw		DHCP PXE boot server list
 * @v raw_len		Length of DHCP PXE boot server list
 * @v ip		IP address list to fill in
 *
 * The caller must ensure that the IP address list has sufficient
 * space.
 */
static void pxebs_list ( struct dhcp_session *dhcp, void *raw,
			 size_t raw_len, struct in_addr *ip ) {
	struct dhcp_pxe_boot_server *server = raw;
	size_t server_len;
	unsigned int i;

	while ( raw_len ) {
		if ( raw_len < sizeof ( *server ) ) {
			DBGC ( dhcp, "DHCP %p malformed PXE server list\n",
			       dhcp );
			break;
		}
		server_len = offsetof ( typeof ( *server ),
					ip[ server->num_ip ] );
		if ( raw_len < server_len ) {
			DBGC ( dhcp, "DHCP %p malformed PXE server list\n",
			       dhcp );
			break;
		}
		if ( server->type == dhcp->pxe_type ) {
			for ( i = 0 ; i < server->num_ip ; i++ )
				*(ip++) = server->ip[i];
		}
		server = ( ( ( void * ) server ) + server_len );
		raw_len -= server_len;
	}
}

/**
 * Start PXE Boot Server Discovery on a network device
 *
 * @v job		Job control interface
 * @v netdev		Network device
 * @v pxe_type		PXE server type
 * @ret rc		Return status code
 *
 * Starts PXE Boot Server Discovery on the specified network device.
 * If successful, the Boot Server ACK will be registered as an option
 * source.
 */
int start_pxebs ( struct interface *job, struct net_device *netdev,
		  unsigned int pxe_type ) {
	struct setting pxe_discovery_control_setting =
		{ .tag = DHCP_PXE_DISCOVERY_CONTROL };
	struct setting pxe_boot_servers_setting =
		{ .tag = DHCP_PXE_BOOT_SERVERS };
	struct setting pxe_boot_server_mcast_setting =
		{ .tag = DHCP_PXE_BOOT_SERVER_MCAST };
	ssize_t pxebs_list_len;
	struct dhcp_session *dhcp;
	struct in_addr *ip;
	unsigned int pxe_discovery_control;
	int rc;

	/* Get upper bound for PXE boot server IP address list */
	pxebs_list_len = fetch_setting_len ( NULL, &pxe_boot_servers_setting );
	if ( pxebs_list_len < 0 )
		pxebs_list_len = 0;

	/* Allocate and initialise structure */
	dhcp = zalloc ( sizeof ( *dhcp ) + sizeof ( *ip ) /* mcast */ +
			sizeof ( *ip ) /* bcast */ + pxebs_list_len +
			sizeof ( *ip ) /* terminator */ );
	if ( ! dhcp )
		return -ENOMEM;
	ref_init ( &dhcp->refcnt, dhcp_free );
	intf_init ( &dhcp->job, &dhcp_job_desc, &dhcp->refcnt );
	intf_init ( &dhcp->xfer, &dhcp_xfer_desc, &dhcp->refcnt );
	timer_init ( &dhcp->timer, dhcp_timer_expired, &dhcp->refcnt );
	dhcp->netdev = netdev_get ( netdev );
	dhcp->local.sin_family = AF_INET;
	fetch_ipv4_setting ( netdev_settings ( netdev ), &ip_setting,
			     &dhcp->local.sin_addr );
	dhcp->local.sin_port = htons ( BOOTPC_PORT );
	dhcp->pxe_type = cpu_to_le16 ( pxe_type );

	/* Construct PXE boot server IP address lists */
	pxe_discovery_control =
		fetch_uintz_setting ( NULL, &pxe_discovery_control_setting );
	ip = ( ( ( void * ) dhcp ) + sizeof ( *dhcp ) );
	dhcp->pxe_attempt = ip;
	if ( ! ( pxe_discovery_control & PXEBS_NO_MULTICAST ) ) {
		fetch_ipv4_setting ( NULL, &pxe_boot_server_mcast_setting, ip);
		if ( ip->s_addr )
			ip++;
	}
	if ( ! ( pxe_discovery_control & PXEBS_NO_BROADCAST ) )
		(ip++)->s_addr = INADDR_BROADCAST;
	if ( pxe_discovery_control & PXEBS_NO_UNKNOWN_SERVERS )
		dhcp->pxe_accept = ip;
	if ( pxebs_list_len ) {
		uint8_t buf[pxebs_list_len];

		fetch_setting ( NULL, &pxe_boot_servers_setting,
				buf, sizeof ( buf ) );
		pxebs_list ( dhcp, buf, sizeof ( buf ), ip );
	}
	if ( ! dhcp->pxe_attempt->s_addr ) {
		DBGC ( dhcp, "DHCP %p has no PXE boot servers for type %04x\n",
		       dhcp, pxe_type );
		rc = -EINVAL;
		goto err;
	}

	/* Dump out PXE server lists */
	DBGC ( dhcp, "DHCP %p attempting", dhcp );
	for ( ip = dhcp->pxe_attempt ; ip->s_addr ; ip++ )
		DBGC ( dhcp, " %s", inet_ntoa ( *ip ) );
	DBGC ( dhcp, "\n" );
	if ( dhcp->pxe_accept ) {
		DBGC ( dhcp, "DHCP %p accepting", dhcp );
		for ( ip = dhcp->pxe_accept ; ip->s_addr ; ip++ )
			DBGC ( dhcp, " %s", inet_ntoa ( *ip ) );
		DBGC ( dhcp, "\n" );
	}

	/* Instantiate child objects and attach to our interfaces */
	if ( ( rc = xfer_open_socket ( &dhcp->xfer, SOCK_DGRAM, &dhcp_peer,
				  ( struct sockaddr * ) &dhcp->local ) ) != 0 )
		goto err;

	/* Enter PXEBS state */
	dhcp_set_state ( dhcp, &dhcp_state_pxebs );

	/* Attach parent interface, mortalise self, and return */
	intf_plug_plug ( &dhcp->job, job );
	ref_put ( &dhcp->refcnt );
	return 0;

 err:
	dhcp_finished ( dhcp, rc );
	ref_put ( &dhcp->refcnt );
	return rc;
}
