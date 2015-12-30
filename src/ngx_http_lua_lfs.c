

/**
 * Copyright (C) Terry AN (anhk)
 **/

#include "ddebug.h"
#include "ngx_http_lua_util.h"
#include "ngx_http_lua_lfs.h"

#if (NGX_THREADS)


/**
 * ctx for task thread
 **/
typedef struct _ngx_http_lua_lfs_task_ctx_s {
    lua_State *L;
    ngx_http_request_t *r;
    ngx_pool_t *pool;
    ngx_fd_t fd;
    ssize_t size;
    off_t offset;
    char *buff;
    ssize_t length;
    ngx_http_lua_co_ctx_t *coctx;
} ngx_http_lua_lfs_task_ctx_t;


/**
 * cached file descriptor
 **/
typedef struct _ngx_http_lfs_cached_fd_s {
    u_char *filename;
    ngx_fd_t fd;
} ngx_http_lfs_cached_fd_t;

static void ngx_http_lua_lfs_fdpool_cleanup(void *data)
{
    ngx_http_request_t *r = data;
    ngx_http_lua_ctx_t *ctx;
    ngx_http_lfs_cached_fd_t *cfd;
    ngx_uint_t i;

    if ((ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module)) == NULL) {
        return;
    }
    cfd = ctx->lfs_post_closed->elts;
    for (i = 0; i < ctx->lfs_post_closed->nelts; i ++) {
        ngx_close_file(cfd[i].fd);
    }
}


static int ngx_http_lua_lfs_fdpool_init(ngx_http_lua_ctx_t *ctx,
        ngx_http_request_t *r)
{
    ngx_http_cleanup_t *cln;
    if (ctx->lfs_post_closed != NULL) {
        return 0;
    }

    if ((cln = ngx_http_cleanup_add(r, 0)) == NULL) {
        return -1;
    }
    cln->handler = ngx_http_lua_lfs_fdpool_cleanup;
    cln->data = r;

    ctx->lfs_post_closed = ngx_array_create(r->pool, 8, sizeof(ngx_http_lfs_cached_fd_t));
    if (ctx->lfs_post_closed == NULL) {
        return -1;
    }

    return 0;
}

static ngx_fd_t ngx_http_lua_lfs_fdpool_get(ngx_http_request_t *r, u_char *filename)
{
    ngx_http_lua_ctx_t *ctx;
    ngx_http_lfs_cached_fd_t *cfd;
    ngx_uint_t i;
    if ((ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module)) == NULL) {
        return -1;
    }

    cfd = ctx->lfs_post_closed->elts;
    for (i = 0; i < ctx->lfs_post_closed->nelts; i ++) {
        if (ngx_strcmp(cfd[i].filename, filename) == 0) {
            return cfd[i].fd;
        }
    }
    return -1;
}


static int ngx_http_lua_lfs_fdpool_add(ngx_http_request_t *r, u_char *filename, ngx_fd_t fd)
{
    ngx_http_lfs_cached_fd_t *cfd;
    ngx_http_lua_ctx_t *ctx;

    if ((ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module)) == NULL) {
        return -1;
    }

    if ((cfd = ngx_array_push(ctx->lfs_post_closed)) == NULL) {
        return -1;
    }

    if ((cfd->filename = ngx_palloc(r->pool, ngx_strlen(filename) + 1)) == NULL) {
        return -1;
    }
    ngx_cpystrn(cfd->filename, filename, ngx_strlen(filename) + 1);
    cfd->fd = fd;


    return 0;
}


/**
 * all the operate functions
 **/
typedef void (*task_callback)(void *data, ngx_log_t *log);
typedef void (*event_callback)(ngx_event_t *ev);

typedef struct _ngx_http_lua_lfs_ops_s {
    task_callback task_callback;
    event_callback event_callback;
} ngx_http_lua_lfs_ops_t;

enum {
    TASK_READ = 0,
    TASK_WRITE,
    TASK_COPY,
} TASK_OPS;


/**
 * resume the lua VM, copied from ngx_http_lua_sleep.c
 **/
static ngx_int_t ngx_http_lua_lfs_event_resume(ngx_http_request_t *r, int nrets)
{
    lua_State *vm;
    ngx_int_t rc;
    ngx_http_lua_ctx_t *ctx;
    ngx_connection_t *c;

    r->main->blocked --;
    r->aio = 0;

    if ((ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module)) == NULL) {
        return NGX_ERROR;
    }

    ctx->resume_handler = ngx_http_lua_wev_handler;

    c = r->connection;
    vm = ngx_http_lua_get_lua_vm(r, ctx);

    rc = ngx_http_lua_run_thread(vm, r, ctx, nrets);

    if (rc == NGX_AGAIN) {
        return ngx_http_lua_run_posted_threads(c, vm, r, ctx);
    }

    if (rc == NGX_DONE) {
        ngx_http_lua_finalize_request(r, NGX_DONE);
        return ngx_http_lua_run_posted_threads(c, vm, r, ctx);
    }

    if (ctx->entered_content_phase) {
        ngx_http_lua_finalize_request(r, rc);
        return NGX_DONE;
    }

    return rc;
}


