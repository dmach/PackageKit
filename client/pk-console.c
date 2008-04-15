/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2008 Richard Hughes <richard@hughsie.com>
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

#include "config.h"

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <dbus/dbus-glib.h>

#include <pk-debug.h>
#include <pk-client.h>
#include <pk-control.h>
#include <pk-package-id.h>
#include <pk-common.h>
#include <pk-connection.h>

#define PROGRESS_BAR_SIZE 15

static GMainLoop *loop = NULL;
static PkRoleEnum roles;
static gboolean is_console = FALSE;
static gboolean has_output_bar = FALSE;
static gboolean need_requeue = FALSE;
static gboolean nowait = FALSE;
static gboolean awaiting_space = FALSE;
static guint timer_id = 0;
static guint percentage_last = 0;
static PkControl *control = NULL;
static PkClient *client = NULL;
static PkClient *client_task = NULL;
static PkClient *client_signature = NULL;

typedef struct {
	gint position;
	gboolean move_forward;
} PulseState;

/**
 * pk_console_bar:
 **/
static void
pk_console_bar (guint subpercentage)
{
	guint section;
	guint i;

	/* don't pretty print */
	if (!is_console) {
		return;
	}
	if (!has_output_bar) {
		return;
	}
	/* restore cursor */
	g_print ("%c8", 0x1B);

	section = (guint) ((gfloat) PROGRESS_BAR_SIZE / (gfloat) 100.0 * (gfloat) subpercentage);
	g_print ("[");
	for (i=0; i<section; i++) {
		g_print ("=");
	}
	for (i=0; i<PROGRESS_BAR_SIZE-section; i++) {
		g_print (" ");
	}
	g_print ("] ");
	if (percentage_last != PK_CLIENT_PERCENTAGE_INVALID) {
		g_print ("(%i%%)", percentage_last);
	} else {
		g_print ("       ");
	}
	awaiting_space = TRUE;
}

/**
 * pk_console_package_cb:
 **/
static void
pk_console_package_cb (PkClient *client, PkInfoEnum info, const gchar *package_id, const gchar *summary, gpointer data)
{
	PkPackageId *ident;
	PkRoleEnum role;
	gchar *package = NULL;
	gchar *info_pad = NULL;
	gchar *package_pad = NULL;

	/* split */
	ident = pk_package_id_new_from_string (package_id);
	if (ident == NULL) {
		pk_warning (_("Could not get valid ident from %s"), package_id);
		return;
	}

	/* make these all the same lenght */
	info_pad = pk_strpad (pk_info_enum_to_text (info), 12);

	/* don't pretty print */
	if (!is_console) {
		g_print ("%s %s-%s.%s\n", info_pad, ident->name, ident->version, ident->arch);
		goto out;
	}

	/* pad the name-version */
	if (pk_strzero (ident->version)) {
		package = g_strdup (ident->name);
	} else {
		package = g_strdup_printf ("%s-%s", ident->name, ident->version);
	}
	package_pad = pk_strpad (package, 40);

	/* mark previous complete */
	if (has_output_bar) {
		pk_console_bar (100);
	}

	if (awaiting_space) {
		g_print ("\n");
	}

	pk_client_get_role (client, &role, NULL, NULL);
	if (role == PK_ROLE_ENUM_SEARCH_NAME ||
	    role == PK_ROLE_ENUM_SEARCH_GROUP ||
	    role == PK_ROLE_ENUM_SEARCH_FILE ||
	    role == PK_ROLE_ENUM_SEARCH_DETAILS ||
	    role == PK_ROLE_ENUM_GET_PACKAGES ||
	    role == PK_ROLE_ENUM_GET_DEPENDS ||
	    role == PK_ROLE_ENUM_GET_REQUIRES ||
	    role == PK_ROLE_ENUM_GET_UPDATES) {
		/* don't do the bar */
		g_print ("%s %s\n", info_pad, package_pad);
		goto out;
	}

	has_output_bar = TRUE;
	/* do we need to new line? */

	/* pretty print */
	g_print ("%s %s ", info_pad, package_pad);

	/* save cursor in new position */
	g_print ("%c7", 0x1B);
	pk_console_bar (0);
out:
	/* free all the data */
	pk_package_id_free (ident);
	g_free (package);
	g_free (info_pad);
	g_free (package_pad);
}

/**
 * pk_console_transaction_cb:
 **/
static void
pk_console_transaction_cb (PkClient *client, const gchar *tid, const gchar *timespec,
			   gboolean succeeded, PkRoleEnum role, guint duration,
			   const gchar *data, gpointer user_data)
{
	const gchar *role_text;
	role_text = pk_role_enum_to_text (role);
	if (awaiting_space) {
		g_print ("\n");
	}
	g_print ("Transaction  : %s\n", tid);
	g_print (" timespec    : %s\n", timespec);
	g_print (" succeeded   : %i\n", succeeded);
	g_print (" role        : %s\n", role_text);
	g_print (" duration    : %i (seconds)\n", duration);
	g_print (" data        : %s\n", data);
}

/**
 * pk_console_update_detail_cb:
 **/
