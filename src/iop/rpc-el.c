/***************************************************************************/
/*                                                                         */
/* Copyright 2022 INTERSEC SA                                              */
/*                                                                         */
/* Licensed under the Apache License, Version 2.0 (the "License");         */
/* you may not use this file except in compliance with the License.        */
/* You may obtain a copy of the License at                                 */
/*                                                                         */
/*     http://www.apache.org/licenses/LICENSE-2.0                          */
/*                                                                         */
/* Unless required by applicable law or agreed to in writing, software     */
/* distributed under the License is distributed on an "AS IS" BASIS,       */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*/
/* See the License for the specific language governing permissions and     */
/* limitations under the License.                                          */
/*                                                                         */
/***************************************************************************/

#include <lib-common/thr.h>
#include <lib-common/iop-rpc.h>
#include <lib-common/datetime.h>

#include "rpc-el.h"

/* {{{ Types */

typedef enum el_thr_status_t {
    EL_THR_NOT_STARTED = 0,
    EL_THR_STARTING = 1,
    EL_THR_STARTED = 2,
    EL_THR_STOPPED = 3,
} el_thr_status_t;

qm_khptr_t(ic_el_server, struct ev_t, ic_el_server_t *);
qh_khptr_t(ic_el_client, ic_el_client_t);

typedef enum ic_el_async_res_t {
    IC_EL_ASYNC_OK,
    IC_EL_ASYNC_ERR,
    IC_EL_ASYNC_PENDING,
} ic_el_async_res_t;

/* }}} */

static struct {
    el_thr_status_t el_thr_status : 2;
    bool el_wait_thr_sigint_received : 1;

    pthread_t main_thread;
    pthread_t el_thread;
    pthread_cond_t el_thr_start_cond;

    /* Don't use these variables directly unless you know what you are doing.
     * Use ic_el_mutex_lock(), ic_el_mutex_unlock(),
     * ic_el_inc_el_mutex_wait_lock_cnt() and
     * ic_el_dec_el_mutex_wait_lock_cnt() instead.
     */
    pthread_mutex_t el_mutex;
    el_t el_mutex_before_lock_el;
    pthread_cond_t el_mutex_after_lock_cond;
    atomic_int el_mutex_wait_lock_cnt;
    int el_thr_signo;

    pthread_cond_t el_wait_thr_cond;
    int el_wait_thr_sigint_cnt;
    struct sigaction el_wait_thr_py_sigint;
    el_t el_wait_thr_sigint_el;

    qm_t(ic_el_server) servers;
    qh_t(ic_el_client) clients;

    dlist_t disconnecting_clients;
    dlist_t destroyed_clients;
} ic_el_g;
#define _G ic_el_g

/* {{{ Helpers */

static bool is_el_thr_stopped(void)
{
    return _G.el_thr_status == EL_THR_STOPPED;
}

static int load_su_from_uri(lstr_t uri, sockunion_t *su, sb_t *err)
{
    pstream_t host;
    in_port_t port;

    if (addr_parse(ps_initlstr(&uri), &host, &port, -1) < 0) {
        sb_setf(err, "invalid uri: %*pM", LSTR_FMT_ARG(uri));
        return -1;
    }
    if (addr_info(su, AF_UNSPEC, host, port) < 0) {
        sb_setf(err, "unable to resolve uri: %*pM", LSTR_FMT_ARG(uri));
        return -1;
    }
    return 0;
}

/* }}} */
/* {{{ El thread */

/** Callback called on signal. */
static void el_loop_thread_on_term(el_t ev, int signo, el_data_t priv)
{
    _G.el_thr_signo = signo;
    _G.el_thr_status = EL_THR_STOPPED;
}

/** Callback called when mutex is requested to be locked. */
static void request_el_mutex_lock_cb(el_t el, el_data_t data)
{
}

/** Wait for _G.el_mutex_after_lock_cond in the el thread loop. */
static void el_loop_wait_after_lock_cond(void)
{
    struct timeval abs_time;
    struct timespec ts;

    lp_gettv(&abs_time);
    abs_time = timeval_addmsec(abs_time, 10);

    ts.tv_sec = abs_time.tv_sec;
    ts.tv_nsec = 1000 * abs_time.tv_usec;
    pthread_cond_timedwait(&_G.el_mutex_after_lock_cond, &_G.el_mutex,
                           &ts);
}

static void ic_el_server_el_process(void);
static void ic_el_client_el_process(void);

/** El thread loop. */
static void *el_loop_thread_fun(void *arg)
{
    static struct sigaction sa_old_term;
    static struct sigaction sa_old_quit;
    static bool sig_handlers_saved;
    el_t el_sigterm;
    el_t el_sigquit;

    pthread_mutex_lock(&_G.el_mutex);

    /* XXX: sa_old_* only written once, no rewritten at fork,
     * sa_handler at NULL cannot be used to know if the action has been
     * rewritten because it stands for default action for the signal (SIG_DFL)
     */
    if (!sig_handlers_saved) {
        sigaction(SIGTERM, NULL, &sa_old_term);
        sigaction(SIGQUIT, NULL, &sa_old_quit);
        sig_handlers_saved = true;
    }

    el_sigterm = el_signal_register(SIGTERM, &el_loop_thread_on_term, NULL);
    el_sigquit = el_signal_register(SIGQUIT, &el_loop_thread_on_term, NULL);

    _G.el_thr_status = EL_THR_STARTED;
    pthread_cond_broadcast(&_G.el_thr_start_cond);

    assert (atomic_load(&_G.el_mutex_wait_lock_cnt) > 0);
    el_loop_wait_after_lock_cond();

    for (;;) {
        int wait_lock_cnt = atomic_load(&_G.el_mutex_wait_lock_cnt);

        /* If we request a mutex lock, we need to process the events without
         * waiting. */
        assert (wait_lock_cnt >= 0);
        el_loop_timeout(wait_lock_cnt > 0 ? 0 : 100);

        ic_el_server_el_process();
        ic_el_client_el_process();

        if (_G.el_thr_signo) {
            break;
        }

        if (el_has_pending_events()
        ||  (wait_lock_cnt = atomic_load(&_G.el_mutex_wait_lock_cnt)) == 0)
        {
            pthread_mutex_unlock(&_G.el_mutex);
            sched_yield();
            pthread_mutex_lock(&_G.el_mutex);
        } else {
            assert (wait_lock_cnt > 0);
            el_loop_wait_after_lock_cond();
        }
    }

    el_unregister(&el_sigterm);
    el_unregister(&el_sigquit);

    sigaction(SIGTERM, &sa_old_term, NULL);
    sigaction(SIGQUIT, &sa_old_quit, NULL);

    if (_G.el_thr_signo != SIGINT) {
        pthread_kill(_G.main_thread, _G.el_thr_signo);
        pthread_detach(pthread_self());
    }

    pthread_mutex_unlock(&_G.el_mutex);

    return NULL;
}

/** Increment wait lock counter. */
static int ic_el_inc_el_mutex_wait_lock_cnt(void)
{
    int old_wait_lock_cnt;

    old_wait_lock_cnt = atomic_fetch_add(&_G.el_mutex_wait_lock_cnt, 1);
    assert (old_wait_lock_cnt >= 0);
    return old_wait_lock_cnt;
}

/** Decrement wait lock counter.
 *
 * It will signal _G.el_mutex_after_lock_cond if needed.
 */
