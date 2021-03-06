/* X-Chat 2.0 PERL Plugin
 * Copyright (C) 1998-2002 Peter Zelezny.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef ENABLE_NLS
#include <locale.h>
#endif
#ifdef WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#undef PACKAGE
#include "../../config.h"		  /* for #define OLD_PERL */
#include "xchat-plugin.h"

static xchat_plugin *ph;		  /* plugin handle */

static int perl_load_file (char *script_name);

#ifdef WIN32
/* STRINGIFY is from perl's CORE/config.h */
#ifndef PERL_REQUIRED_VERSION
	#define PERL_REQUIRED_VERSION STRINGIFY(PERL_REVISION) "." STRINGIFY(PERL_VERSION)
#endif

#ifndef PERL_DLL
	#define PERL_DLL "perl" STRINGIFY(PERL_REVISION) STRINGIFY(PERL_VERSION) ".dll"
#endif

static DWORD
child (char *str)
{
	MessageBoxA (0, str, "Perl DLL Error",
					 MB_OK | MB_ICONHAND | MB_SETFOREGROUND | MB_TASKMODAL);
	return 0;
}

static void
thread_mbox (char *str)
{
	DWORD tid;

	CloseHandle (CreateThread (NULL, 0, (LPTHREAD_START_ROUTINE) child,
										str, 0, &tid));
}

#endif

/* leave this before XSUB.h, to avoid readdir() being redefined */

#ifdef WIN32
static void
perl_auto_load_from_path (const char *path)
{
	WIN32_FIND_DATA find_data;
	HANDLE find_handle;
	char *search_path;
	int path_len = strlen (path);

	/* +6 for \*.pl and \0 */
	search_path = malloc(path_len + 6);
	sprintf (search_path, "%s\\*.pl", path);

	find_handle = FindFirstFile (search_path, &find_data);

	if (find_handle != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY
				||find_data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
			{
				char *full_path =
					malloc (path_len + strlen (find_data.cFileName) + 2);
				sprintf (full_path, "%s\\%s", path, find_data.cFileName);

				perl_load_file (full_path);
				free (full_path);
			}
		}
		while (FindNextFile (find_handle, &find_data) != 0);
		FindClose (find_handle);
	}

	free (search_path);
}
#else
static void
perl_auto_load_from_path (const char *path)
{
	DIR *dir;
	struct dirent *ent;

	dir = opendir (path);
	if (dir) {
		while ((ent = readdir (dir))) {
			int len = strlen (ent->d_name);
			if (len > 3 && strcasecmp (".pl", ent->d_name + len - 3) == 0) {
				char *file = malloc (len + strlen (path) + 2);
				sprintf (file, "%s/%s", path, ent->d_name);
				perl_load_file (file);
				free (file);
			}
		}
		closedir (dir);
	}
}
#endif

static int
perl_auto_load (void *unused)
{
	const char *xdir;
	char *sub_dir;
#ifdef WIN32
	int copied = 0;
	char *slash = NULL;
#endif

	/* get the dir in local filesystem encoding (what opendir() expects!) */
	xdir = xchat_get_info (ph, "xchatdirfs");
	if (!xdir)			/* xchatdirfs is new for 2.0.9, will fail on older */
		xdir = xchat_get_info (ph, "xchatdir");

	/* autoload from ~/.xchat2/ or ${APPDATA}\X-Chat 2\ on win32 */
	perl_auto_load_from_path (xdir);

	sub_dir = malloc (strlen (xdir) + 9);
	strcpy (sub_dir, xdir);
	strcat (sub_dir, "/plugins");
	perl_auto_load_from_path (sub_dir);
	free (sub_dir);

#ifdef WIN32
	/* autoload from  C:\program files\xchat\plugins\ */
	sub_dir = malloc (1025 + 9);
	copied = GetModuleFileName( 0, sub_dir, 1024 );
	sub_dir[copied] = '\0';
	slash = strrchr( sub_dir, '\\' );
	if( slash != NULL ) {
		*slash = '\0';
	}
	perl_auto_load_from_path ( strncat (sub_dir, "\\plugins", 9));
	free (sub_dir);
#endif
	return 0;
}

#include <EXTERN.h>
#define WIN32IOP_H
#include <perl.h>
#include <XSUB.h>

typedef struct
{
	SV *callback;
	SV *userdata;
	xchat_hook *hook;   /* required for timers */
	xchat_context *ctx; /* allow timers to remember their context */
	SV *package;      /* need to track the package name when removing hooks
	                       by returning REMOVE
							   */
	unsigned int depth;
} HookData;

static PerlInterpreter *my_perl = NULL;
extern void boot_DynaLoader (pTHX_ CV * cv);

/*
  this is used for autoload and shutdown callbacks
*/
static int
execute_perl (SV * function, char *args)
{

	int count, ret_value = 1;

	dSP;
	ENTER;
	SAVETMPS;

	PUSHMARK (SP);
	XPUSHs (sv_2mortal (newSVpv (args, 0)));
	PUTBACK;

	count = call_sv (function, G_EVAL | G_SCALAR);
	SPAGAIN;
	if (SvTRUE (ERRSV)) {
		xchat_printf(ph, "Perl error: %s\n", SvPV_nolen (ERRSV));
		if (!SvOK (POPs)) {}		/* remove undef from the top of the stack */
	} else if (count != 1) {
		xchat_printf (ph, "Perl error: expected 1 value from %s, "
						  "got: %d\n", SvPV_nolen (function), count);
	} else {
		ret_value = POPi;
	}
	PUTBACK;
	FREETMPS;
	LEAVE;

	return ret_value;
}

