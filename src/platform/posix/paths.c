/*
 * Copyright 2014-2016, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: http://arcan-fe.com
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <libgen.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <ctype.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <arcan_math.h>
#include <arcan_general.h>

bool arcan_isdir(const char* fn)
{
	struct stat buf;
	bool rv = false;

	if (fn == NULL)
			return false;

	if (stat(fn, &buf) == 0)
		rv = S_ISDIR(buf.st_mode);

	return rv;
}

bool arcan_isfile(const char* fn)
{
	struct stat buf;
	bool rv = false;

	if (fn == NULL)
		return false;

	if (stat(fn, &buf) == 0)
		rv = S_ISREG(buf.st_mode) || S_ISFIFO(buf.st_mode);

	return rv;
}

static char* envvs[] = {
	"ARCAN_APPLPATH",
	"ARCAN_RESOURCEPATH",
	"ARCAN_APPLTEMPPATH",
	"ARCAN_STATEPATH", /* will be ignored */
	"ARCAN_APPLBASEPATH",
	"ARCAN_APPLSTOREPATH",
	"ARCAN_STATEBASEPATH",
	"ARCAN_FONTPATH",
	"ARCAN_BINPATH",
	"ARCAN_LIBPATH",
	"ARCAN_LOGPATH"
};

static char* pinvs[] = {
	"ARCAN_APPLPIN",
	"ARCAN_RESOURCEPIN",
	"ARCAN_APPLTEMPPIN",
	"ARCAN_STATEPIN", /* will be ignored */
	"ARCAN_APPLBASEPIN",
	"ARCAN_APPLSTOREPIN",
	"ARCAN_STATEBASEPIN",
	"ARCAN_FONTPIN",
	"ARCAN_BINPIN",
	"ARCAN_LIBPIN",
	"ARCAN_LOGPIN"
};

static char* alloc_cat(char* a, char* b)
{
	size_t a_sz = strlen(a);
	size_t b_sz = strlen(b);
	char* newstr = malloc(a_sz + b_sz + 1);
	newstr[a_sz + b_sz] = '\0';
	memcpy(newstr, a, a_sz);
	memcpy(newstr + a_sz, b, b_sz);
	return newstr;
}

/*
 * handle:
 * abc [DEF] ghj [ARCAN_APPLPATH] klm [!.bla =>
 * 	abc [DEF] ghj /usr/whatever klm [!.bla
 * abc => abc
 * [ARCAN_APPLPATH] apa [ARCAN_APPLPATH] => /usr/whatever apa /usr/whatever
 * [ARCAN_APPLPATH => [ARCAN_APPLPATH
 */
static char* rep_str(char* instr)
{
	char* pos = instr;
	char* beg;

	while(1){
rep:
		beg = strchr(pos, '[');
		if (!beg)
			return instr;

		char* end = strchr(beg+1, ']');
		if (!end)
			return instr;

/* counter abc [ [ARCAN_APPLPATH] */
		char* step;
		while ((step = strchr(beg+1, '[')) && step < end)
			beg = step;

		*end = '\0';

		for (size_t i = 0; i < sizeof(envvs)/sizeof(envvs[0]); i++){
			if (strcmp(envvs[i], beg+1) == 0){
				char* exp = arcan_expand_resource("", 1 << i);
				if (exp){
					*beg = '\0';
					char* newstr = alloc_cat(instr, exp);
					char* resstr = alloc_cat(newstr, end+1);
					free(instr);
					free(newstr);
					pos = instr = resstr;
				}
				arcan_mem_free(exp);
				goto rep; /* continue 2; */
			}
		}

		*end = ']';
		pos = end;
	}
}

char** arcan_expand_namespaces(char** inargs)
{
	char** work = inargs;

	while(*work){
		*work = rep_str(*work);
		work++;
	}

	return inargs;
}

static char* binpath_unix()
{
	char* binpath = NULL;

	if (arcan_isfile( "./arcan_frameserver") )
		binpath = strdup("./arcan_frameserver" );
	else if (arcan_isfile( "/usr/local/bin/arcan_frameserver"))
		binpath = strdup("/usr/local/bin/arcan_frameserver");
	else if (arcan_isfile( "/usr/bin/arcan_frameserver" ))
		binpath = strdup("/usr/bin/arcan_frameserver");
	else ;

	return binpath;
}