static void ic_el_dec_el_mutex_wait_lock_cnt(void)
{
    int old_wait_lock_cnt;

    old_wait_lock_cnt = atomic_fetch_sub(&_G.el_mutex_wait_lock_cnt, 1);
    assert (old_wait_lock_cnt >= 1);
    if (old_wait_lock_cnt == 1) {
        pthread_cond_signal(&_G.el_mutex_after_lock_cond);
    }
}

/** Request el mutex lock from el loop.
 *
 * It will start the el thread loop if needed.
 * el_mutex will be locked after calling this function.
 */
static void ic_el_mutex_lock(bool start_thr)
{
    int old_wait_lock_cnt;

    old_wait_lock_cnt = ic_el_inc_el_mutex_wait_lock_cnt();
    if (old_wait_lock_cnt == 0) {
        el_wake_fire(_G.el_mutex_before_lock_el);
    }

    pthread_mutex_lock(&_G.el_mutex);

    if (!start_thr
    ||  likely(_G.el_thr_status == EL_THR_STARTED)
    ||  unlikely(is_el_thr_stopped()))
    {
        ic_el_dec_el_mutex_wait_lock_cnt();
        return;
    }

    if (_G.el_thr_status == EL_THR_NOT_STARTED) {
        _G.el_thr_status = EL_THR_STARTING;

        if (thr_create(&_G.el_thread, NULL, &el_loop_thread_fun, NULL) < 0) {
            e_fatal("cannot launch el_loop thread");
        }
    }

    do {
        pthread_cond_wait(&_G.el_thr_start_cond, &_G.el_mutex);
    } while (_G.el_thr_status != EL_THR_STARTED);

    ic_el_dec_el_mutex_wait_lock_cnt();
}

/** Release el mutex lock.
 *
 * el_mutex will be unlocked after calling this function.
 */
static void ic_el_mutex_unlock(void)
{
    pthread_mutex_unlock(&_G.el_mutex);
}

static void ic_el_wait_thr_cond_sigint(el_t ev, int signo, el_data_t priv)
{
    _G.el_wait_thr_sigint_received = true;
    pthread_cond_broadcast(&_G.el_wait_thr_cond);
    ic_el_inc_el_mutex_wait_lock_cnt();
}

static void ic_el_wait_thr_timeout_cb(el_t ev, el_data_t priv)
{
    bool *timeout_expired_ptr = priv.ptr;

    *timeout_expired_ptr = true;
    pthread_cond_broadcast(&_G.el_wait_thr_cond);
    ic_el_inc_el_mutex_wait_lock_cnt();
}

/** Result of wait_thread_cond(). */
typedef enum wait_thread_cond_res_t {
    WAIT_THR_COND_OK      =  0,
    WAIT_THR_COND_TIMEOUT = -1,
    WAIT_THR_COND_SIGINT  = -2,
} wait_thread_cond_res_t;

/** Wait thread for specified condition.
 *
 * XXX: _G.el_mutex must be locked before calling this function.
 *      ic_el_dec_el_mutex_wait_lock_cnt() will be called if the condition has
 *      been fulfilled.
 *      So, you *MUST* call ic_el_inc_el_mutex_wait_lock_cnt() when signaling
 *      the condition.
 *      You also *MUST* use pthread_cond_broadcast() on _G.el_wait_thr_cond
 *      since multiple threads can wait to _G.el_wait_thr_cond
 *
 *      Example:
 *          ctx->is_terminated = true;
 *          pthread_cond_broadcast(&_G.el_wait_thr_cond);
 *          ic_el_inc_el_mutex_wait_lock_cnt();
 *
 * \param[in] is_terminated  A callback called to check if the condition has
 *                           been fulfilled.
 * \param[in] terminated_arg The argument passed to the callback.
 * \param[in] timeout        The timeout in seconds.
 *                           <= 0 for unlimited timeout.
 * \return The result as wait_thread_cond_res_t.
 */
static wait_thread_cond_res_t
wait_thread_cond(bool (*is_terminated)(void *), void *terminated_arg,
                 double timeout)
{
    struct timeval begin_time;
    int64_t timeout_msec = 0;
    bool timeout_expired = false;
    el_t timeout_el = NULL;
    bool terminated;
    wait_thread_cond_res_t res = WAIT_THR_COND_TIMEOUT;

    if (timeout > 0.0) {
        lp_gettv(&begin_time);
        timeout_msec = (int64_t)(timeout * 1000.0);
        timeout_el = el_timer_register(timeout_msec, 0, EL_TIMER_LOWRES,
                                       &ic_el_wait_thr_timeout_cb,
                                       &timeout_expired);
    }

    /* Save old sigint handler if we don't saved it already. */
    assert (_G.el_wait_thr_sigint_cnt >= 0);
    if (_G.el_wait_thr_sigint_cnt++ == 0) {
        sigaction(SIGINT, NULL, &_G.el_wait_thr_py_sigint);
        _G.el_wait_thr_sigint_el =
            el_signal_register(SIGINT, &ic_el_wait_thr_cond_sigint, NULL);
        _G.el_wait_thr_sigint_received = false;
    }

    while (!(terminated = is_terminated(terminated_arg)) &&
           !timeout_expired && !_G.el_wait_thr_sigint_received &&
           !is_el_thr_stopped() &&
           !pthread_cond_wait(&_G.el_wait_thr_cond, &_G.el_mutex))
    {
    }

    if (timeout_expired) {
        struct timeval after_time;
        int64_t diff_msec;

        lp_gettv(&after_time);
        diff_msec = timeval_diffmsec(&after_time, &begin_time);
        if (unlikely((timeout_msec + 500) < diff_msec)) {
            e_warning("thread starvation detected, expected timeout in "
                      "%jdms, took %jdms", timeout_msec, diff_msec);
        }
    } else {
        el_unregister(&timeout_el);
    }

    if (terminated) {
        ic_el_dec_el_mutex_wait_lock_cnt();
        res = WAIT_THR_COND_OK;
    }

    if (_G.el_wait_thr_sigint_received || is_el_thr_stopped()) {
        res = WAIT_THR_COND_SIGINT;
    }

    /* Restore old sigint handler if we are the last one to wait. */
    assert (_G.el_wait_thr_sigint_cnt > 0);
    if (--_G.el_wait_thr_sigint_cnt == 0) {
        el_unregister(&_G.el_wait_thr_sigint_el);
        sigaction(SIGINT, &_G.el_wait_thr_py_sigint, NULL);
    }

    return res;
}

/* }}} */
/* {{{ Server */

struct ic_el_server_t {
    int refcnt;
    const iop_env_t *iop_env;
    ic_el_server_cb_cfg_t cb_cfg;
    void *ext_obj;
    el_t el_ic;
    lstr_t uri;
    qm_t(ic_cbs) impl;
    dlist_t peers;
    bool is_stopping : 1;
    bool wait_for_stop : 1;
};

typedef struct ic_el_peer_t {
    ichannel_t *ic;
    dlist_t node;
} ic_el_peer_t;

static void ic_el_peer_wipe(ic_el_peer_t *peer)
{
    if (peer->ic) {
        peer->ic->priv = NULL;
        peer->ic->peer = NULL;
        ic_delete(&peer->ic);
    }
    dlist_remove(&peer->node);
}

GENERIC_NEW_INIT(ic_el_peer_t, ic_el_peer);
GENERIC_DELETE(ic_el_peer_t, ic_el_peer);