static char *
get_filename (char *word[], char *word_eol[])
{
	int len;
	char *file;

	len = strlen (word[2]);

	/* if called as /load "filename.pl" the only difference between word and
	 * word_eol will be the two quotes
	 */
	
	if (strchr (word[2], ' ') != NULL
		|| (strlen (word_eol[2]) - strlen(word[2])) == 2 )
	{
		file = word[2];
	} else {
		file = word_eol[2];
	}

	len = strlen (file);

	if (len > 3 && strncasecmp (".pl", file + len - 3, 3) == 0) {
		return file;
	}

	return NULL;
}

static SV *
list_item_to_sv ( xchat_list *list, const char *const *fields )
{
	HV *hash = newHV();
	SV *field_value;
	const char *field;
	int field_index = 0;
	const char *field_name;
	int name_len;

	while (fields[field_index] != NULL) {
		field_name = fields[field_index] + 1;
		name_len = strlen (field_name);

		switch (fields[field_index][0]) {
		case 's':
			field = xchat_list_str (ph, list, field_name);
			if (field != NULL) {
				field_value = newSVpvn (field, strlen (field));
			} else {
				field_value = &PL_sv_undef;
			}
			break;
		case 'p':
			field_value = newSViv (PTR2IV (xchat_list_str (ph, list,
																	 field_name)));
			break;
		case 'i':
			field_value = newSVuv (xchat_list_int (ph, list, field_name));
			break;
		case 't':
			field_value = newSVnv (xchat_list_time (ph, list, field_name));
			break;
		default:
			field_value = &PL_sv_undef;
		}
		hv_store (hash, field_name, name_len, field_value, 0);
		field_index++;
	}
	return sv_2mortal (newRV_noinc ((SV *) hash));
}

static AV *
array2av (char *array[])
{
	int count = 0;
	SV *temp = NULL;
	AV *av = newAV();
	sv_2mortal ((SV *)av);

	for (
		count = 1;
		count < 32 && array[count] != NULL && array[count][0] != 0;
		count++
	) {
		temp = newSVpv (array[count], 0);
		SvUTF8_on (temp);
		av_push (av, temp);
	}

	return av;
}

/* sets $Xchat::Embed::current_package */
static void
set_current_package (SV *package)
{
	SV *current_package = get_sv ("Xchat::Embed::current_package", 1);
	SvSetSV_nosteal (current_package, package);
}

