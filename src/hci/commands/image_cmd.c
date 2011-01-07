/*
 * Copyright (C) 2007 Michael Brown <mbrown@fensystems.co.uk>.
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

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <libgen.h>
#include <getopt.h>
#include <ipxe/image.h>
#include <ipxe/command.h>
#include <ipxe/parseopt.h>
#include <usr/imgmgmt.h>

/** @file
 *
 * Image management commands
 *
 */

/**
 * Fill in image command line
 *
 * @v image		Image
 * @v nargs		Argument count
 * @v args		Argument list
 * @ret rc		Return status code
 */
static int imgfill_cmdline ( struct image *image, unsigned int nargs, 
			     char **args ) {
	size_t len;
	unsigned int i;

	/* Determine total length of command line */
	len = 1; /* NUL */
	for ( i = 0 ; i < nargs ; i++ )
		len += ( 1 /* possible space */ + strlen ( args[i] ) );

	{
		char buf[len];
		char *ptr = buf;

		/* Assemble command line */
		buf[0] = '\0';
		for ( i = 0 ; i < nargs ; i++ ) {
			ptr += sprintf ( ptr, "%s%s", ( i ? " " : "" ),
					 args[i] );
		}
		assert ( ptr < ( buf + len ) );

		return image_set_cmdline ( image, buf );
	}
}

/** "imgfetch" options */
struct imgfetch_options {
	/** Image name */
	const char *name;
};

/** "imgfetch" option list */
static struct option_descriptor imgfetch_opts[] = {
	OPTION_DESC ( "name", 'n', required_argument,
		      struct imgfetch_options, name, parse_string ),
};

/** "imgfetch" command descriptor */
static struct command_descriptor imgfetch_cmd =
	COMMAND_DESC ( struct imgfetch_options, imgfetch_opts, 1, MAX_ARGUMENTS,
		       "[--name <name>] <image_url> [<arguments>...]",
		       "Fetch image" );

/** "kernel" command descriptor */
static struct command_descriptor kernel_cmd =
	COMMAND_DESC ( struct imgfetch_options, imgfetch_opts, 1, MAX_ARGUMENTS,
		       "[--name <name>] <image_url> [<arguments>...]",
		       "Fetch and load image" );

/** "chain" command descriptor */
static struct command_descriptor chain_cmd =
	COMMAND_DESC ( struct imgfetch_options, imgfetch_opts, 1, MAX_ARGUMENTS,
		       "[--name <name>] <image_url> [<arguments>...]",
		       "Fetch and execute image" );

/**
 * The "imgfetch" and friends command body
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @v cmd		Command descriptor
 * @v image_register	Image registration action
 * @ret rc		Return status code
 */
static int imgfetch_core_exec ( int argc, char **argv,
				struct command_descriptor *cmd,
				int ( * image_register ) ( struct image * ) ) {
	struct imgfetch_options opts;
	struct image *image;
	char *uri_string;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, cmd, &opts ) ) != 0 )
		return rc;

	/* Parse URI string */
	uri_string = argv[optind];
	if ( ! opts.name )
		opts.name = basename ( uri_string );

	/* Allocate image */
	image = alloc_image();
	if ( ! image ) {
		printf ( "%s\n", strerror ( -ENOMEM ) );
		return -ENOMEM;
	}

	/* Fill in image name */
	if ( ( rc = image_set_name ( image, opts.name ) ) != 0 )
		return rc;

	/* Fill in command line */
	if ( ( rc = imgfill_cmdline ( image, ( argc - optind - 1 ),
				      &argv[ optind + 1 ] ) ) != 0 )
		return rc;

	/* Fetch the image */
	if ( ( rc = imgfetch ( image, uri_string, image_register ) ) != 0 ) {
		printf ( "Could not fetch %s: %s\n",
			 uri_string, strerror ( rc ) );
		image_put ( image );
		return rc;
	}

	image_put ( image );
	return 0;
}

/**
 * The "imgfetch"/"module" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int imgfetch_exec ( int argc, char **argv ) {

	return imgfetch_core_exec ( argc, argv, &imgfetch_cmd,
				    register_image );
}

/**
 * The "kernel" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int kernel_exec ( int argc, char **argv ) {

	return imgfetch_core_exec ( argc, argv, &kernel_cmd,
				    register_and_autoload_image );
}

/**
 * The "chain" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int chain_exec ( int argc, char **argv) {

	return imgfetch_core_exec ( argc, argv, &chain_cmd,
				    register_and_autoexec_image );
}

/** "imgload" options */
struct imgload_options {};

/** "imgload" option list */
static struct option_descriptor imgload_opts[] = {};

/** "imgload" command descriptor */
static struct command_descriptor imgload_cmd =
	COMMAND_DESC ( struct imgload_options, imgload_opts, 1, 1,
		       "<image>", "Load image" );

