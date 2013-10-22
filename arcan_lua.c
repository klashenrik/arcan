/* Arcan-fe, scriptable front-end engine
*
* Arcan-fe is the legal property of its developers, please refer
* to the COPYRIGHT file distributed with this source distribution.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 3
* of the License, or (at your option) any later version.

* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.

* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*
*/

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>

#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <assert.h>

#include GL_HEADERS

#include "arcan_math.h"
#include "arcan_general.h"
#include "arcan_video.h"
#include "arcan_videoint.h"
#include "arcan_shdrmgmt.h"
#include "arcan_3dbase.h"
#include "arcan_audio.h"
#include "arcan_event.h"
#include "arcan_db.h"
#include "arcan_framequeue.h"
#include "arcan_frameserver_backend.h"
#include "arcan_frameserver_shmpage.h"
#include "arcan_target_const.h"
#include "arcan_target_launcher.h"
#include "arcan_lua.h"

#ifndef ARCAN_LUA_NOLED
#include "arcan_led.h"
#endif

#include "arcan_img.h"
#include "arcan_ttf.h"

#ifdef LUA51_JIT
#include "luajit.h"
#endif

#if LUA_VERSION_NUM == 501
	#define lua_rawlen(x, y) lua_objlen(x, y)
#endif

/* these take some explaining:
 * to enforce that actual constants are used in LUA scripts and not magic 
 * numbers the corresponding binding functions check that the match these 
 * global constants (not defines as we want them maintained in debug data
 * as well), but their actual values are set by defines so that they can be
 * swizzled around by the build-system */
#ifndef CONST_ROTATE_RELATIVE
#define CONST_ROTATE_RELATIVE 10
#endif

#ifndef CONST_ROTATE_ABSOLUTE
#define CONST_ROTATE_ABSOLUTE 5
#endif

#ifndef CONST_MAX_SURFACEW
#define CONST_MAX_SURFACEW 2048
#endif

#ifndef CONST_MAX_SURFACEH
#define CONST_MAX_SURFACEH 2048
#endif

#ifndef CONST_RENDERTARGET_DETACH
#define CONST_RENDERTARGET_DETACH 20
#endif

#ifndef CONST_RENDERTARGET_NODETACH
#define CONST_RENDERTARGET_NODETACH 21
#endif

#ifndef CONST_RENDERTARGET_SCALE
#define CONST_RENDERTARGET_SCALE 30
#endif

#ifndef CONST_RENDERTARGET_NOSCALE
#define CONST_RENDERTARGET_NOSCALE 31
#endif

static const int ROTATE_RELATIVE = CONST_ROTATE_RELATIVE;
static const int ROTATE_ABSOLUTE = CONST_ROTATE_ABSOLUTE;

static const int MOUSE_GRAB_ON  = 20;
static const int MOUSE_GRAB_OFF = 21;

static const int MAX_SURFACEW = CONST_MAX_SURFACEH;
static const int MAX_SURFACEH = CONST_MAX_SURFACEW;

static const int FRAMESERVER_LOOP   = 1;
static const int FRAMESERVER_NOLOOP = 0;

static const int FRAMESET_NODETACH = 11;
static const int FRAMESET_DETACH   = 10;

static const int RENDERTARGET_DETACH   = CONST_RENDERTARGET_DETACH;
static const int RENDERTARGET_NODETACH = CONST_RENDERTARGET_NODETACH;
static const int RENDERTARGET_SCALE    = CONST_RENDERTARGET_SCALE;
static const int RENDERTARGET_NOSCALE  = CONST_RENDERTARGET_NOSCALE;
static const int RENDERFMT_COLOR = RENDERTARGET_COLOR;
static const int RENDERFMT_DEPTH = RENDERTARGET_DEPTH;
static const int RENDERFMT_FULL  = RENDERTARGET_COLOR_DEPTH_STENCIL;

static const int BLEND_NONE     = blend_disable;
static const int BLEND_NORMAL   = blend_normal;
static const int BLEND_ADD      = blend_add;
static const int BLEND_MULTIPLY = blend_multiply;
static const int BLEND_FORCE    = blend_force;

static const int POSTFILTER_NTSC = 100;
static const int POSTFILTER_OFF  = 10;

extern char* arcan_themename;
extern arcan_dbh* dbhandle;

enum arcan_cb_source {
	CB_SOURCE_NONE        = 0,
	CB_SOURCE_FRAMESERVER = 1,
	CB_SOURCE_IMAGE       = 2
};

static struct {
	FILE* rfile;
	unsigned char debug;
	unsigned lua_vidbase;
	unsigned char grab;

	enum arcan_cb_source cb_source_kind;
	long long cb_source_tag;

} lua_ctx_store = {
	.lua_vidbase = 0,
	.rfile = NULL,
	.debug = 0,
	.grab = 0
};

extern char* _n_strdup(const char* instr, const char* alt);
static void arcan_lua_warning(const char* const, ...);
static void arcan_lua_fatal(const char* const, ...);

static void dump_call_trace(lua_State* ctx)
{
#if LUA_VERSION_NUM == 501
		arcan_warning("-- call trace --");
		lua_getfield(ctx, LUA_GLOBALSINDEX, "debug");
		if (!lua_istable(ctx, -1))
			lua_pop(ctx, 1);
		else {
			lua_getfield(ctx, -1, "traceback");
			if (!lua_isfunction(ctx, -1))
				lua_pop(ctx, 2);
			else {
				lua_pushvalue(ctx, 1);
				lua_pushinteger(ctx, 2);
				lua_call(ctx, 2, 1);
				const char* str = lua_tostring(ctx, -1);
				arcan_warning("%s\n", str);
			}
	}
#endif
}

/* dump argument stack, stack trace are shown only when --debug is set */
static void dump_stack(lua_State* ctx)
{
	int top = lua_gettop(ctx);
	printf("-- stack dump --\n");

	for (int i = 1; i <= top; i++) {
		int t = lua_type(ctx, i);

		switch (t) {
			case LUA_TBOOLEAN:
				fprintf(stdout, lua_toboolean(ctx, i) ? "true" : "false");
				break;
			case LUA_TSTRING:
				fprintf(stdout, "`%s'", lua_tostring(ctx, i));
				break;
			case LUA_TNUMBER:
				fprintf(stdout, "%g", lua_tonumber(ctx, i));
				break;
			default:
				fprintf(stdout, "%s", lua_typename(ctx, t));
				break;
		}
		fprintf(stdout, "  ");
	}

	fprintf(stdout, "\n");
}


static inline char* findresource(const char* arg, int searchmask)
{
	char* res = arcan_find_resource(arg, searchmask);
/* since this is invoked extremely frequently and is involved in file-system
 * related stalls, maybe a sort of caching mechanism should be implemented
 * (invalidate / refill every N ticks or have a flag to side-step it -- as a lot
 * of resources are quite static and the rest of the API have to handle missing
 * or bad resources anyhow, we also know which subdirectories to attach
 * to OS specific event monitoring effects */

	if (lua_ctx_store.debug){
		arcan_warning("Debug, resource lookup for %s, yielded: %s\n", arg, res);
	}

	return res;
}

static int arcan_lua_zapresource(lua_State* ctx)
{
	char* path = findresource(
		luaL_checkstring(ctx, 1), ARCAN_RESOURCE_THEME);

	if (path && unlink(path) != -1)
		lua_pushboolean(ctx, false);
	else
		lua_pushboolean(ctx, true);

	free(path);
	return 1;
}

static int arcan_lua_rawresource(lua_State* ctx)
{
	char* path = findresource(luaL_checkstring(ctx, 1), 
		ARCAN_RESOURCE_THEME | ARCAN_RESOURCE_SHARED);

	if (lua_ctx_store.rfile)
		fclose(lua_ctx_store.rfile);

	if (!path) {
		char* fname = arcan_expand_resource(luaL_checkstring(ctx, 1), false);
		if (fname){
			lua_ctx_store.rfile = fopen(fname, "w+");
			free(fname);
		}
	} 
	else
		lua_ctx_store.rfile = fopen(path, "r");

	lua_pushboolean(ctx, lua_ctx_store.rfile != NULL);
	free(path);
	return 1;
}

static char* chop(char* str)
{
    char* endptr = str + strlen(str) - 1;
    while(isspace(*str)) str++;

    if(!*str) return str;
    while(endptr > str && isspace(*endptr))
        endptr--;

    *(endptr+1) = 0;

    return str;
}

static int arcan_lua_readrawresource(lua_State* ctx)
{
	if (lua_ctx_store.rfile){
		char line[256];
		if (fgets(line, sizeof(line), lua_ctx_store.rfile) != NULL){
			lua_pushstring(ctx, chop( line ));
			return 1;
		}
	}

	return 0;
}

void arcan_lua_setglobalstr(lua_State* ctx, 
	const char* key, const char* val)
{
	lua_pushstring(ctx, val);
	lua_setglobal(ctx, key);
}

void arcan_lua_setglobalint(lua_State* ctx, const char* key, int val)
{
	lua_pushnumber(ctx, val);
	lua_setglobal(ctx, key);
}

static const char* luaL_lastcaller(lua_State* ctx)
{
	static char msg[1024];
	msg[1023] = '\0';

	lua_Debug dbg;
  lua_getstack(ctx, 1, &dbg);
	lua_getinfo(ctx, "nlS" ,&dbg);
	snprintf(msg, 1023, "%s:%d", dbg.short_src, dbg.currentline);

	return msg; 
}

static inline arcan_aobj_id luaaid_toaid(lua_Number innum)
{
	return (arcan_aobj_id) innum;
}

static inline arcan_vobj_id luavid_tovid(lua_Number innum)
{
	arcan_vobj_id res = ARCAN_VIDEO_WORLDID;

	if (innum != ARCAN_EID && innum != ARCAN_VIDEO_WORLDID)
		res = (arcan_vobj_id) innum - lua_ctx_store.lua_vidbase;
	else if (innum != res)
		res = ARCAN_EID;

	return res;
}

static inline lua_Number vid_toluavid(arcan_vobj_id innum)
{
	if (innum != ARCAN_EID && innum != ARCAN_VIDEO_WORLDID)
		innum += lua_ctx_store.lua_vidbase;

	return (double) innum;
}

static inline arcan_vobj_id luaL_checkvid(lua_State* ctx, int num)
{
	arcan_vobj_id res = luavid_tovid( luaL_checknumber(ctx, num) );

#ifdef _DEBUG
	arcan_vobject* vobj = arcan_video_getobject(luavid_tovid(res));
	if (vobj && vobj->flags.frozen)
		abort();

	if (res == ARCAN_EID){
		dump_call_trace(ctx);
		dump_stack(ctx);
		arcan_fatal("Bad VID requested (%"PRIxVOBJ")\n", res);
	}
#endif

	return res; 
}

static inline arcan_vobj_id luaL_checkaid(lua_State* ctx, int num)
{
	return luaL_checknumber(ctx, num);
}

static inline void lua_pushvid(lua_State* ctx, arcan_vobj_id id)
{
	if (id != ARCAN_EID && id != ARCAN_VIDEO_WORLDID)
		id += lua_ctx_store.lua_vidbase;

	lua_pushnumber(ctx, (double) id);
}

static inline void lua_pushaid(lua_State* ctx, arcan_aobj_id id)
{
	lua_pushnumber(ctx, id);
}

static int arcan_lua_rawclose(lua_State* ctx)
{
	bool res = false;

	if (lua_ctx_store.rfile) {
		res = fclose(lua_ctx_store.rfile);
		lua_ctx_store.rfile = NULL;
	}

	lua_pushboolean(ctx, res);
	return 1;
}

static int arcan_lua_pushrawstr(lua_State* ctx)
{
	bool res = false;
	const char* mesg = luaL_checkstring(ctx, 1);

	if (lua_ctx_store.rfile) {
		size_t fs = fwrite(mesg, strlen(mesg), 1, lua_ctx_store.rfile);
		res = fs == 1;
	}

	lua_pushboolean(ctx, res);
	return 1;
}

#ifdef _DEBUG
static int arcan_lua_freezeimage(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	arcan_vobject* vobj = arcan_video_getobject(vid);
	vobj->flags.frozen = true;
	return 0;
}
#endif

static int arcan_lua_loadimage(lua_State* ctx)
{
	arcan_vobj_id id = ARCAN_EID;
	char* path = findresource(luaL_checkstring(ctx, 1), 
		ARCAN_RESOURCE_SHARED | ARCAN_RESOURCE_THEME);
	uint8_t prio = luaL_optint(ctx, 2, 0);
	unsigned desw = luaL_optint(ctx, 3, 0);
	unsigned desh = luaL_optint(ctx, 4, 0);

	if (path)
		id = arcan_video_loadimage(path, arcan_video_dimensions(desw, desh), prio);

	free(path);
	lua_pushvid(ctx, id);
	return 1;
}

int arcan_lua_loadimageasynch(lua_State* ctx)
{
	arcan_vobj_id id = ARCAN_EID;
	intptr_t ref = 0;

	char* path = findresource(luaL_checkstring(ctx, 1), 
		ARCAN_RESOURCE_SHARED | ARCAN_RESOURCE_THEME);
	if (lua_isfunction(ctx, 2) && !lua_iscfunction(ctx, 2)){
		lua_pushvalue(ctx, 2);
		ref = luaL_ref(ctx, LUA_REGISTRYINDEX);
	}

	if (path && strlen(path) > 0){
		id = arcan_video_loadimageasynch(path, arcan_video_dimensions(0, 0), ref);
	}
	free(path);

	lua_pushvid(ctx, id);
	return 1;
}

int arcan_lua_imageloaded(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj)
		lua_pushnumber(ctx, vobj->feed.state.tag == ARCAN_TAG_IMAGE);
	else
		lua_pushnumber(ctx, false);

	return 1;
}

int arcan_lua_moveimage(lua_State* ctx)
{
	float newx = luaL_optnumber(ctx, 2, 0);
	float newy = luaL_optnumber(ctx, 3, 0);
	int time = luaL_optint(ctx, 4, 0);
	if (time < 0) time = 0;

/* array of VIDs or single VID */
	int argtype = lua_type(ctx, 1);
	if (argtype == LUA_TNUMBER){
		arcan_vobj_id id = luaL_checkvid(ctx, 1);
		arcan_video_objectmove(id, newx, newy, 1.0, time);
	}
	else if (argtype == LUA_TTABLE){
		int nelems = lua_rawlen(ctx, 1);

		for (int i = 0; i < nelems; i++){
			lua_rawgeti(ctx, 1, i+1);
			arcan_vobj_id id = luaL_checkvid(ctx, -1);
			arcan_video_objectmove(id, newx, newy, 1.0, time);
			lua_pop(ctx, 1);
		}
	}
	else
		arcan_fatal("arcan_lua_moveimage(), invalid argument (1) "
			"expected VID or indexed table of VIDs\n");

	return 0;
}

int arcan_lua_nudgeimage(lua_State* ctx)
{
	float newx = luaL_optnumber(ctx, 2, 0);
	float newy = luaL_optnumber(ctx, 3, 0);
	int time = luaL_optint(ctx, 4, 0);
	if (time < 0) time = 0;

	int argtype = lua_type(ctx, 1);
	if (argtype == LUA_TNUMBER){
		arcan_vobj_id id = luaL_checkvid(ctx, 1);
		surface_properties props = arcan_video_current_properties(id);
		arcan_video_objectmove(id, props.position.x + newx, 
			props.position.y + newy, 1.0, time);
	}
	else if (argtype == LUA_TTABLE){
		int nelems = lua_rawlen(ctx, 1);

		for (int i = 0; i < nelems; i++){
			lua_rawgeti(ctx, 1, i+1);
			arcan_vobj_id id = luaL_checkvid(ctx, -1);
			surface_properties props = arcan_video_current_properties(id);
			arcan_video_objectmove(id, props.position.x + newx, 
				props.position.y + newy, 1.0, time);
			lua_pop(ctx, 1);
		}
	}
	else
		arcan_fatal("arcan_lua_nudgeimage(), invalid argument (1) "
			"expected VID or indexed table of VIDs\n");

	return 0;
}

int arcan_lua_instanceimage(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_vobj_id newid = arcan_video_cloneobject(id);

	if (newid != ARCAN_EID){
		enum arcan_transform_mask lmask = MASK_SCALE | MASK_OPACITY 
			| MASK_POSITION | MASK_ORIENTATION;
		arcan_video_transformmask(newid, lmask);

		lua_pushvid(ctx, newid);
		return 1;
	}

	return 0;
}

int arcan_lua_resettransform(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_video_zaptransform(id);

	return 0;
}

int arcan_lua_instanttransform(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_video_instanttransform(id);

	return 0;
}

int arcan_lua_cycletransform(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	bool flag = luaL_checknumber(ctx, 2) != 0;

	arcan_video_transformcycle(sid, flag);

	return 0;
}

int arcan_lua_copytransform(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	arcan_vobj_id did = luaL_checkvid(ctx, 2);

	arcan_video_copytransform(sid, did);

	return 0;
}

int arcan_lua_transfertransform(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	arcan_vobj_id did = luaL_checkvid(ctx, 2);

	arcan_video_transfertransform(sid, did);

	return 0;
}

int arcan_lua_rotateimage(lua_State* ctx)
{
	float ang = luaL_checknumber(ctx, 2);
	int time = luaL_optint(ctx, 3, 0);

	int argtype = lua_type(ctx, 1);
	if (argtype == LUA_TNUMBER){
		arcan_vobj_id id = luaL_checkvid(ctx, 1);
		arcan_video_objectrotate(id, ang, 0.0, 0.0, time);
	}
	else if (argtype == LUA_TTABLE){
		int nelems = lua_rawlen(ctx, 1);

		for (int i = 0; i < nelems; i++){
			lua_rawgeti(ctx, 1, i+1);
			arcan_vobj_id id = luaL_checkvid(ctx, -1);
			arcan_video_objectrotate(id, ang, 0.0, 0.0, time);
			lua_pop(ctx, 1);
		}
	}
	else arcan_fatal("arcan_lua_rotateimage(), invalid argument (1) "
		"expected VID or indexed table of VIDs\n");

	return 0;
}

/* Input is absolute values,
 * arcan_video_objectscale takes relative to initial size */
int arcan_lua_scaleimage2(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	float neww = luaL_checknumber(ctx, 2);
	float newh = luaL_checknumber(ctx, 3);
	int time = luaL_optint(ctx, 4, 0);
	if (time < 0) time = 0;

	if (neww < 0.0001 && newh < 0.0001)
		return 0;

	surface_properties prop = arcan_video_initial_properties(id);
	if (prop.scale.x < 0.001 && prop.scale.y < 0.001) {
		lua_pushnumber(ctx, 0);
		lua_pushnumber(ctx, 0);
	}
	else {
		/* retain aspect ratio in scale */
		if (neww < 0.0001 && newh > 0.0001)
			neww = newh * (prop.scale.x / prop.scale.y);
		else
			if (neww > 0.0001 && newh < 0.0001)
				newh = neww * (prop.scale.y / prop.scale.x);

		neww = ceilf(neww);
		newh = ceilf(newh);
		arcan_video_objectscale(id, neww / prop.scale.x, 
			newh / prop.scale.y, 1.0, time);

		lua_pushnumber(ctx, neww);
		lua_pushnumber(ctx, newh);
	}

	return 2;
}

int arcan_lua_scaleimage(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);

	float desw = luaL_checknumber(ctx, 2);
	float desh = luaL_checknumber(ctx, 3);
	int time = luaL_optint(ctx, 4, 0);
	if (time < 0) time = 0;

	surface_properties cons = arcan_video_current_properties(id);
	surface_properties prop = arcan_video_initial_properties(id);

	/* retain aspect ratio in scale */
	if (desw < 0.0001 && desh > 0.0001)
		desw = desh * (prop.scale.x / prop.scale.y);
	else
		if (desw > 0.0001 && desh < 0.0001)
			desh = desw * (prop.scale.y / prop.scale.x);

	arcan_video_objectscale(id, desw, desh, 1.0, time);

	lua_pushnumber(ctx, desw);
	lua_pushnumber(ctx, desh);

	return 2;
}

int arcan_lua_orderimage(lua_State* ctx)
{
	unsigned int zv = luaL_checknumber(ctx, 2);

/* array of VIDs or single VID */
	int argtype = lua_type(ctx, 1);
	if (argtype == LUA_TNUMBER){
		arcan_vobj_id id = luaL_checkvid(ctx, 1);
		arcan_video_setzv(id, zv);
	}
	else if (argtype == LUA_TTABLE){
		int nelems = lua_rawlen(ctx, 1);

		for (int i = 0; i < nelems; i++){
			lua_rawgeti(ctx, 1, i+1);
			arcan_vobj_id id = luaL_checkvid(ctx, -1);
			arcan_video_setzv(id, zv);
			lua_pop(ctx, 1);
		}
	}
	else
		arcan_fatal("arcan_lua_orderimage(), invalid argument (1) "
			"expected VID or indexed table of VIDs\n");

	return 0;
}

int arcan_lua_maxorderimage(lua_State* ctx)
{
	lua_pushnumber(ctx, arcan_video_maxorder());
	return 1;
}

static inline void massopacity(lua_State* ctx, 
	float val, const char* caller)
{
	float time = luaL_optint(ctx, 3, 0); 

	int argtype = lua_type(ctx, 1);
	if (argtype == LUA_TNUMBER){
		arcan_vobj_id id = luaL_checkvid(ctx, 1);
		arcan_video_objectopacity(id, val, time);
	}
	else if (argtype == LUA_TTABLE){
		int nelems = lua_rawlen(ctx, 1);

		for (int i = 0; i < nelems; i++){
			lua_rawgeti(ctx, 1, i+1);
			arcan_vobj_id id = luaL_checkvid(ctx, -1);
			arcan_video_objectopacity(id, val, time);
			lua_pop(ctx, 1);
		}
	}
	else
		arcan_fatal("%s(), invalid argument (1) "
			"expected VID or indexed table of VIDs\n", caller);
}

int arcan_lua_imageopacity(lua_State* ctx)
{
	float val = luaL_checknumber(ctx, 2);
	massopacity(ctx, val, "arcan_lua_imageopacity");
	return 0;
}

int arcan_lua_showimage(lua_State* ctx)
{
	massopacity(ctx, 1.0, "arcan_lua_showimage");
	return 0;
}

int arcan_lua_hideimage(lua_State* ctx)
{
	massopacity(ctx, 0.0, "arcan_lua_hideimage");
	return 0;
}

int arcan_lua_forceblend(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	enum arcan_blendfunc mode = luaL_optnumber(ctx, 2, BLEND_FORCE);

	if (mode == BLEND_FORCE || mode == BLEND_ADD ||
		mode == BLEND_MULTIPLY || mode == BLEND_NONE || mode == BLEND_NORMAL)
			arcan_video_forceblend(id, mode);

	return 0;
}

int arcan_lua_imagepersist(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	lua_pushboolean(ctx, arcan_video_persistobject(id) == ARCAN_OK);
	return 1;
}

int arcan_lua_dropaudio(lua_State* ctx)
{
	arcan_audio_stop( luaL_checkaid(ctx, 1) );
	return 0;
}

int arcan_lua_gain(lua_State* ctx)
{
	arcan_aobj_id id = luaL_checkaid(ctx, 1);
	float gain = luaL_checknumber(ctx, 2);
	uint16_t time = luaL_optint(ctx, 3, 0);
	arcan_audio_setgain(id, gain, time);
	return 0;
}

int arcan_lua_playaudio(lua_State* ctx)
{
	arcan_aobj_id id = luaL_checkaid(ctx, 1);
	if (lua_isnumber(ctx, 2))
		arcan_audio_play(id, true, luaL_checknumber(ctx, 2));
	else
		arcan_audio_play(id, false, 0.0);

	return 0;
}

int arcan_lua_captureaudio(lua_State* ctx)
{
	char** cptlist = arcan_audio_capturelist();
	const char* luach = luaL_checkstring(ctx, 1);

	bool match = false;
	while (*cptlist && !match)
		match = strcmp(*cptlist++, luach) == 0;

	if (match){
		lua_pushaid(ctx, arcan_audio_capturefeed(luach) );

		return 1;
	}

	return 0;
}

int arcan_lua_capturelist(lua_State* ctx)
{
	char** cptlist = arcan_audio_capturelist();
	int count = 1;

	lua_newtable(ctx);
	int top = lua_gettop(ctx);

	while(*cptlist){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, *cptlist);
		lua_rawset(ctx, top);
		cptlist++;
	}

	return 1;
}