static void ngx_http_lua_lfs_task_read(void *data, ngx_log_t *log)
{
    ngx_http_lua_lfs_task_ctx_t *task_ctx = data;
    ngx_fd_t fd = task_ctx->fd;

    if (task_ctx->offset == -1) {
        task_ctx->length = read(fd, task_ctx->buff, task_ctx->size);
    } else {
        task_ctx->length = pread(fd, task_ctx->buff, task_ctx->size, task_ctx->offset);
    }
}

static void ngx_http_lua_lfs_task_read_event(ngx_event_t *ev)
{
    ngx_int_t nrets = 0;
    ngx_http_lua_lfs_task_ctx_t *task_ctx = ev->data;
    ngx_http_request_t *r = task_ctx->r;
    ngx_connection_t *c = r->connection;
    ngx_http_log_ctx_t *log_ctx;
    ngx_http_lua_ctx_t *ctx;

    if ((ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module)) == NULL) {
        nrets = 2;
        lua_pushnil(task_ctx->L);
        lua_pushstring(task_ctx->L, "no ctx found.");
    } else if (task_ctx->length > 0) {
        nrets = 1;
        lua_pushlstring(task_ctx->L, task_ctx->buff, task_ctx->length);
    } else {
        nrets = 2;
        lua_pushnil(task_ctx->L);
        lua_pushstring(task_ctx->L, "no data");
    }

    task_ctx->coctx->cleanup = NULL;

    if (c->fd != -1) {
        log_ctx = c->log->data;
        log_ctx->current_request = r;
    }
    ctx->cur_co_ctx = task_ctx->coctx;

    ngx_http_lua_lfs_event_resume(r, nrets);
    ngx_http_run_posted_requests(c);

    ngx_pfree(task_ctx->pool, task_ctx->buff);
    ngx_pfree(task_ctx->pool, task_ctx);
}

static void ngx_http_lua_lfs_task_write(void *data, ngx_log_t *log)
{
}

static void ngx_http_lua_lfs_task_write_event(ngx_event_t *ev)
{
}

static void ngx_http_lua_lfs_task_copy(void *data, ngx_log_t *log)
{
}

static void ngx_http_lua_lfs_task_copy_event(ngx_event_t *ev)
{
}

static ngx_http_lua_lfs_ops_t lfs_ops[] = {
    { /** TASK_READ **/
        .task_callback = ngx_http_lua_lfs_task_read,
        .event_callback = ngx_http_lua_lfs_task_read_event,
    },
    { /** TASK_WRITE **/
        .task_callback = ngx_http_lua_lfs_task_write,
        .event_callback = ngx_http_lua_lfs_task_write_event,
    },
    { /** TASK_COPY **/
        .task_callback = ngx_http_lua_lfs_task_copy,
        .event_callback = ngx_http_lua_lfs_task_copy_event,
    }
};

/**
 * create task
 **/
static ngx_thread_task_t *ngx_http_lua_lfs_create_task(ngx_pool_t *pool, int ops)
{
    ngx_thread_task_t *task;

    if ((task = ngx_thread_task_alloc(pool,
                    sizeof(ngx_http_lua_lfs_task_ctx_t))) == NULL) {
        return NULL;
    }

    task->handler = lfs_ops[ops].task_callback;
    task->event.data = task->ctx;
    task->event.handler = lfs_ops[ops].event_callback;

    return task;
}

static int ngx_http_lua_lfs_post_task(ngx_thread_task_t *task)
{
    ngx_thread_pool_t *pool;
    ngx_str_t poolname = ngx_string("luafs");

    if ((pool = ngx_thread_pool_get((ngx_cycle_t*) ngx_cycle, &poolname)) == NULL) {
        return -1;
    }

    if (ngx_thread_task_post(pool, task) != NGX_OK) {
        return -1;
    }

    return 0;
}

/**
 * ngx.lfs.read("/root/1.txt", size, offset)
 **/
