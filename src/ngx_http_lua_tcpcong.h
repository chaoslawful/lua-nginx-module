
/*
 * Copyright (C) Rain Li (blacktear23)
 *
 */

#ifndef _NGX_HTTP_LUA_TCPCONG_H_INCLUDED_
#define _NGX_HTTP_LUA_TCPCONG_H_INCLUDED_

#include "ngx_http_lua_common.h"

#if (NGX_LINUX)

#ifndef TCP_CONGESTION
#define TCP_CONGESTION 13
#endif

#endif

void ngx_http_lua_inject_req_tcp_congestion_api(lua_State *L);

#endif /* _NGX_HTTP_LUA_TCPCONG_H_INCLUDED_ */