int arcan_lua_loadasample(lua_State* ctx)
{
	const char* rname = luaL_checkstring(ctx, 1);
	char* resource = findresource(rname, 
		ARCAN_RESOURCE_SHARED | ARCAN_RESOURCE_THEME);
	float gain = luaL_optnumber(ctx, 2, 1.0);
	arcan_aobj_id sid = arcan_audio_load_sample(resource, gain, NULL);
	free(resource);
	lua_pushaid(ctx, sid);
	return 1;
}

int arcan_lua_pauseaudio(lua_State* ctx)
{
	arcan_aobj_id id = luaL_checkaid(ctx, 1);
	arcan_audio_pause(id);
	return 0;
}

int arcan_lua_buildshader(lua_State* ctx)
{
	extern char* deffprg;
	extern char* defvprg;

	const char* vprog = luaL_optstring(ctx, 1, defvprg);
	const char* fprog = luaL_optstring(ctx, 2, deffprg);
	const char* label = luaL_checkstring(ctx, 3);

	arcan_shader_id rv = arcan_shader_build(label, NULL, vprog, fprog);
	lua_pushnumber(ctx, rv);
	return 1;
}

int arcan_lua_sharestorage(lua_State* ctx)
{
	arcan_vobj_id src = luaL_checkvid(ctx, 1);
	arcan_vobj_id dst = luaL_checkvid(ctx, 2);

	arcan_errc rv = arcan_video_shareglstore(src, dst);
	lua_pushboolean(ctx, rv == ARCAN_OK);
	
	return 1;
}

int arcan_lua_setshader(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_shader_id shid;
	bool stat;

	shid = lua_type(ctx, 2) == LUA_TSTRING ?
		arcan_shader_lookup(luaL_checkstring(ctx, 2)) : luaL_checknumber(ctx, 2);

	if (ARCAN_OK != arcan_video_setprogram(id, shid))
		arcan_warning("arcan_video_setprogram(%d, %d) -- couldn't set shader," 
			"invalid vobj or shader id specified.\n", id, shid);

	return 0;
}

int arcan_lua_setmeshshader(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_shader_id shid = luaL_checknumber(ctx, 2);
	int slot = abs ( luaL_checknumber(ctx, 3) );

	arcan_3d_meshshader(id, shid, slot);

	return 0;
}

int arcan_lua_strsize(lua_State* ctx)
{
	unsigned int width = 0, height = 0;
	const char* message = luaL_checkstring(ctx, 1);
	int vspacing = luaL_optint(ctx, 2, 4);
	int tspacing = luaL_optint(ctx, 2, 64);

	arcan_video_stringdimensions(message, vspacing, tspacing, NULL, 
		&width, &height);

	lua_pushnumber(ctx, width);
	lua_pushnumber(ctx, height);

	return 2;
}

int arcan_lua_buildstr(lua_State* ctx)
{
	arcan_vobj_id id  = ARCAN_EID;

	const char* message = luaL_checkstring(ctx, 1);
	int vspacing = luaL_optint(ctx, 2, 4);
	int tspacing = luaL_optint(ctx, 3, 64);

	unsigned int nlines = 0;
	unsigned int* lineheights = NULL;

	id = arcan_video_renderstring(message, vspacing, tspacing, NULL, 
		&nlines, &lineheights);

	lua_pushvid(ctx, id);

	lua_newtable(ctx);
	int top = lua_gettop(ctx);

	for (int i = 0; i < nlines; i++) {
		lua_pushnumber(ctx, i + 1);
		lua_pushnumber(ctx, lineheights[i]);
		lua_rawset(ctx, top);
	}

	if (lineheights)
		free(lineheights);

	return 2;
}

int arcan_lua_scaletxcos(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	float txs = luaL_checknumber(ctx, 2);
	float txt = luaL_checknumber(ctx, 3);

	arcan_video_scaletxcos(id, txs, txt);

	return 0;
}

int arcan_lua_settxcos_default(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_vobject* dst = arcan_video_getobject(id);
	bool mirror = luaL_optinteger(ctx, 2, 0) != 0;

	if (dst){
		if (mirror)
			generate_mirror_mapping(dst->txcos, 1.0, 1.0);
		else
			generate_basic_mapping(dst->txcos, 1.0, 1.0);
	}

	return 0;
}

int arcan_lua_settxcos(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	float txcos[8];

	if (arcan_video_retrieve_mapping(id, txcos) == ARCAN_OK){
		luaL_checktype(ctx, 2, LUA_TTABLE);
		int ncords = lua_rawlen(ctx, -1);
		if (ncords < 8){
			arcan_warning("Warning: lua_settxcos(), Too few elements in txco tables"
				"(expected 8, got %i)\n", ncords);
			return 0;
		}

		for (int i = 0; i < 8; i++){
			lua_rawgeti(ctx, -1, i+1);
			txcos[i] = lua_tonumber(ctx, -1);
			lua_pop(ctx, 1);
		}

		arcan_video_override_mapping(id, txcos);
	}

	return 0;
}

int arcan_lua_gettxcos(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	int rv = 0;
	float txcos[8] = {0};

	if (arcan_video_retrieve_mapping(id, txcos) == ARCAN_OK){
		lua_newtable(ctx);
		int top = lua_gettop(ctx);

		for (int i = 0; i < 8; i++){
			lua_pushnumber(ctx, i + 1);
			lua_pushnumber(ctx, txcos[i]);
			lua_rawset(ctx, top);
		}

		rv = 1;
	}

	return rv;
}

int arcan_lua_togglemask(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	unsigned val = luaL_checknumber(ctx, 2);

	if ( (val & !(MASK_ALL)) == 0){
		enum arcan_transform_mask mask = arcan_video_getmask(id);
		mask ^= ~val;
		arcan_video_transformmask(id, mask);
	}

	return 0;
}

int arcan_lua_clearall(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	if (id){
		arcan_video_transformmask(id, 0);
	}

	return 0;
}

static char* maskstr(enum arcan_transform_mask mask)
{
	char maskstr[72] = "";

	if ( (mask & MASK_POSITION) > 0)
		strcat(maskstr, "position ");

	if ( (mask & MASK_SCALE) > 0)
		strcat(maskstr, "scale ");

	if ( (mask & MASK_OPACITY) > 0)
		strcat(maskstr, "opacity ");

	if ( (mask & MASK_LIVING) > 0)
		strcat(maskstr, "living ");

	if ( (mask & MASK_ORIENTATION) > 0)
		strcat(maskstr, "orientation ");

	if ( (mask & MASK_UNPICKABLE) > 0)
		strcat(maskstr, "unpickable ");

	if ( (mask & MASK_FRAMESET) > 0)
		strcat(maskstr, "frameset ");

	if ( (mask & MASK_MAPPING) > 0)
		strcat(maskstr, "mapping ");

	return strdup(maskstr);
}

void dumpmask(arcan_vobj_id id)
{
	char* mask = maskstr( arcan_video_getmask(id) );
	arcan_warning("(%d:%s) => %s\n", id, arcan_video_readtag(id), mask);
	free(mask);
}

int arcan_lua_clearmask(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	unsigned val = luaL_checknumber(ctx, 2);

	if ( (val & !(MASK_ALL)) == 0){
		enum arcan_transform_mask mask = arcan_video_getmask(id);
		mask &= ~val;
		arcan_video_transformmask(id, mask);
	}

	return 0;
}

int arcan_lua_setmask(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	enum arcan_transform_mask val = luaL_checknumber(ctx, 2);

	if ( (val & !(MASK_ALL)) == 0){
		enum arcan_transform_mask mask = arcan_video_getmask(id);
		mask |= val;
		arcan_video_transformmask(id, mask);
	} else
		arcan_warning("Script Warning: image_mask_set(),"
			"	bad mask specified (%i)\n", val);

	return 0;
}

int arcan_lua_clipon(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	char clipm = luaL_optint(ctx, 2, ARCAN_CLIP_ON);

	arcan_video_setclip(id, clipm == ARCAN_CLIP_ON ? ARCAN_CLIP_ON :
		ARCAN_CLIP_SHALLOW);
    return 0;
}

int arcan_lua_clipoff(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_video_setclip(id, ARCAN_CLIP_OFF);
    return 0;
}

int arcan_lua_pick(lua_State* ctx)
{
	int x = luaL_checkint(ctx, 1);
	int y = luaL_checkint(ctx, 2);
	bool reverse = luaL_optint(ctx, 4, 0) != 0;

	unsigned int limit = luaL_optint(ctx, 3, 8);

	arcan_vobj_id* pickbuf = (arcan_vobj_id*) malloc(limit*sizeof(arcan_vobj_id));

	unsigned int count = reverse ? arcan_video_rpick(pickbuf, limit, x, y) : 
		arcan_video_pick(pickbuf, limit, x, y);
	unsigned int ofs = 1;

	lua_newtable(ctx);
	int top = lua_gettop(ctx);

	while (count--) {
		lua_pushnumber(ctx, ofs);
		lua_pushvid(ctx, pickbuf[ofs-1]);
		lua_rawset(ctx, top);
		ofs++;
	}

	free(pickbuf);
	return 1;
}

int arcan_lua_hittest(lua_State* state)
{
	arcan_vobj_id id = luaL_checkvid(state, 1);
	unsigned int x = luaL_checkint(state, 2);
	unsigned int y = luaL_checkint(state, 3);

	lua_pushboolean(state, arcan_video_hittest(id, x, y) != 0);

	return 1;
}

int arcan_lua_deleteimage(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	double srcid = luaL_checknumber(ctx, 1);

	/* possibly long journey,
	 * for a vid with a movie associated (or any feedfunc),
	 * the feedfunc will be invoked with the cleanup cmd
	 * which in the movie cause will trigger a full movie cleanup */
	arcan_errc rv = arcan_video_deleteobject(id);

	if (rv != ARCAN_OK){
		if (lua_ctx_store.debug > 0){
			arcan_warning("%s => arcan_lua_deleteimage(%.0lf=>%d) -- Object could not"
			" be deleted, invalid object specified.\n",luaL_lastcaller(ctx),srcid,id);
		}
		else{
			dump_call_trace(ctx);
			dump_stack(ctx);
			arcan_fatal("Theme tried to delete non-existing object (%.0lf=>%d) from "
			"(%s). Relaunch with debug flags (-g) to suppress.\n", 
			srcid, id, luaL_lastcaller(ctx));
		}
	}

	return 0;
}

int arcan_lua_setlife(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	int ttl = luaL_checkint(ctx, 2);
	
	if (ttl <= 0)
			return arcan_lua_deleteimage(ctx);
	else
		arcan_video_setlife(id, ttl);

	return 0;
}

/* to actually put this into effect, change value and pop the entire stack */
int arcan_lua_systemcontextsize(lua_State* ctx)
{
	unsigned newlim = luaL_checkint(ctx, 1);
	if (newlim > 1){
		newlim = newlim > 65536 ? 65536 : newlim;
		arcan_video_contextsize(newlim);
	}

	return 0;
}

int arcan_lua_dofile(lua_State* ctx)
{
	const char* instr = luaL_checkstring(ctx, 1);
	char* fname = findresource(instr, ARCAN_RESOURCE_THEME|ARCAN_RESOURCE_SHARED);
	int res = 0;

	if (fname){
		luaL_loadfile(ctx, fname);
		res = 1;
	}
	else{
		free(fname);
		arcan_warning("Script Warning: system_load(),"
		" couldn't find resource (%s)\n", instr);
	}

	free(fname);
	return res;
}

int arcan_lua_pausemovie(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	vfunc_state* state = arcan_video_feedstate(vid);

	if (state && state->tag == ARCAN_TAG_FRAMESERV && state->ptr)
		arcan_frameserver_pause((arcan_frameserver*) state->ptr, false);

	return 0;
}

int arcan_lua_resumemovie(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	vfunc_state* state = arcan_video_feedstate(vid);

	if (state && state->tag == ARCAN_TAG_FRAMESERV && state->ptr)
		arcan_frameserver_resume((arcan_frameserver*) state->ptr);

	return 0;
}

int arcan_lua_playmovie(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	vfunc_state* state = arcan_video_feedstate(vid);

	if (state && state->tag == ARCAN_TAG_FRAMESERV) {
		arcan_frameserver* movie = (arcan_frameserver*) state->ptr;
		arcan_frameserver_playback(movie);
		lua_pushvid(ctx, movie->vid);
		lua_pushaid(ctx, movie->aid);
		return 2;
	}
	else {
		arcan_vobject* vobj = arcan_video_getobject(vid);
		arcan_warning("arcan_lua_playmovie(%d) -- bad feed / no frameserver (%"
			PRIxPTR ",%d)\n", lua_ctx_store.lua_vidbase + vid, state,
			state ? state->tag : -1);
	}

	return 0;
}

/* NOTE: the is_special_res / loadmovie hack is slated to be replaced for the
 * hardening 0.3 branch, where the nopts / framequeue solution is dropped 
 * altogether in favor of doing synchronization work in frameserver- */
static bool is_special_res(const char* msg)
{
	return strncmp(msg, "device", 6) == 0 || strncmp(msg, "stream", 6) == 0 || 
		strncmp(msg, "capture", 7) == 0;
}

static int arcan_lua_loadmovie(lua_State* ctx)
{
	int loop = luaL_optint(ctx, 2, FRAMESERVER_NOLOOP);
	if (loop != FRAMESERVER_LOOP && loop != FRAMESERVER_NOLOOP){
		arcan_warning("arcan_lua_loadmovie() invalid second argument (%d) specified"
		",	should be FRAMESERVER_NOLOOP or FRAMESERVER_LOOP\n", loop);
		return 0;
	}

	const char* farg = luaL_checkstring(ctx, 1);

	bool special = is_special_res(farg);
	char* fname = special ? strdup(farg) : findresource(farg, 
		ARCAN_RESOURCE_THEME | ARCAN_RESOURCE_SHARED);
	intptr_t ref = (intptr_t) 0;

	const char* argstr = luaL_optstring(ctx, 5, "");
	bool force_nopts = luaL_optnumber(ctx, 6, 0);

	size_t optlen = strlen(argstr);

	if (!fname){
		arcan_warning("arcan_lua_loadmovie() -- unknown resource (%s)"
		"	specified.\n", fname);
		return 0;
	} 
	else{
		if (!special)
			if (optlen > 0){
				size_t fnlen = strlen(fname) + optlen + 8;
				char msg[fnlen];
				msg[fnlen-1] = 0;
				snprintf(msg, fnlen-1, "%s:file=%s", argstr, fname);
				fname = strdup(msg);	
			} 
			else
				fname = strdup(fname);
	}

/* in order to stay backward compatible API wise, 
 * the load_movie with function callback
 * will always need to specify loop condition. */
	if (lua_isfunction(ctx, 3) && !lua_iscfunction(ctx, 3)){
		lua_pushvalue(ctx, 3);
		ref = luaL_ref(ctx, LUA_REGISTRYINDEX);
	};

	arcan_frameserver* mvctx = arcan_frameserver_alloc();
	mvctx->loop     = loop == FRAMESERVER_LOOP;
	mvctx->autoplay = luaL_optint(ctx, 4, 0) > 0 || special;
	mvctx->nopts    = force_nopts || special;

	struct frameserver_envp args = {
		.use_builtin = true,
		.args.builtin.mode = "movie",
		.args.builtin.resource = fname
	};

	if ( arcan_frameserver_spawn_server(mvctx, args) == ARCAN_OK )
	{
		mvctx->tag = ref;

		arcan_video_objectopacity(mvctx->vid, 0.0, 0);
		lua_pushvid(ctx, mvctx->vid);
		lua_pushaid(ctx, mvctx->aid);
	} else {
		free(mvctx);
		lua_pushvid(ctx, ARCAN_EID);
		lua_pushvid(ctx, ARCAN_EID);
	}

	free(fname);

	return 2;
}

#ifndef ARCAN_LUA_NOLED
int arcan_lua_n_leds(lua_State* ctx)
{
	uint8_t id = luaL_checkint(ctx, 1);
	led_capabilities cap = arcan_led_capabilities(id);

	lua_pushnumber(ctx, cap.nleds);
	return 1;
}

int arcan_lua_led_intensity(lua_State* ctx)
{
	uint8_t id = luaL_checkint(ctx, 1);
	int8_t led = luaL_checkint(ctx, 2);
	uint8_t intensity = luaL_checkint(ctx, 3);

	lua_pushnumber(ctx,
		arcan_led_intensity(id, led, intensity));

	return 1;
}

int arcan_lua_led_rgb(lua_State* ctx)
{
	uint8_t id = luaL_checkint(ctx, 1);
	int8_t led = luaL_checkint(ctx, 2);
	uint8_t r  = luaL_checkint(ctx, 3);
	uint8_t g  = luaL_checkint(ctx, 4);
	uint8_t b  = luaL_checkint(ctx, 5);

	lua_pushnumber(ctx, arcan_led_rgb(id, led, r, g, b));
	return 1;
}

int arcan_lua_setled(lua_State* ctx)
{
	int id = luaL_checkint(ctx, 1);
	int num = luaL_checkint(ctx, 2);
	int state = luaL_checkint(ctx, 3);

	lua_pushnumber(ctx, state ? arcan_led_set(id, num):arcan_led_clear(id, num));

	return 1;
}
#endif

/* NOTE: a currently somewhat serious yet unhandled issue concerns what to do
 * with events fires from objects that no longer exist, e.g. the case with 
 * events in the queue preceeding a push_context, pop_context,
 * the -- possibly -- safest option would be to completely flush event queues 
 * between successful context pops, in such cases, it should be handled in the 
 * LUA layer */
int arcan_lua_pushcontext(lua_State* ctx)
{
/* make sure that we save one context for launch_external */

	if (arcan_video_nfreecontexts() > 1)
		lua_pushinteger(ctx, arcan_video_pushcontext());
	else
		lua_pushinteger(ctx, -1);

	return 1;
}

int arcan_lua_popcontext_ext(lua_State* ctx)
{
	arcan_vobj_id newid = ARCAN_EID;

	lua_pushinteger(ctx, arcan_video_nfreecontexts() > 1 ?
		arcan_video_extpopcontext(&newid) : -1);
	lua_pushvid(ctx, newid);	
	
	return 2;
}

int arcan_lua_pushcontext_ext(lua_State* ctx)
{
	arcan_vobj_id newid = ARCAN_EID;

	lua_pushinteger(ctx, arcan_video_nfreecontexts() > 1 ?
		arcan_video_extpushcontext(&newid) : -1);
	lua_pushvid(ctx, newid);	
	
	return 2;
}

int arcan_lua_popcontext(lua_State* ctx)
{
	lua_pushinteger(ctx, arcan_video_popcontext());
	return 1;
}

int arcan_lua_contextusage(lua_State* ctx)
{
	unsigned usecount;
	lua_pushinteger(ctx, arcan_video_contextusage(&usecount));
	lua_pushinteger(ctx, usecount);
	return 2;
}

static inline const char* intblstr(lua_State* ctx, int ind, const char* field){
	lua_getfield(ctx, ind, field);
	return lua_tostring(ctx, -1);
}

static inline int intblnum(lua_State* ctx, int ind, const char* field){
	lua_getfield(ctx, ind, field);
	return lua_tointeger(ctx, -1);
}

static inline bool intblbool(lua_State* ctx, int ind, const char* field){
	lua_getfield(ctx, ind, field);
	return lua_toboolean(ctx, -1);
}

/*
 * Next step in the input mess, take a properly formatted table,
 * convert it back into an arcan event, push that to the target_launcher 
 * or frameserver. The target_launcher will serialise it to a hijack function
 * which then decodes into a native format (currently most likely SDL). 
 * All this hassle (instead of creating a custom LUA object, 
 * tag it with the raw event and be done with it) is to allow the theme- to 
 * modify or even generate new ones based on in-theme actions.
 */

/* there is a slight API inconsistency here in that we had (iotbl, vid) 
 * in the first few versions while other functions tend to lead with vid, 
 * which causes some confusion. So we check whethere the table argument is first
 * or second, and extract accordingly, so both will work */
int arcan_lua_targetinput(lua_State* ctx)
{
	arcan_event ev = {.kind = 0, .category = EVENT_IO };
	int vidind, tblind;

/* swizzle if necessary */
	if (lua_type(ctx, 1) == LUA_TTABLE){
		vidind = 2;
		tblind = 1;
	} else {
		tblind = 2;
		vidind = 1;
	}

	arcan_vobj_id vid = luaL_checkvid(ctx, vidind);
	luaL_checktype(ctx, tblind, LUA_TTABLE);

	vfunc_state* vstate = arcan_video_feedstate(vid);

	if (!vstate || vstate->tag != ARCAN_TAG_FRAMESERV){
		lua_pushnumber(ctx, false);
		return 1;
	}

	const char* label = intblstr(ctx, tblind, "label");
	if (label)
		snprintf(ev.label, 16, "%s", label);

	/* populate all arguments */
	const char* kindlbl = intblstr(ctx, tblind, "kind");
	if (kindlbl == NULL)
		goto kinderr;

	if ( strcmp( kindlbl, "analog") == 0 ){
		ev.kind = EVENT_IO_AXIS_MOVE;
		ev.data.io.devkind = strcmp( intblstr(ctx, tblind, "source"), "mouse") == 0?
			EVENT_IDEVKIND_MOUSE : EVENT_IDEVKIND_GAMEDEV;
		ev.data.io.input.analog.devid  = intblnum(ctx, tblind, "devid");
		ev.data.io.input.analog.subid  = intblnum(ctx, tblind, "subid");
		ev.data.io.input.analog.gotrel = ev.data.io.devkind == EVENT_IDEVKIND_MOUSE;

	/*  sweep the samples subtable, add as many as present (or possible) */
		lua_getfield(ctx, tblind, "samples");
		size_t naxiss = lua_rawlen(ctx, -1);
		for (int i = 0; i < naxiss && 
			i < sizeof(ev.data.io.input.analog.axisval); i++){
			lua_rawgeti(ctx, -1, i+1);
			ev.data.io.input.analog.axisval[i] = lua_tointeger(ctx, -1);
			lua_pop(ctx, 1);
		}

	}
	else if (strcmp(kindlbl, "digital") == 0){
		if (intblbool(ctx, tblind, "translated")){
			ev.data.io.datatype = EVENT_IDATATYPE_TRANSLATED;
			ev.data.io.devkind  = EVENT_IDEVKIND_KEYBOARD;
			ev.data.io.input.translated.active    = intblbool(ctx, tblind, "active");
			ev.data.io.input.translated.scancode  = intblnum(ctx, tblind, "number");
			ev.data.io.input.translated.keysym    = intblnum(ctx, tblind, "keysym");
			ev.data.io.input.translated.modifiers = intblnum(ctx, tblind, "modifiers");
			ev.data.io.input.translated.devid     = intblnum(ctx, tblind, "devid");
			ev.data.io.input.translated.subid     = intblnum(ctx, tblind, "subid");
			ev.kind = ev.data.io.input.translated.active ? 
				EVENT_IO_KEYB_PRESS : EVENT_IO_KEYB_RELEASE;
		}
		else {
			ev.data.io.devkind = strcmp( intblstr(ctx, tblind, "source"), "mouse")==0?
				EVENT_IDEVKIND_MOUSE : EVENT_IDEVKIND_GAMEDEV;
			ev.data.io.datatype = EVENT_IDATATYPE_DIGITAL;
			ev.data.io.input.digital.active= intblbool(ctx, tblind, "active");
			ev.data.io.input.digital.devid = intblnum(ctx, tblind, "devid");
			ev.data.io.input.digital.subid = intblnum(ctx, tblind, "subid");
		}
	}
	else {
kinderr:
		arcan_warning("Script Warning: target_input(), unkown \"kind\""
			"	field in table.\n");
		lua_pushnumber(ctx, false);
		return 1;
	}

	arcan_frameserver_pushevent( (arcan_frameserver*) vstate->ptr, &ev );
	lua_pushnumber(ctx, true);
	return 1;
}

static int arcan_lua_targetsuspend(lua_State* ctx){
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);

	if (vid != ARCAN_EID){
		vfunc_state* state = arcan_video_feedstate(vid);
		if (state && state->ptr && state->tag == ARCAN_TAG_FRAMESERV){
			arcan_event ev = {
				.kind = TARGET_COMMAND_PAUSE,
				.category = EVENT_TARGET
			};

			arcan_frameserver_pushevent( (arcan_frameserver*) state->ptr, &ev);
		}
	}

	return 0;
}

static int arcan_lua_targetresume(lua_State* ctx){
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	if (vid != ARCAN_EID){
		vfunc_state* state = arcan_video_feedstate(vid);
		if (state && state->ptr && state->tag == ARCAN_TAG_FRAMESERV){
			arcan_event ev = {
				.kind = TARGET_COMMAND_UNPAUSE,
				.category = EVENT_TARGET
			};

			arcan_frameserver_pushevent( (arcan_frameserver*) state->ptr, &ev);
		}
	}

	return 0;
}