static void
pk_console_update_detail_cb (PkClient *client, const gchar *package_id,
			     const gchar *updates, const gchar *obsoletes,
			     const gchar *vendor_url, const gchar *bugzilla_url,
			     const gchar *cve_url, PkRestartEnum restart,
			     const gchar *update_text, gpointer data)
{
	if (awaiting_space) {
		g_print ("\n");
	}
	g_print (_("Update detail\n"));
	g_print ("  package:    '%s'\n", package_id);
	if (pk_strzero (updates) == FALSE) {
		g_print ("  updates:    '%s'\n", updates);
	}
	if (pk_strzero (obsoletes) == FALSE) {
		g_print ("  obsoletes:  '%s'\n", obsoletes);
	}
	if (pk_strzero (vendor_url) == FALSE) {
		g_print ("  vendor URL: '%s'\n", vendor_url);
	}
	if (pk_strzero (bugzilla_url) == FALSE) {
		g_print ("  bug URL:    '%s'\n", bugzilla_url);
	}
	if (pk_strzero (cve_url) == FALSE) {
		g_print ("  cve URL:    '%s'\n", cve_url);
	}
	if (restart != PK_RESTART_ENUM_NONE) {
		g_print ("  restart:    '%s'\n", pk_restart_enum_to_text (restart));
	}
	if (pk_strzero (update_text) == FALSE) {
		g_print ("  update_text:'%s'\n", update_text);
	}
}

/**
 * pk_console_repo_detail_cb:
 **/
static void
pk_console_repo_detail_cb (PkClient *client, const gchar *repo_id,
			   const gchar *description, gboolean enabled, gpointer data)
{
	gchar *repo;
	repo = pk_strpad (repo_id, 28);
	if (awaiting_space) {
		g_print ("\n");
	}
	if (enabled) {
		g_print ("  enabled   %s %s\n", repo, description);
	} else {
		g_print ("  disabled  %s %s\n", repo, description);
	}
	g_free (repo);
}

/**
 * pk_console_pulse_bar:
 **/
static gboolean
pk_console_pulse_bar (PulseState *pulse_state)
{
	guint i;

	if (!has_output_bar) {
		return TRUE;
	}

	/* restore cursor */
	g_print ("%c8", 0x1B);

	if (pulse_state->move_forward) {
		if (pulse_state->position == PROGRESS_BAR_SIZE - 1) {
			pulse_state->move_forward = FALSE;
		} else {
			pulse_state->position++;
		}
	} else if (pulse_state->move_forward == FALSE) {
		if (pulse_state->position == 1) {
			pulse_state->move_forward = TRUE;
		} else {
			pulse_state->position--;
		}
	}

	g_print ("[");
	for (i=0; i<pulse_state->position-1; i++) {
		g_print (" ");
	}
	printf("==");
	for (i=0; i<PROGRESS_BAR_SIZE-pulse_state->position-1; i++) {
		g_print (" ");
	}
	g_print ("] ");
	if (percentage_last != PK_CLIENT_PERCENTAGE_INVALID) {
		g_print ("(%i%%)", percentage_last);
	} else {
		g_print ("        ");
	}

	return TRUE;
}

/**
 * pk_console_draw_pulse_bar:
 **/
static void
pk_console_draw_pulse_bar (void)
{
	static PulseState pulse_state;

	/* have we already got zero percent? */
	if (timer_id != 0) {
		return;
	}
	if (is_console) {
		pulse_state.position = 1;
		pulse_state.move_forward = TRUE;
		timer_id = g_timeout_add (40, (GSourceFunc) pk_console_pulse_bar, &pulse_state);
	}
}

/**
 * pk_console_progress_changed_cb:
 **/
static void
pk_console_progress_changed_cb (PkClient *client, guint percentage, guint subpercentage,
				guint elapsed, guint remaining, gpointer data)
{
	if (!is_console) {
		if (percentage != PK_CLIENT_PERCENTAGE_INVALID) {
			g_print ("percentage: %i%%\n", percentage);
		} else {
			g_print ("percentage: unknown\n");
		}
		return;
	}
	percentage_last = percentage;
	if (subpercentage == PK_CLIENT_PERCENTAGE_INVALID) {
		pk_console_bar (0);
		pk_console_draw_pulse_bar ();
	} else {
		if (timer_id != 0) {
			g_source_remove (timer_id);
			timer_id = 0;
		}
		pk_console_bar (subpercentage);
	}
}

/**
 * pk_console_signature_finished_cb:
 **/
static void
pk_console_signature_finished_cb (PkClient *client_signature, PkExitEnum exit, guint runtime, gpointer data)
{
	gboolean ret;
	GError *error = NULL;

	pk_debug ("trying to requeue");
	ret = pk_client_requeue (client, &error);
	if (!ret) {
		pk_warning ("failed to requeue action: %s", error->message);
		g_error_free (error);
		g_main_loop_quit (loop);
	}
}

/**
 * pk_console_finished_cb:
 **/