static char* libpath_unix()
{
	char* libpath = NULL;

	if (arcan_isfile( getenv("ARCAN_HIJACK") ) )
		libpath = strdup( getenv("ARCAN_HIJACK") );
	else if (arcan_isfile( "./libarcan_hijack.so"))
		libpath = realpath( "./", NULL );
	else if (arcan_isfile( "/usr/local/lib/libarcan_hijack.so") )
		libpath = strdup( "/usr/local/lib/");
	else if (arcan_isfile( "/usr/lib/libarcan_hijack.so") )
		libpath = strdup( "/usr/lib/");

	return libpath;
}

static char* unix_find(const char* fname)
{
	char* res = NULL;
	char* pathtbl[] = {
		".",
		NULL, /* fill in with HOME */
		"/usr/local/share/arcan",
		"/usr/share/arcan",
		NULL /* NULL terminates */
	};

	if (getenv("HOME")){
		size_t len = strlen( getenv("HOME") ) + 9;
		pathtbl[1] = malloc(len);
		snprintf(pathtbl[1], len, "%s/.arcan", getenv("HOME") );
	}
	else
		pathtbl[1] = strdup("");

	size_t fn_l = strlen(fname);
	for (char** base = pathtbl; *base != NULL; base++){
		char buf[ fn_l + strlen(*base) + 2 ];
		snprintf(buf, sizeof(buf), "%s/%s", *base, fname);

		if (arcan_isdir(buf)){
			res = strdup(buf);
			break;
		}
	}

	free(pathtbl[1]);
	return res;
}

/*
 * This is set-up to mimic the behavior of previous arcan
 * version as much as possible. For other, more controlled settings,
 * this is a good function to replace.
 */
void arcan_set_namespace_defaults()
{
	char* tmp = NULL;

/*
 * use environment variables as hard overrides
 */
	for (int i = 0; i < sizeof( envvs ) / sizeof( envvs[0] ); i++){
		const char* tmp = getenv(envvs[i]);
		arcan_override_namespace(tmp, 1 << i);
		if (getenv(pinvs[i]))
			arcan_pin_namespace(1 << i);
	}

/*
 * legacy mapping from the < 0.5 days
 */

	arcan_softoverride_namespace(tmp = binpath_unix(), RESOURCE_SYS_BINS);
	free(tmp);
	arcan_softoverride_namespace(tmp = libpath_unix(), RESOURCE_SYS_LIBS);
	free(tmp);

	char* respath = unix_find("resources");

	if (!respath)
		respath = arcan_expand_resource("", RESOURCE_APPL_SHARED);

	if (respath){
		size_t len = strlen(respath);
		char debug_dir[ len + sizeof("/logs") ];
		char font_dir[ len + sizeof("/fonts") ];

		snprintf(debug_dir, sizeof(debug_dir), "%s/logs", respath);
		snprintf(font_dir, sizeof(font_dir), "%s/fonts", respath);

		arcan_softoverride_namespace(respath, RESOURCE_APPL_SHARED);
		arcan_softoverride_namespace(debug_dir, RESOURCE_SYS_DEBUG);
		arcan_softoverride_namespace(respath, RESOURCE_APPL_STATE);
		arcan_softoverride_namespace(font_dir, RESOURCE_SYS_FONT);
		arcan_mem_free(respath);
	}

	char* scrpath = unix_find("appl");

	if (scrpath){
		arcan_softoverride_namespace(scrpath, RESOURCE_SYS_APPLBASE);
		arcan_softoverride_namespace(scrpath, RESOURCE_SYS_APPLSTORE);
		arcan_mem_free(scrpath);
	}

	tmp = arcan_expand_resource("", RESOURCE_SYS_APPLSTATE);
	if (!tmp){
		tmp = arcan_expand_resource("savestates", RESOURCE_APPL_SHARED);
		if (tmp)
			arcan_override_namespace(tmp, RESOURCE_SYS_APPLSTATE);
	}

	arcan_mem_free(tmp);
}