static bool arcan_lua_grabthemefunction(lua_State* ctx, const char* funame)
{
	if (strcmp(arcan_themename, funame) == 0)
		lua_getglobal(ctx, arcan_themename);
	else {
		char* tmpname = (char*) malloc(strlen(arcan_themename) + strlen(funame)+2);
			sprintf(tmpname, "%s_%s", arcan_themename, funame);
			lua_getglobal(ctx, tmpname);
		free(tmpname);
	}

	if (!lua_isfunction(ctx, -1)){
		lua_pop(ctx, 1);
		return false;
	}

	return true;
}

static inline int arcan_lua_funtable(lua_State* ctx, uint32_t kind){
	lua_newtable(ctx);
	int top = lua_gettop(ctx);
	lua_pushstring(ctx, "kind");
	lua_pushnumber(ctx, kind);
	lua_rawset(ctx, top);

	return top;
}

static inline void tblstr(lua_State* ctx, char* k, char* v, int top){
	lua_pushstring(ctx, k);
	lua_pushstring(ctx, v);
	lua_rawset(ctx, top);
}

static inline void tblnum(lua_State* ctx, char* k, double v, int top){
	lua_pushstring(ctx, k);
	lua_pushnumber(ctx, v);
	lua_rawset(ctx, top);
}

static inline void tblbool(lua_State* ctx, char* k, bool v, int top){
	lua_pushstring(ctx, k);
	lua_pushboolean(ctx, v);
	lua_rawset(ctx, top);
}

static char* to_utf8(uint16_t utf16)
{
	static char utf8buf[6] = {0};
	int count = 1, ofs = 0;
	uint32_t mask = 0x800;

	if (utf16 >= 0x80)
		count++;

	for (int i=0; i < 5; i++){
		if ( (uint32_t) utf16 >= mask )
			count++;

		mask <<= 5;
	}

	if (count == 1){
		utf8buf[0] = (char) utf16;
		utf8buf[1] = 0x00;
	} else {
		for (int i = count-1; i >= 0; i--){
			unsigned char ch = ( utf16 >> (6 * i)) & 0x3f;
			ch |= 0x80;
			if (i == count-1)
				ch |= 0xff << (8-count);
			utf8buf[ofs++] = ch;
		}
		utf8buf[ofs++] = 0x00;
	}

	return (char*) utf8buf;
}

int arcan_lua_scale3dverts(lua_State* ctx)
{
    arcan_vobj_id vid = luaL_checkvid(ctx, 1);
    arcan_3d_scalevertices(vid);
    return 0;
}

static void slimpush(char* dst, char ulim, char* inmsg)
{
	ulim--;

	while (*inmsg && ulim--)
		*dst++ = *inmsg++;

	*dst = '\0';
}

static char* streamtype(int num)
{
	switch (num){
		case 0: return "audio";
		case 1: return "video";
		case 2: return "text";
		case 3: return "overlay";
	}
	return "broken";
}

/* emit input() call based on a arcan_event,
 * uses a separate format and translation to make it easier
 * for the user to modify. Perhaps one field should have been used
 * to store the actual event, but it wouldn't really help extraction. */
void arcan_lua_pushevent(lua_State* ctx, arcan_event* ev)
{
	if (ev->category == EVENT_SYSTEM && arcan_lua_grabthemefunction(ctx, "system")){
		lua_newtable(ctx);
		int top = arcan_lua_funtable(ctx, ev->kind);
	}
	if (ev->category == EVENT_IO && arcan_lua_grabthemefunction(ctx, "input")){
		int naxis;
		int top = arcan_lua_funtable(ctx, ev->kind);

		lua_pushstring(ctx, "kind");

		switch (ev->kind) {
		case EVENT_IO_TOUCH:
			lua_pushstring(ctx, "touch");
			lua_rawset(ctx, top);

			tblnum(ctx, "devid",    ev->data.io.input.touch.devid,    top);
			tblnum(ctx, "subid",    ev->data.io.input.touch.subid,    top);
			tblnum(ctx, "pressure", ev->data.io.input.touch.pressure, top);
			tblnum(ctx, "size",     ev->data.io.input.touch.size,     top);
			tblnum(ctx, "x",        ev->data.io.input.touch.x,        top);
			tblnum(ctx, "y",        ev->data.io.input.touch.y,        top);
		break;

		case EVENT_IO_AXIS_MOVE:
			lua_pushstring(ctx, "analog");
			lua_rawset(ctx, top);

			tblstr(ctx, "source", ev->data.io.devkind == 
				EVENT_IDEVKIND_MOUSE ? "mouse" : "joystick", top);
			tblnum(ctx, "devid", ev->data.io.input.analog.devid, top);
			tblnum(ctx, "subid", ev->data.io.input.analog.subid, top);
			tblbool(ctx, "active", true, top);
			tblbool(ctx, "relative", ev->data.io.input.analog.gotrel,top);

			lua_pushstring(ctx, "samples");
			lua_newtable(ctx);
				int top2 = lua_gettop(ctx);
				for (int i = 0; i < ev->data.io.input.analog.nvalues; i++) {
					lua_pushnumber(ctx, i + 1);
					lua_pushnumber(ctx, ev->data.io.input.analog.axisval[i]);
					lua_rawset(ctx, top2);
				}
			lua_rawset(ctx, top);
		break;

		case EVENT_IO_BUTTON_PRESS:
		case EVENT_IO_BUTTON_RELEASE:
		case EVENT_IO_KEYB_PRESS:
		case EVENT_IO_KEYB_RELEASE:
			lua_pushstring(ctx, "digital");
			lua_rawset(ctx, top);

			if (ev->data.io.devkind == EVENT_IDEVKIND_KEYBOARD) {
				tblbool(ctx, "translated", true, top);
				tblnum(ctx, "number", ev->data.io.input.translated.scancode, top);
				tblnum(ctx, "keysym", ev->data.io.input.translated.keysym, top);
				tblnum(ctx, "modifiers", ev->data.io.input.translated.modifiers, top);
				tblnum(ctx, "devid", ev->data.io.input.translated.devid, top);
				tblnum(ctx, "subid", ev->data.io.input.translated.subid, top);
				tblstr(ctx, "utf8", to_utf8(ev->data.io.input.translated.subid), top);
				tblbool(ctx, "active", ev->kind == EVENT_IO_KEYB_PRESS 
					? true : false, top);
				tblstr(ctx, "device", "translated", top);
				tblstr(ctx, "subdevice", "keyboard", top);
			}
			else if (ev->data.io.devkind == EVENT_IDEVKIND_MOUSE ||
				ev->data.io.devkind == EVENT_IDEVKIND_GAMEDEV) {
				tblstr(ctx, "source", ev->data.io.devkind == EVENT_IDEVKIND_MOUSE ?
					"mouse" : "joystick", top);
				tblbool(ctx, "translated", false, top);
				tblnum(ctx, "devid", ev->data.io.input.digital.devid, top);
				tblnum(ctx, "subid", ev->data.io.input.digital.subid, top);
 				tblbool(ctx, "active", ev->data.io.input.digital.active, top);
			}
			else;
		break;

		default:
			lua_pushstring(ctx, "unknown");
			lua_rawset(ctx, top);
			arcan_warning("Engine -> Script: ignoring IO event: %i\n",ev->kind);
		}

		arcan_lua_wraperr(ctx, lua_pcall(ctx, 1, 0, 0), "push event( input )");
	}
	else if (ev->category == EVENT_TIMER){
		arcan_lua_setglobalint(ctx, "CLOCK", ev->tickstamp);

		if (arcan_lua_grabthemefunction(ctx, "clock_pulse")) {
			lua_pushnumber(ctx, ev->tickstamp);
			lua_pushnumber(ctx, ev->data.timer.pulse_count);
			arcan_lua_wraperr(ctx, lua_pcall(ctx, 2, 0, 0),"event loop: clock pulse");
		}
	}
	else if (ev->category == EVENT_NET){
		if (arcan_video_findparent(ev->data.external.source) == ARCAN_EID)
			return;

		arcan_vobject* vobj = arcan_video_getobject(ev->data.network.source);
		arcan_frameserver* fsrv = vobj ? vobj->feed.state.ptr : NULL;

		if (fsrv && fsrv->tag){
			intptr_t dst_cb = fsrv->tag;
			lua_rawgeti(ctx, LUA_REGISTRYINDEX, dst_cb);
			lua_pushvid(ctx, ev->data.network.source);

			lua_newtable(ctx);
			int top = lua_gettop(ctx);

			switch (ev->kind){
				case EVENT_NET_CONNECTED:
					tblstr(ctx, "kind", "connected", top);
					tblnum(ctx, "id", ev->data.network.connid, top);
					tblstr(ctx, "host", ev->data.network.host.addr, top);
				break;

				case EVENT_NET_DISCONNECTED:
					tblstr(ctx, "kind", "disconnected", top);
					tblnum(ctx, "id", ev->data.network.connid, top);
					tblstr(ctx, "host", ev->data.network.host.addr, top);
				break;

				case EVENT_NET_NORESPONSE:
					tblstr(ctx, "kind", "noresponse", top);
					tblstr(ctx, "host", ev->data.network.host.addr, top);
				break;

				case EVENT_NET_CUSTOMMSG:
					tblstr(ctx, "kind", "message", top);
					ev->data.network.message[ sizeof(ev->data.network.message) - 1] = 0;
					tblstr(ctx, "message", ev->data.network.message, top);
					tblnum(ctx, "id", ev->data.network.connid, top);
				break;

				case EVENT_NET_INPUTEVENT:
					arcan_warning("arcan_lua_pushevent(net_inputevent_not_handled )\n");
				break;

				default:
					arcan_warning("arcan_lua_pushevent( net_unknown %d )\n", ev->kind);
			}

			lua_ctx_store.cb_source_tag  = ev->data.external.source;
			lua_ctx_store.cb_source_kind = CB_SOURCE_FRAMESERVER;
			arcan_lua_wraperr(ctx, lua_pcall(ctx, 2, 0, 0), "event_net");

			lua_ctx_store.cb_source_kind = CB_SOURCE_NONE;
		}

	}
	else if (ev->category == EVENT_EXTERNAL){
		char mcbuf[65];
		if (arcan_video_findparent(ev->data.external.source) == ARCAN_EID)
			return;

/* need to jump through a few hoops to get hold of the possible callback */
		arcan_vobject* vobj = arcan_video_getobject(ev->data.external.source);

/* edge case, dangling event with frameserver 
 * that died during initialization but wasn't pruned from the eventqueue */
		if (vobj->feed.state.tag != ARCAN_TAG_FRAMESERV)
			return;

		arcan_frameserver* fsrv = vobj->feed.state.ptr;
		if (fsrv->tag){
			intptr_t dst_cb = fsrv->tag;
			lua_rawgeti(ctx, LUA_REGISTRYINDEX, dst_cb);
			lua_pushvid(ctx, ev->data.external.source);

			lua_newtable(ctx);
			int top = lua_gettop(ctx);
			switch (ev->kind){
				case EVENT_EXTERNAL_NOTICE_IDENT:
					tblstr(ctx, "kind", "ident", top);
					slimpush(mcbuf, sizeof(ev->data.external.message) /
						sizeof(ev->data.external.message[0]), ev->data.external.message);
					tblstr(ctx, "message", mcbuf, top);
				break;
				case EVENT_EXTERNAL_NOTICE_MESSAGE:
					slimpush(mcbuf, sizeof(ev->data.external.message) /
						sizeof(ev->data.external.message[0]), ev->data.external.message);	
					tblstr(ctx, "kind", "message", top);
					tblstr(ctx, "message", mcbuf, top); 
				break;
				case EVENT_EXTERNAL_NOTICE_FAILURE:
					tblstr(ctx, "kind", "failure", top);
					tblnum(ctx, "code", ev->data.external.code, top);
				break;
				case EVENT_EXTERNAL_NOTICE_FRAMESTATUS:
					tblstr(ctx, "kind", "frame", top);
					tblnum(ctx, "frame", 
						ev->data.external.framestatus.framenumber, top);
				break;

				case EVENT_EXTERNAL_NOTICE_STREAMINFO:
					slimpush(mcbuf, sizeof(ev->data.external.streaminf.message) /
						sizeof(ev->data.external.streamstat.timestr[0]),
						ev->data.external.streamstat.timestr);
					tblstr(ctx, "kind", "streaminfo", top);
					tblstr(ctx, "lang", mcbuf, top);
					tblnum(ctx, "streamid", ev->data.external.streaminf.streamid, top);
					tblstr(ctx, "type", 
						streamtype(ev->data.external.streaminf.datakind),top);
				break;

				case EVENT_EXTERNAL_NOTICE_STREAMSTATUS:
					tblstr(ctx, "kind", "streamstatus", top);
			 		slimpush(mcbuf, sizeof(ev->data.external.streamstat.timestr) /
						sizeof(ev->data.external.streamstat.timestr[0]), 
						ev->data.external.streamstat.timestr);
					tblstr(ctx, "ctime", mcbuf, top);
					slimpush(mcbuf, sizeof(ev->data.external.streamstat.timelim) /
						sizeof(ev->data.external.streamstat.timelim[0]),
						ev->data.external.streamstat.timelim);
					tblstr(ctx, "endtime", mcbuf, top);
					tblnum(ctx,"completion",ev->data.external.streamstat.completion,top);
					tblnum(ctx, "frameno", ev->data.external.streamstat.frameno, top);
					tblnum(ctx,"streaming",
						ev->data.external.streamstat.streaming!=0,top);
				break;

				case EVENT_EXTERNAL_NOTICE_STATESIZE:
					tblstr(ctx, "kind", "state_size", top);
					tblnum(ctx, "state_size", ev->data.external.state_sz, top);
				break;
				case EVENT_EXTERNAL_NOTICE_RESOURCE:
					tblstr(ctx, "kind", "resource_status", top);
					tblstr(ctx, "message", ev->data.external.message, top);
				break;
				default:
					tblstr(ctx, "kind", "unknown", top);
					tblnum(ctx, "kind_num", ev->kind, top);
			}

			lua_ctx_store.cb_source_tag  = ev->data.external.source;
			lua_ctx_store.cb_source_kind = CB_SOURCE_FRAMESERVER;
			arcan_lua_wraperr(ctx, lua_pcall(ctx, 2, 0, 0), "event_external");

			lua_ctx_store.cb_source_kind = CB_SOURCE_NONE;
		}

	}
	else if (ev->category == EVENT_FRAMESERVER){
		bool gotfun;
		intptr_t dst_cb = 0;

/*
 * drop frameserver events for which the queue object has died,
 * VID reuse won't actually be a problem unless the user ehrm, allocates/deletes
 * full 32-bit (-context_size) in one go
 */
		if (arcan_video_findparent(ev->data.frameserver.video) == ARCAN_EID)
			return;

		if (arcan_lua_grabthemefunction(ctx, "frameserver_event"))
			gotfun = true;
		else
			gotfun = (lua_pushnumber(ctx, 0), false);

		lua_pushvid(ctx, ev->data.frameserver.video);
		lua_newtable(ctx);
		int top = lua_gettop(ctx);

		switch(ev->kind){
			case EVENT_FRAMESERVER_LOOPED :
				tblstr(ctx, "kind", "frameserver_loop", top);
				dst_cb = ev->data.frameserver.otag;
			break;

			case EVENT_FRAMESERVER_TERMINATED :
				tblstr(ctx, "kind", "frameserver_terminated", top);
				dst_cb = ev->data.frameserver.otag;
			break;

			case EVENT_FRAMESERVER_RESIZED :
				tblstr(ctx, "kind", "resized", top);
				tblnum(ctx, "width", ev->data.frameserver.width, top);
				tblnum(ctx, "height", ev->data.frameserver.height, top);
				tblnum(ctx, "mirrored", ev->data.frameserver.glsource, top);
				tblnum(ctx, "source_audio", ev->data.frameserver.audio, top);

				dst_cb = ev->data.frameserver.otag;
			break;
		}

		if (dst_cb){
			lua_ctx_store.cb_source_tag = ev->data.frameserver.video;
			lua_ctx_store.cb_source_kind = CB_SOURCE_FRAMESERVER;
			lua_rawgeti(ctx, LUA_REGISTRYINDEX, dst_cb);
			lua_replace(ctx, 1);
			gotfun = true;
		}

		if (gotfun)
			arcan_lua_wraperr(ctx, lua_pcall(ctx, 2, 0, 0), "frameserver_event");
		else
			lua_settop(ctx, 0);

		lua_ctx_store.cb_source_kind = CB_SOURCE_NONE;
	}
	else if (ev->category == EVENT_VIDEO){
		intptr_t dst_cb = 0;
		arcan_vobject* srcobj;
		const char* evmsg = "video_event";
		const char* srcres = "";
		bool gotfun;

		if (arcan_lua_grabthemefunction(ctx, "video_event"))
/* due to the asynch- callback approach some additional 
 * checks are needed before bailing */
			gotfun = true;
		else
/* add placeholder if we find another function to call through */
			gotfun = (lua_pushnumber(ctx, 0), false);

		lua_pushvid(ctx, ev->data.video.source);
		lua_newtable(ctx);
		int top = lua_gettop(ctx);

		switch (ev->kind) {
		case EVENT_VIDEO_EXPIRE  : tblstr(ctx, "kind", "expired", top);
															 evmsg = "video_event(expire)"; break;
		case EVENT_VIDEO_SCALED  : tblstr(ctx, "kind", "scaled", top);
														 	 evmsg = "video_event(scale)";  break;
		case EVENT_VIDEO_MOVED   : tblstr(ctx, "kind", "moved", top);
															 evmsg = "video_event(move))";  break;
		case EVENT_VIDEO_BLENDED : tblstr(ctx, "kind", "blended", top);
															 evmsg = "video_event(blend)";  break;
		case EVENT_VIDEO_ROTATED : tblstr(ctx, "kind", "rotated", top);
															 evmsg = "video_event(rotate)"; break;

		case EVENT_VIDEO_ASYNCHIMAGE_LOADED:
			evmsg = "video_event(asynchimg_loaded)";
			tblstr(ctx, "kind", "loaded", top);
			tblnum(ctx, "width", ev->data.video.constraints.w, top);
			tblnum(ctx, "height", ev->data.video.constraints.h, top);
			dst_cb = (intptr_t) ev->data.video.data;

			if (dst_cb){
				evmsg = "video_event(asynchimg_loaded), callback";
				lua_ctx_store.cb_source_kind = CB_SOURCE_IMAGE;
			}
		break;

		case EVENT_VIDEO_ASYNCHIMAGE_LOAD_FAILED:
			srcobj = arcan_video_getobject(ev->data.video.source);
			evmsg = "video_event(asynchimg_load_fail), callback";
			tblstr(ctx, "kind", "load_failed", top);
			tblstr(ctx, "resource", srcobj && srcobj->vstore->vinf.text.source ?
				srcobj->vstore->vinf.text.source : "unknown", top);
			tblnum(ctx, "width", ev->data.video.constraints.w, top);
			tblnum(ctx, "height", ev->data.video.constraints.h, top);
			dst_cb = (intptr_t) ev->data.video.data;

			if (dst_cb){
				evmsg = "video_event(asynchimg_load_fail), callback";
				lua_ctx_store.cb_source_kind = CB_SOURCE_IMAGE;
			}
		break;

		default:
			arcan_warning("Engine -> Script Warning: arcan_lua_pushevent(),"
			"	unknown video event (%i)\n", ev->kind);
		}

		if (lua_ctx_store.cb_source_kind != CB_SOURCE_NONE){
			lua_ctx_store.cb_source_tag = ev->data.video.source;
			lua_rawgeti(ctx, LUA_REGISTRYINDEX, dst_cb);
			lua_replace(ctx, 1);
			gotfun = true;
		}

		if (gotfun)
			arcan_lua_wraperr(ctx, lua_pcall(ctx, 2, 0, 0), evmsg);
		else
			lua_settop(ctx, 0);

		lua_ctx_store.cb_source_kind = CB_SOURCE_NONE;
	}
	else if (ev->category == EVENT_AUDIO && 
		arcan_lua_grabthemefunction(ctx, "audio_event")){
		lua_pushaid(ctx, ev->data.video.source);
		lua_newtable(ctx);

		int top = lua_gettop(ctx);
		switch (ev->kind){
		case EVENT_AUDIO_BUFFER_UNDERRUN: tblstr(
			ctx, "kind", "audio buffer underrun", top); break;
		case EVENT_AUDIO_GAIN_TRANSFORMATION_FINISHED: tblstr(
			ctx, "kind", "gain transformed", top); break;
		case EVENT_AUDIO_PLAYBACK_FINISHED: tblstr(
			ctx, "kind", "playback finished", top); break;
		case EVENT_AUDIO_PLAYBACK_ABORTED: tblstr(
			ctx, "kind", "playback aborted", top); break;
		case EVENT_AUDIO_OBJECT_GONE: tblstr(ctx, "kind", "gone", top); break;
		default:
			arcan_warning("Engine -> Script Warning: arcan_lua_pushevent(),"
				"	unknown audio event (%i)\n", ev->kind);
		}
		arcan_lua_wraperr(ctx, lua_pcall(ctx, 2, 0, 0), "event loop: audio_event");
	}
}

int arcan_lua_imageparent(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_vobj_id pid = arcan_video_findparent(id);

	lua_pushvid( ctx, pid );
	return 1;
}

int arcan_lua_validvid(lua_State* ctx)
{
	arcan_vobj_id res = (arcan_vobj_id) luaL_optnumber(ctx, 1, ARCAN_EID);

	if (res != ARCAN_EID && res != ARCAN_VIDEO_WORLDID)
		res -= lua_ctx_store.lua_vidbase;

	if (res < 0)
		res = ARCAN_EID;

	lua_pushboolean(ctx, arcan_video_findparent(res) != ARCAN_EID);
	return 1;
}

int arcan_lua_imagechildren(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_vobj_id child;
	unsigned ofs = 0, count = 1;

	lua_newtable(ctx);
	int top = lua_gettop(ctx);

	while( (child = arcan_video_findchild(id, ofs++)) != ARCAN_EID){
		lua_pushnumber(ctx, count++);
		lua_pushnumber(ctx, child);
		lua_rawset(ctx, top);
	}

	return 1;
}

int arcan_lua_framesetalloc(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	unsigned num = luaL_checkint(ctx, 2);
	unsigned mode = luaL_optint(ctx, 3, ARCAN_FRAMESET_SPLIT);

	if (num >= 0 && num < 256){
		arcan_video_allocframes(sid, num, mode);
	}

	return 0;
}

int arcan_lua_framesetcycle(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	unsigned num = luaL_optint(ctx, 2, 0);
	arcan_video_framecyclemode(sid, num);
	return 0;
}

int arcan_lua_pushasynch(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	arcan_video_pushasynch(sid);
	return 0;
}

int arcan_lua_activeframe(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	unsigned num = luaL_checkint(ctx, 2);

	arcan_video_setactiveframe(sid, num);

	return 0;
}

int arcan_lua_origoofs(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	float xv = luaL_checknumber(ctx, 2);
	float yv = luaL_checknumber(ctx, 3);
	float zv = luaL_optnumber(ctx, 4, 0.0);

	arcan_video_origoshift(sid, xv, yv, zv);

	return 0;
}

int arcan_lua_orderinherit(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	bool origo = lua_toboolean(ctx, 2);
	arcan_video_inheritorder(sid, origo);
	return 0;
}

int arcan_lua_imageasframe(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	arcan_vobj_id did = luaL_checkvid(ctx, 2);
	unsigned num = luaL_checkint(ctx, 3);
	unsigned detach = luaL_optint(ctx, 4, FRAMESET_NODETACH);

	if (detach != FRAMESET_DETACH && detach != FRAMESET_NODETACH)
		arcan_fatal("set_image_as_frame() -- invalid 4th argument"
			"	(should be FRAMESET_DETACH or FRAMESET_NODETACH)\n");

	arcan_errc errc;
	arcan_vobj_id vid = arcan_video_setasframe(sid, did, num, 
		detach == FRAMESET_DETACH, &errc);

	if (errc == ARCAN_OK)
		lua_pushvid(ctx, vid != sid ? vid : ARCAN_EID);
	else{
		arcan_warning("arcan_lua_imageasframe(%d) failed, couldn't set (%d)"
			"	in slot (%d)\n", sid, did, num);
		lua_pushvid(ctx, ARCAN_EID);
	}

	return 1;
}

int arcan_lua_linkimage(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	arcan_vobj_id did = luaL_checkvid(ctx, 2);

	enum arcan_transform_mask smask = arcan_video_getmask(sid);
	smask |= MASK_LIVING;

	arcan_errc rv = arcan_video_linkobjs(sid, did, smask);
	lua_pushboolean(ctx, rv == ARCAN_OK);

	return 1;
}