static void
pk_console_finished_cb (PkClient *client, PkExitEnum exit, guint runtime, gpointer data)
{
	PkRoleEnum role;
	const gchar *role_text;
	gfloat time;
	PkRestartEnum restart;

	pk_client_get_role (client, &role, NULL, NULL);

	/* mark previous complete */
	if (has_output_bar) {
		pk_console_bar (100);
	}

	/* cancel the spinning */
	if (timer_id != 0) {
		g_source_remove (timer_id);
	}

	role_text = pk_role_enum_to_text (role);
	time = (gfloat) runtime / 1000.0;

	/* do we need to new line? */
	if (awaiting_space) {
		g_print ("\n");
	}
	g_print ("%s runtime was %.1f seconds\n", role_text, time);

	/* is there any restart to notify the user? */
	restart = pk_client_get_require_restart (client);
	if (restart != PK_RESTART_ENUM_NONE) {
		g_print (_("Requires restart: %s\n"), pk_restart_enum_to_text (restart));
	}

	/* have we failed to install, and the gpg key is now installed */
	if (exit == PK_EXIT_ENUM_KEY_REQUIRED && need_requeue) {
		return;
	}

	/* close the loop */
	g_main_loop_quit (loop);
}

/**
 * pk_console_get_number:
 **/
static guint
pk_console_get_number (const gchar *question, guint maxnum)
{
	gint answer = 0;
	gint retval;

	/* pretty print */
	g_print ("%s", question);

	do {
		/* get a number */
		retval = scanf("%u", &answer);

		/* positive */
		if (retval == 1 && answer > 0 && answer <= maxnum) {
			break;
		}
		g_print (_("Please enter a number from 1 to %i: "), maxnum);
	} while (TRUE);
	return answer;
}

/**
 * pk_console_perhaps_resolve:
 **/
static gchar *
pk_console_perhaps_resolve (PkClient *client, PkFilterEnum filter, const gchar *package, GError **error)
{
	gboolean ret;
	gboolean valid;
	guint i;
	guint length;
	PkPackageItem *item;

	/* have we passed a complete package_id? */
	valid = pk_package_id_check (package);
	if (valid) {
		return g_strdup (package);
	}

	/* we need to resolve it */
	ret = pk_client_resolve (client_task, filter, package, error);
	if (ret == FALSE) {
		pk_warning (_("Resolve is not supported in this backend"));
		return NULL;
	}

	/* get length of items found */
	length = pk_client_package_buffer_get_size (client_task);

	/* didn't resolve to anything, try to get a provide */
	if (length == 0) {
		ret = pk_client_reset (client_task, error);
		if (ret == FALSE) {
			pk_warning ("failed to reset client task");
			return NULL;
		}
		ret = pk_client_what_provides (client_task, filter, PK_PROVIDES_ENUM_ANY, package, error);
		if (ret == FALSE) {
			pk_warning (_("WhatProvides is not supported in this backend"));
			return NULL;
		}
	}

	/* get length of items found again (we might have has success) */
	length = pk_client_package_buffer_get_size (client_task);
	if (length == 0) {
		pk_warning (_("Could not find a package match"));
		return NULL;
	}

	/* only found one, great! */
	if (length == 1) {
		item = pk_client_package_buffer_get_item (client_task, 0);
		return g_strdup (item->package_id);
	}

	/* else list the options if multiple matches found */
	if (awaiting_space) {
		g_print ("\n");
	}
	g_print (_("There are multiple matches\n"));
	for (i=0; i<length; i++) {
		item = pk_client_package_buffer_get_item (client_task, i);
		g_print ("%i. %s\n", i+1, item->package_id);
	}

	/* find out what package the user wants to use */
	i = pk_console_get_number (_("Please enter the package number: "), length);
	item = pk_client_package_buffer_get_item (client_task, i-1);
	pk_debug ("package_id = %s", item->package_id);
	return g_strdup (item->package_id);
}

/**
 * pk_console_install_package:
 **/
static gboolean
pk_console_install_package (PkClient *client, const gchar *package, GError **error)
{
	gboolean ret;
	gchar *package_id;
	package_id = pk_console_perhaps_resolve (client, PK_FILTER_ENUM_NOT_INSTALLED, package, error);
	if (package_id == NULL) {
		g_print (_("Could not find a package with that name to install, or package already installed\n"));
		return FALSE;
	}
	ret = pk_client_install_package (client, package_id, error);
	g_free (package_id);
	return ret;
}

/**
 * pk_console_remove_only:
 **/
static gboolean
pk_console_remove_only (PkClient *client, const gchar *package_id, gboolean force, gboolean autoremove, GError **error)
{
	gboolean ret;

	pk_debug ("remove %s", package_id);
	ret = pk_client_reset (client, error);
	if (!ret) {
		return ret;
	}
	return pk_client_remove_package (client, package_id, force, autoremove, error);
}

/**
 * pk_console_get_prompt:
 **/
static gboolean
pk_console_get_prompt (const gchar *question, gboolean defaultyes)
{
	gchar answer = '\0';

	/* pretty print */
	g_print ("%s", question);
	if (defaultyes) {
		g_print (" [Y/n] ");
	} else {
		g_print (" [N/y] ");
	}

	do {
		/* get one char */
		answer = (gchar) getchar();

		/* positive */
		if (answer == 'y' || answer == 'Y') {
			return TRUE;
		}
		/* negative */
		if (answer == 'n' || answer == 'N') {
			return FALSE;
		}

		/* default choice */
		if (answer == '\n' && defaultyes) {
			return TRUE;
		}
		if (answer == '\n' && defaultyes == FALSE) {
			return FALSE;
		}
	} while (TRUE);

	/* keep GCC happy */
	return FALSE;
}

/**
 * pk_console_remove_package:
 **/
