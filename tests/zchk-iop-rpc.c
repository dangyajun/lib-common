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

#include <sys/wait.h>

#include <lib-common/z.h>
#include <lib-common/iop-rpc.h>
#include <lib-common/core/core.iop.h>
#include <lib-common/datetime.h>

#include "iop/tstiop_rpc.iop.h"

typedef struct ctx_t {
    uint32_t u;
} ctx_t;

static struct {
    iop_env_t          *iop_env;
    ichannel_t         *ic_aux;
    ichannel_t         *ic_spawned;
    int                 mode;
    ic_status_t         status;
    core__log_level__t  level;
    ctx_t               ctx;
    int echo_rpc_answered;
    bool reply_callback_called;
    bool reply_callback_called_synchronously;
    bool sub_query_called;
    bool sub_query_called_synchronously;
    bool reply_status_is_abort;
    uint32_t last_user_version;
} z_iop_rpc_g;
#define _G  z_iop_rpc_g

static void g_result_init(void)
{
    _G.status = -1;
    _G.level = INT32_MIN;
    _G.ctx.u = 0;
}

static void IOP_RPC_IMPL(core__core, log, set_root_level)
{
    if (arg->level < LOG_LEVEL_min || LOG_LEVEL_max < arg->level) {
        ic_throw(ic, slot, core__core, log, set_root_level);
        return;
    }
    ic_reply(NULL, slot, core__core, log, set_root_level,
             .level = arg->level);
    arg->level++;
}

static void IOP_RPC_IMPL(core__core, log, reset_logger_level)
{
    _G.sub_query_called = true;
    ic_reply(NULL, slot, core__core, log, reset_logger_level,
             .level = 0);
}

static void IOP_RPC_CB(core__core, log, reset_logger_level)
{
}

static void IOP_RPC_IMPL(core__core, log, reset_root_level)
{
    _G.sub_query_called = false;
    ic_query2(ic, ic_msg_new(0), core__core, log, reset_logger_level,
              .full_name = LSTR(""));

    _G.reply_callback_called = false;
    ic_reply(NULL, slot, core__core, log, reset_root_level,
             .level = 0);

    _G.reply_callback_called_synchronously = _G.reply_callback_called;
    _G.sub_query_called_synchronously = _G.sub_query_called;
}

#define RPC_CB()                                 \
    do {                                         \
        _G.status = status;                      \
        _G.level = res ? res->level : INT32_MIN; \
        _G.ctx = *acast(ctx_t, msg->priv);       \
    } while (0)

static void IOP_RPC_CB(core__core, log, set_root_level)
{
    if (_G.mode) {
        __ic_forward_reply_to(ic, get_unaligned_cpu64(msg->priv),
                              status, res, exn);
    } else {
        RPC_CB();
    }
}

static void IOP_RPC_CB(core__core, log, reset_root_level)
{
    _G.reply_callback_called = true;

    _G.reply_status_is_abort = status == IC_MSG_ABORT;
    _G.level = res ? res->level : INT32_MIN;
}


static void IOP_RPC_IMPL(core__core, log, set_logger_level)
{
    IOP_RPC_T(core__core, log, set_root_level, args) v = {
        .level = arg->level,
    };

    if (_G.mode) {
        ic_query2_p(_G.ic_aux, ic_msg(uint64_t, slot),
                    core__core, log, set_root_level, &v);
    } else {
        ic_query_proxy(_G.ic_aux, slot, core__core, log, set_root_level, &v);
    }
}

static void IOP_RPC_CB(core__core, log, set_logger_level)
{
    RPC_CB();
}

#define RPC_CALL(_ic, _rpc, _force_pack, _force_dup, _level, _u)             \
    do {                                                                     \
        ic_msg_t *ic_msg = ic_msg(ctx_t, .u = (_u));                         \
        int level = (_level);                                                \
        IOP_RPC_T(core__core, log, _rpc, args) arg = { .level = level, };    \
                                                                             \
        g_result_init();                                                     \
        ic_msg->force_pack = (_force_pack);                                  \
        ic_msg->force_dup = (_force_dup);                                    \
        ic_query2_p((_ic), ic_msg, core__core, log, _rpc, &arg);             \
        if (_force_pack || _force_dup) {                                     \
            Z_ASSERT_EQ(arg.level, level, "arg not preserved");              \
        }                                                                    \
    } while (0)