static inline int pushprop(lua_State* ctx, 
	surface_properties prop, unsigned short zv)
{
	lua_createtable(ctx, 0, 6);

	lua_pushstring(ctx, "x");
	lua_pushnumber(ctx, prop.position.x);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "y");
	lua_pushnumber(ctx, prop.position.y);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "z");
	lua_pushnumber(ctx, prop.position.z);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "width");
	lua_pushnumber(ctx, prop.scale.x);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "height");
	lua_pushnumber(ctx, prop.scale.y);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "angle");
	lua_pushnumber(ctx, prop.rotation.roll);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "roll");
	lua_pushnumber(ctx, prop.rotation.roll);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "pitch");
	lua_pushnumber(ctx, prop.rotation.pitch);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "yaw");
	lua_pushnumber(ctx, prop.rotation.yaw);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "opacity");
	lua_pushnumber(ctx, prop.opa);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "order");
	lua_pushnumber(ctx, zv);
	lua_rawset(ctx, -3);

	return 1;
}

int arcan_lua_loadmesh(lua_State* ctx)
{
	arcan_vobj_id did = luaL_checkvid(ctx, 1);
	unsigned nmaps = luaL_optnumber(ctx, 3, 1);
	char* path = findresource(luaL_checkstring(ctx, 2), 
		ARCAN_RESOURCE_SHARED | ARCAN_RESOURCE_THEME);

	if (path){
			data_source indata = arcan_open_resource(path);
			if (indata.fd != BADFD){
				arcan_errc rv = arcan_3d_addmesh(did, indata, nmaps);
				if (rv != ARCAN_OK)
					arcan_warning("arcan_lua_loadmesh(%s) -- "
						"Couldn't add mesh to (%d)\n", path, did);
				arcan_release_resource(&indata);
			}
	}

	free(path);
	return 0;
}

int arcan_lua_attrtag(lua_State* ctx)
{
	arcan_vobj_id did = luaL_checkvid(ctx, 1);
	const char*  attr = luaL_checkstring(ctx, 2);
	int         state =  luaL_checknumber(ctx, 3);

	if (strcmp(attr, "infinite") == 0)
		lua_pushboolean(ctx, 
			arcan_3d_infinitemodel(did, state != 0) != ARCAN_OK);
	else
		lua_pushboolean(ctx, false);

	return 1;
}

int arcan_lua_buildmodel(lua_State* ctx)
{
	arcan_vobj_id id = ARCAN_EID;
	id = arcan_3d_emptymodel();

	if (id != ARCAN_EID)
		arcan_video_objectopacity(id, 0, 0);

	lua_pushvid(ctx, id);
	return 1;
}

int arcan_lua_buildplane(lua_State* ctx)
{
	float minx     = luaL_checknumber(ctx, 1);
	float mind     = luaL_checknumber(ctx, 2);
	float endx     = luaL_checknumber(ctx, 3);
	float endd     = luaL_checknumber(ctx, 4);
	float starty   = luaL_checknumber(ctx, 5);
	float hdens    = luaL_checknumber(ctx, 6);
	float ddens    = luaL_checknumber(ctx, 7);
	unsigned nmaps = luaL_optnumber(ctx, 8, 1);

	lua_pushvid(ctx, arcan_3d_buildplane(minx, mind, endx, endd, starty, 
		hdens, ddens, nmaps));
	return 1;
}

int arcan_lua_buildbox(lua_State* ctx)
{
	point minp, maxp;

	minp.x = luaL_checknumber(ctx, 1);
	minp.y = luaL_checknumber(ctx, 2);
	minp.z = luaL_checknumber(ctx, 3);
	maxp.x = luaL_checknumber(ctx, 4);
	maxp.y = luaL_checknumber(ctx, 5);
	maxp.z = luaL_checknumber(ctx, 6);

	return 0;
}

int arcan_lua_swizzlemodel(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	arcan_errc rv = arcan_3d_swizzlemodel(id);
	lua_pushboolean(ctx, rv == ARCAN_OK);

	return 1;
}

int arcan_lua_camtag(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	float w = arcan_video_display.width;
	float h = arcan_video_display.height;
	float ar = w / h > 1.0 ? w / h : h / w;

	float nv  = luaL_optnumber(ctx, 2, 0.1);
	float fv  = luaL_optnumber(ctx, 3, 100.0);
	float fov = luaL_optnumber(ctx, 4, 45.0);
	ar = luaL_optnumber(ctx, 5, ar);	
	bool front = luaL_optnumber(ctx, 6, true);
	bool back  = luaL_optnumber(ctx, 7, false);

	float projection[16];
	build_projection_matrix(projection, nv, fv, ar, fov);
	arcan_errc rv = arcan_3d_camtag(id, projection, front, back); 

	lua_pushboolean(ctx, rv == ARCAN_OK);
	return 1;
}

int arcan_lua_camtaghmd(lua_State* ctx)
{
	arcan_vobj_id lid = luaL_checkvid(ctx, 1);
	arcan_vobj_id rid = luaL_checkvid(ctx, 2);
	
	float nv = luaL_checknumber(ctx, 3);
	float fv  = luaL_checknumber(ctx, 4);
	float ipd = luaL_checknumber(ctx, 5);

	bool front = luaL_optnumber(ctx, 6, true);
	bool back  = luaL_optnumber(ctx, 7, false);

	float projection[16] = {0};
	float etsd = 0.0640; 
	float fov = 90.0;

	float w = arcan_video_display.width;
	float h = arcan_video_display.height;
	float ar = 2.0 * w / h;

	build_projection_matrix(projection, nv, fv, ar, fov);

/*
 * retrieve values from HMD,
 * hscreen (m), vscreen (m), vscreencenter(x, y)
 * eyetoscreendist (m), ipd, hres, vres
 */
	
	bool rv = arcan_3d_camtag(lid, projection, front, back) == ARCAN_OK && 
		arcan_3d_camtag(rid, projection, front, back) == ARCAN_OK;

	lua_pushboolean(ctx, rv == ARCAN_OK);

	lua_pushnumber(ctx, 1.0);
	lua_pushnumber(ctx, 0.22);
	lua_pushnumber(ctx, 0.24);
	lua_pushnumber(ctx, 0);

	return 5;	
}

int arcan_lua_getimageprop(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	long long dt = luaL_optnumber(ctx, 2, 0);
	surface_properties prop;

	if (dt < 0)
		dt = LONG_MAX;

	prop = dt > 0 ? 
		arcan_video_properties_at(id, dt) : arcan_video_current_properties(id);

	return pushprop(ctx, prop, arcan_video_getzv(id));
}

int arcan_lua_getimageresolveprop(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	unsigned dt = luaL_optnumber(ctx, 2, 0);
/* FIXME: resolve_properties does not take dt into account */
	surface_properties prop = arcan_video_resolve_properties(id);

	return pushprop(ctx, prop, arcan_video_getzv(id));
}

int arcan_lua_getimageinitprop(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	surface_properties prop = arcan_video_initial_properties(id);

	return pushprop(ctx, prop, arcan_video_getzv(id));
}

int arcan_lua_getimagestorageprop(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	img_cons cons = arcan_video_storage_properties(id);
	lua_createtable(ctx, 0, 6);

	lua_pushstring(ctx, "bpp");
	lua_pushnumber(ctx, cons.bpp);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "height");
	lua_pushnumber(ctx, cons.h);
	lua_rawset(ctx, -3);

	lua_pushstring(ctx, "width");
	lua_pushnumber(ctx, cons.w);
	lua_rawset(ctx, -3);

	return 1;
}

int arcan_lua_copyimageprop(lua_State* ctx)
{
	arcan_vobj_id sid = luaL_checkvid(ctx, 1);
	arcan_vobj_id did = luaL_checkvid(ctx, 2);

	arcan_video_copyprops(sid, did);

	return 0;
}

int arcan_lua_storekey(lua_State* ctx)
{
	if (lua_type(ctx, 1) == LUA_TTABLE){
		lua_pushnil(ctx);

		while (lua_next(ctx, 1) != 0){
			const char* key = lua_tostring(ctx, -2);
			const char* val = lua_tostring(ctx, -1);
			arcan_db_kv(dbhandle, key, val);
			lua_pop(ctx, 1);
		}

/* end transaction */
		arcan_db_kv(dbhandle, NULL, NULL);

	} else {
		const char* key = luaL_checkstring(ctx, 1);
		const char* name = luaL_checkstring(ctx, 2);
		arcan_db_theme_kv(dbhandle, arcan_themename, key, name);
	}

	return 0;
}

int arcan_lua_getkey(lua_State* ctx)
{
	const char* key = luaL_checkstring(ctx, 1);
	char* val = arcan_db_theme_val(dbhandle, arcan_themename, key);

	if (val) {
		lua_pushstring(ctx, val);
		free(val);
	}
	else
		lua_pushnil(ctx);

	return 1;
}

static int get_linenumber(lua_State* ctx)
{
	lua_Debug ar;
  lua_getstack(ctx, 1, &ar);
	lua_getinfo(ctx, "Sln", &ar);
	return -1;
}

int arcan_lua_gamefamily(lua_State* ctx)
{
	const int gameid = luaL_checkinteger(ctx, 1);
	arcan_dbh_res res = arcan_db_game_siblings(dbhandle, NULL, gameid);
	int rv = 0;

	if (res.kind == 0 && res.data.strarr) {
		char** curr = res.data.strarr;
		unsigned int count = 1; /* 1 indexing, seriously LUA ... */

		curr = res.data.strarr;

		lua_newtable(ctx);
		int top = lua_gettop(ctx);

		while (*curr) {
			lua_pushnumber(ctx, count++);
			lua_pushstring(ctx, *curr++);
			lua_rawset(ctx, top);
		}

		rv = 1;
		arcan_db_free_res(dbhandle, res);
	}

	return rv;
}

static int push_stringres(lua_State* ctx, arcan_dbh_res res)
{
	int rv = 0;

	if (res.kind == 0 && res.data.strarr) {
		char** curr = res.data.strarr;
		unsigned int count = 1; /* 1 indexing, seriously LUA ... */

		curr = res.data.strarr;

		lua_newtable(ctx);
		int top = lua_gettop(ctx);

		while (*curr) {
			lua_pushnumber(ctx, count++);
			lua_pushstring(ctx, *curr++);
			lua_rawset(ctx, top);
		}

		rv = 1;
	}

	return rv;
}

int arcan_lua_kbdrepeat(lua_State* ctx)
{
	unsigned rrate = luaL_checknumber(ctx, 1);
	arcan_event_keyrepeat(arcan_event_defaultctx(), rrate);
	return 0;
}

int arcan_lua_3dorder(lua_State* ctx)
{
	int order = luaL_checknumber(ctx, 1);

	if (order != order3d_first && order != order3d_last && order != order3d_none)
		arcan_fatal("arcan_lua_3dorder(%d) invalid order specified (%d),"
			"	expected ORDER_FIRST, ORDER_LAST or ORDER_NONE\n");

	arcan_video_3dorder(order);
	return 0;
}

int arcan_lua_mousegrab(lua_State* ctx)
{
	int mode =  luaL_optint( ctx, 1, -1);

	if (mode == -1)
		lua_ctx_store.grab = !lua_ctx_store.grab;
	else if (mode == MOUSE_GRAB_ON)
		lua_ctx_store.grab = true;
	else if (mode == MOUSE_GRAB_OFF)
		lua_ctx_store.grab = false;

	arcan_device_lock(0, lua_ctx_store.grab);
	lua_pushboolean(ctx, lua_ctx_store.grab);

	return 1;
}

int arcan_lua_gettargets(lua_State* ctx)
{
	int rv = 0;

	arcan_dbh_res res = arcan_db_targets(dbhandle);
	rv += push_stringres(ctx, res);
	arcan_db_free_res(dbhandle, res);

	return rv;
}

int arcan_lua_getgenres(lua_State* ctx)
{
	int rv = 0;
	arcan_dbh_res res = arcan_db_genres(dbhandle, false);
	rv = push_stringres(ctx, res);
	arcan_db_free_res(dbhandle, res);

	res = arcan_db_genres(dbhandle, true);
	rv += push_stringres(ctx, res);
	arcan_db_free_res(dbhandle, res);

	return rv;
}

int arcan_lua_fillsurface(lua_State* ctx)
{
	img_cons cons = {.w = 32, .h = 32, .bpp = 4};

	int desw = luaL_checknumber(ctx, 1);
	int desh = luaL_checknumber(ctx, 2);

	uint8_t r = luaL_checknumber(ctx, 3);
	uint8_t g = luaL_checknumber(ctx, 4);
	uint8_t b = luaL_checknumber(ctx, 5);

	cons.w = luaL_optnumber(ctx, 6, 8);
	cons.h = luaL_optnumber(ctx, 7, 8);

	if (cons.w > 0 && cons.w <= MAX_SURFACEW &&
		cons.h > 0 && cons.h <= MAX_SURFACEH){

		uint8_t* buf = (uint8_t*) malloc(cons.w * cons.h * 4);
		if (!buf)
			goto error;

		uint32_t* cptr = (uint32_t*) buf;

		for (int y = 0; y < cons.h; y++)
			for (int x = 0; x < cons.w; x++)
				RGBAPACK(r, g, b, 0xff, cptr++);

		arcan_vobj_id id = arcan_video_rawobject(buf, cons.w * cons.h * 4, cons, 
			desw, desh, 0);
		lua_pushvid(ctx, id);
		return 1;
	}
	else {
		arcan_fatal("arcan_lua_fillsurface(%d, %d) failed, unacceptable "
			"surface dimensions. Compile time restriction (%d,%d)\n",
			desw, desh, MAX_SURFACEW, MAX_SURFACEH);
	}

error:
	return 0;
}

int arcan_lua_imagecolor(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	uint8_t cred = luaL_checknumber(ctx, 2);
	uint8_t cgrn = luaL_checknumber(ctx, 3);
	uint8_t cblu = luaL_checknumber(ctx, 4);

	arcan_vobject* vobj = arcan_video_getobject(vid);
	if (!vobj || vobj->vstore->txmapped){
		lua_pushboolean(ctx, false);
		return 1;
	}

	vobj->vstore->vinf.col.r = (float)cred / 255.0f;
	vobj->vstore->vinf.col.g = (float)cgrn / 255.0f;
	vobj->vstore->vinf.col.b = (float)cblu / 255.0f;

	lua_pushboolean(ctx, true);
	return 1;
}

int arcan_lua_colorsurface(lua_State* ctx)
{
	int desw = luaL_checknumber(ctx, 1);
	int desh = luaL_checknumber(ctx, 2);
	int cred = luaL_checknumber(ctx, 3);
	int cgrn = luaL_checknumber(ctx, 4);
	int cblu = luaL_checknumber(ctx, 5);
	int order = luaL_optnumber(ctx, 6, 1);

	lua_pushvid(ctx, arcan_video_solidcolor(desw, desh, 
		cred, cgrn, cblu, order));
	return 1;
}

int arcan_lua_nullsurface(lua_State* ctx)
{
	int desw = luaL_checknumber(ctx, 1);
	int desh = luaL_checknumber(ctx, 2);
	int order = luaL_optnumber(ctx, 3, 1);

	lua_pushvid(ctx, arcan_video_nullobject(desw, desh, order) );
	return 1;	
}

int arcan_lua_rawsurface(lua_State* ctx)
{
	int desw = luaL_checknumber(ctx, 1);
	int desh = luaL_checknumber(ctx, 2);
	int bpp  = luaL_checknumber(ctx, 3);

	if (bpp != 1 && bpp != 3 && bpp != 4)
		arcan_fatal("arcan_lua_rawsurface(), invalid source channel count (%d)"
			"	accepted values: 1, 2, 4\n", bpp);

	img_cons cons = {.w = desw, .h = desh, .bpp = GL_PIXEL_BPP};

	int nsamples = lua_rawlen(ctx, 4);

	if (nsamples != desw * desh * bpp)
		arcan_fatal("arcan_lua_rawsurface(), number of values (%d) doesn't match"
			"	expected length (%d).\n", nsamples, desw * desh * bpp);

	unsigned ofs = 1;

	if (desw > 0 && desh > 0 && desw <= MAX_SURFACEW && desh <= MAX_SURFACEH){
		uint8_t* buf   = malloc(desw * desh * GL_PIXEL_BPP);
		uint32_t* cptr = (uint32_t*) buf;

		for (int y = 0; y < cons.h; y++)
			for (int x = 0; x < cons.w; x++){
				unsigned char r, g, b, a;
				switch(bpp){
					case 1:
						lua_rawgeti(ctx, 4, ofs++);
						r = lua_tonumber(ctx, -1);
						lua_pop(ctx, 1);
						RGBAPACK( r, r, r, 0xff, cptr++ );
					break;

					case 3:
						lua_rawgeti(ctx, 4, ofs++);
						r = lua_tonumber(ctx, -1);
						lua_rawgeti(ctx, 4, ofs++);
						g = lua_tonumber(ctx, -1);
						lua_rawgeti(ctx, 4, ofs++);
						b = lua_tonumber(ctx, -1);
						lua_pop(ctx, 3);
						RGBAPACK(r, g, b, 0xff, cptr++);
					break;

					case 4:
						lua_rawgeti(ctx, 4, ofs++);
						r = lua_tonumber(ctx, -1);
						lua_rawgeti(ctx, 4, ofs++);
						g = lua_tonumber(ctx, -1);
						lua_rawgeti(ctx, 4, ofs++);
						b = lua_tonumber(ctx, -1);
						lua_rawgeti(ctx, 4, ofs++);
						a = lua_tonumber(ctx, -1);
						lua_pop(ctx, 4);
						RGBAPACK(r, g, b, a, cptr++);
					}
			}

		arcan_vobj_id id = arcan_video_rawobject(buf, 
			cons.w * cons.h * GL_PIXEL_BPP, cons, desw, desh, 0);
		lua_pushvid(ctx, id);
		return 1;
	}
	else
		arcan_fatal("arcan_lua_rawsurface(%d, %d) unacceptable surface dimensions, "
			"compile time restriction 0 > (w,y) <= (%d,%d)\n", 
			desw, desh, MAX_SURFACEW, MAX_SURFACEH);

	return 0;
}

#if _WIN32
/* mingw headers miss this one for some reason, but it's still in the header */
int random(void);
#endif

/* not intendend to be used as a low-frequency noise function (duh) */
int arcan_lua_randomsurface(lua_State* ctx)
{
	int desw = abs( luaL_checknumber(ctx, 1) );
	int desh = abs( luaL_checknumber(ctx, 2) );
	img_cons cons = {.w = desw, .h = desh, .bpp = 4};

	uint32_t* cptr = (uint32_t*) malloc(desw * desh * 4);
	uint8_t* buf = (uint8_t*) cptr;

	for (int y = 0; y < cons.h; y++)
		for (int x = 0; x < cons.w; x++){
			unsigned char val = 20 + random() % 235;
			RGBAPACK(val, val, val, 0xff, cptr++);
		}

	arcan_vobj_id id = arcan_video_rawobject(buf, desw * desh * 4, cons,
		desw, desh, 0);
	arcan_video_objectfilter(id, ARCAN_VFILTER_NONE);
	lua_pushvid(ctx, id);

	return 1;
}

int arcan_lua_getcmdline(lua_State* ctx)
{
	int gameid = 0;
	arcan_errc status = ARCAN_OK;

	if (lua_isstring(ctx, 1)){
		gameid = arcan_db_gameid(dbhandle, luaL_checkstring(ctx, 1), &status);
	}
	else
		gameid = luaL_checknumber(ctx, 1);

	if (status != ARCAN_OK)
		return 0;

	int internal = luaL_optnumber(ctx, 2, 1);

	arcan_dbh_res res = arcan_db_launch_options(dbhandle, gameid, internal == 0);
	int rv = push_stringres(ctx, res);

	arcan_db_free_res(dbhandle, res);
	return rv;
}


void pushgame(lua_State* ctx, arcan_db_game* curr)
{
	int top = lua_gettop(ctx);

	lua_pushstring(ctx, "gameid");
	lua_pushnumber(ctx, curr->gameid);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "targetid");
	lua_pushnumber(ctx, curr->targetid);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "title");
	lua_pushstring(ctx, curr->title);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "genre");
	lua_pushstring(ctx, curr->genre);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "subgenre");
	lua_pushstring(ctx, curr->subgenre);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "setname");
	lua_pushstring(ctx, curr->setname);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "buttons");
	lua_pushnumber(ctx, curr->n_buttons);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "manufacturer");
	lua_pushstring(ctx, curr->manufacturer);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "players");
	lua_pushnumber(ctx, curr->n_players);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "input");
	lua_pushnumber(ctx, curr->input);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "year");
	lua_pushnumber(ctx, curr->year);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "target");
	lua_pushstring(ctx, curr->targetname);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "launch_counter");
	lua_pushnumber(ctx, curr->launch_counter);
	lua_rawset(ctx, top);
	lua_pushstring(ctx, "system");
	lua_pushstring(ctx, curr->system);
	lua_rawset(ctx, top);
}

/* sort-order (asc or desc),
 * year
 * n_players
 * n_buttons
 * genre
 * subgenre
 * manufacturer */
int arcan_lua_filtergames(lua_State* ctx)
{
	int year = -1;
	int n_players = -1;
	int n_buttons = -1;
	int input = 0;
	char* title = NULL;
	char* genre = NULL;
	char* subgenre = NULL;
	char* target = NULL;
	char* manufacturer = NULL;
	char* system = NULL;

	int rv = 0;
	int limit = 0;
	int offset = 0;

/* reason for all this is that lua_tostring MAY return NULL,
 * and if it doesn't, the string can be subject to garbage collection after POP,
 * thus need a working copy */
	luaL_checktype(ctx, 1, LUA_TTABLE);
	char* tv;
	/* populate all arguments */
	lua_pushstring(ctx, "year");
	lua_gettable(ctx, -2);
	year = lua_tonumber(ctx, -1);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "limit");
	lua_gettable(ctx, -2);
	limit = lua_tonumber(ctx, -1);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "offset");
	lua_gettable(ctx, -2);
	offset= lua_tonumber(ctx, -1);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "input");
	lua_gettable(ctx, -2);
	input = lua_tonumber(ctx, -1);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "players");
	lua_gettable(ctx, -2);
	n_players = lua_tonumber(ctx, -1);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "buttons");
	lua_gettable(ctx, -2);
	n_buttons = lua_tonumber(ctx, -1);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "title");
	lua_gettable(ctx, -2);
	title = _n_strdup(lua_tostring(ctx, -1), NULL);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "genre");
	lua_gettable(ctx, -2);
	genre = _n_strdup(lua_tostring(ctx, -1), NULL);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "subgenre");
	lua_gettable(ctx, -2);
	subgenre = _n_strdup(lua_tostring(ctx, -1), NULL);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "target");
	lua_gettable(ctx, -2);
	target = _n_strdup(lua_tostring(ctx, -1), NULL);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "system");
	lua_gettable(ctx, -2);
	system = _n_strdup(lua_tostring(ctx, -1), NULL);
	lua_pop(ctx, 1);

	lua_pushstring(ctx, "manufacturer");
	lua_gettable(ctx, -2);
	manufacturer = _n_strdup(lua_tostring(ctx, -1), NULL);
	lua_pop(ctx, 1);

	arcan_dbh_res dbr = arcan_db_games(dbhandle, year, input, n_players,n_buttons,
		title, genre, subgenre, target, system, manufacturer, offset, limit);
	free(genre);
	free(subgenre);
	free(title);
	free(target);
	free(system);
	free(manufacturer);

	if (dbr.kind == 1 && dbr.count > 0) {
		arcan_db_game** curr = dbr.data.gamearr;
		int count = 1;

		rv = 1;
	/* table of tables .. wtb ruby yield */
		lua_newtable(ctx);
		int rtop = lua_gettop(ctx);

		while (*curr) {
			lua_pushnumber(ctx, count++);
			lua_newtable(ctx);
			pushgame(ctx, *curr);
			lua_rawset(ctx, rtop);
			curr++;
		}
	}
	else {
		arcan_lua_wraperr(ctx,0,"arcan_lua_filtergames(), requires argument table");
	}

	arcan_db_free_res(dbhandle, dbr);
	return rv;
}

int arcan_luac_warning(lua_State* ctx)
{
	char* msg = (char*) luaL_checkstring(ctx, 1);

	if (strlen(msg) > 0)
		arcan_warning("(%s) %s\n", arcan_themename, msg);

	return 0;
}

int arcan_lua_shutdown(lua_State *ctx)
{
	arcan_event ev = {.category = EVENT_SYSTEM, .kind = EVENT_SYSTEM_EXIT};
	arcan_event_enqueue(arcan_event_defaultctx(), &ev);

	const char* str = luaL_optstring(ctx, 1, "");
	if (strlen(str) > 0)
		arcan_warning("%s\n", str);

	return 0;
}