static ic_el_server_t *ic_el_server_init(ic_el_server_t *server)
{
    p_clear(server, 1);
    qm_init(ic_cbs, &server->impl);
    dlist_init(&server->peers);
    return server;
}

static void ic_el_server_wipe(ic_el_server_t *server)
{
    lstr_wipe(&server->uri);
    qm_wipe(ic_cbs, &server->impl);
}

DO_REFCNT(ic_el_server_t, ic_el_server);

static void ic_el_server_clear(ic_el_server_t *server,
                               bool use_wait_for_stop)
{
    if (!server->el_ic) {
        return;
    }

    el_unregister(&server->el_ic);

    dlist_for_each_entry(ic_el_peer_t, peer, &server->peers, node) {
        ic_el_peer_delete(&peer);
    }

    if (use_wait_for_stop && server->wait_for_stop) {
        pthread_cond_broadcast(&_G.el_wait_thr_cond);
        ic_el_inc_el_mutex_wait_lock_cnt();
    }

    lstr_wipe(&server->uri);
}

/** Process stopped ic servers in the event loop. */
static void ic_el_server_el_process(void)
{
    qm_for_each_pos(ic_el_server, pos, &_G.servers) {
        ic_el_server_t *server = _G.servers.values[pos];

        if (!server->is_stopping) {
            continue;
        }
        ic_el_server_clear(server, true);
        server->is_stopping = false;
        ic_el_server_delete(&server);
        qm_del_at(ic_el_server, &_G.servers, pos);
    }
}

ic_el_server_t * nonnull
ic_el_server_create(const iop_env_t * nonnull iop_env,
                    const ic_el_server_cb_cfg_t * nonnull cb_cfg)
{
    ic_el_server_t *server = ic_el_server_new();

    server->iop_env = iop_env;
    server->cb_cfg = *cb_cfg;
    return server;
}

void ic_el_server_destroy(ic_el_server_t **server_ptr)
{
    ic_el_server_t *server = *server_ptr;

    if (!server) {
        return;
    }

    ic_el_mutex_lock(true);
    if (server->el_ic) {
        server->is_stopping = true;
    }
    ic_el_server_delete(server_ptr);
    ic_el_mutex_unlock();
}

void ic_el_server_set_ext_obj(ic_el_server_t *server,
                              void * nullable ext_obj)
{
    server->ext_obj = ext_obj;
}

void * nullable ic_el_server_get_ext_obj(ic_el_server_t *server)
{
    return server->ext_obj;
}

static void ic_el_server_on_event(ichannel_t *ic, ic_event_t evt)
{
    ic_el_server_t *server = ic->priv;

    if (unlikely(!server)) {
        return;
    }

    switch (evt) {
      case IC_EVT_CONNECTED:
        if (server->cb_cfg.on_connect && !is_el_thr_stopped()) {
            t_scope;
            lstr_t server_uri = t_lstr_dup(server->uri);
            lstr_t client_addr = t_lstr_dup(ic_get_client_addr(ic));
            ic_el_server_t *server_dup = ic_el_server_retain(server);

            ic_el_mutex_unlock();
            (*server_dup->cb_cfg.on_connect)(server_dup, server_uri,
                                             client_addr);
            ic_el_mutex_lock(false);
            ic_el_server_delete(&server_dup);
        }
        break;

      case IC_EVT_DISCONNECTED:
        if (server->cb_cfg.on_disconnect && !is_el_thr_stopped()) {
            t_scope;
            lstr_t server_uri = t_lstr_dup(server->uri);
            lstr_t client_addr = t_lstr_dup(ic_get_client_addr(ic));
            ic_el_server_t *server_dup = ic_el_server_retain(server);

            ic_el_mutex_unlock();
            (*server_dup->cb_cfg.on_disconnect)(server_dup, server_uri,
                                                client_addr);
            ic_el_mutex_lock(false);
            ic_el_server_delete(&server_dup);
        }
        if (ic->peer) {
            ((ic_el_peer_t *)ic->peer)->ic = NULL;
            ic_el_peer_delete((ic_el_peer_t **)&ic->peer);
        }
        break;

      default:
        break;
    }
}

static int ic_el_server_on_accept(el_t ev, int fd)
{
    ic_el_server_t *server;
    ic_el_peer_t *peer;

    server = RETHROW_PN(qm_get_def(ic_el_server, &_G.servers, ev, NULL));
    peer = ic_el_peer_new();
    dlist_add_tail(&server->peers, &peer->node);

    peer->ic = ic_new();
    peer->ic->iop_env = server->iop_env;
    peer->ic->on_event = &ic_el_server_on_event;
    peer->ic->impl = &server->impl;
    peer->ic->priv = server;
    peer->ic->peer = peer;
    ic_spawn(peer->ic, fd, NULL);

    return 0;
}

static void ic_el_server_rpc_cb(ichannel_t *ic, uint64_t slot, void *arg,
                                const ic__hdr__t *hdr)
{
    t_scope;
    ic_el_server_t *server = ic->priv;
    ic_status_t status;
    void *res = NULL;
    const iop_struct_t *res_st = NULL;

    if (!unlikely(server)) {
        if (!ic_slot_is_async(slot)) {
            ic_reply_err(ic, slot, IC_MSG_UNIMPLEMENTED);
        }
        return;
    }

    ic_el_server_retain(server);
    ic_el_mutex_unlock();
    status = (*server->cb_cfg.t_on_rpc)(server, ic, slot, arg, hdr, &res,
                                        &res_st);
    ic_el_mutex_lock(false);
    ic_el_server_delete(&server);

    switch (status) {
      case IC_MSG_OK:
      case IC_MSG_EXN:
        if (!ic_slot_is_async(slot)) {
            __ic_reply(ic, slot, status, -1, res_st, res);
        }
        break;

      default:
        if (!ic_slot_is_async(slot)) {
            ic_reply_err(ic, slot, status);
        }
        break;
    }
}

/** Make the IC EL server listening with the mutex locked. */
static int ic_el_server_listen_internal(ic_el_server_t *server,
                                        lstr_t uri, const sockunion_t *su,
                                        sb_t *err)
{
    if (server->el_ic) {
        sb_setf(err, "channel server is already listening on %*pM",
                LSTR_FMT_ARG(server->uri));
        return -1;
    }

    server->el_ic = ic_listento(su, SOCK_STREAM, IPPROTO_TCP,
                                ic_el_server_on_accept);
    if (!server->el_ic) {
        sb_setf(err, "cannot bind channel server on %*pM",
                LSTR_FMT_ARG(server->uri));
        return -1;
    }

    qm_add(ic_el_server, &_G.servers, server->el_ic,
           ic_el_server_retain(server));
    lstr_copy(&server->uri, uri);
    return 0;
}

int ic_el_server_listen(ic_el_server_t *server, lstr_t uri, sb_t *err)
{
    int res;
    sockunion_t su;

    RETHROW(load_su_from_uri(uri, &su, err));

    ic_el_mutex_lock(true);
    res = ic_el_server_listen_internal(server, uri, &su, err);
    ic_el_mutex_unlock();
    return res;
}

/** Callback used by wait_thread_cond() to check if the server has been
 * stopped. */
static bool ic_el_server_is_stopped(void *arg)
{
    ic_el_server_t *server = arg;

    return !server->el_ic;
}

