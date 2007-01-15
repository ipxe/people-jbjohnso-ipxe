#ifndef _GPXE_URI_H
#define _GPXE_URI_H

/** @file
 *
 * Uniform Resource Identifiers
 *
 */

#include <stdlib.h>

/** A Uniform Resource Identifier
 *
 * Terminology for this data structure is as per uri(7), except that
 * "path" is defined to include the leading '/' for an absolute path.
 *
 * Note that all fields within a URI are optional and may be NULL.
 *
 * Some examples are probably helpful:
 *
 *   http://www.etherboot.org/wiki :
 *
 *   scheme = "http", host = "www.etherboot.org", path = "/wiki"
 *
 *   /var/lib/tftpboot :
 *
 *   path = "/var/lib/tftpboot"
 *
 *   mailto:bob@nowhere.com :
 *
 *   scheme = "mailto", opaque = "bob@nowhere.com"
 *
 *   ftp://joe:secret@insecure.org:8081/hidden/path/to?what=is#this
 *
 *   scheme = "ftp", user = "joe", password = "secret",
 *   host = "insecure.org", port = "8081", path = "/hidden/path/to",
 *   query = "what=is", fragment = "this"
 */
struct uri {
	/** Scheme */
	const char *scheme;
	/** Opaque part */
	const char *opaque;
	/** User name */
	const char *user;
	/** Password */
	const char *password;
	/** Host name */
	const char *host;
	/** Port number */
	const char *port;
	/** Path */
	const char *path;
	/** Query */
	const char *query;
	/** Fragment */
	const char *fragment;
};

/**
 * URI is an absolute URI
 *
 * @v uri			URI
 * @ret is_absolute		URI is absolute
 *
 * An absolute URI begins with a scheme, e.g. "http:" or "mailto:".
 * Note that this is a separate concept from a URI with an absolute
 * path.
 */
static inline int uri_is_absolute ( struct uri *uri ) {
	return ( uri->scheme != NULL );
}

/**
 * URI has an absolute path
 *
 * @v uri			URI
 * @ret has_absolute_path	URI has an absolute path
 *
 * An absolute path begins with a '/'.  Note that this is a separate
 * concept from an absolute URI.  Note also that a URI may not have a
 * path at all.
 */
static inline int uri_has_absolute_path ( struct uri *uri ) {
	return ( uri->path && ( uri->path[0] == '/' ) );
}

/**
 * URI has a relative path
 *
 * @v uri			URI
 * @ret has_relative_path	URI has a relative path
 *
 * An relative path begins with something other than a '/'.  Note that
 * this is a separate concept from a relative URI.  Note also that a
 * URI may not have a path at all.
 */
static inline int uri_has_relative_path ( struct uri *uri ) {
	return ( uri->path && ( uri->path[0] != '/' ) );
}

/**
 * Free URI structure
 *
 * @v uri		URI
 *
 * Frees all the dynamically-allocated storage used by the URI
 * structure.
 */
static inline void free_uri ( struct uri *uri ) {
	free ( uri );
}

extern struct uri * parse_uri ( const char *uri_string );

#endif /* _GPXE_URI_H */