int arcan_lua_switchtheme(lua_State *ctx)
{
	arcan_event ev = {.category = EVENT_SYSTEM, .kind = EVENT_SYSTEM_SWITCHTHEME};
	const char* newtheme = luaL_optstring(ctx, 1, arcan_themename);

	snprintf(ev.data.system.data.message, sizeof(ev.data.system.data.message) 
		/ sizeof(ev.data.system.data.message[0]), "%s", newtheme);
	arcan_event_enqueue(arcan_event_defaultctx(), &ev);

	return 0;
}

int arcan_lua_getgame(lua_State* ctx)
{
	int rv = 0;

	if (lua_type(ctx, 1) == LUA_TSTRING){
		const char* game = luaL_checkstring(ctx, 1);

		arcan_dbh_res dbr = arcan_db_games(dbhandle,
 /* year, input, players, buttons */
			0, 0, 0, 0,
/*title, genre, subgenre, target, system, manufacturer */
			game, NULL, NULL, NULL, NULL, NULL, 
			0, 0); /* offset, limit */

		if (dbr.kind == 1 && dbr.count > 0 && 
			dbr.data.gamearr &&(*dbr.data.gamearr)){
			arcan_db_game** curr = dbr.data.gamearr;
/* table of tables .. missing ruby style yield just about now.. */ 
			lua_newtable(ctx);
			int rtop = lua_gettop(ctx);
			int count = 1;

			while (*curr) {
				lua_pushnumber(ctx, count++);
				lua_newtable(ctx);
				pushgame(ctx, *curr);
				lua_rawset(ctx, rtop);
				curr++;
			}

			rv = 1;
			arcan_db_free_res(dbhandle, dbr);
		}
	}
	else {
		arcan_dbh_res dbr = arcan_db_gamebyid(dbhandle, luaL_checkint(ctx, 1));
		if (dbr.kind == 1 && dbr.count > 0 && 
			dbr.data.gamearr &&(*dbr.data.gamearr)){
			lua_newtable(ctx);
			pushgame(ctx, dbr.data.gamearr[0]);
			rv = 1;
			arcan_db_free_res(dbhandle, dbr);
		}
	}

	return rv;
}

void arcan_lua_panic(lua_State* ctx)
{
	lua_ctx_store.debug = 2;

	if (!lua_ctx_store.cb_source_kind == CB_SOURCE_NONE){
		char vidbuf[64] = {0};
		snprintf(vidbuf, 63, "script error in callback for VID (%lld)", 
			lua_ctx_store.lua_vidbase + lua_ctx_store.cb_source_tag);
		arcan_lua_wraperr(ctx, -1, vidbuf);
	} else
		arcan_lua_wraperr(ctx, -1, "(panic)");

	arcan_fatal("LUA VM is in an unrecoverable panic state.\n");
}

void arcan_lua_wraperr(lua_State* ctx, int errc, const char* src)
{
	if (errc == 0)
		return;

	const char* mesg = luaL_optstring(ctx, 1, "unknown");
	if (lua_ctx_store.debug){
		arcan_warning("Warning: arcan_lua_wraperr((), %s, from %s\n", mesg, src);

		if (lua_ctx_store.debug >= 1)
			dump_call_trace(ctx);

		if (lua_ctx_store.debug > 0)
			dump_stack(ctx);

		if (lua_ctx_store.debug > 0){
			time_t logtime = time(NULL);
			struct tm* ltime = localtime(&logtime);
			
			if (ltime) {
				const char* fns = "/logs/crash_mmdd_hhmmss.lua";
				char fname[ strlen(arcan_resourcepath) + strlen(fns) + 1 ]; 
				sprintf(fname, "%s%s", arcan_resourcepath, fns);
				strftime( fname + strlen(arcan_resourcepath) + 
					strlen("/logs/crash_"), 9, "%m%d_%H%M%S", ltime);

				FILE* tmpout = fopen(fname, "w+");
				if (tmpout){
					arcan_lua_statesnap(tmpout);
					fclose(tmpout);
			}
		}
	}

		if (!(lua_ctx_store.debug > 2))
			arcan_fatal("Fatal: arcan_lua_wraperr()\n");

	}
	else{
		while ( arcan_video_popcontext() < CONTEXT_STACK_LIMIT -1);
		arcan_fatal("Fatal: arcan_lua_wraperr(), %s, from %s\n", mesg, src);
	}
}

struct globs{
	lua_State* ctx;
	int top;
	int index;
};

static void globcb(char* arg, void* tag)
{
	struct globs* bptr = (struct globs*) tag;
	lua_pushnumber(bptr->ctx, bptr->index++);
	lua_pushstring(bptr->ctx, arg);
	lua_rawset(bptr->ctx, bptr->top);
}

int arcan_lua_globresource(lua_State* ctx)
{
	struct globs bptr = {
		.ctx = ctx,
		.index = 1
	};

	char* label = (char*) luaL_checkstring(ctx, 1);
	int mask = luaL_optinteger(ctx, 2, 
		ARCAN_RESOURCE_THEME | ARCAN_RESOURCE_SHARED);

	if (mask != ARCAN_RESOURCE_THEME && mask != ARCAN_RESOURCE_SHARED && 
		mask != (ARCAN_RESOURCE_THEME|ARCAN_RESOURCE_SHARED))
		arcan_fatal("arcan_lua_globresource(%s), invalid mask (%d) specified. "
			"Expected: RESOURCE_SHARED or RESOURCE_THEME or nil\n");

	lua_newtable(ctx);
	bptr.top = lua_gettop(ctx);

	arcan_glob(label, mask, globcb, &bptr);

	return 1;
}

int arcan_lua_resource(lua_State* ctx)
{
	const char* label = luaL_checkstring(ctx, 1);
	int mask = luaL_optinteger(ctx, 2, 
		ARCAN_RESOURCE_THEME | ARCAN_RESOURCE_SHARED);
	char* res = findresource(label, mask);
	lua_pushstring(ctx, res);
	free(res);
	return 1;
}

int arcan_lua_screencoord(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	vector cv[4];

	if (ARCAN_OK == arcan_video_screencoords(id, cv)){
		lua_pushnumber(ctx, cv[0].x);
		lua_pushnumber(ctx, cv[0].y);
		lua_pushnumber(ctx, cv[1].x);
		lua_pushnumber(ctx, cv[1].y);
		lua_pushnumber(ctx, cv[2].x);
		lua_pushnumber(ctx, cv[2].y);
		lua_pushnumber(ctx, cv[3].x);
		lua_pushnumber(ctx, cv[3].y);
		return 8;
	}

	return 0;
}

bool arcan_lua_callvoidfun(lua_State* ctx, const char* fun, bool warn)
{
	if ( arcan_lua_grabthemefunction(ctx, fun) ){
		arcan_lua_wraperr(ctx,
	                  lua_pcall(ctx, 0, 0, 0),
	                  fun);
		return true;
	}
	else if (warn)
		arcan_warning("missing expected symbol ( %s )\n", fun);

	return false;
}

int arcan_lua_getqueueopts(lua_State* ctx)
{
	unsigned short rv[4];
	arcan_frameserver_queueopts( &rv[0], &rv[1], &rv[2], &rv[3] );
	lua_pushnumber(ctx, rv[0]);
	lua_pushnumber(ctx, rv[1]);
	lua_pushnumber(ctx, rv[2]);
	lua_pushnumber(ctx, rv[3]);

	return 3;
}

int arcan_lua_setqueueopts(lua_State* ctx)
{
	unsigned char vcellc = luaL_checknumber(ctx, 1);
	unsigned char acellc = luaL_checknumber(ctx, 2);
	unsigned short abufs = luaL_checknumber(ctx, 3);
	unsigned short presilence = luaL_optnumber(ctx, 4, 
		ARCAN_FRAMESERVER_PRESILENCE);

	arcan_frameserver_queueopts_override( vcellc, acellc, abufs, presilence );
	return 0;
}

static bool use_loader(char* fname)
{
	char* ext = strrchr( fname, '.' );
	if (!ext) return false;

/* there are prettier ways to do this . . . */
	return (strcasecmp(ext, ".so") == 0) || 
		(strcasecmp(ext, ".dll") == 0) ? true : false;
}

int arcan_lua_targetlaunch_capabilities(lua_State* ctx)
{
	char* targetname = strdup( luaL_checkstring(ctx, 1) );
	char* targetexec = arcan_db_targetexec(dbhandle, targetname);
	char* resourcestr = targetexec ? arcan_find_resource_path(
		targetexec, "targets", ARCAN_RESOURCE_SHARED) : NULL;

	unsigned rv = 0;

	if (resourcestr){
		lua_newtable(ctx);
		int top = lua_gettop(ctx);

/* currently, this means frameserver / libretro interface
 * so these capabilities are hard-coded rather than queried */
		if (use_loader(resourcestr)){
			tblbool(ctx, "external_launch", false, top);
			tblbool(ctx, "internal_launch", true, top);
			tblbool(ctx, "snapshot", true, top);
			tblbool(ctx, "rewind", false, top);
			tblbool(ctx, "suspend", false, top);
			tblbool(ctx, "reset", true, top);
			tblbool(ctx, "dynamic_input", true, top);
			tblnum(ctx, "ports", 4, top);
		}
	 	else {
/* the plan is to extend the internal launch support with a probe
 * (sortof prepared for in the build-system) to check how the target is linked,
 *  which dependencies it has etc. */
			tblbool(ctx, "external_launch", true, top);
			if (strcmp(internal_launch_support(), "NO SUPPORT") == 0 ||
				(strcmp(internal_launch_support(), "PARTIAL SUPPORT") == 0 && 
				 arcan_libpath == NULL))
				tblbool(ctx, "internal_launch", false, top);
			else
				tblbool(ctx, "internal_launch", true, top);

			tblbool(ctx, "dynamic input", false, top);
			tblbool(ctx, "reset", false, top);
			tblbool(ctx, "snapshot", false, top);
			tblbool(ctx, "rewind", false, top);
			tblbool(ctx, "suspend", false, top);
			tblnum(ctx, "ports", 0, top);
		}

		rv = 1;
	}

	free(targetname);
	free(targetexec);
	free(resourcestr);

	return rv;
}

static inline bool tgtevent(arcan_vobj_id dst, arcan_event ev)
{
	vfunc_state* state = arcan_video_feedstate(dst);

	if (state && state->tag == ARCAN_TAG_FRAMESERV && state->ptr){
		arcan_frameserver* fsrv = (arcan_frameserver*) state->ptr;
		arcan_frameserver_pushevent( fsrv, &ev );
		return true;
	}

	return false;
}

int arcan_lua_targetportcfg(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);
	unsigned tgtport  = luaL_checkinteger(ctx, 2);
	unsigned tgtkind  = luaL_checkinteger(ctx, 3);

	arcan_event ev = {
		.category = EVENT_TARGET,
		.kind = TARGET_COMMAND_SETIODEV};

	ev.data.target.ioevs[0].iv = tgtport;
	ev.data.target.ioevs[1].iv = tgtkind;

	tgtevent(tgt, ev);

	return 0;
}

int arcan_lua_targetgraph(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);
	unsigned ioval = luaL_checkinteger(ctx, 2);

	arcan_event ev = {
		.category = EVENT_TARGET,
		.kind     = TARGET_COMMAND_GRAPHMODE,
		.data.target.ioevs[0].iv = ioval
	};

	tgtevent(tgt, ev);

	return 0;
}

int arcan_lua_targetseek(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);
	float val         = luaL_checknumber(ctx, 2);
	bool relative     = luaL_optnumber(ctx, 3, 1) == 1;

	vfunc_state* state = arcan_video_feedstate(tgt);

	if (!(state && state->tag == ARCAN_TAG_FRAMESERV && state->ptr))
		arcan_fatal("arcan_lua_targetseek() vid(%"PRIxVOBJ") is not "
			"connected to a frameserver\n", tgt);

	arcan_event ev = {
		.category = EVENT_TARGET,
		.kind = TARGET_COMMAND_SEEKTIME
	};

	if (relative)
		ev.data.target.ioevs[1].iv = (int32_t) val;
	else
		ev.data.target.ioevs[0].fv = val;

	tgtevent(tgt, ev);
	
	arcan_frameserver* fsrv = (arcan_frameserver*) state->ptr;
	arcan_frameserver_flush(fsrv);

	return 0;
}

int arcan_lua_targetskipmodecfg(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);
	int skipval       = luaL_checkinteger(ctx, 2);
	int skiparg       = luaL_optinteger(ctx, 3, 0);
	int preaud        = luaL_optinteger(ctx, 4, 0);
	int skipdbg1      = luaL_optinteger(ctx, 5, 0);
	int skipdbg2      = luaL_optinteger(ctx, 6, 0);

	if (skipval < -1) return 0;

	arcan_event ev = {
		.category = EVENT_TARGET,
		.kind = TARGET_COMMAND_FRAMESKIP
	};

	ev.data.target.ioevs[0].iv = skipval;
	ev.data.target.ioevs[1].iv = skiparg;
	ev.data.target.ioevs[2].iv = preaud;
	ev.data.target.ioevs[3].iv = skipdbg1;
	ev.data.target.ioevs[4].iv = skipdbg2;

	tgtevent(tgt, ev);

	return 0;
}

int arcan_lua_targetrestore(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);
	const char* snapkey = luaL_checkstring(ctx, 2);

	vfunc_state* state = arcan_video_feedstate(tgt);
	if (state && state->tag == ARCAN_TAG_FRAMESERV && state->ptr){
		int fd = fmt_open(O_RDONLY, S_IRWXU, "%s/savestates/%s", 
			arcan_resourcepath, snapkey);
		if (-1 != fd){
			arcan_frameserver* fsrv = (arcan_frameserver*) state->ptr;

			if ( ARCAN_OK == arcan_frameserver_pushfd( fsrv, fd ) ){
				arcan_event ev = {
					.category = EVENT_TARGET,
					.kind = TARGET_COMMAND_RESTORE };
				arcan_frameserver_pushevent( fsrv, &ev );

				lua_pushboolean(ctx, true);
				return 1;
			}
			else; /* note that this will leave an empty statefile in the filesystem */
		}
	}
	lua_pushboolean(ctx, false);

	return 1;
}

int arcan_lua_targetlinewidth(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);

	float lsz = luaL_checknumber(ctx, 2);

	arcan_event ev = {
			.category = EVENT_TARGET,
			.kind = TARGET_COMMAND_VECTOR_LINEWIDTH,
			.data.target.ioevs[0].fv = lsz
	};

	tgtevent(tgt, ev);

	return 0;
}

int arcan_lua_targetpointsize(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);

	float psz = luaL_checknumber(ctx, 2);

	arcan_event ev = {
			.category = EVENT_TARGET,
			.kind = TARGET_COMMAND_VECTOR_POINTSIZE,
			.data.target.ioevs[0].fv = psz
	};

	tgtevent(tgt, ev);

	return 0;
}

int arcan_lua_targetpostfilter(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);
	int filtertype = luaL_checknumber(ctx, 2);

	if (filtertype != POSTFILTER_NTSC && filtertype != POSTFILTER_OFF)
		arcan_warning("arcan_lua_targetpostfilter() -- "
			"unknown filter (%d) specified.\n", filtertype);
	else {
		arcan_event ev = {
			.category    = EVENT_TARGET,
			.kind        = TARGET_COMMAND_NTSCFILTER
		};

		ev.data.target.ioevs[0].iv = filtertype == POSTFILTER_NTSC;
		tgtevent(tgt, ev);
	}

	return 0;
}

int arcan_lua_targetpostfilterargs(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);
	int group = luaL_checknumber(ctx, 2);
	float v1  = luaL_optnumber(ctx, 3, 0.0);
	float v2  = luaL_optnumber(ctx, 4, 0.0);
	float v3  = luaL_optnumber(ctx, 5, 0.0);

	arcan_event ev = {
		.category    = EVENT_TARGET,
		.kind        = TARGET_COMMAND_NTSCFILTER_ARGS
	};

	ev.data.target.ioevs[0].iv = group;
	ev.data.target.ioevs[1].fv = v1;
	ev.data.target.ioevs[2].fv = v2;
	ev.data.target.ioevs[3].fv = v3;

	tgtevent(tgt, ev);

	return 0;
}

int arcan_lua_targetstepframe(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);
	int nframes = luaL_checknumber(ctx, 2);
	if (nframes == 0) return 0;

	arcan_event ev = {
			.category = EVENT_TARGET,
			.kind = TARGET_COMMAND_STEPFRAME
	};
	ev.data.target.ioevs[0].iv = nframes;

	tgtevent(tgt, ev);

	return 0;
}

int arcan_lua_targetsnapshot(lua_State* ctx)
{
	arcan_vobj_id tgt = luaL_checkvid(ctx, 1);
	const char* snapkey = luaL_checkstring(ctx, 2);
	bool gotval = false;

	vfunc_state* state = arcan_video_feedstate(tgt);
	if (state && state->tag == ARCAN_TAG_FRAMESERV && state->ptr){

		int fd = fmt_open(O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU, 
			"%s/savestates/%s", arcan_resourcepath, snapkey);
		if (-1 != fd){
			if ( ARCAN_OK == arcan_frameserver_pushfd( 
				(arcan_frameserver*) state->ptr, fd ) ){
				arcan_event ev = {
					.category = EVENT_TARGET,
					.kind = TARGET_COMMAND_STORE };
				arcan_frameserver_pushevent( (arcan_frameserver*) state->ptr, &ev );

				lua_pushboolean(ctx, true);
				gotval = true;
			}
		}
	}

	if (!gotval)
		lua_pushboolean(ctx, false);

	return 1;
}

int arcan_lua_targetreset(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	arcan_event ev = {
		.kind = TARGET_COMMAND_RESET,
		.category = EVENT_TARGET
	};

	tgtevent(vid, ev);

	return 0;
}

static char* lookup_hijack(int gameid)
{
	if (!arcan_libpath)
		return NULL;

	char* res = arcan_db_gametgthijack(dbhandle, gameid);

/* revert to default if the database doesn't tell us anything */
	if (!res)
		res = strdup(LIBNAME);

	char* newstr = malloc(strlen(res) + strlen(arcan_libpath) + 2);
	sprintf(newstr, "%s/%s", arcan_libpath, res);
	free(res);

	return newstr;
}

int arcan_lua_targetlaunch(lua_State* ctx)
{
	int gameid = luaL_checknumber(ctx, 1);
	arcan_errc status = ARCAN_OK;

	if (status != ARCAN_OK)
		return 0;

	int internal = luaL_checkint(ctx, 2) == 1;
	intptr_t ref = (intptr_t) 0;
	int rv = 0;

	if (lua_isfunction(ctx, 3) && !lua_iscfunction(ctx, 3)){
		lua_pushvalue(ctx, 3);
		ref = luaL_ref(ctx, LUA_REGISTRYINDEX);
	}

	/* see if we know what the game is */
	arcan_dbh_res cmdline = arcan_db_launch_options(dbhandle, gameid, internal);
	if (cmdline.kind == 0){
		char* resourcestr = arcan_find_resource_path(cmdline.data.strarr[0],
			"targets", ARCAN_RESOURCE_SHARED);
		if (!resourcestr)
			goto cleanup;

		if (lua_ctx_store.debug > 0){
			char** argbase = cmdline.data.strarr;
				while(*argbase)
					arcan_warning("\t%s\n", *argbase++);
		}

		if (internal && resourcestr)
 /* for lib / frameserver targets, we assume that
	* the argumentlist is just [romsetfull] */
			if (use_loader(resourcestr)){
				char* metastr = resourcestr;
				if ( cmdline.data.strarr[0] && cmdline.data.strarr[1] ){ 
/* launch_options adds exec path first, we already know that one */
					size_t arglen = strlen(resourcestr) + 1 + 
						strlen(cmdline.data.strarr[1]) + 1;

					metastr = (char*) malloc( arglen );
					snprintf(metastr, arglen, "%s*%s", resourcestr, 
						cmdline.data.strarr[1]);
				}

				arcan_frameserver* intarget = arcan_frameserver_alloc();
				intarget->tag = ref;

				struct frameserver_envp args = {
					.use_builtin = true,
					.args.builtin.resource = metastr,
					.args.builtin.mode = "libretro"
				};

				if (arcan_frameserver_spawn_server(intarget, args) == ARCAN_OK){
					lua_pushvid(ctx, intarget->vid);
					lua_pushaid(ctx, intarget->aid);
					arcan_db_launch_counter_increment(dbhandle, gameid);
				}
				else {
					lua_pushvid(ctx, ARCAN_EID);
					lua_pushaid(ctx, ARCAN_EID);
					free(intarget);
				}

				free(metastr);

				rv = 2;
			}
			else {
				char* hijacktgt = lookup_hijack( gameid );

				arcan_frameserver* intarget = arcan_target_launch_internal( resourcestr,
					lookup_hijack( gameid ), cmdline.data.strarr );

				free(hijacktgt);
				if (intarget) {
					intarget->tag = ref;
					lua_pushvid(ctx, intarget->vid);
					lua_pushaid(ctx, intarget->aid);
					arcan_db_launch_counter_increment(dbhandle, gameid);
					rv = 2;
				} else {
					arcan_db_failed_launch(dbhandle, gameid);
				}
			}
		else {
			unsigned long elapsed = arcan_target_launch_external(
				resourcestr, cmdline.data.strarr);

			if (elapsed / 1000 < 3){
				char** argvp = cmdline.data.strarr;
				arcan_db_failed_launch(dbhandle, gameid);
				arcan_warning("Script Warning: launch_external(), "
					"possibly broken target/game combination. %s\n\tArguments:",*argvp++);
				while(*argvp){
					arcan_warning("%s \n", *argvp++);
				}
			} else
				arcan_db_launch_counter_increment(dbhandle, gameid);
		}

		free(resourcestr);
	}
	else
		arcan_warning("arcan_lua_targetlaunch(%i, %i) failed, "
			"no match in database.\n", gameid, internal);

cleanup:
	arcan_db_free_res(dbhandle, cmdline);
	return rv;
}

int arcan_lua_renderattach(lua_State* ctx)
{
	arcan_vobj_id did = luaL_checkvid(ctx, 1);
	arcan_vobj_id sid = luaL_checkvid(ctx, 2);
	int detach        = luaL_checkint(ctx, 3);

	if (detach != RENDERTARGET_DETACH && detach != RENDERTARGET_NODETACH){
		arcan_warning("arcan_lua_renderattach(%d) invalid arg 3, expected "
			"RENDERTARGET_DETACH or RENDERTARGET_NODETACH\n", detach);
		return 0;
	}

/* arcan_video_attachtorendertarget already has pretty aggressive checks */
	arcan_video_attachtorendertarget(did, sid, detach == RENDERTARGET_DETACH);
	return 0;
}

int arcan_lua_renderset(lua_State* ctx)
{
	arcan_vobj_id did = luaL_checkvid(ctx, 1);
	int nvids         = lua_rawlen(ctx, 2);
	int detach        = luaL_checkint(ctx, 3);
	int scale         = luaL_checkint(ctx, 4);
	int format        = luaL_optint(ctx, 5, RENDERFMT_COLOR);

	if (!arcan_video_display.fbo_support){
		arcan_warning("arcan_lua_renderset(%d) FBO support is disabled,"
			"	cannot setup rendertarget.\n");
		return 0;
	}

	if (detach != RENDERTARGET_DETACH && detach != RENDERTARGET_NODETACH){
		arcan_warning("arcan_lua_renderset(%d) invalid arg 3, expected "
			"RENDERTARGET_DETACH or RENDERTARGET_NODETACH\n", detach);
		return 0;
	}

	if (scale != RENDERTARGET_SCALE && scale != RENDERTARGET_NOSCALE){
		arcan_warning("arcan_lua_renderset(%d) invalid arg 4, expected " 
			"RENDERTARGET_SCALE or RENDERTARGET_NOSCALE\n", scale);
		return 0;
	}

	if (nvids > 0){
		arcan_video_setuprendertarget(did, 0, 
			scale == RENDERTARGET_SCALE, format); 

		for (int i = 0; i < nvids; i++){
			lua_rawgeti(ctx, 2, i+1);
			arcan_vobj_id setvid = luavid_tovid( lua_tonumber(ctx, -1) );
			lua_pop(ctx, 1);
			arcan_video_attachtorendertarget(
				did, setvid, detach == RENDERTARGET_DETACH);
		}

	}
	else
		arcan_warning("arcan_lua_renderset(%d, %d) - "
		"	refusing to define empty renderset.\n");

	return 0;
}