static gboolean
pk_console_remove_package (PkClient *client, const gchar *package, GError **error)
{
	gchar *package_id;
	gboolean ret;
	guint length;
	PkPackageItem *item;
	PkPackageId *ident;
	guint i;
	gboolean remove;

	package_id = pk_console_perhaps_resolve (client, PK_FILTER_ENUM_INSTALLED, package, error);
	if (package_id == NULL) {
		g_print (_("Could not find a package with that name to remove\n"));
		return FALSE;
	}

	/* are we dumb and can't check for requires? */
	if (!pk_enums_contain (roles, PK_ROLE_ENUM_GET_REQUIRES)) {
		/* no, just try to remove it without deps */
		ret = pk_console_remove_only (client, package_id, FALSE, FALSE, error);
		g_free (package_id);
		return ret;
	}

	/* see if any packages require this one */
	ret = pk_client_reset (client_task, error);
	if (!ret) {
		pk_warning ("failed to reset");
		return FALSE;
	}

	pk_debug (_("Getting installed requires for %s"), package_id);
	ret = pk_client_get_requires (client_task, PK_FILTER_ENUM_INSTALLED, package_id, TRUE, error);
	if (!ret) {
		return FALSE;
	}

	/* see how many packages there are */
	length = pk_client_package_buffer_get_size (client_task);

	/* if there are no required packages, just do the remove */
	if (length == 0) {
		pk_debug ("no requires");
		ret = pk_console_remove_only (client, package_id, FALSE, FALSE, error);
		g_free (package_id);
		return ret;
	}

	/* present this to the user */
	if (awaiting_space) {
		g_print ("\n");
	}
	g_print (_("The following packages have to be removed:\n"));
	for (i=0; i<length; i++) {
		item = pk_client_package_buffer_get_item (client_task, i);
		ident = pk_package_id_new_from_string (item->package_id);
		g_print ("%i\t%s-%s\n", i, ident->name, ident->version);
		pk_package_id_free (ident);
	}

	/* get user input */
	remove = pk_console_get_prompt (_("Okay to remove additional packages?"), FALSE);

	/* we chickened out */
	if (remove == FALSE) {
		g_print ("Cancelled!\n");
		g_free (package_id);
		return FALSE;
	}

	/* remove all the stuff */
	ret = pk_console_remove_only (client, package_id, TRUE, FALSE, error);
	g_free (package_id);

	return ret;
}

/**
 * pk_console_update_package:
 **/
static gboolean
pk_console_update_package (PkClient *client, const gchar *package, GError **error)
{
	gboolean ret;
	gchar *package_id;
	package_id = pk_console_perhaps_resolve (client, PK_FILTER_ENUM_INSTALLED, package, error);
	if (package_id == NULL) {
		g_print (_("Could not find a package with that name to update\n"));
		return FALSE;
	}
	ret = pk_client_update_package (client, package_id, error);
	g_free (package_id);
	return ret;
}

/**
 * pk_console_get_requires:
 **/
static gboolean
pk_console_get_requires (PkClient *client, const gchar *package, GError **error)
{
	gboolean ret;
	gchar *package_id;
	package_id = pk_console_perhaps_resolve (client, PK_FILTER_ENUM_NONE, package, error);
	if (package_id == NULL) {
		g_print (_("Could not find a package with that name to get requires\n"));
		return FALSE;
	}
	ret = pk_client_get_requires (client, PK_FILTER_ENUM_NONE, package_id, TRUE, error);
	g_free (package_id);
	return ret;
}

/**
 * pk_console_get_depends:
 **/
static gboolean
pk_console_get_depends (PkClient *client, const gchar *package, GError **error)
{
	gboolean ret;
	gchar *package_id;
	package_id = pk_console_perhaps_resolve (client, PK_FILTER_ENUM_NONE, package, error);
	if (package_id == NULL) {
		g_print (_("Could not find a package with that name to get depends\n"));
		return FALSE;
	}
	ret = pk_client_get_depends (client, PK_FILTER_ENUM_NONE, package_id, FALSE, error);
	g_free (package_id);
	return ret;
}

/**
 * pk_console_get_description:
 **/
static gboolean
pk_console_get_description (PkClient *client, const gchar *package, GError **error)
{
	gboolean ret;
	gchar *package_id;
	package_id = pk_console_perhaps_resolve (client, PK_FILTER_ENUM_NONE, package, error);
	if (package_id == NULL) {
		g_print (_("Could not find a package with that name to get description\n"));
		return FALSE;
	}
	ret = pk_client_get_description (client, package_id, error);
	g_free (package_id);
	return ret;
}

/**
 * pk_console_get_files:
 **/
static gboolean
pk_console_get_files (PkClient *client, const gchar *package, GError **error)
{
	gboolean ret;
	gchar *package_id;
	package_id = pk_console_perhaps_resolve (client, PK_FILTER_ENUM_NONE, package, error);
	if (package_id == NULL) {
		g_print (_("Could not find a package with that name to get files\n"));
		return FALSE;
	}
	ret = pk_client_get_files (client, package_id, error);
	g_free (package_id);
	return ret;
}

/**
 * pk_console_get_update_detail
 **/