static int ngx_http_lua_ngx_lfs_read(lua_State *L)
{
    int n;
    ssize_t size;
    off_t offset;
    ngx_http_request_t *r;
    ngx_http_lua_lfs_task_ctx_t *task_ctx;
    ngx_thread_task_t *task;
    ngx_str_t filename;
    ngx_http_lua_ctx_t *ctx;

    n = lua_gettop(L);
    if (n < 1 && n > 3) {
        return luaL_error(L, "expected 1, 2 or 3 arguments, but seen %d", n);
    }

    if (!lua_isstring(L, 1)) {
        return luaL_error(L, "the first argument is expected string");
    }

    filename.data = (u_char*) lua_tolstring(L, 1, &filename.len);
    if (filename.len <= 0) {
        return luaL_error(L, "bad argument 1");
    }

    if (n == 1) { /** n < 2 **/
        size = 65536;
        offset = -1;
    } else if (n == 2) {
        size = (ssize_t) luaL_checknumber(L, 2);
        offset = -1;
    } else {
        size = (ssize_t) luaL_checknumber(L, 2);
        offset = (off_t) luaL_checknumber(L, 3);
    }

    if (size < 0 || offset < -1) {
        return luaL_error(L, "Invalid argument size(%d) or offset(%d)", size, offset);
    }

    if ((r = ngx_http_lua_get_req(L)) == NULL) {
        return luaL_error(L, "no request found");
    }

    if ((ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module)) == NULL) {
        return luaL_error(L, "no request ctx found");
    }

    if (ngx_http_lua_lfs_fdpool_init(ctx, r) != 0) {
        return luaL_error(L, "create fd pool failed.");
    }

    if ((task = ngx_http_lua_lfs_create_task(r->pool, TASK_READ)) == NULL) {
        return luaL_error(L, "can't create task");
    }
    task_ctx = task->ctx;

    if ((task_ctx->buff = ngx_palloc(r->pool, size)) == NULL) {
        return luaL_error(L, "failed to allocate memory");
    }

    if ((task_ctx->fd = ngx_http_lua_lfs_fdpool_get(r, filename.data)) < 0) {
        if ((task_ctx->fd = ngx_open_file(filename.data, NGX_FILE_RDWR,
                        NGX_FILE_CREATE_OR_OPEN, NGX_FILE_DEFAULT_ACCESS)) < 0) {
            return luaL_error(L, "open file %s error", filename.data);
        }
        //ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        //        "[%s:%d] open fd: %d", __FUNCTION__, __LINE__, task_ctx->fd);
        ngx_http_lua_lfs_fdpool_add(r, filename.data, task_ctx->fd);
    }

    task_ctx->length = 0;
    task_ctx->L = L;
    task_ctx->r = r;
    task_ctx->size = size;
    task_ctx->offset = offset;
    task_ctx->coctx = ctx->cur_co_ctx;
    task_ctx->pool = r->pool;

    r->main->blocked ++;
    r->aio = 1;

    ngx_http_lua_cleanup_pending_operation(task_ctx->coctx);
    task_ctx->coctx->cleanup = NULL;

    if (ngx_http_lua_lfs_post_task(task) != 0) {
        return luaL_error(L, "post task error.");
    }
    return lua_yield(L, 0);
}

static int ngx_http_lua_ngx_lfs_write(lua_State *L)
{
    return 0;
}

static int ngx_http_lua_ngx_lfs_copy(lua_State *L)
{
    return 0;
}

static int ngx_http_lua_ngx_lfs_unlink(lua_State *L)
{
    return 0;
}

static int ngx_http_lua_ngx_lfs_truncate(lua_State *L)
{
    return 0;
}

static int ngx_http_lua_ngx_lfs_status(lua_State *L)
{
    return 0;
}

void ngx_http_lua_inject_lfs_api(lua_State *L)
{
    lua_createtable(L, 0 /* narr */, 6 /* nrec */); /* ngx.lfs. */

    lua_pushcfunction(L, ngx_http_lua_ngx_lfs_read);
    lua_setfield(L, -2, "read");

    lua_pushcfunction(L, ngx_http_lua_ngx_lfs_write);
    lua_setfield(L, -2, "write");

    lua_pushcfunction(L, ngx_http_lua_ngx_lfs_copy);
    lua_setfield(L, -2, "copy");

    lua_pushcfunction(L, ngx_http_lua_ngx_lfs_unlink);
    lua_setfield(L, -2, "unlink");

    lua_pushcfunction(L, ngx_http_lua_ngx_lfs_truncate);
    lua_setfield(L, -2, "truncate");

    lua_pushcfunction(L, ngx_http_lua_ngx_lfs_status);
    lua_setfield(L, -2, "status");

    lua_setfield(L, -2, "lfs");
}

#endif

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