/** Wait for the IC EL server to be fully stopped by the event loop. */
static ic_el_sync_res_t
ic_el_server_wait_for_stop(ic_el_server_t *server, double timeout)
{
    ic_el_sync_res_t res = IC_EL_SYNC_OK;
    wait_thread_cond_res_t wait_res;

    server->wait_for_stop = true;

    wait_res = wait_thread_cond(&ic_el_server_is_stopped, server, timeout);
    switch (wait_res) {
      case WAIT_THR_COND_OK:
      case WAIT_THR_COND_TIMEOUT:
        break;

      case WAIT_THR_COND_SIGINT:
        res = IC_EL_SYNC_SIGINT;
        break;
    }

    server->wait_for_stop = false;
    return res;
}

ic_el_sync_res_t
ic_el_server_listen_block(ic_el_server_t *server, lstr_t uri, double timeout,
                          sb_t *err)
{
    ic_el_sync_res_t res;
    sockunion_t su;

    RETHROW(load_su_from_uri(uri, &su, err));

    ic_el_mutex_lock(true);

    if (ic_el_server_listen_internal(server, uri, &su, err) < 0) {
        res = IC_EL_SYNC_ERR;
        goto end;
    }

    res = ic_el_server_wait_for_stop(server, timeout);

    if (server->el_ic) {
        ic_el_server_clear(server, false);
    }

  end:
    ic_el_mutex_unlock();
    return res;
}

ic_el_sync_res_t ic_el_server_stop(ic_el_server_t *server)
{
    ic_el_sync_res_t res = IC_EL_SYNC_OK;

    ic_el_mutex_lock(true);
    if (server->el_ic) {
        server->is_stopping = true;

        if (!server->wait_for_stop) {
            res = ic_el_server_wait_for_stop(server, -1);
        }
    }
    ic_el_mutex_unlock();

    return res;
}

void ic_el_server_register_rpc(ic_el_server_t *server,
                               const iop_rpc_t *rpc, uint32_t cmd)
{
    ic_el_mutex_lock(true);

    qm_put(ic_cbs, &server->impl, cmd, ((ic_cb_entry_t){
               .cb_type = IC_CB_NORMAL,
               .rpc     = rpc,
               .u       = { .cb = { .cb = &ic_el_server_rpc_cb, } },
           }), QHASH_OVERWRITE);

    ic_el_mutex_unlock();
}

void ic_el_server_unregister_rpc(ic_el_server_t *server, uint32_t cmd)
{
    ic_el_mutex_lock(true);

    qm_del_key(ic_cbs, &server->impl, cmd);

    ic_el_mutex_unlock();
}

bool ic_el_server_is_listening(const ic_el_server_t *server)
{
    bool res;

    ic_el_mutex_lock(true);
    res = !!server->el_ic;
    ic_el_mutex_unlock();

    return res;
}

/* }}} */
/* {{{ Client */

/** Context to be used on client asynchronous connection. */
typedef struct ic_el_client_async_connect_t {
    ic_el_client_t *client;
    ic_client_async_connect_f cb;
    void *nullable cb_arg;
    el_t timeout_el;
    dlist_t link;
    bool is_locked_cb : 1;
} ic_el_client_async_connect_t;

static ic_el_client_async_connect_t *
ic_el_client_async_connect_init(ic_el_client_async_connect_t *ctx)
{
    p_clear(ctx, 1);
    dlist_init(&ctx->link);
    return ctx;
}

static void ic_el_client_async_connect_wipe(ic_el_client_async_connect_t *ctx)
{
    dlist_remove(&ctx->link);
    el_unregister(&ctx->timeout_el);
}

GENERIC_NEW(ic_el_client_async_connect_t, ic_el_client_async_connect);
GENERIC_DELETE(ic_el_client_async_connect_t, ic_el_client_async_connect);

struct ic_el_client_t {
    ic_el_client_cb_cfg_t cb_cfg;
    ichannel_t ic;
    void *ext_obj;
    dlist_t disconnecting;
    dlist_t destroyed;
    int wait_connect_sync;
    dlist_t async_connect_ctxs;
    bool in_connect       : 1;
    bool force_disconnect : 1;
    bool connected        : 1;
};

static ic_el_client_t *ic_el_client_init(ic_el_client_t *client)
{
    p_clear(client, 1);
    ic_init(&client->ic);
    dlist_init(&client->disconnecting);
    dlist_init(&client->destroyed);
    dlist_init(&client->async_connect_ctxs);
    return client;
}

static void ic_el_client_clear(ic_el_client_t *client)
{
    client->force_disconnect = true;
    ic_wipe(&client->ic);
    dlist_for_each_entry(ic_el_client_async_connect_t, async_ctx,
                         &client->async_connect_ctxs, link)
    {
        ic_el_client_async_connect_delete(&async_ctx);
    }

    dlist_remove(&client->disconnecting);
    dlist_remove(&client->destroyed);
}

static void ic_el_client_wipe(ic_el_client_t *client)
{
    ic_el_client_clear(client);
    qh_del_key(ic_el_client, &_G.clients, client);
}

GENERIC_NEW(ic_el_client_t, ic_el_client);
GENERIC_DELETE(ic_el_client_t, ic_el_client);

/** Set the error description on client connection error. */
static void ic_el_client_set_connection_error(const ic_el_client_t *client,
                                              sb_t *err)
{
    t_scope;
    lstr_t uri = t_addr_fmt_lstr(&client->ic.su);

    sb_setf(err, "unable to connect to %*pM", LSTR_FMT_ARG(uri));
}

/** Process destroyed ic clients in the event loop. */
static void ic_el_client_el_process(void)
{
    dlist_t disconnecting_clients;

    /* Splice the disconnecting clients here to avoid race conditions when
     * calling the callbacks with the mutex unlocked */
    dlist_init(&disconnecting_clients);
    dlist_splice(&disconnecting_clients, &_G.disconnecting_clients);

    dlist_for_each_entry(ic_el_client_t, client, &disconnecting_clients,
                         disconnecting)
    {
        dlist_remove(&client->disconnecting);
        ic_disconnect(&client->ic);
    }

    dlist_for_each_entry(ic_el_client_t, client, &_G.destroyed_clients,
                         destroyed)
    {
        ic_el_client_delete(&client);
    }
    dlist_init(&_G.destroyed_clients);
}