static gboolean
pk_console_get_update_detail (PkClient *client, const gchar *package, GError **error)
{
	gboolean ret;
	gchar *package_id;
	package_id = pk_console_perhaps_resolve (client, PK_FILTER_ENUM_INSTALLED, package, error);
	if (package_id == NULL) {
		g_print ("Could not find a package with that name to get update details\n");
		return FALSE;
	}
	ret = pk_client_get_update_detail (client, package_id, error);
	g_free (package_id);
	return ret;
}

/**
 * pk_console_error_code_cb:
 **/
static void
pk_console_error_code_cb (PkClient *client, PkErrorCodeEnum error_code, const gchar *details, gpointer data)
{
	/* handled */
	if (need_requeue && error_code == PK_ERROR_ENUM_GPG_FAILURE) {
		pk_debug ("ignoring GPG error as handled");
		return;
	}
	if (awaiting_space) {
		g_print ("\n");
	}
	g_print ("Error: %s : %s\n", pk_error_enum_to_text (error_code), details);
}

/**
 * pk_console_description_cb:
 **/
static void
pk_console_description_cb (PkClient *client, const gchar *package_id,
			   const gchar *license, PkGroupEnum group,
			   const gchar *description, const gchar *url,
			   gulong size, gpointer data)
{
	/* if on console, clear the progress bar line */
	if (awaiting_space) {
		g_print ("\n");
	}
	g_print (_("Package description\n"));
	g_print ("  package:     '%s'\n", package_id);
	g_print ("  license:     '%s'\n", license);
	g_print ("  group:       '%s'\n", pk_group_enum_to_text (group));
	g_print ("  description: '%s'\n", description);
	g_print ("  size:        '%ld' bytes\n", size);
	g_print ("  url:         '%s'\n", url);
}

/**
 * pk_console_files_cb:
 **/
static void
pk_console_files_cb (PkClient *client, const gchar *package_id,
		     const gchar *filelist, gpointer data)
{
	gchar **filevector = g_strsplit (filelist, ";", 0);

	if (awaiting_space) {
		g_print ("\n");
	}
	g_print ("Package files\n");

	if (*filevector != NULL) {
		gchar **current_file = filevector;

		while (*current_file != NULL) {
			g_print ("  %s\n", *current_file);
			current_file++;
		}
	} else {
	    g_print ("  no files\n");
	}

	g_strfreev (filevector);
}

/**
 * pk_console_repo_signature_required_cb:
 **/
static void
pk_console_repo_signature_required_cb (PkClient *client, const gchar *package_id, const gchar *repository_name,
				       const gchar *key_url, const gchar *key_userid, const gchar *key_id,
				       const gchar *key_fingerprint, const gchar *key_timestamp,
				       PkSigTypeEnum type, gpointer data)
{
	gboolean import;
	gboolean ret;
	GError *error = NULL;

	if (awaiting_space) {
		g_print ("\n");
	}
	g_print ("Repository Signature Required\n");
	g_print ("Package:     %s\n", package_id);
	g_print ("Name:        %s\n", repository_name);
	g_print ("URL:         %s\n", key_url);
	g_print ("User:        %s\n", key_userid);
	g_print ("ID:          %s\n", key_id);
	g_print ("Fingerprint: %s\n", key_fingerprint);
	g_print ("Timestamp:   %s\n", key_timestamp);

	/* get user input */
	import = pk_console_get_prompt (_("Okay to import key?"), FALSE);
	if (!import) {
		g_print ("%s\n", _("Did not import key, task will fail"));
		return;
	}

	/* install signature */
	pk_debug ("install signature %s", key_id);
	ret = pk_client_install_signature (client_signature, PK_SIGTYPE_ENUM_GPG,
					   key_id, package_id, &error);
	/* we succeeded, so wait for the requeue */
	if (!ret) {
		pk_warning ("failed to install signature: %s", error->message);
		g_error_free (error);
		return;
	}

	/* we imported a signature */
	need_requeue = TRUE;
}

/**
 * pk_console_eula_required_cb:
 **/
static void
pk_console_eula_required_cb (PkClient *client, const gchar *eula_id, const gchar *package_id,
			     const gchar *vendor_name, const gchar *license_agreement, gpointer data)
{
	gboolean import;

	if (awaiting_space) {
		g_print ("\n");
	}
	g_print ("Eula Required\n");
	g_print ("Eula:        %s\n", eula_id);
	g_print ("Package:     %s\n", package_id);
	g_print ("Vendor:      %s\n", vendor_name);
	g_print ("Agreement:   %s\n", license_agreement);

	/* get user input */
	import = pk_console_get_prompt (_("Do you agree?"), FALSE);
	if (!import) {
		g_print ("%s\n", _("Did not agree to licence, task will fail"));
		return;
	}
	g_print ("Importing licences is not yet supported!\n");
}

/**
 * pk_connection_changed_cb:
 **/
static void
pk_connection_changed_cb (PkConnection *pconnection, gboolean connected, gpointer data)
{
	/* if the daemon crashed, don't hang around */
	if (awaiting_space) {
		g_print ("\n");
	}
	if (connected == FALSE) {
		g_print (_("The daemon crashed mid transaction. This is bad\n"));
		exit (2);
	}
}

/**
 * pk_console_sigint_handler:
 **/