int arcan_lua_recordset(lua_State* ctx)
{
	arcan_vobj_id did = luaL_checkvid(ctx, 1);
	const char* resf  = luaL_checkstring(ctx, 2);
	char* argl        = strdup( luaL_checkstring(ctx, 3) );

	luaL_checktype(ctx, 4, LUA_TTABLE);
	int nvids         = lua_rawlen(ctx, 4);

	int detach        = luaL_checkint(ctx, 6);
	int scale         = luaL_checkint(ctx, 7);
	int pollrate      = luaL_checkint(ctx, 8);

	int naids;
	bool global_monitor = false;

	if (lua_type(ctx, 5) == LUA_TTABLE)
		naids         = lua_rawlen(ctx, 5);
	else if (lua_type(ctx, 5) == LUA_TNUMBER){
		naids         = 1;
		arcan_vobj_id did = luaL_checkvid(ctx, 5);
		if (did != ARCAN_VIDEO_WORLDID){
			arcan_warning("arcan_lua_recordset(%d) Unexpected value for audio, "
				"only a table of selected AID streams or single WORLDID "
				"(global monitor) allowed.\n");
			goto cleanup;
		}

		global_monitor = true;
	}

	intptr_t ref = (intptr_t) 0;

	if (!arcan_video_display.fbo_support){
		arcan_warning("arcan_lua_recordset(%d) FBO support is disabled, "
			"cannot setup recordtarget.\n");
		goto cleanup;
	}

	if (detach != RENDERTARGET_DETACH && detach != RENDERTARGET_NODETACH){
		arcan_warning("arcan_lua_recordset(%d) invalid arg 6, expected"
			"	RENDERTARGET_DETACH or RENDERTARGET_NODETACH\n", detach);
		goto cleanup;
	}

	if (scale != RENDERTARGET_SCALE && scale != RENDERTARGET_NOSCALE){
		arcan_warning("arcan_lua_recordset(%d) invalid arg 7, "
			"expected RENDERTARGET_SCALE or RENDERTARGET_NOSCALE\n", scale);
		goto cleanup;
	}

	if (pollrate == 0){
		arcan_warning("arcan_lua_recordset(%d) invalid arg 8, expected "
			"n < 0 (every n frame) or n > 0 (every n tick)\n", pollrate);
		goto cleanup;
	}

	if (nvids > 0){
		bool rtsetup = false;

		for (int i = 0; i < nvids; i++){
			lua_rawgeti(ctx, 4, i+1);
			arcan_vobj_id setvid = luavid_tovid( lua_tointeger(ctx, -1) );
			lua_pop(ctx, 1);

			if (setvid == ARCAN_VIDEO_WORLDID){
				if (nvids != 1)
					arcan_fatal("arcan_lua_recordset(), with WORLDID in recordset, "
						"no other entries are allowed.\n");

				if (arcan_video_attachtorendertarget(did, setvid, false) != ARCAN_OK){
					arcan_warning("arcan_lua_recordset() -- global capture failed, "
						"setvid dimensions must match VRESW, VRESH\n");
					return 0;
				}

/* since the worldid attach is a special case, 
 * some rendertarget bound options need to be set manually */
				arcan_video_alterreadback(ARCAN_VIDEO_WORLDID, pollrate);
				break;
			}
			else {
				if (!rtsetup)
					rtsetup = (arcan_video_setuprendertarget(did, pollrate, 
						scale == RENDERTARGET_SCALE, RENDERTARGET_COLOR), true);

				arcan_video_attachtorendertarget(did, setvid, 
					detach == RENDERTARGET_DETACH);
			}

		}
	}
	else{
		arcan_warning("arcan_lua_recordset(%d), empty source vid set -- "
			"global capture unimplemented.\n");
		goto cleanup;
	}

	arcan_aobj_id* aidlocks = NULL;
	unsigned n_aidlocks = abs(naids);

	if (naids > 0 && global_monitor == false){
		aidlocks = malloc(sizeof(arcan_aobj_id) * naids + 1);
		aidlocks[naids] = 0; /* terminate */

/* can't hook the monitors until we have the frameserver in place */
		for (int i = 0; i < naids; i++){
			lua_rawgeti(ctx, 5, i+1);
			arcan_aobj_id setaid = luaaid_toaid( lua_tonumber(ctx, -1) );
			lua_pop(ctx, 1);

			if (arcan_audio_kind(setaid) != AOBJ_STREAM && arcan_audio_kind(setaid)
				!= AOBJ_CAPTUREFEED){
				arcan_warning("arcan_lua_recordset(%d), unsupported AID source type,"
					" only STREAMs currently supported. Audio recording disabled.\n");
				free(aidlocks);
				aidlocks = NULL;
				naids = 0;
				char* ol = malloc(strlen(argl) + strlen(":noaudio=true") + 1);
				sprintf(ol, "%s%s", argl, ":noaudio=true");
				free(argl);
				argl = ol;
				break;
			}

			aidlocks[i] = setaid;
		}
	}

/* in order to stay backward compatible API wise, 
 * the load_movie with function callback will always need to specify 
 * loop condition. (or we can switch to heuristic stack management) */
	if (lua_isfunction(ctx, 9) && !lua_iscfunction(ctx, 9)){
		lua_pushvalue(ctx, 9);
		ref = luaL_ref(ctx, LUA_REGISTRYINDEX);
	}

	arcan_frameserver* mvctx = arcan_frameserver_alloc();
	mvctx->loop = FRAMESERVER_NOLOOP;
	mvctx->vid  = did;

	struct frameserver_envp args = {
		.use_builtin = true,
		.custom_feed = true,
		.args.builtin.mode = "record",
		.args.builtin.resource = argl
	};

/* we use a special feed function meant to flush audiobuffer + 
 * a single video frame for encoding */
	vfunc_state fftag = {
		.tag = ARCAN_TAG_FRAMESERV,
		.ptr = mvctx};
	arcan_video_alterfeed(did, arcan_frameserver_avfeedframe, fftag);

	if ( arcan_frameserver_spawn_server(mvctx, args) == ARCAN_OK ){
		arcan_vobject* dobj = arcan_video_getobject(did);

/* we define the size of the recording to be that of the storage
 * of the rendertarget vid, this should be allocated through fill_surface */
		struct frameserver_shmpage* shmpage = mvctx->shm.ptr;
		shmpage->storage.w = dobj->vstore->w;
		shmpage->storage.h = dobj->vstore->h;

/* separate storage and display dimensions to allow 
 * for scaling hints, or cropping */
		shmpage->display.w   = dobj->vstore->w;
		shmpage->display.h   = dobj->vstore->h;

		frameserver_shmpage_calcofs(shmpage, &(mvctx->vidp), &(mvctx->audp));

/* pushing the file descriptor signals the frameserver to start receiving 
 * (and use the proper dimensions), it is permitted to close and push another
 * one to the same session, with special treatment for "dumb" resource names
 * or streaming sessions */
		int fd;
		if (strstr(args.args.builtin.resource, "container=stream") != NULL ||
			strlen(args.args.builtin.resource) == 0)
			fd = open(NULFILE, O_WRONLY);
		else
			fd = fmt_open(O_CREAT | O_RDWR, S_IRWXU, "%s/%s/%s", arcan_themepath,
				arcan_themename, resf);

		if (fd){
			arcan_frameserver_pushfd( mvctx, fd );
			mvctx->alocks = aidlocks;

/* 
 * lastly, lock each audio object and forcibly attach the frameserver as
 * a monitor. NOTE that this currently doesn't handle the case where we we
 * set up multiple recording sessions sharing audio objects. 
 */
			arcan_aobj_id* base = mvctx->alocks;
			while(base && *base){
				void* hookfun;
				arcan_audio_hookfeed(*base++, mvctx, 
					arcan_frameserver_avfeedmon, &hookfun);
			}

/*
 * if we have several input audio sources, we need to set up an intermediate 
 * mixing system, that accumulates samples from each audio source monitor,
 * and emitts a mixed buffer. This requires that the audio sources operate at
 * the same rate and buffering will converge on the biggest- buffer audio source
 */
			if (naids > 1)
				arcan_frameserver_avfeed_mixer(mvctx, naids, aidlocks, NULL);
		}
		else
			arcan_warning("arcan_lua_recordset(%s/%s/%s)--couldn't create output.\n",
				arcan_themepath, arcan_themename, resf);
	}
	else
		free(mvctx);

cleanup:
	free(argl);
	return 0;
}

int arcan_lua_borderscan(lua_State* ctx)
{
	int x1, y1, x2, y2;
	x1 = y1 = 0;
	x2 = arcan_video_screenw();
	y2 = arcan_video_screenh();

	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	arcan_vobject* vobj = arcan_video_getobject(vid);

/* since GLES doesn't support texture readback, the option would be to render
 * then readback pixels as there is limited use for this feature, we'll stick
 * to the cheap route, i.e. assume we don't use memory- conservative mode and
 * just grab the buffer from the cached storage. */
	if (vobj && vobj->vstore->txmapped && 
		vobj->vstore->vinf.text.raw && vobj->vstore->vinf.text.s_raw > 0
		&& vobj->origw > 0 && vobj->origh > 0){

#define sample(x,y) \
	( vobj->vstore->vinf.text.raw[ ((y) * vobj->origw + (x)) * 4 + 3 ] )

		for (x1 = vobj->origw >> 1; 
			x1 >= 0 && sample(x1, vobj->origh >> 1) < 128; x1--);
		for (x2 = vobj->origw >> 1; 
			x2 < vobj->origw && sample(x2, vobj->origh >> 1) < 128; x2++);
		for (y1 = vobj->origh >> 1; 
			y1 >= 0 && sample(vobj->origw >> 1, y1) < 128; y1--);
		for (y2 = vobj->origh >> 1;
			y2 < vobj->origh && sample(vobj->origw >> 1, y2) < 128; y2++);
#undef sample
		}

	lua_pushnumber(ctx, x1);
	lua_pushnumber(ctx, y1);
	lua_pushnumber(ctx, x2);
	lua_pushnumber(ctx, y2);
	return 4;
}

extern arcan_benchdata benchdata;
int arcan_lua_togglebench(lua_State* ctx)
{
	int nargs = lua_gettop(ctx);

	if (nargs)
		benchdata.bench_enabled = lua_toboolean(ctx, 1);
	else
		benchdata.bench_enabled = !benchdata.bench_enabled;

/* always reset on data change */
	memset(benchdata.ticktime, '\0', sizeof(benchdata.ticktime));
	memset(benchdata.frametime, '\0', sizeof(benchdata.frametime));
	memset(benchdata.framecost, '\0', sizeof(benchdata.framecost));
	benchdata.tickofs = benchdata.frameofs = benchdata.costofs = 0;
	benchdata.framecount = benchdata.tickcount = benchdata.costcount = 0;
	return 0;
}

static int arcan_lua_getbenchvals(lua_State* ctx)
{
	size_t bench_sz = sizeof(benchdata.ticktime) / sizeof(benchdata.ticktime[0]);
	
	lua_pushnumber(ctx, benchdata.tickcount);
	lua_newtable(ctx);
	int top = lua_gettop(ctx);
	int i = (benchdata.tickofs + 1) % bench_sz;
	int count = 0;

	while (i != benchdata.tickofs){
		lua_pushnumber(ctx, count++);
		lua_pushnumber(ctx, benchdata.ticktime[i]);
		lua_rawset(ctx, top);
		i = (i + 1) % bench_sz;
	}

	bench_sz = sizeof(benchdata.frametime) / sizeof(benchdata.frametime[0]);
	i = (benchdata.frameofs + 1) % bench_sz;
	lua_pushnumber(ctx, benchdata.framecount);
	lua_newtable(ctx);
	top = lua_gettop(ctx);
	count = 0;

 	while (i != benchdata.frameofs){
		lua_pushnumber(ctx, count++);
		lua_pushnumber(ctx, benchdata.frametime[i]);
		lua_rawset(ctx, top);
		i = (i + 1) % bench_sz;
	}	

	bench_sz = sizeof(benchdata.framecost) / sizeof(benchdata.framecost[0]);
	i = (benchdata.costofs + 1) % bench_sz;
	lua_pushnumber(ctx, benchdata.costcount);
	lua_newtable(ctx);
	top = lua_gettop(ctx);
	count = 0;

 	while (i != benchdata.costofs){
		lua_pushnumber(ctx, count++);
		lua_pushnumber(ctx, benchdata.framecost[i]);
		lua_rawset(ctx, top);
		i = (i + 1) % bench_sz;
	}	

	return 6;	
}

static int arcan_lua_timestamp(lua_State* ctx)
{
	lua_pushnumber(ctx, arcan_timemillis());
	return 1;
}

int arcan_lua_decodemod(lua_State* ctx)
{
	int modval = luaL_checkint(ctx, 1);

	lua_newtable(ctx);
	int top = lua_gettop(ctx);

	int count = 1;
	if ((modval & KMOD_LSHIFT) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "lshift");
		lua_rawset(ctx, top);
	}

	if ((modval & KMOD_RSHIFT) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "rshift");
		lua_rawset(ctx, top);
	}

	if ((modval & KMOD_LALT) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "lalt");
		lua_rawset(ctx, top);
	}

	if ((modval & KMOD_RALT) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "ralt");
		lua_rawset(ctx, top);
	}

	if ((modval & KMOD_LCTRL) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "lctrl");
		lua_rawset(ctx, top);
	}

	if ((modval & KMOD_RCTRL) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "rctrl");
		lua_rawset(ctx, top);
	}

	if ((modval & KMOD_LMETA) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "lmeta");
		lua_rawset(ctx, top);
	}

	if ((modval & KMOD_RMETA) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "rmeta");
		lua_rawset(ctx, top);
	}

	if ((modval & KMOD_NUM) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "num");
		lua_rawset(ctx, top);
	}

	if ((modval & KMOD_CAPS) > 0){
		lua_pushnumber(ctx, count++);
		lua_pushstring(ctx, "caps");
		lua_rawset(ctx, top);
	}

	return 1;
}

int arcan_lua_movemodel(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	float x = luaL_checknumber(ctx, 2);
	float y = luaL_checknumber(ctx, 3);
	float z = luaL_checknumber(ctx, 4);
	unsigned int dt = luaL_optint(ctx, 5, 0);

	arcan_video_objectmove(vid, x, y, z, dt);
	return 0;
}

int arcan_lua_forwardmodel(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	float mag = luaL_checknumber(ctx, 2);
	unsigned int dt = luaL_optint(ctx, 3, 0);
	bool axismask_x = luaL_optnumber(ctx, 4, 0) == 1;
	bool axismask_y = luaL_optnumber(ctx, 5, 0) == 1;
	bool axismask_z = luaL_optnumber(ctx, 6, 0) == 1;

	surface_properties prop = arcan_video_current_properties(vid);

	vector view = taitbryan_forwardv(prop.rotation.roll, 
		prop.rotation.pitch, prop.rotation.yaw);
	view = mul_vectorf(view, mag);
	vector newpos = add_vector(prop.position, view);

	arcan_video_objectmove(vid, 
		axismask_x ? prop.position.x : newpos.x, 
		axismask_y ? prop.position.y : newpos.y,
		axismask_z ? prop.position.z : newpos.z, dt);
	return 0;
}

int arcan_lua_strafemodel(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	float mag = luaL_checknumber(ctx, 2);
	unsigned int dt = luaL_optint(ctx, 3, 0);

	surface_properties prop = arcan_video_current_properties(vid);
	vector view = taitbryan_forwardv(prop.rotation.roll, 
		prop.rotation.pitch, prop.rotation.yaw);
	
	vector up = build_vect(0.0, 1.0, 0.0);
	if (prop.rotation.pitch > 180 || prop.rotation.pitch < -180)
		mag *= -1.0f;

	view = norm_vector(crossp_vector(view, up));

	prop.position.x += view.x * mag;
	prop.position.z += view.z * mag;

	arcan_video_objectmove(vid,
		prop.position.x, prop.position.y, prop.position.z, dt);

	return 0;
}

int arcan_lua_scalemodel(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	float sx = luaL_checknumber(ctx, 2);
	float sy = luaL_checknumber(ctx, 3);
	float sz = luaL_checknumber(ctx, 4);
	unsigned int dt = luaL_optint(ctx, 5, 0);

	arcan_video_objectscale(vid, sx, sy, sz, 0);
	return 0;
}

int arcan_lua_orientmodel(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	double roll       = luaL_checknumber(ctx, 2);
	double pitch      = luaL_checknumber(ctx, 3);
	double yaw        = luaL_checknumber(ctx, 4);
	arcan_3d_baseorient(vid, roll, pitch, yaw);
	return 0;
}

/* map a format string to the arcan_shdrmgmt.h different datatypes */
int arcan_lua_shader_uniform(lua_State* ctx)
{
	float fbuf[16];

	unsigned sid = luaL_checknumber(ctx, 1);
	const char* label = luaL_checkstring(ctx, 2);
	const char* fmtstr = luaL_checkstring(ctx, 3);
	bool persist = luaL_checknumber(ctx, 4) != 0;

	if (arcan_shader_activate(sid) != ARCAN_OK){
		arcan_warning("arcan_lua_shader_uniform(), shader (%d) failed"
			"	to activate.\n", sid);
		return 0;
	}

	if (!label)
		label = "unknown";

	if (strcmp(label, "ff") == 0){
		abort();
	}

	if (fmtstr[0] == 'b'){
		int fmt = luaL_checknumber(ctx, 5) != 0;
		arcan_shader_forceunif(label, shdrbool, &fmt, persist);
	} else if (fmtstr[0] == 'i'){
		int fmt = luaL_checknumber(ctx, 5);
		arcan_shader_forceunif(label, shdrint, &fmt, persist);
	} else {
		unsigned i = 0;
		while(fmtstr[i] == 'f') i++;
		if (i)
			switch(i){
				case 1:
					fbuf[0] = luaL_checknumber(ctx, 5);
					arcan_shader_forceunif(label, shdrfloat, fbuf, persist);
				break;

				case 2:
					fbuf[0] = luaL_checknumber(ctx, 5);
					fbuf[1] = luaL_checknumber(ctx, 6);
					arcan_shader_forceunif(label, shdrvec2, fbuf, persist);
				break;

				case 3:
					fbuf[0] = luaL_checknumber(ctx, 5);
					fbuf[1] = luaL_checknumber(ctx, 6);
					fbuf[2] = luaL_checknumber(ctx, 7);
					arcan_shader_forceunif(label, shdrvec3, fbuf, persist);
				break;

				case 4:
					fbuf[0] = luaL_checknumber(ctx, 5);
					fbuf[1] = luaL_checknumber(ctx, 6);
					fbuf[2] = luaL_checknumber(ctx, 7);
					fbuf[3] = luaL_checknumber(ctx, 8);
					arcan_shader_forceunif(label, shdrvec4, fbuf, persist);
				break;

				case 16:
						while(i--)
							fbuf[i] = luaL_checknumber(ctx, 5 + i);

						arcan_shader_forceunif(label, shdrmat4x4, fbuf, persist);

				break;
				default:
					arcan_warning("arcan_lua_shader_uniform(%s), unsupported format "
						"string accepted f counts are 1..4 and 16\n", label);
		}
		else
			arcan_warning("arcan_lua_shader_uniform(%s), unspported format "
				"	string (%s)\n", label, fmtstr);
	}

	/* shdrbool : b
	 *  shdrint : i
	 *shdrfloat : f
	 *shdrvec2  : ff
	 *shdrvec3  : fff
	 *shdrvec4  : ffff
	 *shdrmat4x4: ffff.... */

	/* check for the special ones, b and i */
	/* count number of f:s, map that to the appropriate subtype */

	return 0;
}

int arcan_lua_rotatemodel(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	double roll       = luaL_checknumber(ctx, 2);
	double pitch      = luaL_checknumber(ctx, 3);
	double yaw        = luaL_checknumber(ctx, 4);
	unsigned int dt   = luaL_optnumber(ctx, 5, 0);
	int rotate_rel    = luaL_optnumber(ctx, 6, CONST_ROTATE_ABSOLUTE);
	
	if (rotate_rel != CONST_ROTATE_RELATIVE && rotate_rel !=CONST_ROTATE_ABSOLUTE)
		arcan_fatal("arcan_lua_rotatemodel(%d), invalid rotation base defined, (%d)"
			"	should be ROTATE_ABSOLUTE or ROTATE_RELATIVE\n", rotate_rel);

	surface_properties prop = arcan_video_current_properties(vid);

	if (rotate_rel == CONST_ROTATE_RELATIVE)
		arcan_video_objectrotate(vid, prop.rotation.roll + roll, 
			prop.rotation.pitch + pitch, prop.rotation.yaw + yaw, dt);
	else
		arcan_video_objectrotate(vid, roll, pitch, yaw, dt);

	return 0;
}

int arcan_lua_setimageproc(lua_State* ctx)
{
	int num = luaL_checknumber(ctx, 1);

	if (num == imageproc_normal || num == imageproc_fliph){
		arcan_video_default_imageprocmode(num);
	} else
		arcan_fatal("arcan_lua_setimageproc(%d), invalid image postprocess "
			"specified, expected IMAGEPROC_NORMAL or IMAGEPROC_FLIPH\n", num);

	return 0;
}

int arcan_lua_settexfilter(lua_State* ctx)
{
	enum arcan_vfilter_mode mode = luaL_checknumber(ctx, 1);

	if (mode == ARCAN_VFILTER_TRILINEAR ||
			mode == ARCAN_VFILTER_BILINEAR ||
		  mode == ARCAN_VFILTER_LINEAR ||
		  mode == ARCAN_VFILTER_NONE){
		arcan_video_default_texfilter(mode);
	}

	return 0;
}

int arcan_lua_changetexfilter(lua_State* ctx)
{
	enum arcan_vfilter_mode mode = luaL_checknumber(ctx, 2);
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);

	if (mode == ARCAN_VFILTER_TRILINEAR ||
			mode == ARCAN_VFILTER_BILINEAR ||
		  mode == ARCAN_VFILTER_LINEAR ||
		  mode == ARCAN_VFILTER_NONE){
		arcan_video_objectfilter(vid, mode);
	} 
	else
		arcan_fatal("image_texfilter(vid, s) -- unsupported mode (%d), expected:"
			"	FILTER_LINEAR, FILTER_BILINEAR or FILTER_TRILINEAR\n", mode);

	return 0;
}

int arcan_lua_settexmode(lua_State* ctx)
{
	int numa = luaL_checknumber(ctx, 1);
	int numb = luaL_checknumber(ctx, 2);
	long int tmpn = luaL_optnumber(ctx, 3, ARCAN_EID);

	if ( (numa == ARCAN_VTEX_CLAMP || numa == ARCAN_VTEX_REPEAT) &&
		(numb == ARCAN_VTEX_CLAMP || numb == ARCAN_VTEX_REPEAT) ){
		if (tmpn != ARCAN_EID){
			arcan_vobj_id dvid = luaL_checkvid(ctx, 3);
			arcan_video_objecttexmode(dvid, numa, numb);
		}
		else
			arcan_video_default_texmode(numa, numb);
	}


	return 0;
}

int arcan_lua_tracetag(lua_State* ctx)
{
	arcan_vobj_id id = luaL_checkvid(ctx, 1);
	const char* const msg = luaL_optstring(ctx, 2, NULL);

	if (!msg){
		const char* tag = arcan_video_readtag(id);
		lua_pushstring(ctx, tag ? tag : "(no tag)");
		return 1;
	} 
	else 
		arcan_video_tracetag(id, msg);

	return 0;
}

int arcan_lua_setscalemode(lua_State* ctx)
{
	int num = luaL_checknumber(ctx, 1);

	if (num == ARCAN_VIMAGE_NOPOW2 || num == ARCAN_VIMAGE_SCALEPOW2){
		arcan_video_default_scalemode(num);
	} else {
		arcan_fatal("arcan_lua_setscalemode(%d), invalid scale-mode specified. Expecting:"
		"SCALE_NOPOW2, SCALE_POW2 \n", num);
	}

	return 0;
}

/* 0 => 7 bit char,
 * 1 => start of char,
 * 2 => in the middle of char */
int arcan_lua_utf8kind(lua_State* ctx)
{
	char num = luaL_checkint(ctx, 1);

	if (num & (1 << 7)){
		lua_pushnumber(ctx, num & (1 << 6) ? 1 : 2);
	} else
		lua_pushnumber(ctx, 0);

	return 1;
}

int arcan_lua_inputfilteranalog(lua_State* ctx)
{
	int joyid = luaL_checknumber(ctx, 1);
/* mode, deadzone, lowth, upth, kernel_sz */
	return 0;
}

int arcan_lua_inputanalogzonemap(lua_State* ctx)
{
/* FIXME: 
 * dev, axis, pressv, releasev, buttonind
 */
	return 0;
}

int arcan_lua_inputanalogquery(lua_State* ctx)
{
/* FIXME:
 * sweep all joys (0..n which is fail) and sweep all axis,
 * expose as one big 'ol table 
 */
	return 0;
}