/** Notify client connection or disconnection for async connections. */
static void ic_el_client_notify_async_connections(ic_el_client_t *client)
{
    SB_1k(connect_err);
    dlist_t async_ctxs;
    sb_t *err_ptr = NULL;

    /* Splice the client asynchronous contexts here to avoid race
     * conditions when calling the callbacks with the mutex unlocked */
    dlist_init(&async_ctxs);
    dlist_splice(&async_ctxs, &client->async_connect_ctxs);

    /* If empty, do nothing */
    if (dlist_is_empty(&async_ctxs)) {
        return;
    }

    /* If the client is not connected, the connection is in error */
    if (!client->connected) {
        ic_el_client_set_connection_error(client, &connect_err);
        err_ptr = &connect_err;
    }

    /* Clear the aynchronous contexts first to avoid race conditions when
     * calling the callbacks with the mutex unlocked */
    dlist_for_each_entry(ic_el_client_async_connect_t, async_ctx,
                         &async_ctxs, link)
    {
        el_unregister(&async_ctx->timeout_el);
        assert(async_ctx->client == client);
        async_ctx->client = NULL;
    }

    /* First, delete the contexts and call all the callbacks that require the
     * lock */
    dlist_for_each_entry(ic_el_client_async_connect_t, async_ctx,
                         &async_ctxs, link)
    {
        if (async_ctx->is_locked_cb) {
            ic_client_async_connect_f cb = async_ctx->cb;
            void *cb_arg = async_ctx->cb_arg;

            ic_el_client_async_connect_delete(&async_ctx);
            (*cb)(err_ptr, cb_arg);
        }
    }

    /* If there are no more contexts, no need to unlock/lock the mutex */
    if (dlist_is_empty(&async_ctxs)) {
        return;
    }

    /* Then, delete the contexts and call all the callbacks with the mutex
     * unlocked */
    ic_el_mutex_unlock();
    dlist_for_each_entry(ic_el_client_async_connect_t, async_ctx,
                         &async_ctxs, link)
    {
        ic_client_async_connect_f cb = async_ctx->cb;
        void *cb_arg = async_ctx->cb_arg;

        assert(!async_ctx->is_locked_cb);
        ic_el_client_async_connect_delete(&async_ctx);
        (*cb)(err_ptr, cb_arg);
    }
    ic_el_mutex_lock(false);
}

/** Notify client connection or disconnection when in_connect. */
static void ic_el_client_notify_in_connect(ic_el_client_t *client)
{
    /* Notify synchronous waiting connections */
    if (client->wait_connect_sync > 0) {
        pthread_cond_broadcast(&_G.el_wait_thr_cond);
        ic_el_inc_el_mutex_wait_lock_cnt();
    }

    /* Notify asynchronous connections */
    ic_el_client_notify_async_connections(client);
}

/** Called on event on the ic channel. */
static void ic_el_client_on_event(ichannel_t *ic, ic_event_t evt)
{
    ic_el_client_t *client = container_of(ic, ic_el_client_t, ic);

    switch (evt) {
    case IC_EVT_CONNECTED: {
        client->connected = true;
        if (client->cb_cfg.on_connect) {
            ic_el_mutex_unlock();
            (*client->cb_cfg.on_connect)(client);
            ic_el_mutex_lock(false);
        }
    } break;

    case IC_EVT_DISCONNECTED: {
        bool connected = client->connected;

        client->connected = false;
        if (client->cb_cfg.on_disconnect && !client->force_disconnect &&
            !is_el_thr_stopped())
        {
            ic_el_mutex_unlock();
            (*client->cb_cfg.on_disconnect)(client, connected);
            ic_el_mutex_lock(false);
        }
    } break;

    case IC_EVT_ACT:
    case IC_EVT_NOACT:
        return;
    }

    if (client->in_connect) {
        if (!client->force_disconnect) {
            ic_el_client_notify_in_connect(client);
        }
        client->in_connect = false;
    }

    client->force_disconnect = false;
}

ic_el_client_t * nullable
ic_el_client_create(const iop_env_t * nonnull iop_env,
                    lstr_t uri, double no_act_timeout,
                    const ic_el_client_cb_cfg_t * nonnull cb_cfg,
                    sb_t *err)
{
    ic_el_client_t *client;
    sockunion_t su;
    const int wa_soft = 10 * 1000;

    RETHROW_NP(load_su_from_uri(uri, &su, err));

    ic_el_mutex_lock(true);

    client = ic_el_client_new();
    client->ic.su = su;
    client->ic.auto_reconn = false;
    client->ic.tls_required = false;
    client->ic.iop_env = iop_env;
    client->ic.on_event = &ic_el_client_on_event;
    client->ic.impl = &ic_no_impl;
    client->cb_cfg = *cb_cfg;

    if (no_act_timeout > 0.0) {
        int wa = (int)(no_act_timeout * 1000.0);

        ic_watch_activity(&client->ic, wa_soft, wa);
    } else {
        ic_watch_activity(&client->ic, wa_soft, 0);
    }

    qh_add(ic_el_client, &_G.clients, client);

    ic_el_mutex_unlock();

    return client;
}

void ic_el_client_destroy(ic_el_client_t **client_ptr)
{
    ic_el_client_t *client = *client_ptr;

    if (!client) {
        return;
    }

    ic_el_mutex_lock(true);
    dlist_add(&_G.destroyed_clients, &client->destroyed);
    ic_el_mutex_unlock();
    *client_ptr = NULL;
}

void ic_el_client_set_ext_obj(ic_el_client_t *client,
                              void * nullable ext_obj)
{
    client->ext_obj = ext_obj;
}

void * nullable ic_el_client_get_ext_obj(ic_el_client_t *client)
{
    return client->ext_obj;
}

/** Callback used by wait_thread_cond() to check if the client has been
 *  connected. */
static bool ic_el_client_connect_is_terminated(void *arg)
{
    ic_el_client_t *client = arg;

    return !client->in_connect;
}

/** Start client connection if not already in connection attempt. */
static int ic_el_client_start_connection(ic_el_client_t *client)
{
    if (client->in_connect) {
        return 0;
    }

    client->in_connect = true;
    assert(!client->force_disconnect);

    return ic_connect(&client->ic);
}

/** Synchronously connect the client and wait for connection.
 *
 * This function will return WAIT_THR_COND_TIMEOUT if the client cannot be
 * connected.
 */
static wait_thread_cond_res_t
ic_el_client_ic_sync_connect_wait(ic_el_client_t *client, int timeout)
{
    wait_thread_cond_res_t wait_res;

    if (ic_el_client_start_connection(client) < 0) {
        return WAIT_THR_COND_TIMEOUT;
    }

    assert(client->wait_connect_sync >= 0);
    client->wait_connect_sync++;

    wait_res = wait_thread_cond(&ic_el_client_connect_is_terminated,
                                client, timeout);

    assert(client->wait_connect_sync > 0);
    client->wait_connect_sync--;

    if (wait_res == WAIT_THR_COND_OK && !client->connected) {
        wait_res = WAIT_THR_COND_TIMEOUT;
    }

    return wait_res;
}

/** Synchronously connect the client with the el mutex locked. */
static ic_el_sync_res_t
ic_el_client_sync_connect_locked(ic_el_client_t *client, double timeout,
                                 sb_t *err)
{
    wait_thread_cond_res_t wait_res;

    if (client->connected) {
        return IC_EL_SYNC_OK;
    }

    wait_res = ic_el_client_ic_sync_connect_wait(client, timeout);
    switch (wait_res) {
    case WAIT_THR_COND_OK:
        return IC_EL_SYNC_OK;

    case WAIT_THR_COND_TIMEOUT:
        ic_el_client_set_connection_error(client, err);
        return IC_EL_SYNC_ERR;

    case WAIT_THR_COND_SIGINT:
        return IC_EL_SYNC_SIGINT;
    }

    assert (false);
    sb_setf(err, "unexpected connect status %d", wait_res);
    return IC_EL_SYNC_ERR;
}

ic_el_sync_res_t
ic_el_client_sync_connect(ic_el_client_t *client, double timeout, sb_t *err)
{
    ic_el_sync_res_t res;

    ic_el_mutex_lock(true);
    res = ic_el_client_sync_connect_locked(client, timeout, err);
    ic_el_mutex_unlock();
    return res;
}

