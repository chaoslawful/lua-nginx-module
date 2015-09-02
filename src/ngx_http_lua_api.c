
/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#include "ddebug.h"

#include "ngx_http_lua_common.h"
#include "api/ngx_http_lua_api.h"
#include "ngx_http_lua_util.h"


typedef struct ngx_http_lua_shm_zone_ctx_s {
    ngx_log_t                *log;
    ngx_http_lua_main_conf_t *lmcf;

    u_char                    data; /* ngx_shm_zone_t */
} ngx_http_lua_shm_zone_ctx_t;


static ngx_int_t
ngx_http_lua_shared_memory_init(ngx_shm_zone_t *shm_zone, void *data);


lua_State *
ngx_http_lua_get_global_state(ngx_conf_t *cf)
{
    ngx_http_lua_main_conf_t *lmcf;

    lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_lua_module);

    return lmcf->lua;
}


ngx_http_request_t *
ngx_http_lua_get_request(lua_State *L)
{
    return ngx_http_lua_get_req(L);
}


ngx_int_t
ngx_http_lua_add_package_preload(ngx_conf_t *cf, const char *package,
    lua_CFunction func)
{
    lua_State                     *L;
    ngx_http_lua_main_conf_t      *lmcf;
    ngx_http_lua_preload_hook_t   *hook;

    lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_lua_module);

    L = lmcf->lua;

    if (L) {
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "preload");
        lua_pushcfunction(L, func);
        lua_setfield(L, -2, package);
        lua_pop(L, 2);

        return NGX_OK;
    }

    /* L == NULL */

    if (lmcf->preload_hooks == NULL) {
        lmcf->preload_hooks =
            ngx_array_create(cf->pool, 4,
                             sizeof(ngx_http_lua_preload_hook_t));

        if (lmcf->preload_hooks == NULL) {
            return NGX_ERROR;
        }
    }

    hook = ngx_array_push(lmcf->preload_hooks);
    if (hook == NULL) {
        return NGX_ERROR;
    }

    hook->package = (u_char *) package;
    hook->loader = func;

    return NGX_OK;
}


ngx_shm_zone_t *
ngx_http_lua_shared_memory_add(ngx_conf_t *cf,
                               ngx_str_t *name,
                               size_t size,
                               void *tag)
{
    ngx_int_t                n;
    ngx_http_lua_main_conf_t *lmcf;
    ngx_shm_zone_t           **zp;
    ngx_shm_zone_t           *zone;
    ngx_http_lua_shm_zone_ctx_t *ctx;

    lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_lua_module);
    if (lmcf == NULL) {
        return NULL;
    }

    if (lmcf->shm_zones == NULL) {
        lmcf->shm_zones = ngx_palloc(cf->pool, sizeof(ngx_array_t));
        if (lmcf->shm_zones == NULL) {
            return NULL;
        }

        if (ngx_array_init(lmcf->shm_zones, cf->pool, 2,
                           sizeof(ngx_shm_zone_t *))
            != NGX_OK)
        {
            return NULL;
        }
    }

    zone = ngx_shared_memory_add(cf, name, (size_t) size, tag);
    if (zone == NULL) {
        return NULL;
    }

    if (zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "share memory \"%V\" of ngx_lua is already defined" , name);
        return NULL;
    }

    n = offsetof(ngx_http_lua_shm_zone_ctx_t, data)
        + sizeof(ngx_shm_zone_t);

    ctx = ngx_pcalloc(cf->pool, n);
    if (ctx == NULL) {
        return NULL;
    }

    ctx->lmcf = lmcf;
    ctx->log = &cf->cycle->new_log;
    ngx_memcpy(&ctx->data, zone, sizeof(*zone));

    zp = ngx_array_push(lmcf->shm_zones);
    if (zp == NULL) {
        return NULL;
    }

    *zp = (ngx_shm_zone_t *)&ctx->data;

    /* set zone init */
    zone->init = ngx_http_lua_shared_memory_init;
    zone->data = ctx;

    lmcf->requires_shm = 1;

    return *zp;
}


/* ngx_int_t */
/* ngx_http_lua_shared_memory_inited(ngx_log_t *log, ngx_conf_t *cf) */
/* { */
/*     ngx_http_lua_main_conf_t *lmcf; */

    /* lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_lua_module); */
    /* if (lmcf == NULL) { */
    /*     return NGX_ERROR; */
    /* } */

    /* lmcf->shm_zones_inited++; */

    /* if (lmcf->shm_zones_inited == lmcf->shm_zones->nelts */
    /*     && lmcf->init_handler) */
    /* { */
    /*     if (lmcf->init_handler(cf->log, lmcf, lmcf->lua) != NGX_OK) { */
    /*         /\* an error happened *\/ */
    /*         return NGX_ERROR; */
    /*     } */
    /* } */

/*     return NGX_OK; */
/* } */


static ngx_int_t
ngx_http_lua_shared_memory_init(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_lua_shm_zone_ctx_t *octx = data;
    ngx_shm_zone_t              *ozone;
    void                        *odata;

    ngx_http_lua_main_conf_t    *lmcf;
    ngx_http_lua_shm_zone_ctx_t *ctx;
    ngx_shm_zone_t              *zone;

    ctx = (ngx_http_lua_shm_zone_ctx_t *)shm_zone->data;
    zone = (ngx_shm_zone_t *)&ctx->data;

    odata = NULL;
    if (octx) {
        ozone = (ngx_shm_zone_t *)&octx->data;
        odata = ozone->data;
        zone->shm  = ozone->shm;
        zone->noreuse = ozone->noreuse;

    } else {
        zone->shm = shm_zone->shm;
        zone->noreuse = shm_zone->noreuse;
    }

    if (zone->init(zone, odata) != NGX_OK) {
        return NGX_ERROR;
    }

    lmcf = ctx->lmcf;
    if (lmcf == NULL) {
        return NGX_ERROR;
    }

    lmcf->shm_zones_inited++;

    if (lmcf->shm_zones_inited == lmcf->shm_zones->nelts
        && lmcf->init_handler)
    {
        if (lmcf->init_handler(ctx->log, lmcf, lmcf->lua) != NGX_OK) {
            /* an error happened */
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}
/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