int arcan_lua_inputanalogtoggle(lua_State* ctx)
{
	bool val = lua_tonumber(ctx, 1) != 0;
	bool mouse = luaL_optnumber(ctx, 2, 0) != 0;

	arcan_event_analogall(val, mouse); 
	return 0;
}

int arcan_lua_screenshot(lua_State* ctx)
{
	void* databuf = NULL;
	size_t bufs;

	const char* const resstr = luaL_checkstring(ctx, 1);
	arcan_vobj_id sid = ARCAN_EID;
	
	if (luaL_optnumber(ctx, 2, ARCAN_EID) != ARCAN_EID){
		sid = luaL_checkvid(ctx, 2);
		arcan_video_forceread(sid, &databuf, &bufs);
	} 
	else 
		arcan_video_screenshot(&databuf, &bufs);

	if (databuf){
		char* fname = arcan_find_resource(resstr, ARCAN_RESOURCE_THEME);
		if (!fname){
			fname = arcan_expand_resource(resstr, false);
			int fd = open(fname, O_CREAT | O_RDWR, 0600);
			if (-1 != fd){
				arcan_rgba32_pngfile(fd, databuf, arcan_video_display.width,
					arcan_video_display.height, true);
			}
			else
				arcan_warning("arcan_lua_screenshot() -- couldn't open (%s) "
					"for writing.\n", fname);

		}
		else{
			arcan_warning("arcan_lua_screenshot() -- refusing to overwrite existing "
				"(%s)\n", fname);
		}
	
		free(fname);
		free(databuf);
	}
	else
		arcan_warning("arcan_lua_screenshot() -- request failed, couldn't allocate "
		"memory.\n");

	return 0;
}

void arcan_lua_eachglobal(lua_State* ctx, char* prefix, 
	int (*callback)(const char*, const char*, void*), void* tag)
{
/* FIXME: incomplete (planned for the sandboxing / hardening release).
 * 1. have a toggle saying that this functionality is desired 
 *    (as the overhead is notable),
 * 2. maintain a trie/prefix tree that this functions just maps to
 * 3. populate the tree with a C version of:

	local metatable = {}
		setmetatable(_G,{
    __index    = metatable, -- or a function handling reads
    __newindex = function(t,k,v)
     -- k and v will contain table key and table value respectively.
     rawset(metatable,k, v)
    end})
	}

	this would in essence intercept all global table updates, meaning that we can,
 	at least, use that as a lookup scope for tab completion etc.
*/
}

static int arcan_lua_net_listen(lua_State* ctx)
{
	arcan_frameserver* intarget = arcan_frameserver_alloc();
  int nargs =	lua_gettop(ctx);
	intptr_t ref = 0;

/* slightly more flexible argument management, just find the first callback */
	for (int i = 1; i <= nargs; i++){
		if (lua_isfunction(ctx, i)){
			lua_pushvalue(ctx, i);
			ref = luaL_ref(ctx, LUA_REGISTRYINDEX);
			break;
		}
	}

	intarget->tag = ref;
	struct frameserver_envp args = {
		.use_builtin = true,
		.args.builtin.mode = "net-srv",
		.args.builtin.resource = "mode=server"
	};

	if (arcan_frameserver_spawn_server(intarget, args) == ARCAN_OK)
		lua_pushvid(ctx, intarget->vid);
	else {
		lua_pushvid(ctx, ARCAN_EID);
		free(intarget);
	}

	return 1;
}

static int arcan_lua_net_open(lua_State* ctx)
{
	arcan_frameserver* intarget = arcan_frameserver_alloc();

	const char* host = lua_isstring(ctx, 1) ? luaL_checkstring(ctx, 1) : NULL;
	int nargs = lua_gettop(ctx);
	intptr_t ref = 0;

/* slightly more flexible argument management, just find the first callback */
	for (int i = 1; i <= nargs; i++)
		if (lua_isfunction(ctx, i)){
			lua_pushvalue(ctx, i);
			ref = luaL_ref(ctx, LUA_REGISTRYINDEX);
			break;
		}

/* populate and escape, due to IPv6 addresses etc. actively using :: */
	char* workstr = NULL;
	size_t work_sz = 0;

	if (host){
		workstr = strdup(host);
		char* tmpstr = workstr;
		while (*tmpstr){
			if (*tmpstr == ':') *tmpstr = '\t';
			tmpstr++;
		}

		work_sz = strlen(workstr);
	}

	char* instr = malloc(work_sz + strlen("mode=client:host=") + 1);
	sprintf(instr,"mode=client%s%s", host ? ":host=" : "", workstr ? workstr:"");

	struct frameserver_envp args = {
		.use_builtin = true,
		.args.builtin.mode = "net-cl",
		.args.builtin.resource = instr
	};

	intarget->tag = ref;
	if (arcan_frameserver_spawn_server(intarget, args) == ARCAN_OK){
		lua_pushvid(ctx, intarget->vid);
	}
	else {
		lua_pushvid(ctx, ARCAN_EID);
		free(intarget);
	}

	free(instr);
	free(workstr);

	return 1;
}

static int arcan_lua_net_pushcl(lua_State* ctx)
{
	arcan_vobj_id did   = luaL_checkvid(ctx, 1);
	arcan_vobject* vobj = arcan_video_getobject(did);

/* arg2 can be (string) => NETMSG, (event) => just push */
	arcan_event outev = {.category = EVENT_NET};

	if (vobj->feed.state.tag != ARCAN_TAG_FRAMESERV || !vobj->feed.state.ptr)
		arcan_fatal("arcan_lua_net_pushcl() -- bad arg1, "
			"VID is not a frameserver.\n");

	arcan_frameserver* fsrv = vobj->feed.state.ptr;

	if (!fsrv->kind == ARCAN_FRAMESERVER_NETCL)
		arcan_fatal("arcan_lua_net_pushcl() -- bad arg1, specified frameserver is"
			"	not in client mode (net_open).\n");

	if (lua_isstring(ctx, 2)){
		outev.kind = EVENT_NET_CUSTOMMSG;

		const char* msg = luaL_checkstring(ctx, 2);
		size_t out_sz = sizeof(outev.data.network.message) / 
			sizeof(outev.data.network.message[0]);
		snprintf(outev.data.network.message, out_sz, "%s", msg);
	}
	else if (lua_isnumber(ctx, 2)){
		arcan_vobj_id tid = luaL_checkvid(ctx, 2);
		arcan_fatal("arcan_lua_net_pushcl() -- pushing frameserver state"
			"	not implemented.\n");
	}
	else if (lua_istable(ctx, 2)){
		arcan_fatal("arcan_lua_net_pushcl() -- pushing frameserver state"
			"	not implemented.\n");
	}
	else
		arcan_fatal("arcan_lua_net_pushcl() -- unexpected data to push, accepted "
			"(string, VID, evtable)\n");

/* for *NUX, setup a pipe() pair, push the output end to the client, 
 * push the input end to the server, emit FDtransfer messages, flagging that
 * it is going to be used for state-transfer. The last bit is important to be 
 * able to support both sending and receiving states, with compression and 
 * deltaframes in load/store operations. this also requires that the 
 * capabilities of the target actually allows for save-states,
 * by default, they don't. */
	arcan_frameserver_pushevent(fsrv, &outev);

	return 0;
}

/* similar to push to client, with the added distinction of broadcast / target,
 * and thus a more complicated pushFD etc. behavior */
static int arcan_lua_net_pushsrv(lua_State* ctx)
{
	arcan_vobj_id did   = luaL_checkvid(ctx, 1);
	arcan_vobject* vobj = arcan_video_getobject(did);
	int domain          = luaL_optnumber(ctx, 3, 0);

/* arg2 can be (string) => NETMSG, (event) => just push */
	arcan_event outev = {.category = EVENT_NET, .data.network.connid = domain};

	if (vobj->feed.state.tag != ARCAN_TAG_FRAMESERV || !vobj->feed.state.ptr)
		arcan_fatal("arcan_lua_net_pushsrv() -- bad arg1, VID "
			"is not a frameserver.\n");

	arcan_frameserver* fsrv = vobj->feed.state.ptr;

	if (!fsrv->kind == ARCAN_FRAMESERVER_NETSRV)
		arcan_fatal("arcan_lua_net_pushsrv() -- bad arg1, specified frameserver"
			" is not in client mode (net_open).\n");

/* we clean this as to not expose stack trash */
	size_t out_sz = sizeof(outev.data.network.message) / 
		sizeof(outev.data.network.message[0]);
	memset(outev.data.network.message, 0, out_sz);

	if (lua_isstring(ctx, 2)){
		outev.kind = EVENT_NET_CUSTOMMSG;

		const char* msg = luaL_checkstring(ctx, 2);
		snprintf(outev.data.network.message, out_sz, "%s", msg);
	}
	else if (lua_isnumber(ctx, 2)){
		arcan_vobj_id tid = luaL_checkvid(ctx, 2);
		arcan_fatal("arcan_lua_net_pushsrv() -- "
			"pushing VID (image, frameserver, ...) not implemented.\n");
	}
	else if (lua_istable(ctx, 2)){
		arcan_fatal("arcan_lua_net_pushsrv() -- "
			"pushing event table not implemented.\n");
	}
	else
		arcan_fatal("arcan_lua_net_pushsrv() -- "
			"unexpected data to push, accepted (string, VID, evtable)\n");

	arcan_frameserver_pushevent(fsrv, &outev);
	return 0;
}

static inline arcan_frameserver* luaL_checknet(lua_State* ctx, 
	bool server, const char* prefix)
{
	arcan_vobj_id did = luaL_checkvid(ctx, 1);
	arcan_vobject* vobj = arcan_video_getobject(did);

	if (vobj->feed.state.tag != ARCAN_TAG_FRAMESERV || !vobj->feed.state.ptr)
		arcan_fatal("%s -- VID is not a frameserver.\n", prefix);

	arcan_frameserver* fsrv = vobj->feed.state.ptr;

	if (server && fsrv->kind != ARCAN_FRAMESERVER_NETSRV){
		arcan_fatal("%s -- Frameserver connected to VID is not in server mode "
			"(net_open vs net_listen)\n", prefix);
	}
	else if (!server && fsrv->kind != ARCAN_FRAMESERVER_NETCL)
		arcan_fatal("%s -- Frameserver connected to VID is not in client mode "
			"(net_open vs net_listen)\n", prefix);
	return fsrv;
}

static int arcan_lua_net_accept(lua_State* ctx)
{
	arcan_frameserver* fsrv = luaL_checknet(ctx, true, 
		"arcan_lua_net_accept(vid, connid)");

	int domain = luaL_checkint(ctx, 2);

	if (domain == 0)
		arcan_fatal("arcan_lua_net_accept(vid, connid) -- NET_BROADCAST is not "
			"allowed for accept call\n");

	arcan_event outev = {.category = EVENT_NET, .data.network.connid = domain};
	arcan_frameserver_pushevent(fsrv, &outev);

	return 0;
}

static int arcan_lua_net_disconnect(lua_State* ctx)
{
	arcan_frameserver* fsrv = luaL_checknet(ctx, true, 
		"arcan_lua_net_disconnect(vid, connid)");

	int domain = luaL_checkint(ctx, 2);

	arcan_event outev = {
		.category = EVENT_NET,
	 	.kind = EVENT_NET_DISCONNECT,
	 	.data.network.connid = domain
	};

	arcan_frameserver_pushevent(fsrv, &outev);
	return 0;
}

static int arcan_lua_net_authenticate(lua_State* ctx)
{
	arcan_frameserver* fsrv = luaL_checknet(ctx, true, 
		"arcan_lua_net_authenticate(vid, connid)");

	int domain = luaL_checkint(ctx, 2);
	if (domain == 0)
		arcan_fatal("arcan_lua_net_authenticate(vid, connid) -- "
			"NET_BROADCAST is not allowed for accept call\n");

	arcan_event outev = {
		.category = EVENT_NET,
	 	.kind = EVENT_NET_AUTHENTICATE,
	 	.data.network.connid = domain
	};
	arcan_frameserver_pushevent(fsrv, &outev);

	return 0;
}

/* 
 * quite expensive pushes and not that everyone has a use-case for this,
 * so we separate it out and if the user wants working graphing, 
 * have him or her push refreshes 
 */ 
static int arcan_lua_net_refresh(lua_State* ctx)
{
	arcan_vobj_id vid = luaL_checkvid(ctx, 1);
	arcan_event outev = {.category = EVENT_NET, .kind = EVENT_NET_GRAPHREFRESH};

	arcan_vobject* vobj = arcan_video_getobject(vid);
	arcan_frameserver* fsrv = vobj->feed.state.ptr;

	if (!fsrv)
		return 0;

	if (!fsrv->kind == ARCAN_FRAMESERVER_NETSRV)
		arcan_fatal("arcan_lua_net_pushsrv() -- bad arg1, specified frameserver "
		"is not in client mode (net_open).\n");

	arcan_frameserver_pushevent(fsrv, &outev);

	return 0;
}

void arcan_lua_cleanup()
{
}

void arcan_lua_pushargv(lua_State* ctx, char** argv)
{
	int argc = 0;

	lua_newtable(ctx);
	int top = lua_gettop(ctx);

	while(argv[argc]){
		lua_pushnumber(ctx, argc + 1);
		lua_pushstring(ctx, argv[argc]);
		lua_rawset(ctx, top);
		argc++;
	}

	lua_setglobal(ctx, "arguments");
}

static void arcan_lua_register_tbl(lua_State* ctx, const luaL_Reg* funtbl) 
{
	while(funtbl->name != NULL){
		lua_pushstring(ctx, funtbl->name);
		lua_pushcclosure(ctx, funtbl->func, 1);
		lua_setglobal(ctx, funtbl->name);
		funtbl++;
	}
}

static void arcan_lua_register(lua_State* ctx, 
	const char* name, lua_CFunction func) 
{
	lua_pushstring(ctx, name);
	lua_pushcclosure(ctx, func, 1);
	lua_setglobal(ctx, name);
}

arcan_errc arcan_lua_exposefuncs(lua_State* ctx, unsigned char debugfuncs)
{
	if (!ctx)
		return ARCAN_ERRC_UNACCEPTED_STATE;

	lua_ctx_store.debug = debugfuncs;
	lua_atpanic(ctx, (lua_CFunction) arcan_lua_panic);

#ifdef _DEBUG
	lua_ctx_store.lua_vidbase = rand() % 32768;
	arcan_warning("lua_exposefuncs() -- videobase is set to %u\n", 
		lua_ctx_store.lua_vidbase);
#endif

/* these defines / tables are also scriptably extracted and 
 * mapped to build / documentation / static verification to ensure
 * coverage in the API binding -- so keep this format (down to the
 * whitespacing, simple regexs used. */
#define EXT_MAPTBL_RESOURCE
static const luaL_Reg resfuns[] = {
{"resource",          arcan_lua_resource        },
{"glob_resource",     arcan_lua_globresource    },
{"zap_resource",      arcan_lua_zapresource     },
{"open_rawresource",  arcan_lua_rawresource     },
{"close_rawresource", arcan_lua_rawclose        },
{"write_rawresource", arcan_lua_pushrawstr      },
{"read_rawresource",  arcan_lua_readrawresource },
{"save_screenshot",   arcan_lua_screenshot      },
{NULL, NULL}
};
#undef EXT_MAPTBL_RESOURCE
	arcan_lua_register_tbl(ctx, resfuns);

#define EXT_MAPTBL_TARGETCONTROL
static const luaL_Reg tgtfuns[] = {
{"launch_target",              arcan_lua_targetlaunch             },
{"launch_target_capabilities", arcan_lua_targetlaunch_capabilities},
{"target_input",               arcan_lua_targetinput              },
{"input_target",               arcan_lua_targetinput              },
{"suspend_target",             arcan_lua_targetsuspend            },
{"resume_target",              arcan_lua_targetresume             },
{"target_portconfig",          arcan_lua_targetportcfg            },
{"target_framemode",           arcan_lua_targetskipmodecfg        },
{"target_pointsize",           arcan_lua_targetpointsize          },
{"target_linewidth",           arcan_lua_targetlinewidth          },
{"target_postfilter",          arcan_lua_targetpostfilter         },
{"target_graphmode",           arcan_lua_targetgraph              },
{"target_postfilter_args",     arcan_lua_targetpostfilterargs     },
{"target_seek",                arcan_lua_targetseek               },
{"stepframe_target",           arcan_lua_targetstepframe          },
{"snapshot_target",            arcan_lua_targetsnapshot           },
{"restore_target",             arcan_lua_targetrestore            },
{"reset_target",               arcan_lua_targetreset              },
{"define_rendertarget",        arcan_lua_renderset                },
{"define_recordtarget",        arcan_lua_recordset                },
{"rendertarget_attach",        arcan_lua_renderattach             },
{"play_movie",                 arcan_lua_playmovie                },
{"load_movie",                 arcan_lua_loadmovie                },
{"pause_movie",                arcan_lua_pausemovie               },
{"resume_movie",               arcan_lua_resumemovie              },
{NULL, NULL}
};
#undef EXT_MAPTBL_TARGETCONTROL
	arcan_lua_register_tbl(ctx, tgtfuns);

#define EXT_MAPTBL_DATABASE
static const luaL_Reg dbfuns[] = {
{"store_key",    arcan_lua_storekey   },
{"get_key",      arcan_lua_getkey     },
{"game_cmdline", arcan_lua_getcmdline },
{"list_games",   arcan_lua_filtergames},
{"list_targets", arcan_lua_gettargets },
{"game_info",    arcan_lua_getgame    },
{"game_family",  arcan_lua_gamefamily },
{"game_genres",  arcan_lua_getgenres  },
{NULL, NULL}
};
#undef EXT_MAPTBL_DATABASE
	arcan_lua_register_tbl(ctx, dbfuns);

#define EXT_MAPTBL_AUDIO
static const luaL_Reg audfuns[] = {
{"play_audio",        arcan_lua_playaudio   },
{"pause_audio",       arcan_lua_pauseaudio  },
{"delete_audio",      arcan_lua_dropaudio   },
{"load_asample",      arcan_lua_loadasample },
{"audio_gain",        arcan_lua_gain        },
{"capture_audio",     arcan_lua_captureaudio},
{"list_audio_inputs", arcan_lua_capturelist },
{NULL, NULL}
};
#undef EXT_MAPTBL_AUDIO
	arcan_lua_register_tbl(ctx, audfuns);
	
#define EXT_MAPTBL_IMAGE
static const luaL_Reg imgfuns[] = {
{"load_image",               arcan_lua_loadimage          },
{"load_image_asynch",        arcan_lua_loadimageasynch    },
{"image_loaded",             arcan_lua_imageloaded        },
{"delete_image",             arcan_lua_deleteimage        },
{"show_image",               arcan_lua_showimage          },
{"hide_image",               arcan_lua_hideimage          },
{"move_image",               arcan_lua_moveimage          },
{"nudge_image",              arcan_lua_nudgeimage         },
{"rotate_image",             arcan_lua_rotateimage        },
{"scale_image",              arcan_lua_scaleimage         },
{"resize_image",             arcan_lua_scaleimage2        },
{"blend_image",              arcan_lua_imageopacity       },
{"persist_image",            arcan_lua_imagepersist       },
{"image_parent",             arcan_lua_imageparent        },
{"image_children",           arcan_lua_imagechildren      },
{"order_image",              arcan_lua_orderimage         },
{"max_current_image_order",  arcan_lua_maxorderimage      },
{"instance_image",           arcan_lua_instanceimage      },
{"link_image",               arcan_lua_linkimage          },
{"set_image_as_frame",       arcan_lua_imageasframe       },
{"image_framesetsize",       arcan_lua_framesetalloc      },
{"image_framecyclemode",     arcan_lua_framesetcycle      },
{"image_pushasynch",         arcan_lua_pushasynch         },
{"image_active_frame",       arcan_lua_activeframe        },
{"image_origo_offset",       arcan_lua_origoofs           },
{"image_inherit_order",      arcan_lua_orderinherit       },
{"expire_image",             arcan_lua_setlife            },
{"reset_image_transform",    arcan_lua_resettransform     },
{"instant_image_transform",  arcan_lua_instanttransform   },
{"image_transform_cycle",    arcan_lua_cycletransform     },
{"copy_image_transform",     arcan_lua_copytransform      },
{"transfer_image_transform", arcan_lua_transfertransform  },
{"copy_surface_properties",  arcan_lua_copyimageprop      },
{"image_set_txcos",          arcan_lua_settxcos           },
{"image_get_txcos",          arcan_lua_gettxcos           },
{"image_set_txcos_default",  arcan_lua_settxcos_default   },
{"image_texfilter",          arcan_lua_changetexfilter    },
{"image_scale_txcos",        arcan_lua_scaletxcos         },
{"image_clip_on",            arcan_lua_clipon             },
{"image_clip_off",           arcan_lua_clipoff            },
{"image_mask_toggle",        arcan_lua_togglemask         },
{"image_mask_set",           arcan_lua_setmask            },
{"image_screen_coordinates", arcan_lua_screencoord        },
{"image_mask_clear",         arcan_lua_clearmask          },
{"image_tracetag",           arcan_lua_tracetag           },
{"image_mask_clearall",      arcan_lua_clearall           },
{"image_shader",             arcan_lua_setshader          },
{"image_sharestorage",       arcan_lua_sharestorage       },
{"image_color",              arcan_lua_imagecolor         },
{"fill_surface",             arcan_lua_fillsurface        },
{"raw_surface",              arcan_lua_rawsurface         },
{"color_surface",            arcan_lua_colorsurface       },
{"null_surface",             arcan_lua_nullsurface        },
{"image_surface_properties", arcan_lua_getimageprop       },
{"image_storage_properties", arcan_lua_getimagestorageprop},
{"render_text",              arcan_lua_buildstr           },
{"text_dimensions",          arcan_lua_strsize            },
{"image_borderscan",         arcan_lua_borderscan         },
{"random_surface",           arcan_lua_randomsurface      },
{"force_image_blend",        arcan_lua_forceblend         },
{"image_hit",                arcan_lua_hittest            },
{"pick_items",               arcan_lua_pick               },
{"image_surface_initial_properties", arcan_lua_getimageinitprop   },
{"image_surface_resolve_properties", arcan_lua_getimageresolveprop},
{NULL, NULL}
};
#undef EXT_MAPTBL_IMAGE
	arcan_lua_register_tbl(ctx, imgfuns);

#define EXT_MAPTBL_3D
static const luaL_Reg threedfuns[] = {
{"new_3dmodel",      arcan_lua_buildmodel   },
{"add_3dmesh",       arcan_lua_loadmesh     },
{"attrtag_model",    arcan_lua_attrtag      },
{"move3d_model",     arcan_lua_movemodel    },
{"rotate3d_model",   arcan_lua_rotatemodel  },
{"orient3d_model",   arcan_lua_orientmodel  },
{"scale3d_model",    arcan_lua_scalemodel   },
{"forward3d_model",  arcan_lua_forwardmodel },
{"strafe3d_model",   arcan_lua_strafemodel  },
{"camtag_model",     arcan_lua_camtag       },
{"camtaghmd_model",  arcan_lua_camtaghmd    },
{"build_3dplane",    arcan_lua_buildplane   },
{"build_3dbox",      arcan_lua_buildbox     },
{"scale_3dvertices", arcan_lua_scale3dverts },
{"swizzle_model",    arcan_lua_swizzlemodel },
{"mesh_shader",      arcan_lua_setmeshshader},
{NULL, NULL}
};
#undef EXT_MAPTBL_3D
	arcan_lua_register_tbl(ctx, threedfuns);

#define EXT_MAPTBL_SYSTEM
static const luaL_Reg sysfuns[] = {
{"shutdown",            arcan_lua_shutdown         },
{"switch_theme",        arcan_lua_switchtheme      },
{"warning",             arcan_luac_warning         },
{"system_load",         arcan_lua_dofile           },
{"system_context_size", arcan_lua_systemcontextsize},
{"utf8kind",            arcan_lua_utf8kind         },
{"decode_modifiers",    arcan_lua_decodemod        },
{"benchmark_enable",    arcan_lua_togglebench      },
{"benchmark_timestamp", arcan_lua_timestamp        },
{"benchmark_data",      arcan_lua_getbenchvals     },
#ifdef _DEBUG
{"freeze_image",        arcan_lua_freezeimage      },
#endif
{NULL, NULL}
};
#undef EXT_MAPTBL_SYSTEM
	arcan_lua_register_tbl(ctx, sysfuns);

#define EXT_MAPTBL_IODEV
static const luaL_Reg iofuns[] = {
{"kbd_repeat",          arcan_lua_kbdrepeat        },
{"toggle_mouse_grab",   arcan_lua_mousegrab        },
#ifndef ARCAN_LUA_NOLED
{"set_led",             arcan_lua_setled           },
{"led_intensity",       arcan_lua_led_intensity    },
{"set_led_rgb",         arcan_lua_led_rgb          },
{"controller_leds",     arcan_lua_n_leds           },
#endif
{"inputanalog_filter",  arcan_lua_inputfilteranalog},
{"inputanalog_query",   arcan_lua_inputanalogquery},
{"inputanalog_toggle",  arcan_lua_inputanalogtoggle},
{NULL, NULL},
};
#undef EXT_MAPTBL_IODEV
	arcan_lua_register_tbl(ctx, iofuns);

#define EXT_MAPTBL_VIDSYS
static const luaL_Reg vidsysfuns[] = {
{"switch_default_scalemode",         arcan_lua_setscalemode   },
{"switch_default_texmode",           arcan_lua_settexmode     },
{"switch_default_imageproc",         arcan_lua_setimageproc   },
{"switch_default_texfilter",         arcan_lua_settexfilter   },
{"video_3dorder",                    arcan_lua_3dorder        },
{"default_movie_queueopts",          arcan_lua_getqueueopts   },
{"default_movie_queueopts_override", arcan_lua_setqueueopts   },
{"build_shader",                     arcan_lua_buildshader    },
{"valid_vid",                        arcan_lua_validvid       },
{"shader_uniform",                   arcan_lua_shader_uniform },
{"push_video_context",               arcan_lua_pushcontext    },
{"storepush_video_context",          arcan_lua_pushcontext_ext},
{"storepop_video_context",           arcan_lua_popcontext_ext },
{"pop_video_context",                arcan_lua_popcontext     },
{"current_context_usage",            arcan_lua_contextusage   },
{NULL, NULL},
};
#undef EXT_MAPTBL_VIDSYS
	arcan_lua_register_tbl(ctx, vidsysfuns);

#define EXT_MAPTBL_NETWORK
static const luaL_Reg netfuns[] = {
{"net_open",         arcan_lua_net_open        },
{"net_push",         arcan_lua_net_pushcl      },
{"net_listen",       arcan_lua_net_listen      },
{"net_push_srv",     arcan_lua_net_pushsrv     },
{"net_disconnect",   arcan_lua_net_disconnect  },
{"net_authenticate", arcan_lua_net_authenticate},
{"net_accept",       arcan_lua_net_accept      },
{"net_refresh",      arcan_lua_net_refresh     },
{NULL, NULL},
};
#undef EXT_MAPTBL_NETWORK
	arcan_lua_register_tbl(ctx, netfuns);

	atexit(arcan_lua_cleanup);
	return ARCAN_OK;
}