/** Callback on asynchronous client connection timeout. */
static void ic_el_client_async_connect_timeout_cb(el_t ev, el_data_t priv)
{
    SB_1k(err);
    ic_el_client_async_connect_t *async_ctx = priv.ptr;
    ic_client_async_connect_f cb = async_ctx->cb;
    void *cb_arg = async_ctx->cb_arg;
    bool is_locked_cb = async_ctx->is_locked_cb;

    ic_el_client_set_connection_error(async_ctx->client, &err);

    /* Delete it here, before the mutex unlock, to avoid race conditions with
     * the client when calling the callback. */
    ic_el_client_async_connect_delete(&async_ctx);

    if (is_locked_cb) {
        (*cb)(&err, cb_arg);
    } else {
        ic_el_mutex_unlock();
        (*cb)(&err, cb_arg);
        ic_el_mutex_lock(false);
    }
}

/** Asynchronously connect the client with the el mutex locked. */
static ic_el_async_res_t ic_el_client_async_connect_locked(
    ic_el_client_t *client, double timeout, ic_client_async_connect_f cb,
    void *nullable cb_arg, bool is_locked_cb,
    ic_el_client_async_connect_t *nullable *nullable async_ctx_ptr, sb_t *err)
{
    ic_el_client_async_connect_t *async_ctx;

    if (client->connected) {
        return IC_EL_ASYNC_OK;
    }

    if (ic_el_client_start_connection(client) < 0) {
        ic_el_client_set_connection_error(client, err);
        return IC_EL_ASYNC_ERR;
    }

    async_ctx = ic_el_client_async_connect_new();
    async_ctx->client = client;
    async_ctx->cb = cb;
    async_ctx->cb_arg = cb_arg;
    async_ctx->is_locked_cb = is_locked_cb;
    dlist_add_tail(&client->async_connect_ctxs, &async_ctx->link);

    if (timeout > 0.0) {
        int64_t timeout_msec = (int64_t)(timeout * 1000.0);

        async_ctx->timeout_el = el_timer_register(
            timeout_msec, 0, EL_TIMER_LOWRES,
            &ic_el_client_async_connect_timeout_cb, async_ctx);
    }

    if (async_ctx_ptr) {
        *async_ctx_ptr = async_ctx;
    }

    return IC_EL_ASYNC_PENDING;
}

void ic_el_client_async_connect(ic_el_client_t *client, double timeout,
                                ic_client_async_connect_f cb,
                                void *nullable cb_arg)
{
    SB_1k(err);
    ic_el_async_res_t res;

    ic_el_mutex_lock(true);
    res = ic_el_client_async_connect_locked(client, timeout, cb, cb_arg,
                                            false, NULL, &err);
    ic_el_mutex_unlock();

    switch (res) {
    case IC_EL_ASYNC_OK:
        (*cb)(NULL, cb_arg);
        break;

    case IC_EL_ASYNC_ERR:
        (*cb)(&err, cb_arg);
        break;

    case IC_EL_ASYNC_PENDING:
        break;
    }
}

void ic_el_client_disconnect(ic_el_client_t *client)
{
    ic_el_mutex_lock(true);
    if (unlikely(client->in_connect)) {
        /* If the client is connecting, we cannot call ic_disconnect()
         * directly. We need to delay it for the next event loop. */
        if (dlist_is_empty(&client->disconnecting)) {
            dlist_add_tail(&_G.disconnecting_clients, &client->disconnecting);
        }
    } else {
        ic_disconnect(&client->ic);
    }
    ic_el_mutex_unlock();
}

bool ic_el_client_is_connected(ic_el_client_t *client)
{
    return client->connected;
}

/* {{{ Call RPC */

/** Pack and do RPC query. */
static void ic_client_query_pack_and_query(ic_el_client_t *client,
                                           const iop_rpc_t *rpc,
                                           const void *arg, ic_msg_t *msg)
{
    __ic_bpack(msg, rpc->args, arg);
    __ic_query_sync(&client->ic, msg);
}

/** Make an RPC call to an async RPC. */
static void
ic_client_query_async_rpc(ic_el_client_t *client, const iop_rpc_t *rpc,
                          int32_t cmd, const ic__hdr__t *hdr, const void *arg)
{
    ic_msg_t *msg;

    assert(rpc->async);
    msg = ic_msg_new(0);
    msg->async = true;
    msg->cmd = cmd;
    msg->rpc = rpc;
    msg->hdr = hdr;
    msg->cb = &ic_drop_ans_cb;

    ic_client_query_pack_and_query(client, rpc, arg, msg);
}

/* {{{ Sync */

/** Context used when doing ic client sync queries.
 *
 * It is refcounted because it will deleted by both ic_el_client_sync_call()
 * and ic_el_client_sync_query_cb().
 */
typedef struct ic_el_client_sync_query_ctx_t {
    int refcnt;
    bool aborted : 1;
    bool completed : 1;
    const iop_rpc_t *rpc;
    ic_status_t status;
    void *res;
} ic_el_client_sync_query_ctx_t;

GENERIC_INIT(ic_el_client_sync_query_ctx_t, ic_el_client_sync_query_ctx);
GENERIC_WIPE(ic_el_client_sync_query_ctx_t, ic_el_client_sync_query_ctx);
DO_REFCNT(ic_el_client_sync_query_ctx_t, ic_el_client_sync_query_ctx);

/** Callback for syncronous client queries. */
static void ic_el_client_sync_query_cb(ichannel_t *ic, ic_msg_t *msg,
                                       ic_status_t status, void *res,
                                       void *exn)
{
    ic_el_client_t *client = container_of(ic, ic_el_client_t, ic);
    ic_el_client_sync_query_ctx_t *query_ctx;

    query_ctx = *(ic_el_client_sync_query_ctx_t **)msg->priv;

    if (client->force_disconnect) {
        goto end;
    }

    if (query_ctx->aborted) {
        goto end;
    }

    query_ctx->status = status;

    switch (status) {
      case IC_MSG_OK:
        query_ctx->res = mp_iop_dup_desc_sz(NULL, query_ctx->rpc->result, res,
                                            NULL);
        break;

      case IC_MSG_EXN:
        query_ctx->res = mp_iop_dup_desc_sz(NULL, query_ctx->rpc->exn, exn,
                                            NULL);
        break;

      default:
        break;
    }

    query_ctx->completed = true;
    pthread_cond_broadcast(&_G.el_wait_thr_cond);
    ic_el_inc_el_mutex_wait_lock_cnt();

  end:
    ic_el_client_sync_query_ctx_delete(&query_ctx);
}

/** Callback used by wait_thread_cond() to check if the query has been
 *  completed. */
static bool ic_el_client_sync_query_is_completed(void *arg)
{
    ic_el_client_sync_query_ctx_t *query_ctx = arg;

    return query_ctx->completed;
}