/**
 * The "imgload" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int imgload_exec ( int argc, char **argv ) {
	struct imgload_options opts;
	struct image *image;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &imgload_cmd, &opts ) ) != 0 )
		return rc;

	/* Parse image name */
	if ( ( rc = parse_image ( argv[optind], &image ) ) != 0 )
		return rc;

	/* Load image */
	if ( ( rc = imgload ( image ) ) != 0 ) {
		printf ( "Could not load %s: %s\n",
			 image->name, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/** "imgargs" options */
struct imgargs_options {};

/** "imgargs" option list */
static struct option_descriptor imgargs_opts[] = {};

/** "imgargs" command descriptor */
static struct command_descriptor imgargs_cmd =
	COMMAND_DESC ( struct imgargs_options, imgargs_opts, 1, MAX_ARGUMENTS,
		       "<image> [<arguments>...]",
		       "Set arguments for image" );

/**
 * The "imgargs" command body
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int imgargs_exec ( int argc, char **argv ) {
	struct imgargs_options opts;
	struct image *image;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &imgargs_cmd, &opts ) ) != 0 )
		return rc;

	/* Parse image name */
	if ( ( rc = parse_image ( argv[optind], &image ) ) != 0 )
		return rc;

	/* Fill in command line */
	if ( ( rc = imgfill_cmdline ( image, ( argc - optind - 1 ),
				      &argv[ optind + 1 ] ) ) != 0 )
		return rc;

	return 0;
}

/** "imgexec" options */
struct imgexec_options {};

/** "imgexec" option list */
static struct option_descriptor imgexec_opts[] = {};

/** "imgexec" command descriptor */
static struct command_descriptor imgexec_cmd =
	COMMAND_DESC ( struct imgexec_options, imgexec_opts, 0, 1,
		       "[<image>]", "Execute image" );

/**
 * The "imgexec" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int imgexec_exec ( int argc, char **argv ) {
	struct imgexec_options opts;
	struct image *image;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &imgexec_cmd, &opts ) ) != 0 )
		return rc;

	/* Parse image name */
	if ( optind < argc ) {
		if ( ( rc = parse_image ( argv[optind], &image ) ) != 0 )
			return rc;
	} else {
		image = imgautoselect();
		if ( ! image ) {
			printf ( "No (unique) loaded image\n" );
			return -ENOTTY;
		}
	}

	/* Execute image */
	if ( ( rc = imgexec ( image ) ) != 0 ) {
		printf ( "Could not execute %s: %s\n",
			 image->name, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/** "imgstat" options */
struct imgstat_options {};

/** "imgstat" option list */
static struct option_descriptor imgstat_opts[] = {};

/** "imgstat" command descriptor */
static struct command_descriptor imgstat_cmd =
	COMMAND_DESC ( struct imgstat_options, imgstat_opts, 0, 0,
		       "", "List images" );

/**
 * The "imgstat" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int imgstat_exec ( int argc, char **argv ) {
	struct imgstat_options opts;
	struct image *image;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &imgstat_cmd, &opts ) ) != 0 )
		return rc;

	/* Show status of all images */
	for_each_image ( image ) {
		imgstat ( image );
	}

	return 0;
}

/** "imgfree" options */
struct imgfree_options {};

/** "imgfree" option list */
static struct option_descriptor imgfree_opts[] = {};

/** "imgfree" command descriptor */
static struct command_descriptor imgfree_cmd =
	COMMAND_DESC ( struct imgfree_options, imgfree_opts, 0, 1,
		       "[<image>]", "Free image(s)" );

/**
 * The "imgfree" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int imgfree_exec ( int argc, char **argv ) {
	struct imgfree_options opts;
	struct image *image;
	struct image *tmp;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &imgfree_cmd, &opts ) ) != 0 )
		return rc;

	if ( optind < argc ) {
		/* Free specified image */
		if ( ( rc = parse_image ( argv[optind], &image ) ) != 0 )
			return rc;
		imgfree ( image );
	} else {
		/* Free all images */
		list_for_each_entry_safe ( image, tmp, &images, list ) {
			imgfree ( image );
		}
	}

	return 0;
}

/** Image management commands */
struct command image_commands[] __command = {
	{
		.name = "imgfetch",
		.exec = imgfetch_exec,
	},
	{
		.name = "module",
		.exec = imgfetch_exec, /* synonym for "imgfetch" */
	},
	{
		.name = "initrd",
		.exec = imgfetch_exec, /* synonym for "imgfetch" */
	},
	{
		.name = "kernel",
		.exec = kernel_exec,
	},
	{
		.name = "chain",
		.exec = chain_exec,
	},
	{
		.name = "imgload",
		.exec = imgload_exec,
	},
	{
		.name = "imgargs",
		.exec = imgargs_exec,
	},
	{
		.name = "imgexec",
		.exec = imgexec_exec,
	},
	{
		.name = "boot", /* synonym for "imgexec" */
		.exec = imgexec_exec,
	},
	{
		.name = "imgstat",
		.exec = imgstat_exec,
	},
	{
		.name = "imgfree",
		.exec = imgfree_exec,
	},
};
