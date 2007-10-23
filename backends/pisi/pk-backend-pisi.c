/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2007 S.Çağlar Onur <caglar@pardus.org.tr>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <gmodule.h>
#include <glib.h>
#include <string.h>
#include <pk-backend.h>

/**
 * backend_get_depends:
 */
static void
backend_get_depends (PkBackend *backend, const gchar *package_id)
{
	g_return_if_fail (backend != NULL);
	pk_backend_spawn_helper (backend, "get-depends.py", package_id, NULL);
}

/**
 * backend_get_requires:
 */
static void
backend_get_requires (PkBackend *backend, const gchar *package_id)
{
	g_return_if_fail (backend != NULL);
	pk_backend_spawn_helper (backend, "get-requires.py", package_id, NULL);
}

/**
 * backend_get_updates:
 */
static void
backend_get_updates (PkBackend *backend)
{
	g_return_if_fail (backend != NULL);
	pk_backend_spawn_helper (backend, "get-updates.py", NULL);
}

/**
 * backend_install_package:
 */
static void
backend_install_package (PkBackend *backend, const gchar *package_id)
{
	g_return_if_fail (backend != NULL);
	/* check network state */
	if (pk_backend_network_is_online (backend) == FALSE) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_NO_NETWORK, "Cannot install when offline");
		pk_backend_finished (backend);
		return;
	}
	pk_backend_spawn_helper (backend, "install.py", package_id, NULL);
}

/**
 * backend_refresh_cache:
 */
static void
backend_refresh_cache (PkBackend *backend, gboolean force)
{
	g_return_if_fail (backend != NULL);
	/* check network state */
	if (pk_backend_network_is_online (backend) == FALSE) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_NO_NETWORK, "Cannot refresh cache whilst offline");
		pk_backend_finished (backend);
		return;
	}
	pk_backend_spawn_helper (backend, "refresh-cache.py", NULL);
}

/**
 * backend_remove_package:
 */
static void
backend_remove_package (PkBackend *backend, const gchar *package_id, gboolean allow_deps)
{
	g_return_if_fail (backend != NULL);
	const gchar *deps;
	if (allow_deps == TRUE) {
		deps = "yes";
	} else {
		deps = "no";
	}
	pk_backend_spawn_helper (backend, "remove.py", deps, package_id, NULL);
}

/**
 * backend_resolve:
 */
static void
backend_resolve (PkBackend *backend, const gchar *filter, const gchar *package_id)
{
	g_return_if_fail (backend != NULL);
	pk_backend_spawn_helper (backend, "resolve.py", filter, package_id, NULL);
}

/**
 * backend_update_package:
 */
static void
backend_update_package (PkBackend *backend, const gchar *package_id)
{
	g_return_if_fail (backend != NULL);
	/* check network state */
	if (pk_backend_network_is_online (backend) == FALSE) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_NO_NETWORK, "Cannot update when offline");
		pk_backend_finished (backend);
		return;
	}
	pk_backend_spawn_helper (backend, "update.py", package_id, NULL);
}

/**
 * backend_get_repo_list:
 */
static void
backend_get_repo_list (PkBackend *backend)
{
	g_return_if_fail (backend != NULL);
	pk_backend_spawn_helper (backend, "get-repo-list.py", NULL);
}

PK_BACKEND_OPTIONS (
	"PiSi",						/* description */
	"0.0.1",					/* version */
	"S.Çağlar Onur <caglar@pardus.org.tr>",		/* author */
	NULL,						/* initalize */
	NULL,						/* destroy */
	NULL,						/* get_groups */
	NULL,						/* get_filters */
	NULL,						/* cancel */
	backend_get_depends,				/* get_depends */
	NULL,						/* get_description */
	backend_get_requires,				/* get_requires */
	NULL,						/* get_update_detail */
	backend_get_updates,				/* get_updates */
	backend_install_package,			/* install_package */
	NULL,						/* install_file */
	backend_refresh_cache,				/* refresh_cache */
	backend_remove_package,				/* remove_package */
	backend_resolve,				/* resolve */
	NULL,						/* rollback */
	NULL,						/* search_details */
	NULL,						/* search_file */
	NULL,						/* search_group */
	NULL,						/* search_name */
	backend_update_package,				/* update_package */
	NULL,						/* update_system */
	backend_get_repo_list,				/* get_repo_list */
	NULL,						/* repo_enable */
	NULL						/* repo_set_data */
);