ic_el_sync_res_t
ic_el_client_sync_call(ic_el_client_t *client, const iop_rpc_t *rpc,
                       int32_t cmd, const ic__hdr__t *hdr, double timeout,
                       const void *arg, ic_status_t *status, void **res,
                       sb_t *err)
{
    ic_el_sync_res_t call_res = IC_EL_SYNC_OK;
    ic_el_client_sync_query_ctx_t *query_ctx = NULL;
    ic_msg_t *msg;
    wait_thread_cond_res_t wait_res;

    ic_el_mutex_lock(true);

    if (!client->connected) {
        call_res = ic_el_client_sync_connect_locked(client, timeout, err);
        if (call_res != IC_EL_SYNC_OK) {
            goto end;
        }
    }

    if (rpc->async) {
        ic_client_query_async_rpc(client, rpc, cmd, hdr, arg);
        goto end;
    }

    query_ctx = ic_el_client_sync_query_ctx_new();
    query_ctx->rpc = rpc;

    msg = ic_msg(ic_el_client_sync_query_ctx_t *, query_ctx);
    msg->async = false;
    msg->cmd = cmd;
    msg->rpc = rpc;
    msg->hdr = hdr;
    msg->cb = &ic_el_client_sync_query_cb;

    ic_el_client_sync_query_ctx_retain(query_ctx);
    ic_client_query_pack_and_query(client, rpc, arg, msg);

    wait_res = wait_thread_cond(&ic_el_client_sync_query_is_completed,
                                query_ctx, timeout);
    switch (wait_res) {
      case WAIT_THR_COND_OK:
        break;

      case WAIT_THR_COND_TIMEOUT:
        sb_setf(err, "timeout on query `%*pM`", LSTR_FMT_ARG(rpc->name));
        call_res = IC_EL_SYNC_ERR;
        query_ctx->aborted = true;
        goto end;

      case WAIT_THR_COND_SIGINT:
        call_res = IC_EL_SYNC_SIGINT;
        query_ctx->aborted = true;
        goto end;
    }

    *status = query_ctx->status;
    *res = query_ctx->res;

end:
    ic_el_client_sync_query_ctx_delete(&query_ctx);
    ic_el_mutex_unlock();
    return call_res;
}

/* }}} */
/* {{{ Async */

/** Context to be used on asynchronous query on an RPC. */
typedef struct ic_el_client_async_query_ctx_t {
    int refcnt;
    const iop_rpc_t *rpc;
    ic_client_async_call_f cb;
    void *nullable cb_arg;
    el_t timeout_el;
    bool aborted : 1;
} ic_el_client_async_query_ctx_t;

static void ic_el_client_async_query_ctx_wipe(
    ic_el_client_async_query_ctx_t *query_ctx)
{
    el_unregister(&query_ctx->timeout_el);
}

GENERIC_INIT(ic_el_client_async_query_ctx_t, ic_el_client_async_query_ctx);
DO_REFCNT(ic_el_client_async_query_ctx_t, ic_el_client_async_query_ctx);

/** Callback on asynchronous client queries timeout. */
static void ic_el_client_async_query_timeout_cb(el_t ev, el_data_t priv)
{
    SB_1k(err);
    ic_el_client_async_query_ctx_t *query_ctx = priv.ptr;
    ic_client_async_call_f cb = query_ctx->cb;
    void *cb_arg = query_ctx->cb_arg;

    query_ctx->aborted = true;
    sb_setf(&err, "timeout on query `%*pM`",
            LSTR_FMT_ARG(query_ctx->rpc->name));

    /* Force unregister timeout_el here to avoid dandling reference on
     * ic_el_client_async_query_cb().
     * Do the cleanup before calling the callback to avoid races.
     */
    el_unregister(&query_ctx->timeout_el);
    ic_el_client_async_query_ctx_delete(&query_ctx);

    /* Call callback with timeout error. */
    ic_el_mutex_unlock();
    (*cb)(&err, IC_MSG_TIMEDOUT, NULL, cb_arg);
    ic_el_mutex_lock(false);
}

/** Callback for asynchronous client queries. */
static void ic_el_client_async_query_cb(ichannel_t *ic, ic_msg_t *msg,
                                        ic_status_t status, void *res,
                                        void *exn)
{
    ic_el_client_t *client = container_of(ic, ic_el_client_t, ic);
    ic_el_client_async_query_ctx_t *query_ctx;
    bool aborted;
    ic_client_async_call_f cb;
    void *cb_arg;

    query_ctx = *(ic_el_client_async_query_ctx_t **)msg->priv;

    aborted = query_ctx->aborted;
    cb = query_ctx->cb;
    cb_arg = query_ctx->cb_arg;

    /* Do the cleanup before calling the callback to avoid races.
     */
    if (query_ctx->timeout_el) {
        /* Force unregister timeout_el and release reference here to avoid
         * calling ic_el_client_async_query_timeout_cb(). */
        el_unregister(&query_ctx->timeout_el);
        ic_el_client_async_query_ctx_release(&query_ctx);
    }
    ic_el_client_async_query_ctx_delete(&query_ctx);

    if (client->force_disconnect || aborted) {
        /* Do not send the result in those cases. */
        return;
    }

    if (status == IC_MSG_EXN) {
        res = exn;
    }

    /* Call callback with query result. */
    ic_el_mutex_unlock();
    (*cb)(NULL, status, res, cb_arg);
    ic_el_mutex_lock(false);
}

/** Make the asynchronous RPC call with the connected IC EL client. */
static void ic_el_client_async_call_connected(
    ic_el_client_t *client, const iop_rpc_t *rpc, int32_t cmd,
    const ic__hdr__t *hdr, double timeout, const void *arg,
    ic_client_async_call_f cb, void *nullable cb_arg)
{
    ic_el_client_async_query_ctx_t *query_ctx;
    ic_msg_t *msg;

    assert(client->connected);

    if (rpc->async) {
        ic_client_query_async_rpc(client, rpc, cmd, hdr, arg);

        /* Async RPC call is always OK. */
        ic_el_mutex_unlock();
        (*cb)(NULL, IC_MSG_OK, NULL, cb_arg);
        ic_el_mutex_lock(false);
        return;
    }

    query_ctx = ic_el_client_async_query_ctx_new();
    query_ctx->rpc = rpc;
    query_ctx->cb = cb;
    query_ctx->cb_arg = cb_arg;

    if (timeout > 0.0) {
        int64_t timeout_msec = (int64_t)(timeout * 1000.0);

        /* Retain the query synce it is used by timeout_el and the query
         * callback. */
        ic_el_client_async_query_ctx_retain(query_ctx);
        query_ctx->timeout_el = el_timer_register(
            timeout_msec, 0, EL_TIMER_LOWRES,
            &ic_el_client_async_query_timeout_cb, query_ctx);
    }

    msg = ic_msg(ic_el_client_async_query_ctx_t *, query_ctx);
    msg->async = false;
    msg->cmd = cmd;
    msg->rpc = rpc;
    msg->hdr = hdr;
    msg->cb = &ic_el_client_async_query_cb;

    ic_client_query_pack_and_query(client, rpc, arg, msg);
}

/** Arguments passed to a IC EL client async RPC call.
 *
 * It is used to store the arguments while the client is connecting before
 * making an RPC call.
 */
typedef struct ic_el_client_async_call_args_t {
    ic_el_client_t *client;
    const iop_rpc_t *rpc;
    int32_t cmd;
    ic__hdr__t *hdr;
    double timeout;
    void *arg;
    ic_client_async_call_f cb;
    void *nullable cb_arg;
} ic_el_client_async_call_args_t;

static void ic_el_client_async_call_args_wipe(
    ic_el_client_async_call_args_t *call_args)
{
    p_delete(&call_args->hdr);
    p_delete(&call_args->arg);
}

GENERIC_NEW_INIT(ic_el_client_async_call_args_t,
                 ic_el_client_async_call_args);
GENERIC_DELETE(ic_el_client_async_call_args_t,
               ic_el_client_async_call_args);

