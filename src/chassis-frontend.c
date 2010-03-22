/* $%BEGINLICENSE%$
 Copyright (C) 2010, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 $%ENDLICENSE%$ */
 

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <io.h> /* open, close, ...*/
#endif
#include <errno.h>

#include <glib.h>
#include <gmodule.h>
#include <lua.h> /* for LUA_PATH */

#include <event.h>

#include "chassis-frontend.h"
#include "chassis-path.h"
#include "chassis-plugin.h"
#include "chassis-keyfile.h"
#include "chassis-filemode.h"
#include "chassis-options.h"

/**
 * initialize the basic components of the chassis
 */
int chassis_frontend_init_glib() {
	const gchar *check_str = NULL;
#if 0
	g_mem_set_vtable(glib_mem_profiler_table);
#endif

	if (!GLIB_CHECK_VERSION(2, 6, 0)) {
		g_critical("the glib header are too old, need at least 2.6.0, got: %d.%d.%d", 
				GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

		return -1;
	}

	check_str = glib_check_version(GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

	if (check_str) {
		g_critical("%s, got: lib=%d.%d.%d, headers=%d.%d.%d", 
			check_str,
			glib_major_version, glib_minor_version, glib_micro_version,
			GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

		return -1;
	}

	if (!g_module_supported()) {
		g_critical("loading modules is not supported on this platform");
		return -1;
	}

	g_thread_init(NULL);

	return 0;
}

/**
 * init the win32 specific components
 *
 * - setup winsock32
 */
int chassis_frontend_init_win32() {
#ifdef _WIN32
	WSADATA wsaData;

	if (0 != WSAStartup(MAKEWORD( 2, 2 ), &wsaData)) {
		g_critical("%s: WSAStartup(2, 2) failed to initialize the socket library",
				G_STRLOC);

		return -1;
	}

	return 0;
#else
	return -1;
#endif
}

/**
 * setup and check the basedir if nessesary 
 */
int chassis_frontend_init_basedir(const char *prg_name, char **_base_dir) {
	char *base_dir = *_base_dir;

	if (base_dir) { /* basedir is already known, check if it is absolute */
		if (!g_path_is_absolute(base_dir)) {
			g_critical("%s: --basedir option must be an absolute path, but was %s",
					G_STRLOC,
					base_dir);
			return -1;
		} else {
			return 0;
		}
	}

	/* find our installation directory if no basedir was given
	 * this is necessary for finding files when we daemonize
	 */
	base_dir = chassis_get_basedir(prg_name);
	if (!base_dir) {
		g_critical("%s: Failed to get base directory",
				G_STRLOC);
		return -1;
	}

	*_base_dir = base_dir;

	return 0;

}

/**
 * set the environment as Lua expects it
 *
 * on Win32 glib uses _wputenv to set the env variable,
 * but Lua uses getenv. Those two don't see each other,
 * so we use _putenv. Since we only set ASCII chars, this
 * is safe.
 */
static int chassis_frontend_lua_setenv(const char *key, const char *value) {
	int r;
#if _WIN32
	r = _putenv_s(key, value);
#else
	r = g_setenv(key, value, 1) ? 0 : -1; /* g_setenv() returns TRUE/FALSE */
#endif

	if (0 == r) {
		/* the setenv() succeeded, double-check it */
		if (!getenv(key)) {
			/* check that getenv() returns what we did set */
			g_critical("%s: setting %s = %s failed: (getenv() == NULL)", G_STRLOC,
					key, value);
		} else if (0 != strcmp(getenv(key), value)) {
			g_critical("%s: setting %s = %s failed: (getenv() == %s)", G_STRLOC,
					key, value,
					getenv(key));
		}
	}

	return r;
}

/**
 * get the default value for LUA_PATH
 */
char *chassis_frontend_get_default_lua_path(const char *base_dir, const char *prg_name) {
	return g_build_filename(base_dir, "lib", prg_name, "lua", "?.lua", NULL);
}

/**
 * get the default value for LUA_PATH
 */
char *chassis_frontend_get_default_lua_cpath(const char *base_dir, const char *prg_name) {
	/* each OS has its own way of declaring a shared-lib extension
	 *
	 * win32 has .dll
	 * macosx has .so or .dylib
	 * hpux has .sl
	 */ 
#  if _WIN32
	return g_build_filename(base_dir, "bin", "lua-?." G_MODULE_SUFFIX, NULL);
#  else
	return g_build_filename(base_dir, "lib", prg_name, "lua", "?." G_MODULE_SUFFIX, NULL);
#  endif
}

/**
 * set the LUA_PATH
 */
int chassis_frontend_init_lua_path(const char *set_path, const char *base_dir, const char *prg_name) {
	/**
	 * if the LUA_PATH or LUA_CPATH are not set, set a good default 
	 *
	 * we want to derive it from the basedir ...
	 */
	if (set_path) {
		if (0 != chassis_frontend_lua_setenv(LUA_PATH, set_path)) {
			g_critical("%s: setting %s = %s failed: %s", G_STRLOC,
					LUA_PATH, set_path,
					g_strerror(errno));
		}
	} else if (!g_getenv(LUA_PATH)) {
		gchar *path = chassis_frontend_get_default_lua_path(base_dir, prg_name);

		if (chassis_frontend_lua_setenv(LUA_PATH, path)) {
			g_critical("%s: setting %s = %s failed: %s", G_STRLOC,
					LUA_PATH, path,
					g_strerror(errno));
		}
		
		g_free(path);
	}

	return 0;
}

/**
 * set the LUA_CPATH 
 */
int chassis_frontend_init_lua_cpath(const char *set_path, const char *base_dir, const char *prg_name) {
	if (set_path) {
		if (chassis_frontend_lua_setenv(LUA_CPATH, set_path)) {
			g_critical("%s: setting %s = %s failed: %s", G_STRLOC,
					LUA_CPATH, set_path,
					g_strerror(errno));
		}
	} else if (!g_getenv(LUA_CPATH)) {
		gchar *path = chassis_frontend_get_default_lua_cpath(base_dir, prg_name);

		if (chassis_frontend_lua_setenv(LUA_CPATH, path)) {
			g_critical("%s: setting %s = %s failed: %s", G_STRLOC,
					LUA_CPATH, path,
					g_strerror(errno));
		}

		g_free(path);
	}

	return 0;
}

int chassis_frontend_init_plugin_dir(char **_plugin_dir, const char *base_dir) {
	char *plugin_dir = *_plugin_dir;

	if (plugin_dir) return 0;

#ifdef WIN32
	plugin_dir = g_build_filename(base_dir, "bin", NULL);
#else
	plugin_dir = g_build_filename(base_dir, "lib", PACKAGE, "plugins", NULL);
#endif

	*_plugin_dir = plugin_dir;

	return 0;
}

int chassis_frontend_load_plugins(GPtrArray *plugins, const gchar *plugin_dir, gchar **plugin_names) {
	int i;

	/* load the plugins */
	for (i = 0; plugin_names && plugin_names[i]; i++) {
		chassis_plugin *p;
#ifdef WIN32
#define G_MODULE_PREFIX "plugin-" /* we build the plugins with a prefix on win32 to avoid name-clashing in bin/ */
#else
#define G_MODULE_PREFIX "lib"
#endif
/* we have to hack around some glib distributions that
 * don't set the correct G_MODULE_SUFFIX, notably MacPorts
 */
#ifndef SHARED_LIBRARY_SUFFIX
#define SHARED_LIBRARY_SUFFIX G_MODULE_SUFFIX
#endif
		char *plugin_filename;
		/* skip trying to load a plugin when the parameter was --plugins= 
		   that will never work...
		*/
		if (!g_strcmp0("", plugin_names[i])) {
			continue;
		}

		plugin_filename = g_strdup_printf("%s%c%s%s.%s", 
				plugin_dir, 
				G_DIR_SEPARATOR, 
				G_MODULE_PREFIX,
				plugin_names[i],
				SHARED_LIBRARY_SUFFIX);

		p = chassis_plugin_load(plugin_filename);
		g_free(plugin_filename);

		if (NULL == p) {
			g_critical("setting --plugin-dir=<dir> might help");
			return -1;
		}

		g_ptr_array_add(plugins, p);
	}
	return 0;
}

int chassis_frontend_init_plugins(GPtrArray *plugins,
		GOptionContext *option_ctx,
		int *argc_p, char ***argv_p,
		GKeyFile *keyfile,
		const char *base_dir) {
	guint i;

	for (i = 0; i < plugins->len; i++) {
		GOptionEntry *config_entries;
		chassis_plugin *p = plugins->pdata[i];

		if (NULL != (config_entries = chassis_plugin_get_options(p))) {
			GError *gerr = NULL;
			gchar *group_desc = g_strdup_printf("%s-module", p->name);
			gchar *help_msg = g_strdup_printf("Show options for the %s-module", p->name);
			const gchar *group_name = p->name;

			GOptionGroup *option_grp = g_option_group_new(group_name, group_desc, help_msg, NULL, NULL);
			g_option_group_add_entries(option_grp, config_entries);
			g_option_context_add_group(option_ctx, option_grp);

			g_free(help_msg);
			g_free(group_desc);

			/* parse the new options */
			if (FALSE == g_option_context_parse(option_ctx, argc_p, argv_p, &gerr)) {
				g_critical("%s", gerr->message);
				g_clear_error(&gerr);

				return -1;
			}
	
			if (keyfile) {
				if (chassis_keyfile_to_options(keyfile, "mysql-proxy", config_entries)) {
					return -1;
				}
			}

			/* resolve the path names for these config entries */
			chassis_keyfile_resolve_path(base_dir, config_entries); 
		}
	}

	return 0;
}
int chassis_frontend_init_base_options(GOptionContext *option_ctx,
		int *argc_p, char ***argv_p,
		int *print_version,
		char **config_file) {
	chassis_options_t *opts;
	GOptionEntry *base_main_entries;
	GError *gerr = NULL;
	int ret = 0;

	opts = chassis_options_new();
	chassis_options_set_cmdline_only_options(opts, print_version, config_file);
	base_main_entries = chassis_options_to_g_option_entries(opts);

	g_option_context_add_main_entries(option_ctx, base_main_entries, NULL);
	g_option_context_set_help_enabled(option_ctx, FALSE);
	g_option_context_set_ignore_unknown_options(option_ctx, TRUE);

	if (FALSE == g_option_context_parse(option_ctx, argc_p, argv_p, &gerr)) {
		g_clear_error(&gerr);
		ret = -1;
	}

	g_free(base_main_entries);
	chassis_options_free(opts);

	return ret;
}

GKeyFile *chassis_frontend_open_config_file(const char *filename) {
	GKeyFile *keyfile;
	GError *gerr = NULL;

	if (chassis_filemode_check(filename) != 0) {
		return NULL;
	}
	keyfile = g_key_file_new();
	g_key_file_set_list_separator(keyfile, ',');

	if (FALSE == g_key_file_load_from_file(keyfile, filename, G_KEY_FILE_NONE, &gerr)) {
		g_critical("%s: loading configuration from %s failed: %s", 
				G_STRLOC,
				filename,
				gerr->message);
		g_clear_error(&gerr);

		g_key_file_free(keyfile);

		return NULL;
	}

	return keyfile;
}

/**
 * setup the options that can only appear on the command-line
 */
int chassis_options_set_cmdline_only_options(chassis_options_t *opts,
		int *print_version,
		char **config_file) {

	chassis_options_add(opts,
		"version", 'V', 0, G_OPTION_ARG_NONE, print_version, "Show version", NULL);

	chassis_options_add(opts,
		"defaults-file", 0, 0, G_OPTION_ARG_STRING, config_file, "configuration file", "<file>");

	return 0;
}


int chassis_frontend_print_version() {
	/* allow to pass down a build-tag at build-time which gets hard-coded into the binary */
#ifndef CHASSIS_BUILD_TAG
#define CHASSIS_BUILD_TAG PACKAGE_STRING
#endif
	g_print("%s" CHASSIS_NEWLINE, CHASSIS_BUILD_TAG); 
	g_print("  glib2: %d.%d.%d" CHASSIS_NEWLINE, GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
#ifdef HAVE_EVENT_H
	g_print("  libevent: %s" CHASSIS_NEWLINE, event_get_version());
#endif

	return 0;
}

int chassis_frontend_write_pidfile(const char *pid_file, GError **gerr) {
	int fd;
	int ret = 0;

	gchar *pid_str;

	/**
	 * write the PID file
	 */

	if (-1 == (fd = open(pid_file, O_WRONLY|O_TRUNC|O_CREAT, 0600))) {
		g_set_error(gerr,
				G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"%s: open(%s) failed: %s", 
				G_STRLOC,
				pid_file,
				g_strerror(errno));

		return -1;
	}

	pid_str = g_strdup_printf("%d", getpid());
	if (write(fd, pid_str, strlen(pid_str)) < 0) {
		g_set_error(gerr,
				G_FILE_ERROR,
				g_file_error_from_errno(errno),
				"%s: write(%s) of %s failed: %s", 
				G_STRLOC,
				pid_file,
				pid_str,
				g_strerror(errno));
		ret = -1;
	}
	g_free(pid_str);

	close(fd);

	return ret;
}