#define TEST_RPC_CALL(_ic, _rpc, _force_pack, _force_dup, _suffix)           \
    do {                                                                     \
        ichannel_t *__ic = (_ic);                                            \
        bool __force_pack = (_force_pack);                                   \
        bool __force_dup = (_force_dup);                                     \
                                                                             \
        /* call with no error */                                             \
        RPC_CALL(__ic, _rpc, __force_pack, __force_dup, 1, 0xabcdef);        \
        Z_ASSERT_EQ(_G.status, (ic_status_t)IC_MSG_OK,                       \
                    "rpc returned bad status"_suffix);                       \
        Z_ASSERT_EQ(_G.level, 1, "rpc returned bad result"_suffix);          \
        Z_ASSERT_EQ(_G.ctx.u, 0xabcdefU, "rpc returned bad msg priv"_suffix);\
                                                                             \
        /* call with throw */                                                \
        RPC_CALL(__ic, _rpc, __force_pack, __force_dup,                      \
                 LOG_LEVEL_min - 1, 0);                                      \
        Z_ASSERT_EQ(_G.status, (ic_status_t)IC_MSG_EXN,                      \
                    "rpc returned bad status"_suffix);                       \
    } while (0)

/* {{{ Reset logger level RPC */

typedef struct echo_ctx_t {
    int received;
    bool has_answer;
} echo_ctx_t;

static void IOP_RPC_CB(tstiop_rpc__rpc, test, echo)
{
    echo_ctx_t *ctx = *acast(echo_ctx_t *, msg->priv);

    assert (res != NULL);
    ctx->has_answer = true;
    ctx->received = res->i;
}

static void IOP_RPC_IMPL(tstiop_rpc__rpc, test, echo)
{
    ic_reply(ic, slot, tstiop_rpc__rpc, test, echo, arg->i);
    _G.echo_rpc_answered++;
}

/* }}} */
/* {{{ Helpers */

static void dummy_on_event(ichannel_t *ic, ic_event_t evt)
{
    return;
}

static bool check_user_version_true(uint32_t user_version)
{
    _G.last_user_version = user_version;

    return true;
}

static bool check_user_version_false(uint32_t user_version)
{
    _G.last_user_version = user_version;

    return false;
}

static int z_ic_on_accept(el_t ev, int fd)
{
    _G.ic_spawned = ic_new();
    _G.ic_spawned->iop_env = _G.iop_env;
    _G.ic_spawned->on_event = &dummy_on_event;
    _G.ic_spawned->impl = &ic_no_impl;
    _G.ic_spawned->do_el_unref = true;
    _G.ic_spawned->no_autodel = true;
    _G.ic_spawned->auto_reconn = false;

    ic_spawn(_G.ic_spawned, fd, NULL);
    return 0;
}

static int
z_connect_ics_and_wait(ichannel_t *ic_client, const sockunion_t *su)
{
    time_t start_ts = lp_getsec();

    ic_init(ic_client);
    ic_client->su          = *su;
    ic_client->iop_env     = _G.iop_env;
    ic_client->on_event    = &dummy_on_event;
    ic_client->impl        = &ic_no_impl;
    ic_client->auto_reconn = false;
    Z_ASSERT_N(ic_connect(ic_client));

    ic_delete(&_G.ic_spawned);
    while (!(_G.ic_spawned && ic_is_ready(_G.ic_spawned) &&
             ic_is_ready(ic_client)) &&
           (lp_getsec() - start_ts) <= 2)
    {
        el_loop_timeout(100);
    }

    Z_ASSERT_P(_G.ic_spawned);

    Z_HELPER_END;
}

/* }}} */
/* {{{ Tests */