static void ic_el_client_async_call_connect_cb(const sb_t *nullable err,
                                               void *nullable cb_arg)
{
    ic_el_client_async_call_args_t *call_args = cb_arg;

    if (err) {
        /* The client cannot be connected, return the error. */
        ic_el_mutex_unlock();
        (*call_args->cb)(err, IC_MSG_TIMEDOUT, NULL, call_args->cb_arg);
        ic_el_mutex_lock(false);
    } else {
        /* The client is connected, make the RPC query. */
        ic_el_client_async_call_connected(call_args->client, call_args->rpc,
                                          call_args->cmd, call_args->hdr,
                                          call_args->timeout, call_args->arg,
                                          call_args->cb, call_args->cb_arg);
    }

    ic_el_client_async_call_args_delete(&call_args);
}

void ic_el_client_async_call(ic_el_client_t *client, const iop_rpc_t *rpc,
                             int32_t cmd, const ic__hdr__t *hdr,
                             double timeout, const void *arg,
                             ic_client_async_call_f cb,
                             void *nullable cb_arg)
{
    SB_1k(err);
    ic_el_client_async_connect_t *async_ctx = NULL;
    ic_el_async_res_t res;

    ic_el_mutex_lock(true);

    /* cb_arg as NULL here to avoid creating the call_args when not necessary.
     */
    res = ic_el_client_async_connect_locked(
        client, timeout, &ic_el_client_async_call_connect_cb, NULL, true,
        &async_ctx, &err);

    switch (res) {
    case IC_EL_ASYNC_OK:
        /* The client is connected, make the RPC query. */
        ic_el_client_async_call_connected(client, rpc, cmd, hdr, timeout, arg,
                                          cb, cb_arg);
        break;

    case IC_EL_ASYNC_ERR:
        /* Call cb with the error after unlocking the mutex. */
        break;

    case IC_EL_ASYNC_PENDING: {
        /* Wait for client connection to make the RPC. */
        ic_el_client_async_call_args_t *call_args;

        call_args = ic_el_client_async_call_args_new();
        call_args->client = client;
        call_args->rpc = rpc;
        call_args->cmd = cmd;
        call_args->hdr = iop_dup(ic__hdr, hdr);
        call_args->timeout = timeout;
        call_args->arg = mp_iop_dup_desc_sz(NULL, rpc->args, arg, NULL);
        call_args->cb = cb;
        call_args->cb_arg = cb_arg;

        assert(async_ctx != NULL);
        async_ctx->cb_arg = call_args;
    } break;
    }

    ic_el_mutex_unlock();

    if (res == IC_EL_ASYNC_ERR) {
        (*cb)(&err, IC_MSG_TIMEDOUT, NULL, cb_arg);
    }
}

/* }}} */
/* }}} */
/* }}} */
/* {{{ Module init */

/** Initialize variables for el thread. */
static void ic_el_thr_vars_init(void)
{
    pthread_mutexattr_t attr;

    _G.el_thr_status = EL_THR_NOT_STARTED;
    _G.main_thread = pthread_self();
    atomic_init(&_G.el_mutex_wait_lock_cnt, 0);

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&_G.el_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_cond_init(&_G.el_thr_start_cond, NULL);
    pthread_cond_init(&_G.el_mutex_after_lock_cond, NULL);
    pthread_cond_init(&_G.el_wait_thr_cond, NULL);
}

/** Wipe variables for el thread. */
static void ic_el_thr_vars_wipe(void)
{
    pthread_cond_destroy(&_G.el_thr_start_cond);
    pthread_mutex_destroy(&_G.el_mutex);
    pthread_cond_destroy(&_G.el_mutex_after_lock_cond);
    pthread_cond_destroy(&_G.el_wait_thr_cond);
}

/** Callback called by pthread before fork in the parent process. */
static void ic_el_atfork_prepare(void)
{
    if (_G.el_thr_status != EL_THR_NOT_STARTED
    &&  pthread_self() == _G.el_thread)
    {
        e_fatal("forking in event loop thread is not allowed");
    }

    ic_el_mutex_lock(false);
}

/** Callback called by pthread after fork in the parent process. */
static void ic_el_atfork_parent(void)
{
    ic_el_mutex_unlock();
}

/** Callback called by pthread after fork in the child process. */
static void ic_el_atfork_child(void)
{
    /* Stop all servers. */
    qm_for_each_value_p(ic_el_server, server_ptr, &_G.servers) {
        ic_el_server_clear(*server_ptr, false);
        ic_el_server_delete(server_ptr);
    }
    qm_clear(ic_el_server, &_G.servers);

    /* Disconnect all clients. They will reconnect on the next RPC call. */
    qh_for_each_key(ic_el_client, client, &_G.clients) {
        client->force_disconnect = true;
        ic_disconnect(&client->ic);
    }

    ic_el_thr_vars_init();
}

static int ic_el_initialize(void *arg)
{
    ic_el_thr_vars_init();

    _G.el_mutex_before_lock_el = el_wake_register(request_el_mutex_lock_cb,
                                                  NULL);

    qm_init(ic_el_server, &_G.servers);
    qh_init(ic_el_client, &_G.clients);
    dlist_init(&_G.disconnecting_clients);
    dlist_init(&_G.destroyed_clients);

    pthread_atfork(&ic_el_atfork_prepare, &ic_el_atfork_parent,
                   &ic_el_atfork_child);

    return 0;
}

static int ic_el_shutdown(void)
{
    assert (_G.el_thr_status == EL_THR_NOT_STARTED
         || _G.el_thr_status == EL_THR_STOPPED);

    ic_el_thr_vars_wipe();

    el_unregister(&_G.el_mutex_before_lock_el);
    el_unregister(&_G.el_wait_thr_sigint_el);

    qm_for_each_pos(ic_el_server, pos, &_G.servers) {
        ic_el_server_t *server = _G.servers.values[pos];

        ic_el_server_clear(server, false);
        ic_el_server_delete(&server);
    }
    qh_for_each_pos(ic_el_client, pos, &_G.clients) {
        ic_el_client_clear(_G.clients.keys[pos]);
    }

    qm_wipe(ic_el_server, &_G.servers);
    qh_wipe(ic_el_client, &_G.clients);
    return 0;
}

static MODULE_BEGIN(ic_el)
    MODULE_DEPENDS_ON(el);
    MODULE_DEPENDS_ON(ic);
MODULE_END()

void ic_el_module_init(void)
{
    module_register_at_fork();

    pthread_force_use();

    MODULE_REQUIRE(ic_el);
}

void ic_el_module_stop(void)
{
    el_thr_status_t old_thr_status;

    ic_el_mutex_lock(false);

    old_thr_status = _G.el_thr_status;
    _G.el_thr_status = EL_THR_STOPPED;

    switch (old_thr_status) {
      case EL_THR_STARTING:
      case EL_THR_STARTED: {
        el_t el = el_signal_register(SIGINT, &el_loop_thread_on_term, NULL);

        if (_G.el_wait_thr_sigint_cnt > 0) {
            pthread_cond_broadcast(&_G.el_wait_thr_cond);
        }

        ic_el_mutex_unlock();
        pthread_kill(_G.el_thread, SIGINT);
        pthread_join(_G.el_thread, NULL);

        el_unregister(&el);
      } break;

      case EL_THR_NOT_STARTED:
      case EL_THR_STOPPED:
        ic_el_mutex_unlock();
        break;
    }
}

void ic_el_module_cleanup(void)
{
    MODULE_RELEASE(ic_el);
}

/* }}} */