static int
fd_cb (int fd, int flags, void *userdata)
{
	HookData *data = (HookData *) userdata;
	int retVal = 0;
	int count = 0;

	dSP;
	ENTER;
	SAVETMPS;

	PUSHMARK (SP);
	XPUSHs (data->userdata);
	PUTBACK;

	set_current_package (data->package);
	count = call_sv (data->callback, G_EVAL);
	set_current_package (&PL_sv_undef);
	SPAGAIN;

	if (SvTRUE (ERRSV)) {
		xchat_printf (ph, "Error in fd callback %s", SvPV_nolen (ERRSV));
		if (!SvOK (POPs)) {}		  /* remove undef from the top of the stack */
		retVal = XCHAT_EAT_ALL;
	} else {
		if (count != 1) {
			xchat_print (ph, "Fd handler should only return 1 value.");
			retVal = XCHAT_EAT_NONE;
		} else {
			retVal = POPi;
			if (retVal == 0) {
				/* if 0 is returned, the fd is going to get unhooked */
				PUSHMARK (SP);
				XPUSHs (sv_2mortal (newSViv (PTR2IV (data->hook))));
				PUTBACK;

				call_pv ("Xchat::unhook", G_EVAL);
				SPAGAIN;

				SvREFCNT_dec (data->callback);

				if (data->userdata) {
					SvREFCNT_dec (data->userdata);
				}
				free (data);
			}
		}

	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retVal;
}

static int
timer_cb (void *userdata)
{
	HookData *data = (HookData *) userdata;
	int retVal = 0;
	int count = 0;

	dSP;
	ENTER;
	SAVETMPS;

	PUSHMARK (SP);
	XPUSHs (data->userdata);
	PUTBACK;

	if (data->ctx) {
		xchat_set_context (ph, data->ctx);
	}

	set_current_package (data->package);
	count = call_sv (data->callback, G_EVAL);
	set_current_package (&PL_sv_undef);
	SPAGAIN;

	if (SvTRUE (ERRSV)) {
		xchat_printf (ph, "Error in timer callback %s", SvPV_nolen (ERRSV));
		if (!SvOK (POPs)) {}		  /* remove undef from the top of the stack */
		retVal = XCHAT_EAT_ALL;
	} else {
		if (count != 1) {
			xchat_print (ph, "Timer handler should only return 1 value.");
			retVal = XCHAT_EAT_NONE;
		} else {
			retVal = POPi;
			if (retVal == 0) {
				/* if 0 is return the timer is going to get unhooked */
				PUSHMARK (SP);
				XPUSHs (sv_2mortal (newSViv (PTR2IV (data->hook))));
				XPUSHs (sv_mortalcopy (data->package));
				PUTBACK;

				call_pv ("Xchat::unhook", G_EVAL);
				SPAGAIN;
			}
		}

	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retVal;
}

static int
server_cb (char *word[], char *word_eol[], void *userdata)
{
	HookData *data = (HookData *) userdata;
	int retVal = 0;
	int count = 0;

	dSP;
	ENTER;
	SAVETMPS;

	if (data->depth)
		return XCHAT_EAT_NONE;

	/*               xchat_printf (ph, */
	/*                               "Recieved %d words in server callback", av_len (wd)); */
	PUSHMARK (SP);
	XPUSHs (newRV_noinc ((SV *) array2av (word)));
	XPUSHs (newRV_noinc ((SV *) array2av (word_eol)));
	XPUSHs (data->userdata);
	PUTBACK;

	data->depth++;
	set_current_package (data->package);
	count = call_sv (data->callback, G_EVAL);
	set_current_package (&PL_sv_undef);
	data->depth--;
	SPAGAIN;
	if (SvTRUE (ERRSV)) {
		xchat_printf (ph, "Error in server callback %s", SvPV_nolen (ERRSV));
		if (!SvOK (POPs)) {}		  /* remove undef from the top of the stack */
		retVal = XCHAT_EAT_NONE;
	} else {
		if (count != 1) {
			xchat_print (ph, "Server handler should only return 1 value.");
			retVal = XCHAT_EAT_NONE;
		} else {
			retVal = POPi;
		}

	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retVal;
}

static int
command_cb (char *word[], char *word_eol[], void *userdata)
{
	HookData *data = (HookData *) userdata;
	int retVal = 0;
	int count = 0;

	dSP;
	ENTER;
	SAVETMPS;
	
	if (data->depth)
		return XCHAT_EAT_NONE;

	/*               xchat_printf (ph, "Recieved %d words in command callback", */
	/*                               av_len (wd)); */
	PUSHMARK (SP);
	XPUSHs (newRV_noinc ((SV *) array2av (word)));
	XPUSHs (newRV_noinc ((SV *) array2av (word_eol)));
	XPUSHs (data->userdata);
	PUTBACK;

	data->depth++;
	set_current_package (data->package);
	count = call_sv (data->callback, G_EVAL);
	set_current_package (&PL_sv_undef);
	data->depth--;
	SPAGAIN;
	if (SvTRUE (ERRSV)) {
		xchat_printf (ph, "Error in command callback %s", SvPV_nolen (ERRSV));
		if (!SvOK (POPs)) {}		  /* remove undef from the top of the stack */
		retVal = XCHAT_EAT_XCHAT;
	} else {
		if (count != 1) {
			xchat_print (ph, "Command handler should only return 1 value.");
			retVal = XCHAT_EAT_NONE;
		} else {
			retVal = POPi;
		}

	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retVal;
}

static int
print_cb (char *word[], void *userdata)
{

	HookData *data = (HookData *) userdata;
	SV *temp = NULL;
	int retVal = 0;
	int count = 1;
	int last_index = 31;
	/* must be initialized after SAVETMPS */
	AV *wd = NULL;

	dSP;
	ENTER;
	SAVETMPS;

	if (data->depth)
		return XCHAT_EAT_NONE;

	wd = newAV ();
	sv_2mortal ((SV *) wd);

	/* need to scan backwards to find the index of the last element since some
	   events such as "DCC Timeout" can have NULL elements in between non NULL
	   elements */

	while (last_index >= 0
			 && (word[last_index] == NULL || word[last_index][0] == 0)) {
		last_index--;
	}

	for (count = 1; count <= last_index; count++) {
		if (word[count] == NULL) {
			av_push (wd, &PL_sv_undef);
		} else if (word[count][0] == 0) {
			av_push (wd, newSVpvn ("",0));	
		} else {
			temp = newSVpv (word[count], 0);
			SvUTF8_on (temp);
			av_push (wd, temp);
		}
	}

	/*xchat_printf (ph, "Recieved %d words in print callback", av_len (wd)+1); */
	PUSHMARK (SP);
	XPUSHs (newRV_noinc ((SV *) wd));
	XPUSHs (data->userdata);
	PUTBACK;

	data->depth++;
	set_current_package (data->package);
	count = call_sv (data->callback, G_EVAL);
	set_current_package (&PL_sv_undef);
	data->depth--;
	SPAGAIN;
	if (SvTRUE (ERRSV)) {
		xchat_printf (ph, "Error in print callback %s", SvPV_nolen (ERRSV));
		if (!SvOK (POPs)) {}		  /* remove undef from the top of the stack */
		retVal = XCHAT_EAT_NONE;
	} else {
		if (count != 1) {
			xchat_print (ph, "Print handler should only return 1 value.");
			retVal = XCHAT_EAT_NONE;
		} else {
			retVal = POPi;
		}

	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	return retVal;
}

/* custom IRC perl functions for scripting */

/* Xchat::Internal::register (scriptname, version, desc, shutdowncallback, filename)
 *
 */

static
XS (XS_Xchat_register)
{
	char *name, *version, *desc, *filename;
	void *gui_entry;
	dXSARGS;
	if (items != 4) {
		xchat_printf (ph,
						  "Usage: Xchat::Internal::register(scriptname, version, desc, filename)");
	} else {
		name = SvPV_nolen (ST (0));
		version = SvPV_nolen (ST (1));
		desc = SvPV_nolen (ST (2));
		filename = SvPV_nolen (ST (3));

		gui_entry = xchat_plugingui_add (ph, filename, name,
													desc, version, NULL);

		XSRETURN_IV (PTR2IV (gui_entry));

	}
}


/* Xchat::print(output) */
static
XS (XS_Xchat_print)
{

	char *text = NULL;

	dXSARGS;
	if (items != 1) {
		xchat_print (ph, "Usage: Xchat::Internal::print(text)");
	} else {
		text = SvPV_nolen (ST (0));
		xchat_print (ph, text);
	}
	XSRETURN_EMPTY;
}

static
XS (XS_Xchat_emit_print)
{
	char *event_name;
	int RETVAL;
	int count;

	dXSARGS;
	if (items < 1) {
		xchat_print (ph, "Usage: Xchat::emit_print(event_name, ...)");
	} else {
		event_name = (char *) SvPV_nolen (ST (0));
		RETVAL = 0;

		/* we need to figure out the number of defined values passed in */
		for (count = 0; count < items; count++) {
			if (!SvOK (ST (count))) {
				break;
			}
		}

		switch (count) {
		case 1:
			RETVAL = xchat_emit_print (ph, event_name, NULL);
			break;
		case 2:
			RETVAL = xchat_emit_print (ph, event_name,
												SvPV_nolen (ST (1)), NULL);
			break;
		case 3:
			RETVAL = xchat_emit_print (ph, event_name,
												SvPV_nolen (ST (1)),
												SvPV_nolen (ST (2)), NULL);
			break;
		case 4:
			RETVAL = xchat_emit_print (ph, event_name,
												SvPV_nolen (ST (1)),
												SvPV_nolen (ST (2)),
												SvPV_nolen (ST (3)), NULL);
			break;
		case 5:
			RETVAL = xchat_emit_print (ph, event_name,
												SvPV_nolen (ST (1)),
												SvPV_nolen (ST (2)),
												SvPV_nolen (ST (3)),
												SvPV_nolen (ST (4)), NULL);
			break;

		}

		XSRETURN_IV (RETVAL);
	}
}

static
XS (XS_Xchat_send_modes)
{
	AV *p_targets = NULL;
	int modes_per_line = 0;
	char sign;
	char mode;
	int i = 0;
	const char **targets;
	int target_count = 0;
	SV **elem;

	dXSARGS;
	if (items < 3 || items > 4) {
		xchat_print (ph,
			"Usage: Xchat::send_modes( targets, sign, mode, modes_per_line)"
		);
	} else {
		if (SvROK (ST (0))) {
			p_targets = (AV*) SvRV (ST (0));
			target_count = av_len (p_targets) + 1;
			targets = malloc (target_count * sizeof (char *));
			for (i = 0; i < target_count; i++ ) {
				elem = av_fetch (p_targets, i, 0);

				if (elem != NULL) {
					targets[i] = SvPV_nolen (*elem);
				} else {
					targets[i] = "";
				}
			}
		} else{
			targets = malloc (sizeof (char *));
			targets[0] = SvPV_nolen (ST (0));
			target_count = 1;
		}
		
		if (target_count == 0) {
			XSRETURN_EMPTY;
		}

		sign = (SvPV_nolen (ST (1)))[0];
		mode = (SvPV_nolen (ST (2)))[0];

		if (items == 4 ) {
			modes_per_line = (int) SvIV (ST (3)); 
		}

		xchat_send_modes (ph, targets, target_count, modes_per_line, sign, mode);
		free (targets);
	}
}
static
XS (XS_Xchat_get_info)
{
	SV *temp = NULL;
	dXSARGS;
	if (items != 1) {
		xchat_print (ph, "Usage: Xchat::get_info(id)");
	} else {
		SV *id = ST (0);
		const char *RETVAL;

		RETVAL = xchat_get_info (ph, SvPV_nolen (id));
		if (RETVAL == NULL) {
			XSRETURN_UNDEF;
		}

		if (!strncmp ("win_ptr", SvPV_nolen (id), 7)
			|| !strncmp ("gtkwin_ptr", SvPV_nolen (id), 10))
		{
			XSRETURN_IV (PTR2IV (RETVAL));
		} else {
			
			if (
				!strncmp ("libdirfs", SvPV_nolen (id), 8) ||
				!strncmp ("xchatdirfs", SvPV_nolen (id), 10)
			) {
				XSRETURN_PV (RETVAL);
			} else {
				temp = newSVpv (RETVAL, 0);
				SvUTF8_on (temp);
				PUSHMARK (SP);
				XPUSHs (sv_2mortal (temp));
				PUTBACK;
			}
		}
	}
}

static
XS (XS_Xchat_context_info)
{
	const char *const *fields;
	dXSARGS;

	if (items > 0 ) {
		xchat_print (ph, "Usage: Xchat::Internal::context_info()");
	}
	fields = xchat_list_fields (ph, "channels" );
	XPUSHs (list_item_to_sv (NULL, fields));
	XSRETURN (1);
}

static
XS (XS_Xchat_get_prefs)
{
	const char *str;
	int integer;
	SV *temp = NULL;
	dXSARGS;
	if (items != 1) {
		xchat_print (ph, "Usage: Xchat::get_prefs(name)");
	} else {


		switch (xchat_get_prefs (ph, SvPV_nolen (ST (0)), &str, &integer)) {
		case 0:
			XSRETURN_UNDEF;
			break;
		case 1:
			temp = newSVpv (str, 0);
			SvUTF8_on (temp);
			SP -= items;
			sp = mark;
			XPUSHs (sv_2mortal (temp));
			PUTBACK;
			break;
		case 2:
			XSRETURN_IV (integer);
			break;
		case 3:
			if (integer) {
				XSRETURN_YES;
			} else {
				XSRETURN_NO;
			}
		}
	}
}

/* Xchat::Internal::hook_server(name, priority, callback, userdata) */
static
XS (XS_Xchat_hook_server)
{

	char *name;
	int pri;
	SV *callback;
	SV *userdata;
	SV *package;
	xchat_hook *hook;
	HookData *data;

	dXSARGS;

	if (items != 5) {
		xchat_print (ph,
						 "Usage: Xchat::Internal::hook_server(name, priority, callback, userdata, package)");
	} else {
		name = SvPV_nolen (ST (0));
		pri = (int) SvIV (ST (1));
		callback = ST (2);
		userdata = ST (3);
		package = ST (4);
		data = NULL;
		data = malloc (sizeof (HookData));
		if (data == NULL) {
			XSRETURN_UNDEF;
		}

		data->callback = newSVsv (callback);
		data->userdata = newSVsv (userdata);
		data->depth = 0;
		data->package = newSVsv (package);

		hook = xchat_hook_server (ph, name, pri, server_cb, data);

		XSRETURN_IV (PTR2IV (hook));
	}
}

/* Xchat::Internal::hook_command(name, priority, callback, help_text, userdata) */
static
XS (XS_Xchat_hook_command)
{
	char *name;
	int pri;
	SV *callback;
	char *help_text = NULL;
	SV *userdata;
	SV *package;
	xchat_hook *hook;
	HookData *data;

	dXSARGS;

	if (items != 6) {
		xchat_print (ph,
						 "Usage: Xchat::Internal::hook_command(name, priority, callback, help_text, userdata, package)");
	} else {
		name = SvPV_nolen (ST (0));
		pri = (int) SvIV (ST (1));
		callback = ST (2);

		/* leave the help text as NULL if the help text is undefined to avoid
		 * overriding the default help message for builtin commands */
		if (SvOK(ST (3))) {
			help_text = SvPV_nolen (ST (3));
		}

		userdata = ST (4);
		package = ST (5);
		data = NULL;

		data = malloc (sizeof (HookData));
		if (data == NULL) {
			XSRETURN_UNDEF;
		}

		data->callback = newSVsv (callback);
		data->userdata = newSVsv (userdata);
		data->depth = 0;
		data->package = newSVsv (package);
		hook = xchat_hook_command (ph, name, pri, command_cb, help_text, data);

		XSRETURN_IV (PTR2IV (hook));
	}

}

/* Xchat::Internal::hook_print(name, priority, callback, [userdata]) */
static
XS (XS_Xchat_hook_print)
{

	char *name;
	int pri;
	SV *callback;
	SV *userdata;
	SV *package;
	xchat_hook *hook;
	HookData *data;
	dXSARGS;
	if (items != 5) {
		xchat_print (ph,
						 "Usage: Xchat::Internal::hook_print(name, priority, callback, userdata, package)");
	} else {
		name = SvPV_nolen (ST (0));
		pri = (int) SvIV (ST (1));
		callback = ST (2);
		data = NULL;
		userdata = ST (3);
		package = ST (4);

		data = malloc (sizeof (HookData));
		if (data == NULL) {
			XSRETURN_UNDEF;
		}

		data->callback = newSVsv (callback);
		data->userdata = newSVsv (userdata);
		data->depth = 0;
		data->package = newSVsv (package);
		hook = xchat_hook_print (ph, name, pri, print_cb, data);

		XSRETURN_IV (PTR2IV (hook));
	}
}

/* Xchat::Internal::hook_timer(timeout, callback, userdata) */
static
XS (XS_Xchat_hook_timer)
{
	int timeout;
	SV *callback;
	SV *userdata;
	xchat_hook *hook;
	SV *package;
	HookData *data;

	dXSARGS;

	if (items != 4) {
		xchat_print (ph,
						 "Usage: Xchat::Internal::hook_timer(timeout, callback, userdata, package)");
	} else {
		timeout = (int) SvIV (ST (0));
		callback = ST (1);
		data = NULL;
		userdata = ST (2);
		package = ST (3);

		data = malloc (sizeof (HookData));
		if (data == NULL) {
			XSRETURN_UNDEF;
		}

		data->callback = newSVsv (callback);
		data->userdata = newSVsv (userdata);
		data->ctx = xchat_get_context (ph);
		data->package = newSVsv (package);
		hook = xchat_hook_timer (ph, timeout, timer_cb, data);
		data->hook = hook;

		XSRETURN_IV (PTR2IV (hook));
	}
}

/* Xchat::Internal::hook_fd(fd, callback, flags, userdata) */
static
XS (XS_Xchat_hook_fd)
{
	int fd;
	SV *callback;
	int flags;
	SV *userdata;
	SV *package;
	xchat_hook *hook;
	HookData *data;

	dXSARGS;

	if (items != 5) {
		xchat_print (ph,
						 "Usage: Xchat::Internal::hook_fd(fd, callback, flags, userdata)");
	} else {
		fd = (int) SvIV (ST (0));
		callback = ST (1);
		flags = (int) SvIV (ST (2));
		userdata = ST (3);
		package = ST (4);
		data = NULL;

#ifdef WIN32
		if ((flags & XCHAT_FD_NOTSOCKET) == 0) {
			/* this _get_osfhandle if from win32iop.h in the perl distribution,
			 *  not the one provided by Windows
			 */ 
			fd = _get_osfhandle(fd);
			if (fd < 0) {
				xchat_print(ph, "Invalid file descriptor");
				XSRETURN_UNDEF;
			}
		}
#endif

		data = malloc (sizeof (HookData));
		if (data == NULL) {
			XSRETURN_UNDEF;
		}

		data->callback = newSVsv (callback);
		data->userdata = newSVsv (userdata);
		data->depth = 0;
		data->package = newSVsv (package);
		hook = xchat_hook_fd (ph, fd, flags, fd_cb, data);
		data->hook = hook;

		XSRETURN_IV (PTR2IV (hook));
	}
}

static
XS (XS_Xchat_unhook)
{
	xchat_hook *hook;
	HookData *userdata;
	int retCount = 0;
	dXSARGS;
	if (items != 1) {
		xchat_print (ph, "Usage: Xchat::unhook(hook)");
	} else {
		hook = INT2PTR (xchat_hook *, SvUV (ST (0)));
		userdata = (HookData *) xchat_unhook (ph, hook);

		if (userdata != NULL) {
			if (userdata->callback != NULL) {
				SvREFCNT_dec (userdata->callback);
			}

			if (userdata->userdata != NULL) {
				XPUSHs (sv_mortalcopy (userdata->userdata));
				SvREFCNT_dec (userdata->userdata);
				retCount = 1;
			}

			if (userdata->package != NULL) {
				SvREFCNT_dec (userdata->package);
			}

			free (userdata);
		}
		XSRETURN (retCount);
	}
	XSRETURN_EMPTY;
}

/* Xchat::Internal::command(command) */
static
XS (XS_Xchat_command)
{
	char *cmd = NULL;

	dXSARGS;
	if (items != 1) {
		xchat_print (ph, "Usage: Xchat::Internal::command(command)");
	} else {
		cmd = SvPV_nolen (ST (0));
		xchat_command (ph, cmd);

	}
	XSRETURN_EMPTY;
}

static
XS (XS_Xchat_find_context)
{
	char *server = NULL;
	char *chan = NULL;
	xchat_context *RETVAL;

	dXSARGS;
	if (items > 2)
		xchat_print (ph, "Usage: Xchat::find_context ([channel, [server]])");
	{

		switch (items) {
		case 0:						  /* no server name and no channel name */
			/* nothing to do, server and chan are already NULL */
			break;
		case 1:						  /* channel name only */
			/* change channel value only if it is true or 0 */
			/* otherwise leave it as null */
			if (SvTRUE (ST (0)) || SvNIOK (ST (0))) {
				chan = SvPV_nolen (ST (0));
				/*                               xchat_printf( ph, "XSUB - find_context( %s, NULL )", chan ); */
			}
			/* else { xchat_print( ph, "XSUB - find_context( NULL, NULL )" ); } */
			/* chan is already NULL */
			break;
		case 2:						  /* server and channel */
			/* change channel value only if it is true or 0 */
			/* otherwise leave it as NULL */
			if (SvTRUE (ST (0)) || SvNIOK (ST (0))) {
				chan = SvPV_nolen (ST (0));
				/*                               xchat_printf( ph, "XSUB - find_context( %s, NULL )", SvPV_nolen(ST(0) )); */
			}

			/* else { xchat_print( ph, "XSUB - 2 arg NULL chan" ); } */
			/* change server value only if it is true or 0 */
			/* otherwise leave it as NULL */
			if (SvTRUE (ST (1)) || SvNIOK (ST (1))) {
				server = SvPV_nolen (ST (1));
				/*                               xchat_printf( ph, "XSUB - find_context( NULL, %s )", SvPV_nolen(ST(1) )); */
			}
			/*  else { xchat_print( ph, "XSUB - 2 arg NULL server" ); } */
			break;
		}

		RETVAL = xchat_find_context (ph, server, chan);
		if (RETVAL != NULL) {
			/*                      xchat_print (ph, "XSUB - context found"); */
			XSRETURN_IV (PTR2IV (RETVAL));
		} else {
			/*           xchat_print (ph, "XSUB - context not found"); */
			XSRETURN_UNDEF;
		}
	}
}

static
XS (XS_Xchat_get_context)
{
	dXSARGS;
	if (items != 0) {
		xchat_print (ph, "Usage: Xchat::get_context()");
	} else {
		XSRETURN_IV (PTR2IV (xchat_get_context (ph)));
	}
}

static
XS (XS_Xchat_set_context)
{
	xchat_context *ctx;
	dXSARGS;
	if (items != 1) {
		xchat_print (ph, "Usage: Xchat::set_context(ctx)");
	} else {
		ctx = INT2PTR (xchat_context *, SvUV (ST (0)));
		XSRETURN_IV ((IV) xchat_set_context (ph, ctx));
	}
}

static
XS (XS_Xchat_nickcmp)
{
	dXSARGS;
	if (items != 2) {
		xchat_print (ph, "Usage: Xchat::nickcmp(s1, s2)");
	} else {
		XSRETURN_IV ((IV) xchat_nickcmp (ph, SvPV_nolen (ST (0)),
													SvPV_nolen (ST (1))));
	}
}

static
XS (XS_Xchat_get_list)
{
	SV *name;
	xchat_list *list;
	const char *const *fields;
	int count = 0;					  /* return value for scalar context */
	dXSARGS;

	if (items != 1) {
		xchat_print (ph, "Usage: Xchat::get_list(name)");
	} else {
		SP -= items;				  /*remove the argument list from the stack */

		name = ST (0);

		list = xchat_list_get (ph, SvPV_nolen (name));

		if (list == NULL) {
			XSRETURN_EMPTY;
		}

		if (GIMME_V == G_SCALAR) {
			while (xchat_list_next (ph, list)) {
				count++;
			}
			xchat_list_free (ph, list);
			XSRETURN_IV ((IV) count);
		}

		fields = xchat_list_fields (ph, SvPV_nolen (name));
		while (xchat_list_next (ph, list)) {
			XPUSHs (list_item_to_sv (list, fields));
		}
		xchat_list_free (ph, list);

		PUTBACK;
		return;
	}
}

static
XS (XS_Xchat_Embed_plugingui_remove)
{
	void *gui_entry;
	dXSARGS;
	if (items != 1) {
		xchat_print (ph, "Usage: Xchat::Embed::plugingui_remove(handle)");
	} else {
		gui_entry = INT2PTR (void *, SvUV (ST (0)));
		xchat_plugingui_remove (ph, gui_entry);
	}
	XSRETURN_EMPTY;
}

/* xs_init is the second argument perl_parse. As the name hints, it
   initializes XS subroutines (see the perlembed manpage) */
static void
xs_init (pTHX)
{
	HV *stash;
	SV *version;
	/* This one allows dynamic loading of perl modules in perl
	   scripts by the 'use perlmod;' construction */
	newXS ("DynaLoader::boot_DynaLoader", boot_DynaLoader, __FILE__);
	/* load up all the custom IRC perl functions */
	newXS ("Xchat::Internal::register", XS_Xchat_register, __FILE__);
	newXS ("Xchat::Internal::hook_server", XS_Xchat_hook_server, __FILE__);
	newXS ("Xchat::Internal::hook_command", XS_Xchat_hook_command, __FILE__);
	newXS ("Xchat::Internal::hook_print", XS_Xchat_hook_print, __FILE__);
	newXS ("Xchat::Internal::hook_timer", XS_Xchat_hook_timer, __FILE__);
	newXS ("Xchat::Internal::hook_fd", XS_Xchat_hook_fd, __FILE__);
	newXS ("Xchat::Internal::unhook", XS_Xchat_unhook, __FILE__);
	newXS ("Xchat::Internal::print", XS_Xchat_print, __FILE__);
	newXS ("Xchat::Internal::command", XS_Xchat_command, __FILE__);
	newXS ("Xchat::Internal::set_context", XS_Xchat_set_context, __FILE__);
	newXS ("Xchat::Internal::get_info", XS_Xchat_get_info, __FILE__);
	newXS ("Xchat::Internal::context_info", XS_Xchat_context_info, __FILE__);
	newXS ("Xchat::Internal::get_list", XS_Xchat_get_list, __FILE__);
	
	newXS ("Xchat::find_context", XS_Xchat_find_context, __FILE__);
	newXS ("Xchat::get_context", XS_Xchat_get_context, __FILE__);
	newXS ("Xchat::get_prefs", XS_Xchat_get_prefs, __FILE__);
	newXS ("Xchat::emit_print", XS_Xchat_emit_print, __FILE__);
	newXS ("Xchat::send_modes", XS_Xchat_send_modes, __FILE__);
	newXS ("Xchat::nickcmp", XS_Xchat_nickcmp, __FILE__);

	newXS ("Xchat::Embed::plugingui_remove", XS_Xchat_Embed_plugingui_remove,
			 __FILE__);

	stash = get_hv ("Xchat::", TRUE);
	if (stash == NULL) {
		exit (1);
	}

	newCONSTSUB (stash, "PRI_HIGHEST", newSViv (XCHAT_PRI_HIGHEST));
	newCONSTSUB (stash, "PRI_HIGH", newSViv (XCHAT_PRI_HIGH));
	newCONSTSUB (stash, "PRI_NORM", newSViv (XCHAT_PRI_NORM));
	newCONSTSUB (stash, "PRI_LOW", newSViv (XCHAT_PRI_LOW));
	newCONSTSUB (stash, "PRI_LOWEST", newSViv (XCHAT_PRI_LOWEST));

	newCONSTSUB (stash, "EAT_NONE", newSViv (XCHAT_EAT_NONE));
	newCONSTSUB (stash, "EAT_XCHAT", newSViv (XCHAT_EAT_XCHAT));
	newCONSTSUB (stash, "EAT_PLUGIN", newSViv (XCHAT_EAT_PLUGIN));
	newCONSTSUB (stash, "EAT_ALL", newSViv (XCHAT_EAT_ALL));
	newCONSTSUB (stash, "FD_READ", newSViv (XCHAT_FD_READ));
	newCONSTSUB (stash, "FD_WRITE", newSViv (XCHAT_FD_WRITE));
	newCONSTSUB (stash, "FD_EXCEPTION", newSViv (XCHAT_FD_EXCEPTION));
	newCONSTSUB (stash, "FD_NOTSOCKET", newSViv (XCHAT_FD_NOTSOCKET));
	newCONSTSUB (stash, "KEEP", newSViv (1));
	newCONSTSUB (stash, "REMOVE", newSViv (0));

	version = get_sv( "Xchat::VERSION", 1 );
	sv_setpv( version, PACKAGE_VERSION );
}

static void
perl_init (void)
{
	int warn;
	int arg_count;
	char *perl_args[] = { "", "-e", "0", "-w" };
	char *env[] = { "" };
	static const char xchat_definitions[] = {
		/* Redefine the $SIG{__WARN__} handler to have XChat
		   printing warnings in the main window. (TheHobbit) */
#include "xchat.pm.h"
	};
#ifdef OLD_PERL
	static const char irc_definitions[] = {
#include "irc.pm.h"
	};
#endif
#ifdef ENABLE_NLS

	/* Problem is, dynamicaly loaded modules check out the $]
	   var. It appears that in the embedded interpreter we get
	   5,00503 as soon as the LC_NUMERIC locale calls for a comma
	   instead of a point in separating integer and decimal
	   parts. I realy can't understant why... The following
	   appears to be an awful workaround... But it'll do until I
	   (or someone else :)) found the "right way" to solve this
	   nasty problem. (TheHobbit <thehobbit@altern.org>) */

	setlocale (LC_NUMERIC, "C");

#endif

	warn = 0;
	xchat_get_prefs (ph, "perl_warnings", NULL, &warn);
	arg_count = warn ? 4 : 3;

	PERL_SYS_INIT3 (&arg_count, (char ***)&perl_args, (char ***)&env);
	my_perl = perl_alloc ();
	perl_construct (my_perl);
	PL_exit_flags |= PERL_EXIT_DESTRUCT_END;
	perl_parse (my_perl, xs_init, arg_count, perl_args, (char **)NULL);

	/*
	   Now initialising the perl interpreter by loading the
	   perl_definition array.
	 */

	eval_pv (xchat_definitions, TRUE);
#ifdef OLD_PERL
	eval_pv (irc_definitions, TRUE);
#endif

}


static int
perl_load_file (char *filename)
{
#ifdef WIN32
	static HMODULE lib = NULL;

	if (!lib) {
		lib = LoadLibraryA (PERL_DLL);
		if (!lib) {
			if (GetLastError () == ERROR_BAD_EXE_FORMAT)
				/* http://forum.xchat.org/viewtopic.php?t=3277 */
				thread_mbox ("Cannot use this " PERL_DLL "\n\n"
								 "32-bit ActivePerl is required.");
			else {
				/* a lot of people install this old version */
				lib = LoadLibraryA ("perl56.dll");
				if (lib) {
					FreeLibrary (lib);
					lib = NULL;
					thread_mbox ("Cannot open " PERL_DLL "\n\n"
									 "You must have either ActivePerl or Straberry Perl"
									 PERL_REQUIRED_VERSION
									 " installed in order to\n"
									 "run perl scripts.\n\n"
									 "I have found Perl 5.6, but that is too old.");
				} else {
					thread_mbox ("Cannot open " PERL_DLL "\n\n"
									 "You must have either ActivePerl or Strawberry Perl "
									 PERL_REQUIRED_VERSION " installed in order to\n"
									 "run perl scripts.\n\n"
									 "http://www.activestate.com/ActivePerl/\n"
									 "http://strawberryperl.com/\n"
									 "Make sure perl's bin directory is in your PATH.");
				}
			}
			/* failure */
			return FALSE;
		}

		/* success */
		FreeLibrary (lib);
	}
#endif

	if (my_perl == NULL) {
		perl_init ();
	}

	return execute_perl (sv_2mortal (newSVpv ("Xchat::Embed::load", 0)),
								filename);

}

static void
perl_end (void)
{

	if (my_perl != NULL) {
		execute_perl (sv_2mortal (newSVpv ("Xchat::Embed::unload_all", 0)), "");
		PL_perl_destruct_level = 1;
		perl_destruct (my_perl);
		perl_free (my_perl);
		PERL_SYS_TERM();
		my_perl = NULL;
	}

}

static int
perl_command_unloadall (char *word[], char *word_eol[], void *userdata)
{
	if (my_perl != NULL) {
		execute_perl (sv_2mortal (newSVpv ("Xchat::Embed::unload_all", 0)), "");
		return XCHAT_EAT_XCHAT;
	}

	return XCHAT_EAT_XCHAT;
}

static int
perl_command_reloadall (char *word[], char *word_eol[], void *userdata)
{
	if (my_perl != NULL) {
		execute_perl (sv_2mortal (newSVpv ("Xchat::Embed::reload_all", 0)), "");

		return XCHAT_EAT_XCHAT;
	} else {
		perl_auto_load( NULL );
	}
	return XCHAT_EAT_XCHAT;
}

static int
perl_command_load (char *word[], char *word_eol[], void *userdata)
{
	char *file = get_filename (word, word_eol);

	if (file != NULL )
	{
		perl_load_file (file);
		return XCHAT_EAT_XCHAT;
	}

	return XCHAT_EAT_NONE;
}

static int
perl_command_unload (char *word[], char *word_eol[], void *userdata)
{
	char *file = get_filename (word, word_eol);
	
	if (my_perl != NULL && file != NULL) {
		execute_perl (sv_2mortal (newSVpv ("Xchat::Embed::unload", 0)), file);
		return XCHAT_EAT_XCHAT;
	}

	return XCHAT_EAT_NONE;
}

static int
perl_command_reload (char *word[], char *word_eol[], void *userdata)
{
	char *file = get_filename (word, word_eol);
	
	if (my_perl != NULL && file != NULL) {
		execute_perl (sv_2mortal (newSVpv ("Xchat::Embed::reload", 0)), file);
		return XCHAT_EAT_XCHAT;
	}
	
	return XCHAT_EAT_XCHAT;
}

void
xchat_plugin_get_info (char **name, char **desc, char **version,
							  void **reserved)
{
	*name = "Perl";
	*desc = "Perl scripting interface";
	*version = PACKAGE_VERSION;
	if (reserved)
		*reserved = NULL;
}


/* Reinit safeguard */

static int initialized = 0;

int
xchat_plugin_init (xchat_plugin * plugin_handle, char **plugin_name,
						 char **plugin_desc, char **plugin_version, char *arg)
{
	if (initialized != 0) {
		xchat_print (plugin_handle, "Perl interface already loaded\n");
		return 0;
	}

	ph = plugin_handle;
	initialized = 1;

	*plugin_name = "Perl";
	*plugin_desc = "Perl scripting interface";
	*plugin_version = PACKAGE_VERSION;

	xchat_hook_command (ph, "load", XCHAT_PRI_NORM, perl_command_load, 0, 0);
	xchat_hook_command (ph, "unload", XCHAT_PRI_NORM, perl_command_unload, 0,
							  0);
	xchat_hook_command (ph, "reload", XCHAT_PRI_NORM, perl_command_reload, 0,
							  0);
	xchat_hook_command (ph, "pl_reload", XCHAT_PRI_NORM, perl_command_reload, 0,
							  0);
	xchat_hook_command (ph, "unloadall", XCHAT_PRI_NORM,
							  perl_command_unloadall, 0, 0);
	xchat_hook_command (ph, "reloadall", XCHAT_PRI_NORM,
							  perl_command_reloadall, 0, 0);

	/*perl_init (); */
	xchat_hook_timer (ph, 0, perl_auto_load, NULL );

	xchat_print (ph, "Perl interface loaded\n");

	return 1;
}

int
xchat_plugin_deinit (xchat_plugin * plugin_handle)
{
	perl_end ();

	initialized = 0;
	xchat_print (plugin_handle, "Perl interface unloaded\n");

	return 1;
}
