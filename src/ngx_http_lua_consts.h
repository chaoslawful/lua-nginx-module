#ifndef NGX_HTTP_LUA_CONSTS_INCLUDED_
#define NGX_HTTP_LUA_CONSTS_INCLUDED_


#include "ngx_http_lua_common.h"


void ngx_http_lua_inject_http_consts(lua_State *L);
void ngx_http_lua_inject_core_consts(lua_State *L);


#endif /* NGX_HTTP_LUA_CONSTS_INCLUDED_ */