Z_GROUP_EXPORT(iop_rpc)
{
    _G.iop_env = iop_env_new();
    MODULE_REQUIRE(ic);

    Z_TEST(ic_local, "iop-rpc: ic local") {
        ichannel_t ic;
        qm_t(ic_cbs) impl = QM_INIT(ic_cbs, impl);
        qm_t(ic_cbs) impl_aux = QM_INIT(ic_cbs, impl_aux);

        ic_init(&ic);
        ic.iop_env = _G.iop_env;
        ic_set_local(&ic, false);

        _G.ic_aux = ic_new();
        _G.ic_aux->iop_env = _G.iop_env;
        ic_set_local(_G.ic_aux, false);

        for (int force_pack = 0; force_pack <= 1; force_pack++) {
            for (int force_dup = 0; force_dup <= !force_pack; force_dup++) {

                ic.impl = NULL;
                _G.ic_aux->impl = NULL;

                /* check behavior when ic->impl is null */
                RPC_CALL(&ic, set_root_level, force_pack, force_dup, 0, 0);
                Z_ASSERT_EQ(_G.status, (ic_status_t)IC_MSG_UNIMPLEMENTED,
                            "rpc returned bad status");

                ic_register(&impl, core__core, log, set_root_level);
                ic_register(&impl, core__core, log, set_logger_level);

                ic.impl = &impl;
                _G.ic_aux->impl = &impl_aux;

                TEST_RPC_CALL(&ic, set_root_level, force_pack, force_dup, "");

                ic_register_proxy(&impl_aux, core__core, log, set_root_level,
                                  &ic);
                TEST_RPC_CALL(_G.ic_aux, set_root_level, force_pack,
                              force_dup, " for register proxy");
                ic_unregister(&impl_aux, core__core, log, set_root_level);

                /* ic:set_logger_level --query_proxy--> ic_aux:set_root_level
                 */
                ic_register(&impl_aux, core__core, log, set_root_level);
                TEST_RPC_CALL(&ic, set_logger_level, force_pack, force_dup,
                              " for query_proxy");

                /* ic:set_logger_level --query--> ic_aux:set_root_level */
                _G.mode = 1;
                TEST_RPC_CALL(&ic, set_logger_level, force_pack, force_dup,
                              " for forward reply");
                _G.mode = 0;

                ic_unregister(&impl, core__core, log, set_root_level);
                ic_unregister(&impl, core__core, log, set_logger_level);
                ic_unregister(&impl_aux, core__core, log, set_root_level);
            }
        }

        qm_wipe(ic_cbs, &impl);
        qm_wipe(ic_cbs, &impl_aux);
        ic_disconnect(&ic);
        ic_disconnect(_G.ic_aux);
        ic_wipe(&ic);
        ic_delete(&_G.ic_aux);
    } Z_TEST_END;

    Z_TEST(ic_spawn_with_socketpair, "iop-rpc: socketpair and fork") {
        /* A process, in order to share an IC with one of its children, may
         * create two connected sockets with socketpair and then use them as
         * an IC. This is done by calling ic_spawn on both ends. This test
         * does exactly that and thus test that everything works well here. */
        int sv[2];
        ichannel_t *ic1 = ic_new();
        ichannel_t *ic2 = ic_new();
        qm_t(ic_cbs) impl = QM_INIT(ic_cbs, impl);
        int child_pid;
        int zombie_status;

        Z_ASSERT_P(ic1);
        Z_ASSERT_P(ic2);
        ic1->no_autodel = ic2->no_autodel = true;
        ic1->iop_env = ic2->iop_env = _G.iop_env;
        ic1->on_event = ic2->on_event = dummy_on_event;

        Z_ASSERT_N(socketpairx(AF_UNIX, SOCK_SEQPACKET, 0, O_NONBLOCK, sv));
        ic_spawn(ic1, sv[0], NULL);
        ic_spawn(ic2, sv[1], NULL);

        ic_register(&impl, tstiop_rpc__rpc, test, echo);
        Z_ASSERT(ic1->is_connected);
        Z_ASSERT(ic2->is_connected);

        child_pid = ifork();
        Z_ASSERT_N(child_pid);
        if (!child_pid) {
            /* The child echoes the father's messages. */
            ic_delete(&ic1);
            ic2->impl = &impl;

            while (_G.echo_rpc_answered < 1) {
                el_fd_loop(ic2->elh, 1000, EV_FDLOOP_HANDLE_TIMERS);
            }

            ic_unregister(&impl, tstiop_rpc__rpc, test, echo);
            qm_wipe(ic_cbs, &impl);
            ic_delete(&ic2);
            MODULE_RELEASE(ic);
            exit(0);
        } else {
            echo_ctx_t ctx;
            ic_msg_t *msg;

            /* The father tries to get an echo from its children. */
            ic_delete(&ic2);
            ic1->impl = &impl;

            p_clear(&ctx, 1);
            msg = ic_msg(echo_ctx_t *, &ctx);
            ic_query2(ic1, msg, tstiop_rpc__rpc, test, echo, 1);
            while (!ctx.has_answer) {
                Z_ASSERT(ic1->is_connected);
                el_fd_loop(ic1->elh, 1000, EV_FDLOOP_HANDLE_TIMERS);
            }
            Z_ASSERT_EQ(ctx.received, 1);

            waitpid(child_pid, &zombie_status, 0);
            Z_ASSERT_EQ(WEXITSTATUS(zombie_status), 0);

            ic_unregister(&impl, core__core, log, set_root_level);
            qm_wipe(ic_cbs, &impl);
            ic_delete(&ic1);
        }
    } Z_TEST_END;

    Z_TEST(ic_local_async, "iop-rpc: ic local async") {
        ichannel_t ic;
        qm_t(ic_cbs) impl = QM_INIT(ic_cbs, impl);

        ic_register(&impl, core__core, log, reset_root_level);
        ic_register(&impl, core__core, log, reset_logger_level);

        ic_init(&ic);
        ic.iop_env = _G.iop_env;
        ic_set_local(&ic, true);
        ic.impl = &impl;

        g_result_init();

        ic_query2(&ic, ic_msg_new(0), core__core, log, reset_root_level);
        Z_ASSERT_EQ(_G.level, INT32_MIN);
        /* First timeout will only execute the query. We need a second one to
         * execute the reply (which will modify the level).
         */
        el_loop_timeout(0);
        el_loop_timeout(0);
        Z_ASSERT_EQ(_G.level, 0);
        Z_ASSERT(_G.reply_callback_called);
        Z_ASSERT(!_G.reply_callback_called_synchronously);
        Z_ASSERT(!_G.sub_query_called_synchronously);
        Z_ASSERT(_G.sub_query_called);
        Z_ASSERT(!_G.reply_status_is_abort);

        _G.reply_callback_called = false;
        ic_query2(&ic, ic_msg_new(0), core__core, log, reset_root_level);
        ic_wipe(&ic);
        Z_ASSERT(_G.reply_callback_called);
        Z_ASSERT(_G.reply_status_is_abort);
        qm_wipe(ic_cbs, &impl);

        /* Check that ic is correctly cleaned up. If this el_loop_timeout
         * call crash, it means that the pending query was not correctly
         * wiped.
         */
        el_loop_timeout(0);
    } Z_TEST_END;

    Z_TEST(ic_hook_ctx, "iop-rpc: ic hook ctx leak") {
        /* Test that allocated hook contexts are properly wiped when ichannel
         * module shuts down, which can happens in real-life when a program
         * stops and there are some pending RPC queries. This test would fail
         * in ASAN mode if the contexts are not properly wiped. */
        ic_hook_ctx_t *ctx;

        ctx = ic_hook_ctx_new(0, 0);
        ctx = ic_hook_ctx_new(1, 0);
        ctx = ic_hook_ctx_new(2, 0);

        Z_ASSERT_P(ctx);
    } Z_TEST_END;

    Z_TEST(ic_user_version, "ipo-rpc: user version tests") {
        el_t server_ev;
        ichannel_t ic_client;
        int port;
        sockunion_t su = {
            .sin = {
                .sin_family = AF_INET,
                .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
            }
        };

        server_ev = ic_listento(&su, SOCK_STREAM, IPPROTO_TCP,
                                &z_ic_on_accept);
        Z_ASSERT_P(server_ev);

        port = getsockport(el_fd_get_fd(server_ev), AF_INET);
        sockunion_setport(&su, port);

        /* Test to establish a connection between 2 ICs with compatible
         * versions. */
        _G.last_user_version = 0;
        iop_env_set_ic_user_version(_G.iop_env, (ic_user_version_t) {
            .current_version = 0xdeadbeef,
            .check_cb = &check_user_version_true,
        });

        Z_HELPER_RUN(z_connect_ics_and_wait(&ic_client, &su));

        /* Both ICs should be connected, the version set should be the one to
         * be tested. */
        Z_ASSERT(_G.ic_spawned->is_connected);
        Z_ASSERT(ic_client.is_connected);
        Z_ASSERT_EQ(_G.last_user_version, 0xdeadbeef);

        ic_wipe(&ic_client);
        ic_delete(&_G.ic_spawned);

        /* Now test to establish a connection between 2 ICs with incompatible
         * versions. */
        _G.last_user_version = 0;
        iop_env_set_ic_user_version(_G.iop_env, (ic_user_version_t) {
            .current_version = 0xdeadbeef,
            .check_cb = &check_user_version_false,
        });

        Z_HELPER_RUN(z_connect_ics_and_wait(&ic_client, &su));

        /* Both ICs should stayed disconnected since the user version was
         * rejected. */
        Z_ASSERT(!_G.ic_spawned->is_connected);
        Z_ASSERT(!ic_client.is_connected);

        ic_wipe(&ic_client);
        ic_delete(&_G.ic_spawned);
        el_unregister(&server_ev);
    } Z_TEST_END;

    MODULE_RELEASE(ic);
    iop_env_delete(&_G.iop_env);
} Z_GROUP_END;

/* }}} */