static void
pk_console_sigint_handler (int sig)
{
	PkRoleEnum role;
	gboolean ret;
	GError *error = NULL;
	pk_debug ("Handling SIGINT");

	/* restore default ASAP, as the cancels might hang */
	signal (SIGINT, SIG_DFL);

	/* cancel any tasks */
	pk_client_get_role (client, &role, NULL, NULL);
	if (role != PK_ROLE_ENUM_UNKNOWN) {
		ret = pk_client_cancel (client, &error);
		if (!ret) {
			pk_warning ("failed to cancel normal client: %s", error->message);
			g_error_free (error);
			error = NULL;
		}
	}
	pk_client_get_role (client_task, &role, NULL, NULL);
	if (role != PK_ROLE_ENUM_UNKNOWN) {
		ret = pk_client_cancel (client_task, &error);
		if (!ret) {
			pk_warning ("failed to cancel task client: %s", error->message);
			g_error_free (error);
		}
	}

	/* kill ourselves */
	pk_debug ("Retrying SIGINT");
	kill (getpid (), SIGINT);
}

/**
 * pk_console_get_summary:
 **/
static gchar *
pk_console_get_summary (PkRoleEnum roles)
{
	GString *string;
	string = g_string_new ("");

	/* header */
	g_string_append_printf (string, "%s\n\n%s\n", _("PackageKit Console Interface"), _("Subcommands:"));

	/* always */
	g_string_append_printf (string, "  %s\n", "get-actions");
	g_string_append_printf (string, "  %s\n", "get-groups");
	g_string_append_printf (string, "  %s\n", "get-filters");
	g_string_append_printf (string, "  %s\n", "get-transactions");
	g_string_append_printf (string, "  %s\n", "get-time");

	if (pk_enums_contain (roles, PK_ROLE_ENUM_SEARCH_NAME) ||
	    pk_enums_contain (roles, PK_ROLE_ENUM_SEARCH_DETAILS) ||
	    pk_enums_contain (roles, PK_ROLE_ENUM_SEARCH_GROUP) ||
	    pk_enums_contain (roles, PK_ROLE_ENUM_SEARCH_FILE)) {
		g_string_append_printf (string, "  %s\n", "search [name|details|group|file] [data]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_INSTALL_PACKAGE) ||
	    pk_enums_contain (roles, PK_ROLE_ENUM_INSTALL_FILE)) {
		g_string_append_printf (string, "  %s\n", "install [package|file]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_INSTALL_SIGNATURE)) {
		g_string_append_printf (string, "  %s\n", "install-sig [type] [key_id] [package_id]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_REMOVE_PACKAGE)) {
		g_string_append_printf (string, "  %s\n", "remove [package]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_UPDATE_SYSTEM) ||
	    pk_enums_contain (roles, PK_ROLE_ENUM_UPDATE_PACKAGES)) {
		g_string_append_printf (string, "  %s\n", "update <package>");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_REFRESH_CACHE)) {
		g_string_append_printf (string, "  %s\n", "refresh");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_RESOLVE)) {
		g_string_append_printf (string, "  %s\n", "resolve [package]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_GET_UPDATES)) {
		g_string_append_printf (string, "  %s\n", "get-updates");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_GET_DEPENDS)) {
		g_string_append_printf (string, "  %s\n", "get-depends [package]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_GET_REQUIRES)) {
		g_string_append_printf (string, "  %s\n", "get-requires [package]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_GET_DESCRIPTION)) {
		g_string_append_printf (string, "  %s\n", "get-description [package]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_GET_FILES)) {
		g_string_append_printf (string, "  %s\n", "get-files [package]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_GET_UPDATE_DETAIL)) {
		g_string_append_printf (string, "  %s\n", "get-update-detail [package]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_GET_PACKAGES)) {
		g_string_append_printf (string, "  %s\n", "get-packages");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_GET_REPO_LIST)) {
		g_string_append_printf (string, "  %s\n", "repo-list");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_REPO_ENABLE)) {
		g_string_append_printf (string, "  %s\n", "repo-enable [repo_id]");
		g_string_append_printf (string, "  %s\n", "repo-disable [repo_id]");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_REPO_SET_DATA)) {
		g_string_append_printf (string, "  %s\n", "repo-set-data [repo_id] [parameter] [value];");
	}
	if (pk_enums_contain (roles, PK_ROLE_ENUM_WHAT_PROVIDES)) {
		g_string_append_printf (string, "  %s\n", "what-provides [search]");
	}
	return g_string_free (string, FALSE);
}

/**
 * main:
 **/
int
main (int argc, char *argv[])
{
	DBusGConnection *system_connection;
	GError *error = NULL;
	PkConnection *pconnection;
	gboolean verbose = FALSE;
	gboolean program_version = FALSE;
	GOptionContext *context;
	gchar *options_help;
	gchar *filter = NULL;
	gchar *summary;
	gboolean ret;
	const gchar *mode;
	const gchar *value = NULL;
	const gchar *details = NULL;
	const gchar *parameter = NULL;
	PkRoleEnum roles;
	PkGroupEnum groups;
	gchar *text;
	ret = FALSE;
	gboolean maybe_sync = TRUE;
	PkFilterEnum filters = 0;

	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			_("Show extra debugging information"), NULL },
		{ "version", '\0', 0, G_OPTION_ARG_NONE, &program_version,
			_("Show the program version and exit"), NULL},
		{ "filter", '\0', 0, G_OPTION_ARG_STRING, &filter,
			_("Set the filter, e.g. installed"), NULL},
		{ "nowait", 'n', 0, G_OPTION_ARG_NONE, &nowait,
			_("Exit without waiting for actions to complete"), NULL},
		{ NULL}
	};

	if (! g_thread_supported ()) {
		g_thread_init (NULL);
	}
	dbus_g_thread_init ();
	g_type_init ();

	/* do stuff on ctrl-c */
	signal (SIGINT, pk_console_sigint_handler);

	/* check if we are on console */
	if (isatty (fileno (stdout)) == 1) {
		is_console = TRUE;
	}

	/* check dbus connections, exit if not valid */
	system_connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error) {
		pk_warning ("%s", error->message);
		g_error_free (error);
		g_error (_("Could not connect to system DBUS."));
	}

	/* we need the roles early, as we only show the user only what they can do */
	control = pk_control_new ();
	roles = pk_control_get_actions (control);
	summary = pk_console_get_summary (roles);

	context = g_option_context_new ("PackageKit Console Program");
	g_option_context_set_summary (context, summary) ;
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_parse (context, &argc, &argv, NULL);
	/* Save the usage string in case command parsing fails. */
	options_help = g_option_context_get_help (context, TRUE, NULL);
	g_option_context_free (context);

	pk_debug_init (verbose);

	if (program_version) {
		g_print (VERSION "\n");
		return 0;
	}

	if (argc < 2) {
		g_print ("%s", options_help);
		return 1;
	}

	loop = g_main_loop_new (NULL, FALSE);

	pconnection = pk_connection_new ();
	g_signal_connect (pconnection, "connection-changed",
			  G_CALLBACK (pk_connection_changed_cb), loop);

	client = pk_client_new ();
	pk_client_set_use_buffer (client, TRUE, NULL);
	g_signal_connect (client, "package",
			  G_CALLBACK (pk_console_package_cb), NULL);
	g_signal_connect (client, "transaction",
			  G_CALLBACK (pk_console_transaction_cb), NULL);
	g_signal_connect (client, "description",
			  G_CALLBACK (pk_console_description_cb), NULL);
	g_signal_connect (client, "files",
			  G_CALLBACK (pk_console_files_cb), NULL);
	g_signal_connect (client, "repo-signature-required",
			  G_CALLBACK (pk_console_repo_signature_required_cb), NULL);
	g_signal_connect (client, "eula-required",
			  G_CALLBACK (pk_console_eula_required_cb), NULL);
	g_signal_connect (client, "update-detail",
			  G_CALLBACK (pk_console_update_detail_cb), NULL);
	g_signal_connect (client, "repo-detail",
			  G_CALLBACK (pk_console_repo_detail_cb), NULL);
	g_signal_connect (client, "progress-changed",
			  G_CALLBACK (pk_console_progress_changed_cb), NULL);
	g_signal_connect (client, "finished",
			  G_CALLBACK (pk_console_finished_cb), NULL);
	g_signal_connect (client, "error-code",
			  G_CALLBACK (pk_console_error_code_cb), NULL);

	client_task = pk_client_new ();
	pk_client_set_use_buffer (client_task, TRUE, NULL);
	pk_client_set_synchronous (client_task, TRUE, NULL);
	g_signal_connect (client_task, "finished",
			  G_CALLBACK (pk_console_finished_cb), NULL);

	client_signature = pk_client_new ();
	g_signal_connect (client_signature, "finished",
			  G_CALLBACK (pk_console_signature_finished_cb), NULL);

	if (filter != NULL) {
		filters = pk_filter_enums_from_text (filter);
	}
	pk_debug ("filter=%s, filters=%i", filter, filters);

	mode = argv[1];
	if (argc > 2) {
		value = argv[2];
	}
	if (argc > 3) {
		details = argv[3];
	}
	if (argc > 4) {
		parameter = argv[4];
	}

	/* parse the big list */
	if (strcmp (mode, "search") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a search type"));
			goto out;

		} else if (strcmp (value, "name") == 0) {
			if (details == NULL) {
				g_print (_("You need to specify a search term"));
				goto out;
			}
			ret = pk_client_search_name (client, filters, details, &error);

		} else if (strcmp (value, "details") == 0) {
			if (details == NULL) {
				g_print (_("You need to specify a search term"));
				goto out;
			}
			ret = pk_client_search_details (client, filters, details, &error);

		} else if (strcmp (value, "group") == 0) {
			if (details == NULL) {
				g_print (_("You need to specify a search term"));
				goto out;
			}
			ret = pk_client_search_group (client, filters, details, &error);

		} else if (strcmp (value, "file") == 0) {
			if (details == NULL) {
				g_print (_("You need to specify a search term"));
				goto out;
			}
			ret = pk_client_search_file (client, filters, details, &error);
		} else {
			g_print (_("Invalid search type"));
		}

	} else if (strcmp (mode, "install") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a package or file to install"));
			goto out;
		}
		/* is it a local file? */
		ret = g_file_test (value, G_FILE_TEST_EXISTS);
		if (ret) {
			ret = pk_client_install_file (client, value, &error);
		} else {
			ret = pk_console_install_package (client, value, &error);
		}

	} else if (strcmp (mode, "install-sig") == 0) {
		if (value == NULL || details == NULL || parameter == NULL) {
			g_print (_("You need to specify a type, key_id and package_id"));
			goto out;
		}
		ret = pk_client_install_signature (client, PK_SIGTYPE_ENUM_GPG, details, parameter, &error);

	} else if (strcmp (mode, "remove") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a package to remove"));
			goto out;
		}
		ret = pk_console_remove_package (client, value, &error);

	} else if (strcmp (mode, "update") == 0) {
		if (value == NULL) {
			/* do the system update */
			ret = pk_client_update_system (client, &error);
		}
		ret = pk_console_update_package (client, value, &error);

	} else if (strcmp (mode, "resolve") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a package name to resolve"));
			goto out;
		}
		ret = pk_client_resolve (client, filters, value, &error);

	} else if (strcmp (mode, "repo-enable") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a repo name"));
			goto out;
		}
		ret = pk_client_repo_enable (client, value, TRUE, &error);

	} else if (strcmp (mode, "repo-disable") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a repo name"));
			goto out;
		}
		ret = pk_client_repo_enable (client, value, FALSE, &error);

	} else if (strcmp (mode, "repo-set-data") == 0) {
		if (value == NULL || details == NULL || parameter == NULL) {
			g_print (_("You need to specify a repo name/parameter and value"));
			goto out;
		}
		ret = pk_client_repo_set_data (client, value, details, parameter, &error);

	} else if (strcmp (mode, "repo-list") == 0) {
		ret = pk_client_get_repo_list (client, filters, &error);

	} else if (strcmp (mode, "get-time") == 0) {
		PkRoleEnum role;
		guint time;
		gboolean ret;
		if (value == NULL) {
			g_print (_("You need to specify a time term"));
			goto out;
		}
		role = pk_role_enum_from_text (value);
		if (role == PK_ROLE_ENUM_UNKNOWN) {
			g_print (_("You need to specify a correct role"));
			goto out;
		}
		ret = pk_control_get_time_since_action (control, role, &time, &error);
		if (ret == FALSE) {
			g_print (_("Failed to get last time"));
			goto out;
		}
		g_print ("time since %s is %is\n", value, time);
		maybe_sync = FALSE;

	} else if (strcmp (mode, "get-depends") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a search term"));
			goto out;
		}
		ret = pk_console_get_depends (client, value, &error);

	} else if (strcmp (mode, "get-update-detail") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a search term"));
			goto out;
		}
		ret = pk_console_get_update_detail (client, value, &error);

	} else if (strcmp (mode, "get-requires") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a search term"));
			goto out;
		}
		ret = pk_console_get_requires (client, value, &error);

	} else if (strcmp (mode, "what-provides") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a search term"));
			goto out;
		}
		ret = pk_client_what_provides (client, filters, PK_PROVIDES_ENUM_CODEC, value, &error);

	} else if (strcmp (mode, "get-description") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a package to find the description for"));
			goto out;
		}
		ret = pk_console_get_description (client, value, &error);

	} else if (strcmp (mode, "get-files") == 0) {
		if (value == NULL) {
			g_print (_("You need to specify a package to find the files for"));
			goto out;
		}
		ret = pk_console_get_files (client, value, &error);

	} else if (strcmp (mode, "get-updates") == 0) {
		ret = pk_client_get_updates (client, filters, &error);

	} else if (strcmp (mode, "get-packages") == 0) {
		ret = pk_client_get_packages (client, filters, &error);

	} else if (strcmp (mode, "get-actions") == 0) {
		roles = pk_control_get_actions (control);
		text = pk_role_enums_to_text (roles);
		g_print ("roles=%s\n", text);
		g_free (text);
		maybe_sync = FALSE;
		/* these can never fail */
		ret = TRUE;

	} else if (strcmp (mode, "get-filters") == 0) {
		filters = pk_control_get_filters (control);
		text = pk_filter_enums_to_text (filters);
		g_print ("filters=%s\n", text);
		g_free (text);
		maybe_sync = FALSE;
		/* these can never fail */
		ret = TRUE;

	} else if (strcmp (mode, "get-groups") == 0) {
		groups = pk_control_get_groups (control);
		text = pk_group_enums_to_text (groups);
		g_print ("groups=%s\n", text);
		g_free (text);
		maybe_sync = FALSE;
		/* these can never fail */
		ret = TRUE;

	} else if (strcmp (mode, "get-transactions") == 0) {
		ret = pk_client_get_old_transactions (client, 10, &error);

	} else if (strcmp (mode, "refresh") == 0) {
		ret = pk_client_refresh_cache (client, FALSE, &error);

	} else {
		g_print (_("Option '%s' not supported\n"), mode);
	}

	/* do we wait for the method? */
	if (maybe_sync && !nowait && ret) {
		g_main_loop_run (loop);
	}

out:
	if (ret == FALSE) {
		g_print (_("Command failed\n"));
	}
	if (error != NULL) {
		if (g_str_has_prefix (error->message, "org.freedesktop.packagekit."))  {
			g_print (_("You don't have the necessary privileges for this operation\n"));
		} else {
			g_print ("Error:\n  %s\n\n", error->message);
			g_error_free (error);
		}
	}

	g_free (options_help);
	g_free (filter);
	g_free (summary);
	g_object_unref (control);
	g_object_unref (client);
	g_object_unref (client_task);
	g_object_unref (client_signature);

	return 0;
}