void arcan_lua_pushglobalconsts(lua_State* ctx){
#define EXT_CONSTTBL_GLOBINT
	struct { const char* key; int val; } consttbl[] = {
{"VRESH", arcan_video_screenh()},
{"VRESW", arcan_video_screenw()},
{"VSYNCH_TIMING", arcan_video_display.vsync_timing},
{"VSYNCH_STDDEV", arcan_video_display.vsync_stddev},
{"VSYNCH_VARIANCE", arcan_video_display.vsync_variance},
{"MAX_SURFACEW",   MAX_SURFACEW        },
{"MAX_SURFACEH",   MAX_SURFACEH        },
{"STACK_MAXCOUNT", CONTEXT_STACK_LIMIT },
{"FRAMESET_SPLIT",        ARCAN_FRAMESET_SPLIT       },
{"FRAMESET_MULTITEXTURE", ARCAN_FRAMESET_MULTITEXTURE},
{"FRAMESET_NODETACH",     FRAMESET_NODETACH          },
{"FRAMESET_DETACH",       FRAMESET_DETACH            },
{"BLEND_NONE",     BLEND_NONE    },
{"BLEND_ADD",      BLEND_ADD     },
{"BLEND_MULTIPLY", BLEND_MULTIPLY},
{"BLEND_NORMAL",   BLEND_NORMAL  },
{"RENDERTARGET_NOSCALE",  RENDERTARGET_NOSCALE },
{"RENDERTARGET_SCALE",    RENDERTARGET_SCALE   },
{"RENDERTARGET_NODETACH", RENDERTARGET_NODETACH},
{"RENDERTARGET_DETACH",   RENDERTARGET_DETACH  },
{"RENDERTARGET_COLOR", RENDERFMT_COLOR },
{"RENDERTARGET_DEPTH", RENDERFMT_DEPTH },
{"RENDERTARGET_FULL", RENDERFMT_FULL }, 
{"ROTATE_RELATIVE", CONST_ROTATE_RELATIVE},
{"ROTATE_ABSOLUTE", CONST_ROTATE_ABSOLUTE},
{"TEX_REPEAT",       ARCAN_VTEX_REPEAT      },
{"TEX_CLAMP",        ARCAN_VTEX_CLAMP       },
{"FILTER_NONE",      ARCAN_VFILTER_NONE     },
{"FILTER_LINEAR",    ARCAN_VFILTER_LINEAR   },
{"FILTER_BILINEAR",  ARCAN_VFILTER_BILINEAR },
{"FILTER_TRILINEAR", ARCAN_VFILTER_TRILINEAR},
{"SCALE_NOPOW2",     ARCAN_VIMAGE_NOPOW2},
{"SCALE_POW2",       ARCAN_VIMAGE_SCALEPOW2},
{"IMAGEPROC_NORMAL", imageproc_normal},
{"IMAGEPROC_FLIPH",  imageproc_fliph },
{"WORLDID", ARCAN_VIDEO_WORLDID},
{"CLIP_ON", ARCAN_CLIP_ON},
{"CLIP_OFF", ARCAN_CLIP_OFF},
{"CLIP_SHALLOW", ARCAN_CLIP_SHALLOW},
{"BADID",   ARCAN_EID         },
{"CLOCKRATE", ARCAN_TIMER_TICK},
{"CLOCK",     0               },
{"THEME_RESOURCE",    ARCAN_RESOURCE_THEME                        },
{"SHARED_RESOURCE",   ARCAN_RESOURCE_SHARED                       },
{"ALL_RESOURCES",     ARCAN_RESOURCE_THEME | ARCAN_RESOURCE_SHARED},
{"API_VERSION_MAJOR", 0},
{"API_VERSION_MINOR", 6},
{"LAUNCH_EXTERNAL",   0},
{"LAUNCH_INTERNAL",   1},
{"MASK_LIVING",      MASK_LIVING     },
{"MASK_ORIENTATION", MASK_ORIENTATION},
{"MASK_OPACITY",     MASK_OPACITY    },
{"MASK_POSITION",    MASK_POSITION   },
{"MASK_SCALE",       MASK_SCALE      },
{"MASK_UNPICKABLE",  MASK_UNPICKABLE },
{"MASK_FRAMESET",    MASK_FRAMESET   },
{"MASK_MAPPING",     MASK_MAPPING    },
{"ORDER_FIRST",      order3d_first   },
{"ORDER_NONE",       order3d_none    },
{"ORDER_LAST",       order3d_last    },
{"ORDER_SKIP",       order3d_none    },
{"MOUSE_GRABON",       MOUSE_GRAB_ON      },
{"MOUSE_GRABOFF",      MOUSE_GRAB_OFF     },
{"FRAMESERVER_LOOP",   FRAMESERVER_LOOP   },
{"FRAMESERVER_NOLOOP", FRAMESERVER_NOLOOP },
{"POSTFILTER_NTSC",    POSTFILTER_NTSC    },
{"POSTFILTER_OFF",     POSTFILTER_OFF     },
#ifndef ARCAN_LUA_NOLED
{"LEDCONTROLLERS",     arcan_led_controllers()},
#endif
{"NOW",           0},
{"NOPERSIST",     0},
{"PERSIST",       1},
{"NET_BROADCAST", 0},
{"DEBUGLEVEL",    lua_ctx_store.debug}	
};
#undef EXT_CONSTTBL_GLOBINT

	for (int i = 0; i < sizeof(consttbl) / sizeof(consttbl[0]); i++)
		arcan_lua_setglobalint(ctx, consttbl[i].key, consttbl[i].val);

	arcan_lua_setglobalstr(ctx, "THEMENAME",    arcan_themename          );
	arcan_lua_setglobalstr(ctx, "RESOURCEPATH", arcan_resourcepath       );
	arcan_lua_setglobalstr(ctx, "THEMEPATH",    arcan_themepath          );
	arcan_lua_setglobalstr(ctx, "BINPATH",      arcan_binpath            );
	arcan_lua_setglobalstr(ctx, "LIBPATH",      arcan_libpath            );
	arcan_lua_setglobalstr(ctx, "INTERNALMODE", internal_launch_support());
}

/* 
 * What follows is just the mass of coded needed to serialize
 * as much of the internal state of the engine as possible
 * over a FILE* stream in a form that can be decoded and used
 * by a monitoring script to help debugging and performance
 * optimizations
 */
static const char* const vobj_flags(arcan_vobject* src)
{
	static char fbuf[64];
	fbuf[0] = '\0';
	if (src->flags.persist)
		strcat(fbuf, "persist ");
	if (src->flags.clone)
		strcat(fbuf, "clone ");
	if (src->flags.cliptoparent)
		strcat(fbuf, "clip ");
	if (src->flags.asynchdisable)
		strcat(fbuf, "noasynch ");
	if (src->flags.cycletransform)
		strcat(fbuf, "cycletransform ");
	if (src->flags.origoofs)
		strcat(fbuf, "origo ");
	if (src->flags.orderofs)
		strcat(fbuf, "order ");
	return fbuf;
}

static inline char* lut_filtermode(enum arcan_vfilter_mode mode)
{
	switch(mode){
	case ARCAN_VFILTER_NONE     : return "none";
	case ARCAN_VFILTER_LINEAR   : return "linear";
	case ARCAN_VFILTER_BILINEAR : return "bilinear";
	case ARCAN_VFILTER_TRILINEAR: return "trilinear";
	}
}

static inline char* lut_imageproc(enum arcan_imageproc_mode mode)
{
	switch(mode){
	case imageproc_normal: return "normal";
	case imageproc_fliph : return "vflip";
	}
}

static inline char* lut_scale(enum arcan_vimage_mode mode)
{
	switch(mode){
	case ARCAN_VIMAGE_NOPOW2    : return "nopow2";
	case ARCAN_VIMAGE_SCALEPOW2 : return "scalepow2"; 
	}
}

static inline char* lut_framemode(enum arcan_framemode mode)
{
	switch(mode){
	case ARCAN_FRAMESET_SPLIT        : return "split"; 
	case ARCAN_FRAMESET_MULTITEXTURE : return "multitexture";
	}
}

static inline char* lut_clipmode(enum arcan_clipmode mode)
{
	switch(mode){
	case ARCAN_CLIP_OFF     : return "disabled";
	case ARCAN_CLIP_ON      : return "stencil deep";
	case ARCAN_CLIP_SHALLOW : return "stencil shallow";
	case ARCAN_CLIP_SCISSOR : return "scissor";
	}
}

static inline char* lut_blendmode(enum arcan_blendfunc func)
{
	switch(func){
	case blend_disable  : return "disabled";
	case blend_normal   : return "normal";
	case blend_force    : return "forceblend";
	case blend_add      : return "additive";
	case blend_multiply : return "multiply";
	}
}

/*
 * Ignore LOCALE RADIX settings...
 * Some dependencies actually dynamically change locale
 * affecting float representation during fprintf (nice race there) 
 * by defining nan and infinite locally first (see further below)
 * we cover that case, but still need to go through the headache
 * of splitting
 */
static inline void fprintf_float(FILE* dst, 
	const char* pre, float in, const char* post)
{
	float intp, fractp;
 	fractp = modff(in, &intp);

	if (isnan(in))
		fprintf(dst, "%snan%s", pre, post);
	else if (isinf(in))
		fprintf(dst, "%sinf%s", pre, post);
	else
		fprintf(dst, "%s%d.%d%s", pre, (int)intp, abs(fractp), post);
}

static inline char* lut_kind(arcan_vobject* src)
{
	if (src->feed.state.tag == ARCAN_TAG_IMAGE)
		return src->vstore->txmapped ? "textured" : "single color";
	else if (src->feed.state.tag == ARCAN_TAG_FRAMESERV)
		return "frameserver";
	else if (src->feed.state.tag == ARCAN_TAG_ASYNCIMG)
		return "textured_loading";
	else if (src->feed.state.tag == ARCAN_TAG_3DOBJ)
		return "3dobject";
	else
		return "dead";
}

static inline void dump_props(FILE* dst, surface_properties props)
{	
	fprintf_float(dst, "props.position = {", props.position.x, ", ");
	fprintf_float(dst, "", props.position.y, ", ");
	fprintf_float(dst, "", props.position.z, "};\n");

	fprintf_float(dst, "props.scale = {", props.scale.x, ", ");
	fprintf_float(dst, "", props.scale.y, ", ");
	fprintf_float(dst, "", props.scale.z, "};\n");

	fprintf_float(dst, "props.rotation = {", props.rotation.roll, ", ");
	fprintf_float(dst, "", props.rotation.pitch, ", ");
	fprintf_float(dst, "", props.rotation.yaw, "};\n");
}

static inline void dump_vobject(FILE* dst, arcan_vobject* src)
{
	char* mask = maskstr(src->mask);

	fprintf(dst,
"vobj = {\n\
\torigw = %d,\n\
\torigh = %d,\n\
\torder = %d,\n\
\tlast_updated = %d,\n\
\tlifetime = %d,\n\
\tcellid = %d,\n\
\tvalid_cache = %d,\n\
\trotate_state = %d,\n\
\tframeset_capacity = %d,\n\
\tframeset_mode = %d,\n\
\tframeset_counter = %d,\n\
\tframeset_current = %d,\n\
\textrefc_framesets = %d,\n\
\textrefc_instances = %d,\n\
\textrefc_attachments = %d,\n\
\textrefc_links = %d,\n\
\tstorage_source = [[%s]],\n\
\tstorage_size = %d,\n\
\tglstore_w = %d,\n\
\tglstore_h = %d,\n\
\tglstore_bpp = %d,\n\
\tglstore_txu = %d,\n\
\tglstore_txv = %d,\n\
\tglstore_prg = [[%s]],\n\
\tscalemode  = [[%s]],\n\
\timageproc = [[%s]],\n\
\tblendmode = [[%s]],\n\
\tclipmode  = [[%s]],\n\
\tfiltermode = [[%s]],\n\
\tflags = [[%s]],\n\
\tmask = [[%s]],\n\
\tframeset = {},\n\
\tkind = [[%s]],\n\
\ttracetag = [[%s]],\n\
",
(int) src->origw,
(int) src->origh,
(int) src->order,
(int) src->last_updated,
(int) src->lifetime,
(int) src->cellid,
(int) src->valid_cache,
(int) src->rotate_state,
(int) src->frameset_meta.capacity,
(int) src->frameset_meta.mode,
(int) src->frameset_meta.counter,
(int) src->frameset_meta.current,
(int) src->extrefc.framesets,
(int) src->extrefc.instances,
(int) src->extrefc.attachments,
(int) src->extrefc.links,
(src->vstore->txmapped && src->vstore->vinf.text.source) ?
	src->vstore->vinf.text.source : "unknown",
(int) src->vstore->vinf.text.s_raw,
(int) src->vstore->w,
(int) src->vstore->h,
(int) src->vstore->bpp,
(int) src->vstore->txu,
(int) src->vstore->txv,
arcan_shader_lookuptag(src->program),
lut_scale(src->vstore->scale),
lut_imageproc(src->vstore->imageproc),
lut_blendmode(src->blendmode),
lut_clipmode(src->flags.cliptoparent),
lut_filtermode(src->vstore->filtermode),
vobj_flags(src), 
mask,
lut_kind(src),
src->tracetag ? src->tracetag : "no tag");

	fprintf_float(dst, "origoofs = {", src->origo_ofs.x, ", ");
	fprintf_float(dst, "", src->origo_ofs.y, ", ");
	fprintf_float(dst, "", src->origo_ofs.z, "}\n};\n");

	if (src->vstore->txmapped){
		fprintf(dst, "vobj.glstore_glid = %d;\n\
vobj.glstore_refc = %d;\n", src->vstore->vinf.text.glid,
			src->vstore->refcount);
	} else {
		fprintf_float(dst, "vobj.glstore_col = {", src->vstore->vinf.col.r, ", ");
		fprintf_float(dst, "", src->vstore->vinf.col.g, ", ");
		fprintf_float(dst, "", src->vstore->vinf.col.b, "};\n");
	}

	for (int i = 0; i < src->frameset_meta.capacity; i++)
	{
		fprintf(dst, "vobj.frameset[%d] = %"PRIxVOBJ";\n", i + 1,
			src->frameset[i] ? src->frameset[i]->cellid : ARCAN_EID);
	}

	if (src->children){
		fprintf(dst, "vobj.children = {};\n");
		fprintf(dst, "vobj.childslots = %d;\n", (int) src->childslots);
		for (int i = 0; i < src->childslots; i++){
			fprintf(dst, "vobj.children[%d] = %"PRIxVOBJ";\n", i+1, src->children[i] ?
				src->children[i]->cellid : ARCAN_EID);
		}
	}

	if (src->parent->cellid == ARCAN_VIDEO_WORLDID){
		fprintf(dst, "vobj.parent = \"WORLD\";\n");
	} else {
		fprintf(dst, "vobj.parent = %"PRIxVOBJ";\n", src->parent->cellid);
	}

	fprintf(dst, "props = {};\n");
	dump_props(dst, src->current);
	fprintf(dst, "vobj.props = props;\n"); 

	free(mask);
}

/*
 * missing;
 *  frameset_framemode
 *  transform_chains
 *  texture_coordinates
 *  blendmode
 *  frameset
 *  ffunc mask
 *  current_frame reference
 *  vstore->values
 */

void arcan_lua_statesnap(FILE* dst)
{
/*
 * display global settings, wrap to local ptr for shorthand */
/* missing shaders,
 * missing event-queues
 */
	struct arcan_video_display* disp = &arcan_video_display;
fprintf(dst, " do \n\
local nan = 0/0;\n\
local inf = math.huge;\n\
local vobj = {};\n\
local props = {};\n\
local restbl = {\n\
\tdisplay = {\n\
\t\twidth = %d,\n\
\t\theight = %d,\n\
\t\tconservative = %d,\n\
\t\tvsync = %d,\n\
\t\tmsasamples = %d,\n\
\t\tticks = %lld,\n\
\t\tdefault_vitemlim = %d,\n\
\t\timageproc = %d,\n\
\t\tscalemode = %d,\n\
\t\tfiltermode = %d,\n\
\t};\n\
\tvcontexts = {};\
};\n\
", disp->width, disp->height, disp->conservative ? 1 : 0, disp->vsync ? 1 : 0, 
	(int)disp->msasamples, (long long int)disp->c_ticks, 
	(int)disp->default_vitemlim,
	(int)disp->imageproc, (int)disp->scalemode, (int)disp->filtermode);

	int cctx = vcontext_ind;
	while (cctx >= 0){
/* foreach context, header */
fprintf(dst,
"local ctx = {\n\
\tvobjs = {},\n\
\trtargets = {}\n\
};");

	struct arcan_video_context* ctx = &vcontext_stack[cctx];
	fprintf(dst, 
"ctx.ind = %d;\n\
ctx.alive = %d;\n\
ctx.limit = %d;\n\
ctx.tickstamp = %lld;\n", 
(int) cctx,
(int) ctx->nalive,
(int) ctx->vitem_limit,
(long long int) ctx->last_tickstamp
);

		for (int i = 0; i < ctx->vitem_limit; i++){
			if (ctx->vitems_pool[i].flags.in_use == false)
				continue;

			dump_vobject(dst, ctx->vitems_pool + i);
			fprintf(dst, "\
vobj.cellid_translated = %ld;\n\
ctx.vobjs[vobj.cellid] = vobj;\n", (long int)vid_toluavid(i));
		}

/* missing, rendertarget dump */
		fprintf(dst,"table.insert(restbl.vcontexts, ctx);");
		cctx--;
	}

	if (benchdata.bench_enabled){
		size_t bsz = sizeof(benchdata.ticktime)  / sizeof(benchdata.ticktime[0]);
		size_t fsz = sizeof(benchdata.frametime) / sizeof(benchdata.frametime[0]);
		size_t csz = sizeof(benchdata.framecost) / sizeof(benchdata.framecost[0]);

		int i = (benchdata.tickofs + 1) % bsz;
		fprintf(dst, "\nrestbl.benchmark = {};\nrestbl.benchmark.ticks = {");
		while (i != benchdata.tickofs){
			fprintf(dst, "%d,", benchdata.ticktime[i]);
			i = (i + 1) % bsz;
		}
		fprintf(dst, "};\nrestbl.benchmark.frames = {");
		i = (benchdata.frameofs + 1) % fsz;
		while (i != benchdata.frameofs){
			fprintf(dst, "%d,", benchdata.frametime[i]);
			i = (i + 1) % fsz;
		}
		fprintf(dst, "};\nrestbl.benchmark.framecost = {");
		i = (benchdata.costofs + 1) % csz;
		while (i != benchdata.costofs){
			fprintf(dst, "%d,", benchdata.framecost[i]);
			i = (i + 1) % csz;
		}
		fprintf(dst, "};\n");
	
		memset(benchdata.ticktime, '\0', sizeof(benchdata.ticktime));
		memset(benchdata.frametime, '\0', sizeof(benchdata.frametime));
		memset(benchdata.framecost, '\0', sizeof(benchdata.framecost));
		benchdata.tickofs = benchdata.frameofs = benchdata.costofs = 0;
	}

/* foreach context, footer */
 	fprintf(dst, "return restbl;\nend\n#ENDBLOCK\n");
	fflush(dst);
}

#ifndef _WIN32
#include <poll.h>
/* this assumes a trusted (src), as injected \0 could make the 
 * strstr fail and buffer indefinately
 * (so if this assumption breaks in the future,
 * scan and strip the input dataset for \0s */
void arcan_lua_stategrab(lua_State* ctx, char* dstfun, int src)
{
/* maintaing a growing buffer that is just populated with lines read 
 * from (src). When we uncover an endblock, we do a pcall and then 
 * push the value to the stack. */
	static char* statebuf;
	static size_t statebuf_sz, statebuf_ofs;
	static struct pollfd inpoll;

	ssize_t nread;
	size_t len;
	char* line = NULL;
	bool done = false;

/* initial setup */
	if (!statebuf){
		statebuf      = malloc(1024);
		statebuf_sz   = 1024;
		inpoll.fd     = src;
		inpoll.events = POLLIN;
		memset(statebuf, '\0', statebuf_sz);
	}

/* flush read into buffer, parse buffer for \n#ENDBLOCK\n pattern */
	if (poll(&inpoll, 1, 0) > 0){
		int ntr = statebuf_sz - 1 - statebuf_ofs;
		int nr = read(src, statebuf + statebuf_ofs, ntr);

		if (nr > 0){
			statebuf_ofs += nr;
			char* substrp = strstr(statebuf, "\n#ENDBLOCK\n");
/* got one? parse then slide */
			if (substrp){
				substrp[1] = '\0';
	
/*
 * FILE* outf = fopen("dumpfile", "w+");
 * fwrite(statebuf, 1, strlen(statebuf), outf);
 * fclose(outf);
 */
				lua_getglobal(ctx, "sample");
				if (!lua_isfunction(ctx, -1)){
					lua_pop(ctx, 1);
					arcan_warning("arcan_lua_stategrab(), couldn't find "
						"function 'sample' in debugscript. Sample ignored.\n");
				} else {
					int top = lua_gettop(ctx);
					luaL_loadstring(ctx, statebuf);
					lua_call(ctx, 0, LUA_MULTRET);
					int narg = lua_gettop(ctx) - top;
					lua_call(ctx, narg, 0);
				}
				
/* statebuf:****substrp\n#ENDBLOCK\n(11)****(statebuf_ofs) **** statebufsz-1 \0*/
				substrp += 11; 
				int ntm = statebuf_ofs - (substrp - statebuf);
				if (ntm > 0){
					memmove(statebuf, substrp, ntm);
					statebuf_ofs = ntm; 
				} else {
					statebuf_ofs = 0;
				}
				memset(statebuf + statebuf_ofs, '\0', statebuf_sz - statebuf_ofs);
			}
/* need more data, buffer or possibly realloc */	
		}

		if (statebuf_ofs == statebuf_sz - 1){
			statebuf_sz <<= 1;
			char* newp = realloc(statebuf, statebuf_sz);
			if (newp)
				statebuf = newp;
			else 
				statebuf_sz >>= 1;
		}
		
	}

}
#endif

