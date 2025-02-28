/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * mot_internal.cpp
 *    MOT Foreign Data Wrapper internal interfaces to the MOT engine.
 *
 * IDENTIFICATION
 *    src/gausskernel/storage/mot/fdw_adapter/src/mot_internal.cpp
 *
 * -------------------------------------------------------------------------
 */
#include <google/protobuf/io/gzip_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>

#include <ostream>
#include <istream>
#include <iomanip>
#include <pthread.h>
#include <cstring>
#include "transaction.pb.h"
#include "node.pb.h"
#include "client.pb.h"
#include "server.pb.h"
#include "storage.pb.h"
#include "message.pb.h"
#include "postgres.h"
#include "access/dfs/dfs_query.h"
#include "access/sysattr.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/syscache.h"
#include "executor/executor.h"
#include "storage/ipc.h"
#include "commands/dbcommands.h"
#include "knl/knl_session.h"

#include "mot_internal.h"
#include "row.h"
#include "log_statistics.h"
#include "spin_lock.h"
#include "txn.h"
#include "table.h"
#include "utilities.h"
#include "mot_engine.h"
#include "sentinel.h"
#include "txn.h"
#include "txn_access.h"
#include "index_factory.h"
#include "column.h"
#include "mm_raw_chunk_store.h"
#include "ext_config_loader.h"
#include "config_manager.h"
#include "mot_error.h"
#include "utilities.h"
#include "jit_context.h"
#include "mm_cfg.h"
#include "jit_statistics.h"
#include "gaussdb_config_loader.h"
#include <cstdio>
#include <stdio.h>
#include <iostream>
#include "string"
#include "sstream"
#include <fstream>
#include <atomic>
#include <sys/time.h>
// #include "neu_concurrency_tools/blockingconcurrentqueue.h"
// #include "neu_concurrency_tools/blocking_mpmc_queue.h"
// #include "neu_concurrency_tools/ThreadPool.h"
// #include "epoch_merge.h"
#include "zmq.hpp"
#include "zmq.h"
#include <sched.h>
#include "utils/timestamp.h"
#include "postmaster/postmaster.h"

/** @define masks for CSN word   */
#define CSN_BITS 0x1FFFFFFFFFFFFFFFUL

#define STATUS_BITS 0xE000000000000000UL

#define IS_CHAR_TYPE(oid) (oid == VARCHAROID || oid == BPCHAROID || oid == TEXTOID || oid == CLOBOID || oid == BYTEAOID)
#define IS_INT_TYPE(oid)                                                                                           \
    (oid == BOOLOID || oid == CHAROID || oid == INT8OID || oid == INT2OID || oid == INT4OID || oid == FLOAT4OID || \
        oid == FLOAT8OID || oid == INT1OID || oid == DATEOID || oid == TIMEOID || oid == TIMESTAMPOID ||           \
        oid == TIMESTAMPTZOID)

MOT::MOTEngine* MOTAdaptor::m_engine = nullptr;
static XLOGLogger xlogger;

// enable MOT Engine logging facilities
DECLARE_LOGGER(InternalExecutor, FDW)

/** @brief on_proc_exit() callback for cleaning up current thread - only when thread pool is ENABLED. */
static void MOTCleanupThread(int status, Datum ptr);

/** @brief Helper for cleaning up all JIT context objects stored in all CachedPlanSource of the current session. */
static void DestroySessionJitContexts();

// in a thread-pooled environment we need to ensure thread-locals are initialized properly
static inline void EnsureSafeThreadAccessInline()
{
    if (MOTCurrThreadId == INVALID_THREAD_ID) {
        MOT_LOG_DEBUG("Initializing safe thread access for current thread");
        MOT::AllocThreadId();
        // register for cleanup only once - not having a current thread id is the safe indicator we never registered
        // proc-exit callback for this thread
        if (g_instance.attr.attr_common.enable_thread_pool) {
            on_proc_exit(MOTCleanupThread, PointerGetDatum(nullptr));
            MOT_LOG_DEBUG("Registered current thread for proc-exit callback for thread %p", (void*)pthread_self());
        }
    }
    if (MOTCurrentNumaNodeId == MEM_INVALID_NODE) {
        MOT::InitCurrentNumaNodeId();
    }
    MOT::InitMasstreeThreadinfo();
}

extern void EnsureSafeThreadAccess()
{
    EnsureSafeThreadAccessInline();
}

static void DestroySession(MOT::SessionContext* sessionContext)
{
    MOT_ASSERT(MOTAdaptor::m_engine);
    MOT_LOG_DEBUG("Destroying session context %p, connection_id %u", sessionContext, sessionContext->GetConnectionId());

    if (u_sess->mot_cxt.jit_session_context_pool) {
        JitExec::FreeSessionJitContextPool(u_sess->mot_cxt.jit_session_context_pool);
    }
    MOT::GetSessionManager()->DestroySessionContext(sessionContext);
}

// Global map of PG session identification (required for session statistics)
// This approach is safer than saving information in the session context
static pthread_spinlock_t sessionDetailsLock;
typedef std::map<MOT::SessionId, pair<::ThreadId, pg_time_t>> SessionDetailsMap;
static SessionDetailsMap sessionDetailsMap;

static void InitSessionDetailsMap()
{
    pthread_spin_init(&sessionDetailsLock, 0);
}

static void DestroySessionDetailsMap()
{
    pthread_spin_destroy(&sessionDetailsLock);
}

static void RecordSessionDetails()
{
    MOT::SessionId sessionId = u_sess->mot_cxt.session_id;
    if (sessionId != INVALID_SESSION_ID) {
        pthread_spin_lock(&sessionDetailsLock);
        sessionDetailsMap.emplace(sessionId, std::make_pair(t_thrd.proc->pid, t_thrd.proc->myStartTime));
        pthread_spin_unlock(&sessionDetailsLock);
    }
}

static void ClearSessionDetails(MOT::SessionId sessionId)
{
    if (sessionId != INVALID_SESSION_ID) {
        pthread_spin_lock(&sessionDetailsLock);
        SessionDetailsMap::iterator itr = sessionDetailsMap.find(sessionId);
        if (itr != sessionDetailsMap.end()) {
            sessionDetailsMap.erase(itr);
        }
        pthread_spin_unlock(&sessionDetailsLock);
    }
}

inline void ClearCurrentSessionDetails()
{
    ClearSessionDetails(u_sess->mot_cxt.session_id);
}

static void GetSessionDetails(MOT::SessionId sessionId, ::ThreadId* gaussSessionId, pg_time_t* sessionStartTime)
{
    // although we have the PGPROC in the user data of the session context, we prefer not to use
    // it due to safety (in some unknown constellation we might hold an invalid pointer)
    // it is much safer to save a copy of the two required fields
    pthread_spin_lock(&sessionDetailsLock);
    SessionDetailsMap::iterator itr = sessionDetailsMap.find(sessionId);
    if (itr != sessionDetailsMap.end()) {
        *gaussSessionId = itr->second.first;
        *sessionStartTime = itr->second.second;
    }
    pthread_spin_unlock(&sessionDetailsLock);
}

// provide safe session auto-cleanup in case of missing session closure
// This mechanism relies on the fact that when a session ends, eventually its thread is terminated
// ATTENTION: in thread-pooled envelopes this assumption no longer holds true, since the container thread keeps
// running after the session ends, and a session might run each time on a different thread, so we
// disable this feature, instead we use this mechanism to generate thread-ended event into the MOT Engine
static pthread_key_t sessionCleanupKey;

static void SessionCleanup(void* key)
{
    MOT_ASSERT(!g_instance.attr.attr_common.enable_thread_pool);

    // in order to ensure session-id cleanup for session 0 we use positive values
    MOT::SessionId sessionId = (MOT::SessionId)(((uint64_t)key) - 1);
    if (sessionId != INVALID_SESSION_ID) {
        MOT_LOG_WARN("Encountered unclosed session %u (missing call to DestroyTxn()?)", (unsigned)sessionId);
        ClearSessionDetails(sessionId);
        MOT_LOG_DEBUG("SessionCleanup(): Calling DestroySessionJitContext()");
        DestroySessionJitContexts();
        if (MOTAdaptor::m_engine) {
            MOT::SessionContext* sessionContext = MOT::GetSessionManager()->GetSessionContext(sessionId);
            if (sessionContext != nullptr) {
                DestroySession(sessionContext);
            }
            // since a call to on_proc_exit(destroyTxn) was probably missing, we should also cleanup thread-locals
            // pay attention that if we got here it means the thread pool is disabled, so we must ensure thread-locals
            // are cleaned up right now. Due to these complexities, onCurrentThreadEnding() was designed to be proof
            // for repeated calls.
            MOTAdaptor::m_engine->OnCurrentThreadEnding();
        }
    }
}

static void InitSessionCleanup()
{
    pthread_key_create(&sessionCleanupKey, SessionCleanup);
}

static void DestroySessionCleanup()
{
    pthread_key_delete(sessionCleanupKey);
}

static void ScheduleSessionCleanup()
{
    pthread_setspecific(sessionCleanupKey, (const void*)(uint64_t)(u_sess->mot_cxt.session_id + 1));
}

static void CancelSessionCleanup()
{
    pthread_setspecific(sessionCleanupKey, nullptr);
}

static GaussdbConfigLoader* gaussdbConfigLoader = nullptr;

bool MOTAdaptor::m_initialized = false;
bool MOTAdaptor::m_callbacks_initialized = false;

static void WakeupWalWriter()
{
    if (g_instance.proc_base->walwriterLatch != nullptr) {
        SetLatch(g_instance.proc_base->walwriterLatch);
    }
}

void MOTAdaptor::Init()
{
    if (m_initialized) {
        // This is highly unexpected, and should especially be guarded in scenario of switch-over to standby.
        elog(FATAL, "Double attempt to initialize MOT engine, it is already initialized");
    }

    m_engine = MOT::MOTEngine::CreateInstanceNoInit(g_instance.attr.attr_common.MOTConfigFileName, 0, nullptr);
    if (m_engine == nullptr) {
        elog(FATAL, "Failed to create MOT engine");
    }

    MOT::MOTConfiguration& motCfg = MOT::GetGlobalConfiguration();
    motCfg.SetTotalMemoryMb(g_instance.attr.attr_memory.max_process_memory / KILO_BYTE);

    gaussdbConfigLoader = new (std::nothrow) GaussdbConfigLoader();
    if (gaussdbConfigLoader == nullptr) {
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to allocate memory for GaussDB/MOTEngine configuration loader.");
    }
    MOT_LOG_TRACE("Adding external configuration loader for GaussDB");
    if (!m_engine->AddConfigLoader(gaussdbConfigLoader)) {
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to add GaussDB/MOTEngine configuration loader");
    }

    if (!m_engine->LoadConfig()) {
        m_engine->RemoveConfigLoader(gaussdbConfigLoader);
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to load configuration for MOT engine.");
    }

    // Check max process memory here - we do it anyway to protect ourselves from miscalculations.
    // Attention: the following values are configured during the call to MOTEngine::LoadConfig() just above
    uint64_t globalMemoryKb = MOT::g_memGlobalCfg.m_maxGlobalMemoryMb * KILO_BYTE;
    uint64_t localMemoryKb = MOT::g_memGlobalCfg.m_maxLocalMemoryMb * KILO_BYTE;
    uint64_t maxReserveMemoryKb = globalMemoryKb + localMemoryKb;

    // check whether the 2GB gap between MOT and envelope is still kept
    if ((g_instance.attr.attr_memory.max_process_memory < (int32)maxReserveMemoryKb) ||
        ((g_instance.attr.attr_memory.max_process_memory - maxReserveMemoryKb) < MIN_DYNAMIC_PROCESS_MEMORY)) {
        // we allow one extreme case: GaussDB is configured to its limit, and zero memory is left for us
        if (maxReserveMemoryKb <= motCfg.MOT_MIN_MEMORY_USAGE_MB * KILO_BYTE) {
            MOT_LOG_WARN("Allowing MOT to work in minimal memory mode");
        } else {
            m_engine->RemoveConfigLoader(gaussdbConfigLoader);
            delete gaussdbConfigLoader;
            gaussdbConfigLoader = nullptr;
            MOT::MOTEngine::DestroyInstance();
            elog(FATAL,
                "The value of pre-reserved memory for MOT engine is not reasonable: "
                "Request for a maximum of %" PRIu64 " KB global memory, and %" PRIu64
                " KB session memory (total of %" PRIu64 " KB) is invalid since max_process_memory is %u KB",
                globalMemoryKb,
                localMemoryKb,
                maxReserveMemoryKb,
                g_instance.attr.attr_memory.max_process_memory);
        }
    }

    if (!m_engine->Initialize()) {
        m_engine->RemoveConfigLoader(gaussdbConfigLoader);
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to initialize MOT engine.");
    }

    if (!JitExec::JitStatisticsProvider::CreateInstance()) {
        m_engine->RemoveConfigLoader(gaussdbConfigLoader);
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
        MOT::MOTEngine::DestroyInstance();
        elog(FATAL, "Failed to initialize JIT statistics.");
    }

    // make sure current thread is cleaned up properly when thread pool is enabled
    EnsureSafeThreadAccessInline();

    if (motCfg.m_enableRedoLog && motCfg.m_loggerType == MOT::LoggerType::EXTERNAL_LOGGER) {
        m_engine->GetRedoLogHandler()->SetLogger(&xlogger);
        m_engine->GetRedoLogHandler()->SetWalWakeupFunc(WakeupWalWriter);
    }

    InitSessionDetailsMap();
    if (!g_instance.attr.attr_common.enable_thread_pool) {
        InitSessionCleanup();
    }
    InitDataNodeId();
    InitKeyOperStateMachine();
    m_initialized = true;
}

void MOTAdaptor::NotifyConfigChange()
{
    if (gaussdbConfigLoader != nullptr) {
        gaussdbConfigLoader->MarkChanged();
    }
}

void MOTAdaptor::InitDataNodeId()
{
    MOT::GetGlobalConfiguration().SetPgNodes(1, 1);
}

void MOTAdaptor::Destroy()
{
    if (!m_initialized) {
        return;
    }

    JitExec::JitStatisticsProvider::DestroyInstance();
    if (!g_instance.attr.attr_common.enable_thread_pool) {
        DestroySessionCleanup();
    }
    DestroySessionDetailsMap();
    if (gaussdbConfigLoader != nullptr) {
        m_engine->RemoveConfigLoader(gaussdbConfigLoader);
        delete gaussdbConfigLoader;
        gaussdbConfigLoader = nullptr;
    }

    EnsureSafeThreadAccessInline();
    MOT::MOTEngine::DestroyInstance();
    m_engine = nullptr;
    knl_thread_mot_init();  // reset all thread-locals, mandatory for standby switch-over
    m_initialized = false;
}

MOT::TxnManager* MOTAdaptor::InitTxnManager(
    const char* callerSrc, MOT::ConnectionId connection_id /* = INVALID_CONNECTION_ID */)
{
    if (!u_sess->mot_cxt.txn_manager) {
        bool attachCleanFunc =
            (MOTCurrThreadId == INVALID_THREAD_ID ? true : !g_instance.attr.attr_common.enable_thread_pool);

        // First time we handle this connection
        if (m_engine == nullptr) {
            elog(ERROR, "initTxnManager: MOT engine is not initialized");
            return nullptr;
        }

        // create new session context
        MOT::SessionContext* session_ctx =
            MOT::GetSessionManager()->CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, connection_id);
        if (session_ctx == nullptr) {
            MOT_REPORT_ERROR(
                MOT_ERROR_INTERNAL, "Session Initialization", "Failed to create session context in %s", callerSrc);
            ereport(ERROR, (errmsg("Session startup: failed to create session context.")));
            return nullptr;
        }
        MOT_ASSERT(u_sess->mot_cxt.session_context == session_ctx);
        MOT_ASSERT(u_sess->mot_cxt.session_id == session_ctx->GetSessionId());
        MOT_ASSERT(u_sess->mot_cxt.connection_id == session_ctx->GetConnectionId());

        // make sure we cleanup leftovers from other session
        u_sess->mot_cxt.jit_context_count = 0;

        // record session details for statistics report
        RecordSessionDetails();

        if (attachCleanFunc) {
            // schedule session cleanup when thread pool is not used
            if (!g_instance.attr.attr_common.enable_thread_pool) {
                on_proc_exit(DestroyTxn, PointerGetDatum(session_ctx));
                ScheduleSessionCleanup();
            } else {
                on_proc_exit(MOTCleanupThread, PointerGetDatum(nullptr));
                MOT_LOG_DEBUG("Registered current thread for proc-exit callback for thread %p", (void*)pthread_self());
            }
        }

        u_sess->mot_cxt.txn_manager = session_ctx->GetTxnManager();
        elog(DEBUG1, "Init TXN_MAN for thread %u", MOTCurrThreadId);
    }

    return u_sess->mot_cxt.txn_manager;
}

static void DestroySessionJitContexts()
{
    // we must release all JIT context objects associated with this session now.
    // it seems that when thread pool is disabled, all cached plan sources for the session are not
    // released explicitly, but rather implicitly as part of the release of the memory context of the session.
    // in any case, we guard against repeated destruction of the JIT context by nullifying it
    MOT_LOG_DEBUG("Cleaning up all JIT context objects for current session");
    CachedPlanSource* psrc = u_sess->pcache_cxt.first_saved_plan;
    while (psrc != nullptr) {
        if (psrc->mot_jit_context != nullptr) {
            MOT_LOG_DEBUG("DestroySessionJitContexts(): Calling DestroyJitContext(%p)", psrc->mot_jit_context);
            JitExec::DestroyJitContext(psrc->mot_jit_context);
            psrc->mot_jit_context = nullptr;
        }
        psrc = psrc->next_saved;
    }
    MOT_LOG_DEBUG("DONE Cleaning up all JIT context objects for current session");
}

/** @brief Notification from thread pool that a session ended (only when thread pool is ENABLED). */
extern void MOTOnSessionClose()
{
    MOT_LOG_TRACE("Received session close notification (current session id: %u, current connection id: %u)",
        u_sess->mot_cxt.session_id,
        u_sess->mot_cxt.connection_id);
    if (u_sess->mot_cxt.session_id != INVALID_SESSION_ID) {
        ClearCurrentSessionDetails();
        MOT_LOG_DEBUG("MOTOnSessionClose(): Calling DestroySessionJitContexts()");
        DestroySessionJitContexts();
        if (!MOTAdaptor::m_engine) {
            MOT_LOG_ERROR("MOTOnSessionClose(): MOT engine is not initialized");
        } else {
            EnsureSafeThreadAccessInline();  // this is ok, it wil be cleaned up when thread exits
            MOT::SessionContext* sessionContext = u_sess->mot_cxt.session_context;
            if (sessionContext == nullptr) {
                MOT_LOG_WARN("Received session close notification, but no current session is found. Current session id "
                             "is %u. Request ignored.",
                    u_sess->mot_cxt.session_id);
            } else {
                DestroySession(sessionContext);
                MOT_ASSERT(u_sess->mot_cxt.session_id == INVALID_SESSION_ID);
            }
        }
    }
}

/** @brief Notification from thread pool that a pooled thread ended (only when thread pool is ENABLED). */
static void MOTOnThreadShutdown()
{
    if (!MOTAdaptor::m_initialized) {
        return;
    }

    MOT_LOG_TRACE("Received thread shutdown notification");
    if (!MOTAdaptor::m_engine) {
        MOT_LOG_ERROR("MOTOnThreadShutdown(): MOT engine is not initialized");
    } else {
        MOTAdaptor::m_engine->OnCurrentThreadEnding();
    }
    knl_thread_mot_init();  // reset all thread locals
}

/**
 * @brief on_proc_exit() callback to handle thread-cleanup - regardless of whether thread pool is enabled or not.
 * registration to on_proc_exit() is triggered by first call to EnsureSafeThreadAccessInline().
 */
static void MOTCleanupThread(int status, Datum ptr)
{
    MOT_ASSERT(g_instance.attr.attr_common.enable_thread_pool);
    MOT_LOG_TRACE("Received thread cleanup notification (thread-pool ON)");

    // when thread pool is used we just cleanup current thread
    // this might be a duplicate because thread pool also calls MOTOnThreadShutdown() - this is still ok
    // because we guard against repeated calls in MOTEngine::onCurrentThreadEnding()
    MOTOnThreadShutdown();
}

void MOTAdaptor::DestroyTxn(int status, Datum ptr)
{
    MOT_ASSERT(!g_instance.attr.attr_common.enable_thread_pool);

    // cleanup session
    if (!g_instance.attr.attr_common.enable_thread_pool) {
        CancelSessionCleanup();
    }
    ClearCurrentSessionDetails();
    MOT_LOG_DEBUG("DestroyTxn(): Calling DestroySessionJitContexts()");
    DestroySessionJitContexts();
    MOT::SessionContext* session = (MOT::SessionContext*)DatumGetPointer(ptr);
    if (m_engine == nullptr) {
        elog(ERROR, "destroyTxn: MOT engine is not initialized");
    }

    if (session != MOT_GET_CURRENT_SESSION_CONTEXT()) {
        MOT_LOG_WARN("Ignoring request to delete session context: already deleted");
    } else if (session != nullptr) {
        elog(DEBUG1, "Destroy SessionContext, connection_id = %u \n", session->GetConnectionId());
        EnsureSafeThreadAccessInline();  // may be accessed from new thread pool worker
        MOT::GcManager* gc = MOT_GET_CURRENT_SESSION_CONTEXT()->GetTxnManager()->GetGcSession();
        if (gc != nullptr) {
            gc->GcEndTxn();
        }
        DestroySession(session);
    }

    // clean up thread
    MOTOnThreadShutdown();
}

MOT::RC MOTAdaptor::ValidateCommit()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    if (!IS_PGXC_COORDINATOR) {
        return txn->ValidateCommit();
    } else {
        // Nothing to do in coordinator
        return MOT::RC_OK;
    }
}

void MOTAdaptor::RecordCommit(uint64_t csn)
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetCommitSequenceNumber(csn);
    if (!IS_PGXC_COORDINATOR) {
        txn->RecordCommit();
    } else {
        txn->LiteCommit();
    }
}

MOT::RC MOTAdaptor::Commit(uint64_t csn)
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetCommitSequenceNumber(csn);
    if (!IS_PGXC_COORDINATOR) {
        return txn->Commit();
    } else {
        txn->LiteCommit();
        return MOT::RC_OK;
    }
}

void MOTAdaptor::EndTransaction()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    // Nothing to do in coordinator
    if (!IS_PGXC_COORDINATOR) {
        txn->EndTransaction();
    }
}

void MOTAdaptor::Rollback()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    if (!IS_PGXC_COORDINATOR) {
        txn->Rollback();
    } else {
        txn->LiteRollback();
    }
}

MOT::RC MOTAdaptor::Prepare()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    if (!IS_PGXC_COORDINATOR) {
        return txn->Prepare();
    } else {
        txn->LitePrepare();
        return MOT::RC_OK;
    }
}

void MOTAdaptor::CommitPrepared(uint64_t csn)
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetCommitSequenceNumber(csn);
    if (!IS_PGXC_COORDINATOR) {
        txn->CommitPrepared();
    } else {
        txn->LiteCommitPrepared();
    }
}

void MOTAdaptor::RollbackPrepared()
{
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    if (!IS_PGXC_COORDINATOR) {
        txn->RollbackPrepared();
    } else {
        txn->LiteRollbackPrepared();
    }
}

MOT::RC MOTAdaptor::InsertRow(MOTFdwStateSt* fdwState, TupleTableSlot* slot)
{
    EnsureSafeThreadAccessInline();
    uint8_t* newRowData = nullptr;
    fdwState->m_currTxn->SetTransactionId(fdwState->m_txnId);
    MOT::Table* table = fdwState->m_table;
    MOT::Row* row = table->CreateNewRow();
    if (row == nullptr) {
        MOT_REPORT_ERROR(
            MOT_ERROR_OOM, "Insert Row", "Failed to create new row for table %s", table->GetLongTableName().c_str());
        return MOT::RC_MEMORY_ALLOCATION_ERROR;
    }
    newRowData = const_cast<uint8_t*>(row->GetData());
    PackRow(slot, table, fdwState->m_attrsUsed, newRowData);

    MOT::RC res = table->InsertRow(row, fdwState->m_currTxn);
    if ((res != MOT::RC_OK) && (res != MOT::RC_UNIQUE_VIOLATION)) {
        MOT_REPORT_ERROR(
            MOT_ERROR_OOM, "Insert Row", "Failed to insert new row for table %s", table->GetLongTableName().c_str());
    }
    return res;
}

MOT::RC MOTAdaptor::UpdateRow(MOTFdwStateSt* fdwState, TupleTableSlot* slot, MOT::Row* currRow)
{
    EnsureSafeThreadAccessInline();
    MOT::RC rc;

    do {
        fdwState->m_currTxn->SetTransactionId(fdwState->m_txnId);
        rc = fdwState->m_currTxn->UpdateLastRowState(MOT::AccessType::WR);
        if (rc != MOT::RC::RC_OK) {
            break;
        }
        uint8_t* rowData = const_cast<uint8_t*>(currRow->GetData());
        PackUpdateRow(slot, fdwState->m_table, fdwState->m_attrsModified, rowData);
        MOT::BitmapSet modified_columns(fdwState->m_attrsModified, fdwState->m_table->GetFieldCount() - 1);

        rc = fdwState->m_currTxn->OverwriteRow(currRow, modified_columns);
    } while (0);

    return rc;
}

MOT::RC MOTAdaptor::DeleteRow(MOTFdwStateSt* fdwState, TupleTableSlot* slot)
{
    EnsureSafeThreadAccessInline();
    fdwState->m_currTxn->SetTransactionId(fdwState->m_txnId);
    MOT::RC rc = fdwState->m_currTxn->DeleteLastRow();
    return rc;
}

// NOTE: colId starts from 1
bool MOTAdaptor::SetMatchingExpr(
    MOTFdwStateSt* state, MatchIndexArr* marr, int16_t colId, KEY_OPER op, Expr* expr, Expr* parent, bool set_local)
{
    bool res = false;
    uint16_t numIx = state->m_table->GetNumIndexes();

    for (uint16_t i = 0; i < numIx; i++) {
        MOT::Index* ix = state->m_table->GetIndex(i);
        if (ix != nullptr && ix->IsFieldPresent(colId)) {
            if (marr->m_idx[i] == nullptr) {
                marr->m_idx[i] = (MatchIndex*)palloc0(sizeof(MatchIndex));
                marr->m_idx[i]->Init();
                marr->m_idx[i]->m_ix = ix;
            }

            res |= marr->m_idx[i]->SetIndexColumn(state, colId, op, expr, parent, set_local);
        }
    }

    return res;
}

MatchIndex* MOTAdaptor::GetBestMatchIndex(MOTFdwStateSt* festate, MatchIndexArr* marr, int numClauses, bool setLocal)
{
    MatchIndex* best = nullptr;
    double bestCost = INT_MAX;
    uint16_t numIx = festate->m_table->GetNumIndexes();
    uint16_t bestI = (uint16_t)-1;

    for (uint16_t i = 0; i < numIx; i++) {
        if (marr->m_idx[i] != nullptr && marr->m_idx[i]->IsUsable()) {
            double cost = marr->m_idx[i]->GetCost(numClauses);
            if (cost < bestCost) {
                if (bestI < MAX_NUM_INDEXES) {
                    if (marr->m_idx[i]->GetNumMatchedCols() < marr->m_idx[bestI]->GetNumMatchedCols())
                        continue;
                }
                bestCost = cost;
                bestI = i;
            }
        }
    }

    if (bestI < MAX_NUM_INDEXES) {
        best = marr->m_idx[bestI];
        for (int k = 0; k < 2; k++) {
            for (int j = 0; j < best->m_ix->GetNumFields(); j++) {
                if (best->m_colMatch[k][j]) {
                    if (best->m_opers[k][j] < KEY_OPER::READ_INVALID) {
                        best->m_params[k][j] = AddParam(&best->m_remoteConds, best->m_colMatch[k][j]);
                        if (!list_member(best->m_remoteCondsOrig, best->m_parentColMatch[k][j])) {
                            best->m_remoteCondsOrig = lappend(best->m_remoteCondsOrig, best->m_parentColMatch[k][j]);
                        }

                        if (j > 0 && best->m_opers[k][j - 1] != KEY_OPER::READ_KEY_EXACT &&
                            !list_member(festate->m_localConds, best->m_parentColMatch[k][j])) {
                            if (setLocal)
                                festate->m_localConds = lappend(festate->m_localConds, best->m_parentColMatch[k][j]);
                        }
                    } else if (!list_member(festate->m_localConds, best->m_parentColMatch[k][j]) &&
                               !list_member(best->m_remoteCondsOrig, best->m_parentColMatch[k][j])) {
                        if (setLocal)
                            festate->m_localConds = lappend(festate->m_localConds, best->m_parentColMatch[k][j]);
                        best->m_colMatch[k][j] = nullptr;
                        best->m_parentColMatch[k][j] = nullptr;
                    }
                }
            }
        }
    }

    for (uint16_t i = 0; i < numIx; i++) {
        if (marr->m_idx[i] != nullptr) {
            MatchIndex* mix = marr->m_idx[i];
            if (i != bestI) {
                if (setLocal) {
                    for (int k = 0; k < 2; k++) {
                        for (int j = 0; j < mix->m_ix->GetNumFields(); j++) {
                            if (mix->m_colMatch[k][j] &&
                                !list_member(festate->m_localConds, mix->m_parentColMatch[k][j]) &&
                                !(best != nullptr &&
                                    list_member(best->m_remoteCondsOrig, mix->m_parentColMatch[k][j]))) {
                                festate->m_localConds = lappend(festate->m_localConds, mix->m_parentColMatch[k][j]);
                            }
                        }
                    }
                }
                pfree(mix);
                marr->m_idx[i] = nullptr;
            }
        }
    }
    if (best != nullptr && best->m_ix != nullptr) {
        for (uint16_t i = 0; i < numIx; i++) {
            if (best->m_ix == festate->m_table->GetIndex(i)) {
                best->m_ixPosition = i;
                break;
            }
        }
    }

    return best;
}

void MOTAdaptor::OpenCursor(Relation rel, MOTFdwStateSt* festate)
{
    bool matchKey = true;
    bool forwardDirection = true;
    bool found = false;

    EnsureSafeThreadAccessInline();

    // GetTableByExternalId cannot return nullptr at this stage, because it is protected by envelope's table lock.
    festate->m_table = festate->m_currTxn->GetTableByExternalId(rel->rd_id);

    do {
        // this scan all keys case
        // we need to open both cursors on start and end to prevent
        // infinite scan in case "insert into table A ... as select * from table A ...
        if (festate->m_bestIx == nullptr) {
            int fIx, bIx;
            uint8_t* buf = nullptr;
            // assumption that primary index cannot be changed, can take it from
            // table and not look on ddl_access
            MOT::Index* ix = festate->m_table->GetPrimaryIndex();
            uint16_t keyLength = ix->GetKeyLength();

            if (festate->m_order == SORTDIR_ENUM::SORTDIR_ASC) {
                fIx = 0;
                bIx = 1;
                festate->m_forwardDirectionScan = true;
            } else {
                fIx = 1;
                bIx = 0;
                festate->m_forwardDirectionScan = false;
            }

            festate->m_cursor[fIx] = festate->m_table->Begin(festate->m_currTxn->GetThdId());

            festate->m_stateKey[bIx].InitKey(keyLength);
            buf = festate->m_stateKey[bIx].GetKeyBuf();
            errno_t erc = memset_s(buf, keyLength, 0xff, keyLength);
            securec_check(erc, "\0", "\0");
            festate->m_cursor[bIx] =
                ix->Search(&festate->m_stateKey[bIx], false, false, festate->m_currTxn->GetThdId(), found);
            break;
        }

        for (int i = 0; i < 2; i++) {
            if (i == 1 && festate->m_bestIx->m_end < 0) {
                if (festate->m_forwardDirectionScan) {
                    uint8_t* buf = nullptr;
                    MOT::Index* ix = festate->m_bestIx->m_ix;
                    uint16_t keyLength = ix->GetKeyLength();

                    festate->m_stateKey[1].InitKey(keyLength);
                    buf = festate->m_stateKey[1].GetKeyBuf();
                    errno_t erc = memset_s(buf, keyLength, 0xff, keyLength);
                    securec_check(erc, "\0", "\0");
                    festate->m_cursor[1] =
                        ix->Search(&festate->m_stateKey[1], false, false, festate->m_currTxn->GetThdId(), found);
                } else {
                    festate->m_cursor[1] = festate->m_bestIx->m_ix->Begin(festate->m_currTxn->GetThdId());
                }
                break;
            }

            KEY_OPER oper = (i == 0 ? festate->m_bestIx->m_ixOpers[0] : festate->m_bestIx->m_ixOpers[1]);

            forwardDirection = ((oper & ~KEY_OPER_PREFIX_BITMASK) < KEY_OPER::READ_KEY_OR_PREV);

            CreateKeyBuffer(rel, festate, i);

            if (i == 0) {
                festate->m_forwardDirectionScan = forwardDirection;
            }

            switch (oper) {
                case KEY_OPER::READ_KEY_EXACT:
                case KEY_OPER::READ_KEY_OR_NEXT:
                case KEY_OPER::READ_KEY_LIKE:
                case KEY_OPER::READ_PREFIX_LIKE:
                case KEY_OPER::READ_PREFIX:
                case KEY_OPER::READ_PREFIX_OR_NEXT:
                    matchKey = true;
                    forwardDirection = true;
                    break;

                case KEY_OPER::READ_KEY_AFTER:
                case KEY_OPER::READ_PREFIX_AFTER:
                    matchKey = false;
                    forwardDirection = true;
                    break;

                case KEY_OPER::READ_KEY_OR_PREV:
                case KEY_OPER::READ_PREFIX_OR_PREV:
                    matchKey = true;
                    forwardDirection = false;
                    break;

                case KEY_OPER::READ_KEY_BEFORE:
                case KEY_OPER::READ_PREFIX_BEFORE:
                    matchKey = false;
                    forwardDirection = false;
                    break;

                default:
                    elog(INFO, "Invalid key operation: %u", oper);
                    break;
            }

            festate->m_cursor[i] = festate->m_bestIx->m_ix->Search(
                &festate->m_stateKey[i], matchKey, forwardDirection, festate->m_currTxn->GetThdId(), found);

            if (!found && oper == KEY_OPER::READ_KEY_EXACT && festate->m_bestIx->m_ix->GetUnique()) {
                festate->m_cursor[i]->Invalidate();
                festate->m_cursor[i]->Destroy();
                delete festate->m_cursor[i];
                festate->m_cursor[i] = nullptr;
            }
        }
    } while (0);
}

static void VarLenFieldType(
    Form_pg_type typeDesc, Oid typoid, int32_t colLen, int16* typeLen, bool& isBlob, MOT::RC& res)
{
    isBlob = false;
    res = MOT::RC_OK;
    if (typeDesc->typlen < 0) {
        *typeLen = colLen;
        switch (typeDesc->typstorage) {
            case 'p':
                break;
            case 'x':
            case 'm':
                if (typoid == NUMERICOID) {
                    *typeLen = DECIMAL_MAX_SIZE;
                    break;
                }
                /* fall through */
            case 'e':
#ifdef USE_ASSERT_CHECKING
                if (typoid == TEXTOID)
                    *typeLen = colLen = MAX_VARCHAR_LEN;
#endif
                if (colLen > MAX_VARCHAR_LEN || colLen < 0) {
                    res = MOT::RC_COL_SIZE_INVALID;
                } else {
                    isBlob = true;
                }
                break;
            default:
                break;
        }
    }
}

static MOT::RC TableFieldType(
    const ColumnDef* colDef, MOT::MOT_CATALOG_FIELD_TYPES& type, int16* typeLen, Oid& typoid, bool& isBlob)
{
    MOT::RC res = MOT::RC_OK;
    Type tup;
    Form_pg_type typeDesc;
    int32_t colLen;

    if (colDef->typname->arrayBounds != nullptr) {
        return MOT::RC_UNSUPPORTED_COL_TYPE_ARR;
    }

    tup = typenameType(nullptr, colDef->typname, &colLen);
    typeDesc = ((Form_pg_type)GETSTRUCT(tup));
    typoid = HeapTupleGetOid(tup);
    *typeLen = typeDesc->typlen;

    // Get variable-length field length.
    VarLenFieldType(typeDesc, typoid, colLen, typeLen, isBlob, res);

    switch (typoid) {
        case CHAROID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_CHAR;
            break;
        case INT1OID:
        case BOOLOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TINY;
            break;
        case INT2OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_SHORT;
            break;
        case INT4OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_INT;
            break;
        case INT8OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_LONG;
            break;
        case DATEOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DATE;
            break;
        case TIMEOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TIME;
            break;
        case TIMESTAMPOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TIMESTAMP;
            break;
        case TIMESTAMPTZOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TIMESTAMPTZ;
            break;
        case INTERVALOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_INTERVAL;
            break;
        case TINTERVALOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TINTERVAL;
            break;
        case TIMETZOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_TIMETZ;
            break;
        case FLOAT4OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_FLOAT;
            break;
        case FLOAT8OID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DOUBLE;
            break;
        case NUMERICOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DECIMAL;
            break;
        case VARCHAROID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_VARCHAR;
            break;
        case BPCHAROID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_VARCHAR;
            break;
        case TEXTOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_VARCHAR;
            break;
        case CLOBOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_BLOB;
            break;
        case BYTEAOID:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_VARCHAR;
            break;
        default:
            type = MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_UNKNOWN;
            res = MOT::RC_UNSUPPORTED_COL_TYPE;
    }

    if (tup) {
        ReleaseSysCache(tup);
    }

    return res;
}

void MOTAdaptor::ValidateCreateIndex(IndexStmt* stmt, MOT::Table* table, MOT::TxnManager* txn)
{
    if (stmt->primary) {
        if (!table->IsTableEmpty(txn->GetThdId())) {
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FDW_ERROR),
                    errmsg(
                        "Table %s is not empty, create primary index is not allowed", table->GetTableName().c_str())));
            return;
        }
    } else if (table->GetNumIndexes() == MAX_NUM_INDEXES) {
        ereport(ERROR,
            (errmodule(MOD_MOT),
                errcode(ERRCODE_FDW_TOO_MANY_INDEXES),
                errmsg("Can not create index, max number of indexes %u reached", MAX_NUM_INDEXES)));
        return;
    }

    if (strcmp(stmt->accessMethod, "btree") != 0) {
        ereport(ERROR, (errmodule(MOD_MOT), errmsg("MOT supports indexes of type BTREE only (btree or btree_art)")));
        return;
    }

    if (list_length(stmt->indexParams) > (int)MAX_KEY_COLUMNS) {
        ereport(ERROR,
            (errmodule(MOD_MOT),
                errcode(ERRCODE_FDW_TOO_MANY_INDEX_COLUMNS),
                errmsg("Can't create index"),
                errdetail(
                    "Number of columns exceeds %d max allowed %u", list_length(stmt->indexParams), MAX_KEY_COLUMNS)));
        return;
    }
}

MOT::RC MOTAdaptor::CreateIndex(IndexStmt* stmt, ::TransactionId tid)
{
    MOT::RC res;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);
    MOT::Table* table = txn->GetTableByExternalId(stmt->relation->foreignOid);

    if (table == nullptr) {
        ereport(ERROR,
            (errmodule(MOD_MOT),
                errcode(ERRCODE_UNDEFINED_TABLE),
                errmsg("Table not found for oid %u", stmt->relation->foreignOid)));
        return MOT::RC_ERROR;
    }

    ValidateCreateIndex(stmt, table, txn);

    elog(LOG,
        "creating %s index %s (OID: %u), for table: %s",
        (stmt->primary ? "PRIMARY" : "SECONDARY"),
        stmt->idxname,
        stmt->indexOid,
        stmt->relation->relname);
    uint64_t keyLength = 0;
    MOT::Index* index = nullptr;
    MOT::IndexOrder index_order = MOT::IndexOrder::INDEX_ORDER_SECONDARY;

    // Use the default index tree flavor from configuration file
    MOT::IndexingMethod indexing_method = MOT::IndexingMethod::INDEXING_METHOD_TREE;
    MOT::IndexTreeFlavor flavor = MOT::GetGlobalConfiguration().m_indexTreeFlavor;

    // check if we have primary and delete previous definition
    if (stmt->primary) {
        index_order = MOT::IndexOrder::INDEX_ORDER_PRIMARY;
    }

    index = MOT::IndexFactory::CreateIndex(index_order, indexing_method, flavor);
    if (index == nullptr) {
        report_pg_error(MOT::RC_ABORT);
        return MOT::RC_ABORT;
    }
    index->SetExtId(stmt->indexOid);
    index->SetNumTableFields((uint32_t)table->GetFieldCount());
    int count = 0;

    ListCell* lc = nullptr;
    foreach (lc, stmt->indexParams) {
        IndexElem* ielem = (IndexElem*)lfirst(lc);

        uint64_t colid = table->GetFieldId((ielem->name != nullptr ? ielem->name : ielem->indexcolname));
        if (colid == (uint64_t)-1) {  // invalid column
            delete index;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
                    errmsg("Can't create index on field"),
                    errdetail("Specified column not found in table definition")));
            return MOT::RC_ERROR;
        }

        MOT::Column* col = table->GetField(colid);

        // Temp solution for NULLs, do not allow index creation on column that does not carry not null flag
        if (!MOT::GetGlobalConfiguration().m_allowIndexOnNullableColumn && !col->m_isNotNull) {
            delete index;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FDW_INDEX_ON_NULLABLE_COLUMN_NOT_ALLOWED),
                    errmsg("Can't create index on nullable columns"),
                    errdetail("Column %s is nullable", col->m_name)));
            return MOT::RC_ERROR;
        }

        // Temp solution, we have to support DECIMAL and NUMERIC indexes as well
        if (col->m_type == MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DECIMAL) {
            delete index;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Can't create index on field"),
                    errdetail("INDEX on NUMERIC or DECIMAL fields not supported yet")));
            return MOT::RC_ERROR;
        }
        if (col->m_keySize > MAX_KEY_SIZE) {
            delete index;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
                    errmsg("Can't create index on field"),
                    errdetail("Column size is greater than maximum index size")));
            return MOT::RC_ERROR;
        }
        keyLength += col->m_keySize;

        index->SetLenghtKeyFields(count, colid, col->m_keySize);
        count++;
    }

    index->SetNumIndexFields(count);

    if ((res = index->IndexInit(keyLength, stmt->unique, stmt->idxname, nullptr)) != MOT::RC_OK) {
        delete index;
        report_pg_error(res);
        return res;
    }

    res = txn->CreateIndex(table, index, stmt->primary);
    if (res != MOT::RC_OK) {
        delete index;
        if (res == MOT::RC_TABLE_EXCEEDS_MAX_INDEXES) {
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FDW_TOO_MANY_INDEXES),
                    errmsg("Can not create index, max number of indexes %u reached", MAX_NUM_INDEXES)));
            return MOT::RC_TABLE_EXCEEDS_MAX_INDEXES;
        } else {
            report_pg_error(txn->m_err, stmt->idxname, txn->m_errMsgBuf);
            return MOT::RC_UNIQUE_VIOLATION;
        }
    }

    return MOT::RC_OK;
}

void MOTAdaptor::AddTableColumns(MOT::Table* table, List *tableElts, bool& hasBlob)
{
    hasBlob = false;
    ListCell* cell = nullptr;
    foreach (cell, tableElts) {
        int16 typeLen = 0;
        bool isBlob = false;
        MOT::MOT_CATALOG_FIELD_TYPES colType;
        ColumnDef* colDef = (ColumnDef*)lfirst(cell);

        if (colDef == nullptr || colDef->typname == nullptr) {
            delete table;
            table = nullptr;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
                    errmsg("Column definition is not complete"),
                    errdetail("target table is a foreign table")));
            break;
        }

        Oid typoid = InvalidOid;
        MOT::RC res = TableFieldType(colDef, colType, &typeLen, typoid, isBlob);
        if (res != MOT::RC_OK) {
            delete table;
            table = nullptr;
            report_pg_error(res, colDef, (void*)(int64)typeLen);
            break;
        }
        hasBlob |= isBlob;

        if (colType == MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_DECIMAL) {
            if (list_length(colDef->typname->typmods) > 0) {
                bool canMakeShort = true;
                int precision = 0;
                int scale = 0;
                int count = 0;

                ListCell* c = nullptr;
                foreach (c, colDef->typname->typmods) {
                    Node* d = (Node*)lfirst(c);
                    if (!IsA(d, A_Const)) {
                        canMakeShort = false;
                        break;
                    }
                    A_Const* ac = (A_Const*)d;

                    if (ac->val.type != T_Integer) {
                        canMakeShort = false;
                        break;
                    }

                    if (count == 0) {
                        precision = ac->val.val.ival;
                    } else {
                        scale = ac->val.val.ival;
                    }

                    count++;
                }

                if (canMakeShort) {
                    int len = 0;

                    len += scale / DEC_DIGITS;
                    len += (scale % DEC_DIGITS > 0 ? 1 : 0);

                    precision -= scale;

                    len += precision / DEC_DIGITS;
                    len += (precision % DEC_DIGITS > 0 ? 1 : 0);

                    typeLen = sizeof(MOT::DecimalSt) + len * sizeof(NumericDigit);
                }
            }
        }

        res = table->AddColumn(colDef->colname, typeLen, colType, colDef->is_not_null, typoid);
        if (res != MOT::RC_OK) {
            delete table;
            table = nullptr;
            report_pg_error(res, colDef, (void*)(int64)typeLen);
            break;
        }
    }
}

MOT::RC MOTAdaptor::CreateTable(CreateForeignTableStmt* stmt, ::TransactionId tid)
{
    bool hasBlob = false;
    MOT::Index* primaryIdx = nullptr;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__, tid);
    MOT::Table* table = nullptr;
    MOT::RC res = MOT::RC_ERROR;
    std::string tname("");
    char* dbname = NULL;

    do {
        table = new (std::nothrow) MOT::Table();
        if (table == nullptr) {
            ereport(ERROR,
                (errmodule(MOD_MOT), errcode(ERRCODE_OUT_OF_MEMORY), errmsg("Allocation of table metadata failed")));
            break;
        }

        uint32_t columnCount = list_length(stmt->base.tableElts);

        // once the columns have been counted, we add one more for the nullable columns
        ++columnCount;

        // prepare table name
        dbname = get_database_name(u_sess->proc_cxt.MyDatabaseId);
        if (dbname == nullptr) {
            delete table;
            table = nullptr;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database with OID %u does not exist", u_sess->proc_cxt.MyDatabaseId)));
            break;
        }
        tname.append(dbname);
        tname.append("_");
        if (stmt->base.relation->schemaname != nullptr) {
            tname.append(stmt->base.relation->schemaname);
        } else {
            tname.append("#");
        }

        tname.append("_");
        tname.append(stmt->base.relation->relname);

        if (!table->Init(
                stmt->base.relation->relname, tname.c_str(), columnCount, stmt->base.relation->foreignOid)) {
            delete table;
            table = nullptr;
            report_pg_error(MOT::RC_MEMORY_ALLOCATION_ERROR);
            break;
        }

        // the null fields are copied verbatim because we have to give them back at some point
        res = table->AddColumn(
            "null_bytes", BITMAPLEN(columnCount - 1), MOT::MOT_CATALOG_FIELD_TYPES::MOT_TYPE_NULLBYTES);
        if (res != MOT::RC_OK) {
            delete table;
            table = nullptr;
            report_pg_error(MOT::RC_MEMORY_ALLOCATION_ERROR);
            break;
        }

        /*
         * Add all the columns.
         * NOTE: On failure, table object will be deleted and ereport will be done in AddTableColumns.
         */
        AddTableColumns(table, stmt->base.tableElts, hasBlob);

        table->SetFixedLengthRow(!hasBlob);

        uint32_t tupleSize = table->GetTupleSize();
        if (tupleSize > (unsigned int)MAX_TUPLE_SIZE) {
            delete table;
            table = nullptr;
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Un-support feature"),
                    errdetail("MOT: Table %s tuple size %u exceeds MAX_TUPLE_SIZE=%u !!!",
                        stmt->base.relation->relname,
                        tupleSize,
                        (unsigned int)MAX_TUPLE_SIZE)));
        }

        if (!table->InitRowPool()) {
            delete table;
            table = nullptr;
            report_pg_error(MOT::RC_MEMORY_ALLOCATION_ERROR);
            break;
        }

        elog(LOG,
            "creating table %s (OID: %u), num columns: %u, tuple: %u",
            table->GetLongTableName().c_str(),
            stmt->base.relation->foreignOid,
            columnCount,
            tupleSize);

        res = txn->CreateTable(table);
        if (res != MOT::RC_OK) {
            delete table;
            table = nullptr;
            report_pg_error(res);
            break;
        }

        // add default PK index
        primaryIdx = MOT::IndexFactory::CreatePrimaryIndexEx(MOT::IndexingMethod::INDEXING_METHOD_TREE,
            DEFAULT_TREE_FLAVOR,
            8,
            table->GetLongTableName(),
            res,
            nullptr);
        if (res != MOT::RC_OK) {
            txn->DropTable(table);
            report_pg_error(res);
            break;
        }
        primaryIdx->SetExtId(stmt->base.relation->foreignOid + 1);
        primaryIdx->SetNumTableFields(columnCount);
        primaryIdx->SetNumIndexFields(1);
        primaryIdx->SetLenghtKeyFields(0, -1, 8);
        primaryIdx->SetFakePrimary(true);

        // Add default primary index
        res = txn->CreateIndex(table, primaryIdx, true);
    } while (0);

    if (res != MOT::RC_OK) {
        if (table != nullptr) {
            txn->DropTable(table);
        }
        if (primaryIdx != nullptr) {
            delete primaryIdx;
        }
    }

    return res;
}

MOT::RC MOTAdaptor::DropIndex(DropForeignStmt* stmt, ::TransactionId tid)
{
    MOT::RC res = MOT::RC_OK;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);

    elog(LOG, "dropping index %s, ixoid: %u, taboid: %u", stmt->name, stmt->indexoid, stmt->reloid);

    // get table
    do {
        MOT::Index* index = txn->GetIndexByExternalId(stmt->reloid, stmt->indexoid);
        if (index == nullptr) {
            elog(LOG,
                "Drop index %s error, index oid %u of table oid %u not found.",
                stmt->name,
                stmt->indexoid,
                stmt->reloid);
            res = MOT::RC_INDEX_NOT_FOUND;
        } else if (index->IsPrimaryKey()) {
            elog(LOG, "Drop primary index is not supported, failed to drop index: %s", stmt->name);
        } else {
            MOT::Table* table = index->GetTable();
            uint64_t table_relid = table->GetTableExId();
            JitExec::PurgeJitSourceCache(table_relid, false);
            table->WrLock();
            res = txn->DropIndex(index);
            table->Unlock();
        }
    } while (0);

    return res;
}

MOT::RC MOTAdaptor::DropTable(DropForeignStmt* stmt, ::TransactionId tid)
{
    MOT::RC res = MOT::RC_OK;
    MOT::Table* tab = nullptr;
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);

    elog(LOG, "dropping table %s, oid: %u", stmt->name, stmt->reloid);
    do {
        tab = txn->GetTableByExternalId(stmt->reloid);
        if (tab == nullptr) {
            res = MOT::RC_TABLE_NOT_FOUND;
            elog(LOG, "Drop table %s error, table oid %u not found.", stmt->name, stmt->reloid);
        } else {
            uint64_t table_relid = tab->GetTableExId();
            JitExec::PurgeJitSourceCache(table_relid, false);
            res = txn->DropTable(tab);
        }
    } while (0);

    return res;
}

MOT::RC MOTAdaptor::TruncateTable(Relation rel, ::TransactionId tid)
{
    MOT::RC res = MOT::RC_OK;
    MOT::Table* tab = nullptr;

    EnsureSafeThreadAccessInline();

    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);

    elog(LOG, "truncating table %s, oid: %u", NameStr(rel->rd_rel->relname), rel->rd_id);
    do {
        tab = txn->GetTableByExternalId(rel->rd_id);
        if (tab == nullptr) {
            elog(LOG, "Truncate table %s error, table oid %u not found.", NameStr(rel->rd_rel->relname), rel->rd_id);
            break;
        }

        JitExec::PurgeJitSourceCache(rel->rd_id, true);
        tab->WrLock();
        res = txn->TruncateTable(tab);
        tab->Unlock();
    } while (0);

    return res;
}

MOT::RC MOTAdaptor::VacuumTable(Relation rel, ::TransactionId tid)
{
    MOT::RC res = MOT::RC_OK;
    MOT::Table* tab = nullptr;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    txn->SetTransactionId(tid);

    elog(LOG, "vacuuming table %s, oid: %u", NameStr(rel->rd_rel->relname), rel->rd_id);
    do {
        tab = MOT::GetTableManager()->GetTableSafeByExId(rel->rd_id);
        if (tab == nullptr) {
            elog(LOG, "Vacuum table %s error, table oid %u not found.", NameStr(rel->rd_rel->relname), rel->rd_id);
            break;
        }

        tab->Compact(txn);
        tab->Unlock();
    } while (0);
    return res;
}

uint64_t MOTAdaptor::GetTableIndexSize(uint64_t tabId, uint64_t ixId)
{
    uint64_t res = 0;
    EnsureSafeThreadAccessInline();
    MOT::TxnManager* txn = GetSafeTxn(__FUNCTION__);
    MOT::Table* tab = nullptr;
    MOT::Index* ix = nullptr;

    do {
        tab = txn->GetTableByExternalId(tabId);
        if (tab == nullptr) {
            ereport(ERROR,
                (errmodule(MOD_MOT),
                    errcode(ERRCODE_FDW_TABLE_NOT_FOUND),
                    errmsg("Get table size error, table oid %lu not found.", tabId)));
            break;
        }

        if (ixId > 0) {
            ix = tab->GetIndexByExtId(ixId);
            if (ix == nullptr) {
                ereport(ERROR,
                    (errmodule(MOD_MOT),
                        errcode(ERRCODE_FDW_TABLE_NOT_FOUND),
                        errmsg("Get index size error, index oid %lu for table oid %lu not found.", ixId, tabId)));
                break;
            }
            res = ix->GetIndexSize();
        } else
            res = tab->GetTableSize();
    } while (0);

    return res;
}

MotMemoryDetail* MOTAdaptor::GetMemSize(uint32_t* nodeCount, bool isGlobal)
{
    EnsureSafeThreadAccessInline();
    MotMemoryDetail* result = nullptr;
    *nodeCount = 0;

    /* We allocate an array of size (m_nodeCount + 1) to accommodate one aggregated entry of all global pools. */
    uint32_t statsArraySize = MOT::g_memGlobalCfg.m_nodeCount + 1;
    MOT::MemRawChunkPoolStats* chunkPoolStatsArray =
        (MOT::MemRawChunkPoolStats*)palloc(statsArraySize * sizeof(MOT::MemRawChunkPoolStats));
    if (chunkPoolStatsArray != nullptr) {
        errno_t erc = memset_s(chunkPoolStatsArray,
            statsArraySize * sizeof(MOT::MemRawChunkPoolStats),
            0,
            statsArraySize * sizeof(MOT::MemRawChunkPoolStats));
        securec_check(erc, "\0", "\0");

        uint32_t realStatsEntries;
        if (isGlobal) {
            realStatsEntries = MOT::MemRawChunkStoreGetGlobalStats(chunkPoolStatsArray, statsArraySize);
        } else {
            realStatsEntries = MOT::MemRawChunkStoreGetLocalStats(chunkPoolStatsArray, statsArraySize);
        }

        MOT_ASSERT(realStatsEntries <= statsArraySize);
        if (realStatsEntries > 0) {
            result = (MotMemoryDetail*)palloc(realStatsEntries * sizeof(MotMemoryDetail));
            if (result != nullptr) {
                for (uint32_t node = 0; node < realStatsEntries; ++node) {
                    result[node].numaNode = chunkPoolStatsArray[node].m_node;
                    result[node].reservedMemory = chunkPoolStatsArray[node].m_reservedBytes;
                    result[node].usedMemory = chunkPoolStatsArray[node].m_usedBytes;
                }
                *nodeCount = realStatsEntries;
            }
        }
        pfree(chunkPoolStatsArray);
    }

    return result;
}

MotSessionMemoryDetail* MOTAdaptor::GetSessionMemSize(uint32_t* sessionCount)
{
    EnsureSafeThreadAccessInline();
    MotSessionMemoryDetail* result = nullptr;
    *sessionCount = 0;

    uint32_t session_count = MOT::g_memGlobalCfg.m_maxThreadCount;
    MOT::MemSessionAllocatorStats* session_stats_array =
        (MOT::MemSessionAllocatorStats*)palloc(session_count * sizeof(MOT::MemSessionAllocatorStats));
    if (session_stats_array != nullptr) {
        uint32_t real_session_count = MOT::MemSessionGetAllStats(session_stats_array, session_count);
        if (real_session_count > 0) {
            result = (MotSessionMemoryDetail*)palloc(real_session_count * sizeof(MotSessionMemoryDetail));
            if (result != nullptr) {
                for (uint32_t session_index = 0; session_index < real_session_count; ++session_index) {
                    GetSessionDetails(session_stats_array[session_index].m_sessionId,
                        &result[session_index].threadid,
                        &result[session_index].threadStartTime);
                    result[session_index].totalSize = session_stats_array[session_index].m_reservedSize;
                    result[session_index].usedSize = session_stats_array[session_index].m_usedSize;
                    result[session_index].freeSize = result[session_index].totalSize - result[session_index].usedSize;
                }
                *sessionCount = real_session_count;
            }
        }
        pfree(session_stats_array);
    }

    return result;
}

void MOTAdaptor::CreateKeyBuffer(Relation rel, MOTFdwStateSt* festate, int start)
{
    uint8_t* buf = nullptr;
    uint8_t pattern = 0x00;
    EnsureSafeThreadAccessInline();
    int16_t num = festate->m_bestIx->m_ix->GetNumFields();
    const uint16_t* fieldLengths = festate->m_bestIx->m_ix->GetLengthKeyFields();
    const int16_t* orgCols = festate->m_bestIx->m_ix->GetColumnKeyFields();
    TupleDesc desc = rel->rd_att;
    uint16_t offset = 0;
    int32_t* exprs = nullptr;
    KEY_OPER* opers = nullptr;
    uint16_t keyLength;
    KEY_OPER oper;

    if (start == 0) {
        exprs = festate->m_bestIx->m_params[festate->m_bestIx->m_start];
        opers = festate->m_bestIx->m_opers[festate->m_bestIx->m_start];
        oper = festate->m_bestIx->m_ixOpers[0];
    } else {
        exprs = festate->m_bestIx->m_params[festate->m_bestIx->m_end];
        opers = festate->m_bestIx->m_opers[festate->m_bestIx->m_end];
        // end may be equal start but the operation maybe different, look at getCost
        oper = festate->m_bestIx->m_ixOpers[1];
    }

    keyLength = festate->m_bestIx->m_ix->GetKeyLength();
    festate->m_stateKey[start].InitKey(keyLength);
    buf = festate->m_stateKey[start].GetKeyBuf();

    switch (oper) {
        case KEY_OPER::READ_KEY_EXACT:
        case KEY_OPER::READ_KEY_OR_NEXT:
        case KEY_OPER::READ_KEY_BEFORE:
        case KEY_OPER::READ_KEY_LIKE:
        case KEY_OPER::READ_PREFIX:
        case KEY_OPER::READ_PREFIX_LIKE:
        case KEY_OPER::READ_PREFIX_OR_NEXT:
        case KEY_OPER::READ_PREFIX_BEFORE:
            pattern = 0x00;
            break;

        case KEY_OPER::READ_KEY_OR_PREV:
        case KEY_OPER::READ_PREFIX_AFTER:
        case KEY_OPER::READ_PREFIX_OR_PREV:
        case KEY_OPER::READ_KEY_AFTER:
            pattern = 0xff;
            break;

        default:
            elog(LOG, "Invalid key operation: %u", oper);
            break;
    }

    for (int i = 0; i < num; i++) {
        if (opers[i] < KEY_OPER::READ_INVALID) {
            bool is_null = false;
            ExprState* expr = (ExprState*)list_nth(festate->m_execExprs, exprs[i] - 1);
            Datum val = ExecEvalExpr((ExprState*)(expr), festate->m_econtext, &is_null, nullptr);
            if (is_null) {
                MOT_ASSERT((offset + fieldLengths[i]) <= keyLength);
                errno_t erc = memset_s(buf + offset, fieldLengths[i], 0x00, fieldLengths[i]);
                securec_check(erc, "\0", "\0");
            } else {
                MOT::Column* col = festate->m_table->GetField(orgCols[i]);
                uint8_t fill = 0x00;

                // in case of like fill rest of the key with appropriate to direction values
                if (opers[i] == KEY_OPER::READ_KEY_LIKE) {
                    switch (oper) {
                        case KEY_OPER::READ_KEY_LIKE:
                        case KEY_OPER::READ_KEY_OR_NEXT:
                        case KEY_OPER::READ_KEY_AFTER:
                        case KEY_OPER::READ_PREFIX:
                        case KEY_OPER::READ_PREFIX_LIKE:
                        case KEY_OPER::READ_PREFIX_OR_NEXT:
                        case KEY_OPER::READ_PREFIX_AFTER:
                            break;

                        case KEY_OPER::READ_PREFIX_BEFORE:
                        case KEY_OPER::READ_PREFIX_OR_PREV:
                        case KEY_OPER::READ_KEY_BEFORE:
                        case KEY_OPER::READ_KEY_OR_PREV:
                            fill = 0xff;
                            break;

                        case KEY_OPER::READ_KEY_EXACT:
                        default:
                            elog(LOG, "Invalid key operation: %u", oper);
                            break;
                    }
                }

                DatumToMOTKey(col,
                    expr->resultType,
                    val,
                    desc->attrs[orgCols[i] - 1]->atttypid,
                    buf + offset,
                    fieldLengths[i],
                    opers[i],
                    fill);
            }
        } else {
            MOT_ASSERT((offset + fieldLengths[i]) <= keyLength);
            festate->m_stateKey[start].FillPattern(pattern, fieldLengths[i], offset);
        }

        offset += fieldLengths[i];
    }

    festate->m_bestIx->m_ix->AdjustKey(&festate->m_stateKey[start], pattern);
}

bool MOTAdaptor::IsScanEnd(MOTFdwStateSt* festate)
{
    bool res = false;
    EnsureSafeThreadAccessInline();

    // festate->cursor[1] (end iterator) might be NULL (in case it is not in use). If this is the case, return false
    // (which means we have not reached the end yet)
    if (festate->m_cursor[1] == nullptr) {
        return false;
    }

    if (!festate->m_cursor[1]->IsValid()) {
        return true;
    } else {
        const MOT::Key* startKey = nullptr;
        const MOT::Key* endKey = nullptr;
        MOT::Index* ix = (festate->m_bestIx != nullptr ? festate->m_bestIx->m_ix : festate->m_table->GetPrimaryIndex());

        startKey = reinterpret_cast<const MOT::Key*>(festate->m_cursor[0]->GetKey());
        endKey = reinterpret_cast<const MOT::Key*>(festate->m_cursor[1]->GetKey());
        if (startKey != nullptr && endKey != nullptr) {
            int cmpRes = memcmp(startKey->GetKeyBuf(), endKey->GetKeyBuf(), ix->GetKeySizeNoSuffix());

            if (festate->m_forwardDirectionScan) {
                if (cmpRes > 0)
                    res = true;
            } else {
                if (cmpRes < 0)
                    res = true;
            }
        }
    }

    return res;
}

void MOTAdaptor::PackRow(TupleTableSlot* slot, MOT::Table* table, uint8_t* attrs_used, uint8_t* destRow)
{
    errno_t erc;
    EnsureSafeThreadAccessInline();
    HeapTuple srcData = (HeapTuple)slot->tts_tuple;
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    bool hasnulls = HeapTupleHasNulls(srcData);
    uint64_t i = 0;
    uint64_t j = 1;
    uint64_t cols = table->GetFieldCount() - 1;  // column count includes null bits field

    // the null bytes are necessary and have to give them back
    if (!hasnulls) {
        erc = memset_s(destRow + table->GetFieldOffset(i), table->GetFieldSize(i), 0xff, table->GetFieldSize(i));
        securec_check(erc, "\0", "\0");
    } else {
        erc = memcpy_s(destRow + table->GetFieldOffset(i),
            table->GetFieldSize(i),
            &srcData->t_data->t_bits[0],
            table->GetFieldSize(i));
        securec_check(erc, "\0", "\0");
    }

    // we now copy the fields, for the time being the null ones will be copied as well
    for (; i < cols; i++, j++) {
        bool isnull = false;
        Datum value = heap_slot_getattr(slot, j, &isnull);

        if (!isnull) {
            DatumToMOT(table->GetField(j), value, tupdesc->attrs[i]->atttypid, destRow);
        }
    }
}

void MOTAdaptor::PackUpdateRow(TupleTableSlot* slot, MOT::Table* table, const uint8_t* attrs_used, uint8_t* destRow)
{
    EnsureSafeThreadAccessInline();
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    uint8_t* bits;
    uint64_t i = 0;
    uint64_t j = 1;

    // column count includes null bits field
    uint64_t cols = table->GetFieldCount() - 1;
    bits = destRow + table->GetFieldOffset(i);

    for (; i < cols; i++, j++) {
        if (BITMAP_GET(attrs_used, i)) {
            bool isnull = false;
            Datum value = heap_slot_getattr(slot, j, &isnull);

            if (!isnull) {
                DatumToMOT(table->GetField(j), value, tupdesc->attrs[i]->atttypid, destRow);
                BITMAP_SET(bits, i);
            } else {
                BITMAP_CLEAR(bits, i);
            }
        }
    }
}

void MOTAdaptor::UnpackRow(TupleTableSlot* slot, MOT::Table* table, const uint8_t* attrs_used, uint8_t* srcRow)
{
    EnsureSafeThreadAccessInline();
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    uint64_t i = 0;

    // column count includes null bits field
    uint64_t cols = table->GetFieldCount() - 1;

    for (; i < cols; i++) {
        if (BITMAP_GET(attrs_used, i))
            MOTToDatum(table, tupdesc->attrs[i], srcRow, &(slot->tts_values[i]), &(slot->tts_isnull[i]));
        else {
            slot->tts_isnull[i] = true;
            slot->tts_values[i] = PointerGetDatum(nullptr);
        }
    }
}

// useful functions for data conversion: utils/fmgr/gmgr.cpp
void MOTAdaptor::MOTToDatum(MOT::Table* table, const Form_pg_attribute attr, uint8_t* data, Datum* value, bool* is_null)
{
    EnsureSafeThreadAccessInline();
    if (!BITMAP_GET(data, (attr->attnum - 1))) {
        *is_null = true;
        *value = PointerGetDatum(nullptr);

        return;
    }

    size_t len = 0;
    MOT::Column* col = table->GetField(attr->attnum);

    *is_null = false;
    switch (attr->atttypid) {
        case VARCHAROID:
        case BPCHAROID:
        case TEXTOID:
        case CLOBOID:
        case BYTEAOID: {
            uintptr_t tmp;
            col->Unpack(data, &tmp, len);

            bytea* result = (bytea*)palloc(len + VARHDRSZ);
            errno_t erc = memcpy_s(VARDATA(result), len, (uint8_t*)tmp, len);
            securec_check(erc, "\0", "\0");
            SET_VARSIZE(result, len + VARHDRSZ);

            *value = PointerGetDatum(result);
            break;
        }
        case NUMERICOID: {
            MOT::DecimalSt* d;
            col->Unpack(data, (uintptr_t*)&d, len);

            *value = NumericGetDatum(MOTNumericToPG(d));
            break;
        }
        default:
            col->Unpack(data, value, len);
            break;
    }
}

void MOTAdaptor::DatumToMOT(MOT::Column* col, Datum datum, Oid type, uint8_t* data)
{
    EnsureSafeThreadAccessInline();
    switch (type) {
        case BYTEAOID:
        case TEXTOID:
        case VARCHAROID:
        case CLOBOID:
        case BPCHAROID: {
            bytea* txt = DatumGetByteaP(datum);
            size_t size = VARSIZE(txt);  // includes header len VARHDRSZ
            char* src = VARDATA(txt);
            col->Pack(data, (uintptr_t)src, size - VARHDRSZ);

            if ((char*)datum != (char*)txt) {
                pfree(txt);
            }

            break;
        }
        case NUMERICOID: {
            Numeric n = DatumGetNumeric(datum);
            char buf[DECIMAL_MAX_SIZE];
            MOT::DecimalSt* d = (MOT::DecimalSt*)buf;

            if (NUMERIC_NDIGITS(n) > DECIMAL_MAX_DIGITS) {
                ereport(ERROR,
                    (errmodule(MOD_MOT),
                        errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                        errmsg("Value exceeds maximum precision: %d", NUMERIC_MAX_PRECISION)));
                break;
            }
            PGNumericToMOT(n, *d);
            col->Pack(data, (uintptr_t)d, DECIMAL_SIZE(d));

            break;
        }
        default:
            col->Pack(data, datum, col->m_size);
            break;
    }
}

inline void MOTAdaptor::VarcharToMOTKey(
    MOT::Column* col, Oid datumType, Datum datum, Oid colType, uint8_t* data, size_t len, KEY_OPER oper, uint8_t fill)
{
    bool noValue = false;
    switch (datumType) {
        case BYTEAOID:
        case TEXTOID:
        case VARCHAROID:
        case CLOBOID:
        case BPCHAROID:
            break;
        default:
            noValue = true;
            errno_t erc = memset_s(data, len, 0x00, len);
            securec_check(erc, "\0", "\0");
            break;
    }

    if (noValue) {
        return;
    }

    bytea* txt = DatumGetByteaP(datum);
    size_t size = VARSIZE(txt);  // includes header len VARHDRSZ
    char* src = VARDATA(txt);

    if (size > len) {
        size = len;
    }

    size -= VARHDRSZ;
    if (oper == KEY_OPER::READ_KEY_LIKE) {
        if (src[size - 1] == '%') {
            size -= 1;
        } else {
            // switch to equal
            if (colType == BPCHAROID) {
                fill = 0x20;  // space ' ' == 0x20
            } else {
                fill = 0x00;
            }
        }
    } else if (colType == BPCHAROID) {  // handle padding for blank-padded type
        fill = 0x20;
    }
    col->PackKey(data, (uintptr_t)src, size, fill);

    if ((char*)datum != (char*)txt) {
        pfree(txt);
    }
}

inline void MOTAdaptor::FloatToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    if (datumType == FLOAT8OID) {
        MOT::DoubleConvT dc;
        MOT::FloatConvT fc;
        dc.m_r = (uint64_t)datum;
        fc.m_v = (float)dc.m_v;
        uint64_t u = (uint64_t)fc.m_r;
        col->PackKey(data, u, col->m_size);
    } else {
        col->PackKey(data, datum, col->m_size);
    }
}

inline void MOTAdaptor::NumericToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    Numeric n = DatumGetNumeric(datum);
    char buf[DECIMAL_MAX_SIZE];
    MOT::DecimalSt* d = (MOT::DecimalSt*)buf;
    PGNumericToMOT(n, *d);
    col->PackKey(data, (uintptr_t)d, DECIMAL_SIZE(d));
}

inline void MOTAdaptor::TimestampToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    if (datumType == TIMESTAMPTZOID) {
        Timestamp result = DatumGetTimestamp(DirectFunctionCall1(timestamptz_timestamp, datum));
        col->PackKey(data, result, col->m_size);
    } else if (datumType == DATEOID) {
        Timestamp result = DatumGetTimestamp(DirectFunctionCall1(date_timestamp, datum));
        col->PackKey(data, result, col->m_size);
    } else {
        col->PackKey(data, datum, col->m_size);
    }
}

inline void MOTAdaptor::TimestampTzToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    if (datumType == TIMESTAMPOID) {
        TimestampTz result = DatumGetTimestampTz(DirectFunctionCall1(timestamp_timestamptz, datum));
        col->PackKey(data, result, col->m_size);
    } else if (datumType == DATEOID) {
        TimestampTz result = DatumGetTimestampTz(DirectFunctionCall1(date_timestamptz, datum));
        col->PackKey(data, result, col->m_size);
    } else {
        col->PackKey(data, datum, col->m_size);
    }
}

inline void MOTAdaptor::DateToMOTKey(MOT::Column* col, Oid datumType, Datum datum, uint8_t* data)
{
    if (datumType == TIMESTAMPOID) {
        DateADT result = DatumGetDateADT(DirectFunctionCall1(timestamp_date, datum));
        col->PackKey(data, result, col->m_size);
    } else if (datumType == TIMESTAMPTZOID) {
        DateADT result = DatumGetDateADT(DirectFunctionCall1(timestamptz_date, datum));
        col->PackKey(data, result, col->m_size);
    } else {
        col->PackKey(data, datum, col->m_size);
    }
}

void MOTAdaptor::DatumToMOTKey(
    MOT::Column* col, Oid datumType, Datum datum, Oid colType, uint8_t* data, size_t len, KEY_OPER oper, uint8_t fill)
{
    EnsureSafeThreadAccessInline();
    switch (colType) {
        case BYTEAOID:
        case TEXTOID:
        case VARCHAROID:
        case CLOBOID:
        case BPCHAROID:
            VarcharToMOTKey(col, datumType, datum, colType, data, len, oper, fill);
            break;
        case FLOAT4OID:
            FloatToMOTKey(col, datumType, datum, data);
            break;
        case NUMERICOID:
            NumericToMOTKey(col, datumType, datum, data);
            break;
        case TIMESTAMPOID:
            TimestampToMOTKey(col, datumType, datum, data);
            break;
        case TIMESTAMPTZOID:
            TimestampTzToMOTKey(col, datumType, datum, data);
            break;
        case DATEOID:
            DateToMOTKey(col, datumType, datum, data);
            break;
        default:
            col->PackKey(data, datum, col->m_size);
            break;
    }
}

MOTFdwStateSt* InitializeFdwState(void* fdwState, List** fdwExpr, uint64_t exTableID)
{
    MOTFdwStateSt* state = (MOTFdwStateSt*)palloc0(sizeof(MOTFdwStateSt));
    List* values = (List*)fdwState;

    state->m_allocInScan = true;
    state->m_foreignTableId = exTableID;
    if (list_length(values) > 0) {
        ListCell* cell = list_head(values);
        int type = ((Const*)lfirst(cell))->constvalue;
        if (type != FDW_LIST_STATE) {
            return state;
        }
        cell = lnext(cell);
        state->m_cmdOper = (CmdType)((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_order = (SORTDIR_ENUM)((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_hasForUpdate = (bool)((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_foreignTableId = ((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_numAttrs = ((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_ctidNum = ((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);
        state->m_numExpr = ((Const*)lfirst(cell))->constvalue;
        cell = lnext(cell);

        int len = BITMAP_GETLEN(state->m_numAttrs);
        state->m_attrsUsed = (uint8_t*)palloc0(len);
        state->m_attrsModified = (uint8_t*)palloc0(len);
        BitmapDeSerialize(state->m_attrsUsed, len, &cell);

        if (cell != NULL) {
            state->m_bestIx = &state->m_bestIxBuf;
            state->m_bestIx->Deserialize(cell, exTableID);
        }

        if (fdwExpr != NULL && *fdwExpr != NULL) {
            ListCell* c = NULL;
            int i = 0;

            // divide fdw expr to param list and original expr
            state->m_remoteCondsOrig = NULL;

            foreach (c, *fdwExpr) {
                if (i < state->m_numExpr) {
                    i++;
                    continue;
                } else {
                    state->m_remoteCondsOrig = lappend(state->m_remoteCondsOrig, lfirst(c));
                }
            }

            *fdwExpr = list_truncate(*fdwExpr, state->m_numExpr);
        }
    }
    return state;
}

void* SerializeFdwState(MOTFdwStateSt* state)
{
    List* result = NULL;

    // set list type to FDW_LIST_STATE
    result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, FDW_LIST_STATE, false, true));
    result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->m_cmdOper), false, true));
    result = lappend(result, makeConst(INT1OID, -1, InvalidOid, 4, Int8GetDatum(state->m_order), false, true));
    result = lappend(result, makeConst(BOOLOID, -1, InvalidOid, 1, BoolGetDatum(state->m_hasForUpdate), false, true));
    result =
        lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->m_foreignTableId), false, true));
    result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->m_numAttrs), false, true));
    result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->m_ctidNum), false, true));
    result = lappend(result, makeConst(INT2OID, -1, InvalidOid, 2, Int16GetDatum(state->m_numExpr), false, true));
    int len = BITMAP_GETLEN(state->m_numAttrs);
    result = BitmapSerialize(result, state->m_attrsUsed, len);

    if (state->m_bestIx != nullptr) {
        state->m_bestIx->Serialize(&result);
    }
    ReleaseFdwState(state);
    return result;
}

void ReleaseFdwState(MOTFdwStateSt* state)
{
    CleanCursors(state);

    if (state->m_currTxn) {
        state->m_currTxn->m_queryState.erase((uint64_t)state);
    }

    if (state->m_bestIx && state->m_bestIx != &state->m_bestIxBuf)
        pfree(state->m_bestIx);

    if (state->m_remoteCondsOrig != nullptr)
        list_free(state->m_remoteCondsOrig);

    if (state->m_attrsUsed != NULL)
        pfree(state->m_attrsUsed);

    if (state->m_attrsModified != NULL)
        pfree(state->m_attrsModified);

    state->m_table = NULL;
    pfree(state);
}




























































std::atomic<int> cpu_index(1);
void SetCPU(){
    cpu_set_t logicalEpochSet;
    CPU_ZERO(&logicalEpochSet);
    CPU_SET(cpu_index.fetch_add(1), &logicalEpochSet); //2就是核心号
    int rc = sched_setaffinity(0, sizeof(cpu_set_t), &logicalEpochSet);
    if (rc == -1) {
        ereport(FATAL, (errmsg("绑核失败")));
    }
}

void string_free(void *data, void *hint){
    delete static_cast<std::string*>(hint);
}

uint64_t now_to_us(){
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

template<typename key, typename value>
class ConcurrentHashMap {
public:
    typedef typename std::unordered_map<key, value>::iterator map_iterator;
    typedef typename std::unordered_map<key, value>::size_type size_type;

    bool insert(key &k, value &v, value *p) {
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if (iter == _map_temp.end()) {
            _map_temp[k] = v;
            *p = nullptr;
        } else {
            *p = _map_temp[k];
            _map_temp[k] = v;
        }
        return true;
    }

    void insert(key &k, value &v) {
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        _map_temp[k] = v;
    }


    void remove(key &k, value &v) {
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if (iter != _map_temp.end()) {
            if (iter->second == v) {
                _map_temp.erase(iter);
            }
        }
    }

    void remove(key &k) {
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if (iter != _map_temp.end()) {
            _map_temp.erase(iter);
        }
    }

    void clear() {
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
            _map[i].clear();
        }
    }

    void unsafe_clear() {
        for(uint64_t i = 0; i < _N; i ++){
            _map[i].clear();
        }
    }

    bool contain(key &k, value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(iter->second == v){
                return true;
            }
        }
        return false;
    }

    bool contain(key &k){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            return true;
        }
        return false;
    }

    bool unsafe_contain(key &k, value &v){
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()){
            if(iter->second == v){
                return true;
            }
        }
        return false;
    }

    bool get_value(key &k, value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        std::mutex& _mutex_temp = GetMutexRef(k);
        std::unique_lock<std::mutex> lock(_mutex_temp);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()) {
            v = iter->second;
            return true;
        }
        else {
            return false;
        }
    }

    bool unsafe_get_value(key &k, value &v) {
        std::unordered_map<key, value>& _map_temp = GetMapRef(k);
        map_iterator iter = _map_temp.find(k);
        if(iter != _map_temp.end()) {
            v = iter->second;
            return true;
        }
        else {
            return false;
        }
    }

    size_type size() {
        size_type ans = 0;
        for(uint64_t i = 0; i < _N; i ++){
            std::unique_lock<std::mutex> lock(_mutex[i]);
            ans += _map[i].size();
        }
        return ans;
    }

protected:
    inline std::unordered_map<key, value>& GetMapRef(const key k){ return _map[(_hash(k) % _N)]; }
    inline std::unordered_map<key, value>& GetMapRef(const key k) const { return _map[(_hash(k) % _N)]; }
    inline std::mutex& GetMutexRef(const key k) { return _mutex[(_hash(k) % _N)]; }
    inline std::mutex& GetMutexRef(const key k) const {return _mutex[(_hash(k) % _N)]; }

private:
    const static uint64_t _N = 101;//521 997 1217 12281 122777 prime
    std::hash<key> _hash;
    std::unordered_map<key, value> _map[_N];
    std::mutex _mutex[_N];
};




template<typename T>
using BlockingConcurrentQueue =  moodycamel::BlockingConcurrentQueue<T>;

struct send_thread_params{
    uint64_t current_epoch;
    uint64_t tot;
    std::string* merge_request_ptr;
    send_thread_params(uint64_t ce, uint64_t tot_temp, std::string* ptr1):
        current_epoch(ce), tot(tot_temp), merge_request_ptr(ptr1){}
    send_thread_params(){}
};

bool Gzip(google::protobuf::MessageLite* ptr, std::string* serialized_str_ptr) {
    //        google::protobuf::io::GzipOutputStream::Options options;
    //        options.format = google::protobuf::io::GzipOutputStream::GZIP;
    //        options.compression_level = 9;
    //        google::protobuf::io::StringOutputStream outputStream(serialized_str_ptr);
    //        google::protobuf::io::GzipOutputStream gzipStream(&outputStream, options);
    //        auto res = ptr->SerializeToZeroCopyStream(&gzipStream);
    //        gzipStream.Close();

    google::protobuf::io::StringOutputStream outputStream(serialized_str_ptr);
    auto res = ptr->SerializeToZeroCopyStream(&outputStream);
    return res;
}

bool UnGzip(google::protobuf::MessageLite* ptr, const std::string* str) {
    //    auto message_string_ptr = std::make_unique<std::string>(static_cast<const char*>(message_ptr->data()), message_ptr->size());
    //        google::protobuf::io::ArrayInputStream inputStream(str->data(), (int)str->size());
    //        google::protobuf::io::GzipInputStream gzipStream(&inputStream);
    //        return ptr->ParseFromZeroCopyStream(&gzipStream);
    google::protobuf::io::ArrayInputStream inputStream(str->data(), (int)str->size());
    return ptr->ParseFromZeroCopyStream(&inputStream);
}




































//Storage
BlockingConcurrentQueue<std::unique_ptr<proto::Transaction>> storage_update_queue;


//**************************************************************************************************************************************************************************************************
//*****                                                                                                                                                                                        *****
//*****                                                                                                                                                                                        *****
//*****                                                                                      TAAS    Client                                                                                    *****
//*****                                                                                                                                                                                        *****
//*****                                                                                                                                                                                        *****
//**************************************************************************************************************************************************************************************************

BlockingConcurrentQueue<std::unique_ptr<zmq::message_t>> client_listen_message_queue;
BlockingConcurrentQueue<std::unique_ptr<send_thread_params>> client_send_message_queue;
BlockingConcurrentQueue<std::unique_ptr<proto::Message>> client_other_message_queue;

// std::unique_ptr<std::mutex> client_send_mutex, client_listen_mutex;
// std::shared_ptr<zmq::context_t> client_send_context, client_listen_context;
// std::shared_ptr<zmq::socket_t> client_send_socket, client_listen_socket;

std::atomic<bool> client_init_ok_flag(false), client_start_flag(false);
ConcurrentHashMap<uint64_t, MOT::TxnManager*> txn_map;
std::atomic<uint64_t> local_csn(5);

bool MOTAdaptor::InsertTxntoLocalChangeSet(MOT::TxnManager* txMan){
    auto msg = std::make_unique<proto::Message>();
    auto* txn = msg->mutable_txn();
    proto::Row *row;
    proto::Column* col;
    proto::OpType op_type;
    MOT::Row* local_row = nullptr;
    MOT::Key* key = nullptr;
    void* buf = nullptr;
    uint64_t fieldCnt;
    const MOT::Access* access = nullptr;
    MOT::BitmapSet* bmp;
    MOT::TxnOrderedSet_t &orderedSet = txMan->m_accessMgr->GetOrderedRowSet();
    int num = 0;
    for (const auto &raPair : orderedSet){
        num ++;
        access = raPair.second;
        row = txn->add_row();
        if (access->m_type == MOT::RD) {
            op_type = proto::OpType::Read;
            local_row = access->m_localRow;
        }
        if (access->m_type == MOT::WR){
            op_type = proto::OpType::Update;
            local_row = access->m_localRow;
        }
        else if (access->m_type == MOT::INS){
            op_type = proto::OpType::Insert;
            local_row = access->m_localInsertRow;
        }
        else if (access->m_type == MOT::DEL){
            op_type = proto::OpType::Delete;
            local_row = access->m_localRow;
        }

        if(local_row == nullptr || local_row->GetTable() == nullptr){
            return false;
        }
        key = local_row->GetTable()->BuildKeyByRow(local_row, txMan, buf);
        if (key == nullptr) {
            return false;
        }
        row->set_key(std::move(std::string(key->GetKeyBuf(), key->GetKeyBuf() + key->GetKeyLength())));
        row->set_table_name(local_row->GetTable()->GetLongTableName());
//        if(op_type == proto::OpType::Update || op_type == proto::OpType::Insert) {
//            row->set_data(local_row->GetData(), local_row->GetTable()->GetTupleSize());
//        }
        row->set_data(local_row->GetData(), local_row->GetTable()->GetTupleSize());
        row->set_op_type(op_type);
    }
    txn->set_client_ip(kLocalIp);
    txMan->SetCommitSequenceNumber(local_csn.fetch_add(1));
    txn->set_client_txn_id(txMan->GetCommitSequenceNumber());
    txn->set_csn(txMan->GetCommitSequenceNumber());
    txn->set_storage_type("mot");

    MOT::TxnManager* txnMan_ptr = nullptr;
    auto csn = txMan->GetCommitSequenceNumber();
    txn_map.insert(csn, txMan, &txnMan_ptr);
    if(txnMan_ptr != nullptr && txnMan_ptr->commit_state == MOT::RC::RC_WAIT) {
        //抢占了别人的 hash map 出现冲突 abort 前一个，然后存储当前的
        txnMan_ptr->commit_state = MOT::RC::RC_ABORT;
        txnMan_ptr->cv.notify_all();
    }

    {
        string* serialized_txn_str_ptr = new string();
//        Gzip(msg.get(), serialized_txn_str_ptr);

        google::protobuf::io::StringOutputStream outputStream(serialized_txn_str_ptr);
        auto res = msg->SerializeToZeroCopyStream(&outputStream);

        client_send_message_queue.enqueue(std::move(std::make_unique<send_thread_params>(0, 0, serialized_txn_str_ptr)));
        client_send_message_queue.enqueue(std::move(std::make_unique<send_thread_params>(0, 0, nullptr)));
    }
    return true;
}




void ClientSendThreadMain(uint64_t id) {
    SetCPU();
    MOT_LOG_INFO("线程 ClientSendThreadMain 开始工作 %llu", id);
    std::vector<std::shared_ptr<zmq::socket_t>> client_send_sockets;
    auto client_send_context = std::make_shared<zmq::context_t>(1);
    for(int i = 0; i < kTxnNodeIp.size(); i ++) {
        auto client_send_socket = std::make_shared<zmq::socket_t>(*client_send_context, ZMQ_PUSH);
        client_send_socket->connect("tcp://" + kTxnNodeIp[i] + ":5551");
        client_send_sockets.emplace_back(client_send_socket);
    }

    std::unique_ptr<send_thread_params> params;
    std::unique_ptr<zmq::message_t> msg;
    auto cnt = 0;
    while(!client_init_ok_flag.load()) usleep(200);
    while(true) {
        client_send_message_queue.wait_dequeue(params);
        if(params != nullptr && params->merge_request_ptr != nullptr) {
            msg = std::make_unique<zmq::message_t>(static_cast<void*>(const_cast<char*>(params->merge_request_ptr->data())),
                    params->merge_request_ptr->size(), string_free, static_cast<void*>(params->merge_request_ptr));
            client_send_sockets[cnt]->send(*(msg));
            cnt = (cnt + 1) % kTxnNodeIp.size();
            // MOT_LOG_INFO("ClientSendThreadMain 发送一个事务");
        }
    }
}

void ClientListenThreadMain(uint64_t id) {
    SetCPU();
    MOT_LOG_INFO("线程 ClientListenThreadMain 开始工作 %llu", id);

    //================PULL==================
    zmq::context_t listen_context(1);
    zmq::socket_t socket_listen(listen_context, ZMQ_PULL);
    int queue_length = 0;
    socket_listen.setsockopt(ZMQ_RCVHWM, &queue_length, sizeof(queue_length));
    socket_listen.bind("tcp://*:5552");
    MOT_LOG_INFO("线程开始工作 ClientListenThreadMain Client PULL tcp://*:5552");
    std::unique_ptr<zmq::message_t> message_ptr;
    while(true) {
        message_ptr = std::make_unique<zmq::message_t>();
        socket_listen.recv(&(*message_ptr));
//        MOT_LOG_INFO("Client PULL Receive a Txn");
        client_listen_message_queue.enqueue(std::move(message_ptr));
        client_listen_message_queue.enqueue(std::move(nullptr));
    }
}

void ClientWorker1ThreadMain(uint64_t id) { // handle result return from Txn node/ Storage Node
    SetCPU();
    MOT_LOG_INFO("线程 ClientWorker1ThreadMain 开始工作 %llu", id);
    std::unique_ptr<zmq::message_t> message_ptr;
    std::unique_ptr<std::string> message_string_ptr;
    std::unique_ptr<proto::Message> msg_ptr;
    MOT::TxnManager* txnMan;
    uint64_t csn = 0;

    while(true) {
        client_listen_message_queue.wait_dequeue(message_ptr);
//        MOT_LOG_INFO("Client 收到一个事务");
        if(message_ptr != nullptr && message_ptr->size() > 0) {
//            message_string_ptr = std::make_unique<std::string>(static_cast<const char *>(message_ptr->data()),message_ptr->size());
//            msg_ptr = std::make_unique<proto::Message>();
//            UnGzip(msg_ptr.get(), message_string_ptr.get());

            msg_ptr = std::make_unique<proto::Message>();
            message_string_ptr = std::make_unique<std::string>(static_cast<const char*>(message_ptr->data()), message_ptr->size());
            google::protobuf::io::ArrayInputStream inputStream(message_string_ptr->data(), message_string_ptr->size());
            msg_ptr->ParseFromZeroCopyStream(&inputStream);

            if(msg_ptr->type_case() == proto::Message::TypeCase::kReplyTxnResultToClient) {
                //wake up local thread and return the commit result
                auto& txn = msg_ptr->reply_txn_result_to_client();
                csn = txn.client_txn_id();
//                 MOT_LOG_INFO("唤醒2 csn %llu %llu", csn, now_to_us());
                if(txn_map.get_value(csn, txnMan)) {
                    if(txn.txn_state() == proto::TxnState::Commit) {
                        txnMan->commit_state = MOT::RC::RC_OK;
                    }
                    else{
                        txnMan->commit_state = MOT::RC::RC_ABORT;
                    }
                    txnMan->cv.notify_all();
                    // MOT_LOG_INFO("唤醒3 csn %llu, txn txnid %llu, txn_state %llu %llu", csn, txnMan->GetCommitSequenceNumber(), txnMan->commit_state, now_to_us());
                    txn_map.remove(csn);
                }
                else {
                    // MOT_LOG_INFO("未找到 csn %llu", csn);
                }
            }
            else if(msg_ptr->type_case() == proto::Message::TypeCase::kClientReadResponse) {
                //wake up local thread and return the read result
            }
            else {
//                client_other_message_queue.enqueue(std::move(msg_ptr));
//                client_other_message_queue.enqueue(std::move(std::make_unique<proto::Message>()));
            }
        }
    }
}

void ClientManagerThreadMain(uint64_t id) { //handle other status
    MOT_LOG_INFO("线程 ClientManagerThreadMain 开始工作 %llu", id);
    client_init_ok_flag.store(true);
    usleep(1000000);
    client_start_flag.store(true);
    auto msg_ptr = std::make_unique<proto::Message>();
    while(true) {
        client_other_message_queue.wait_dequeue(msg_ptr);
    }
}



























//**************************************************************************************************************************************************************************************************
//*****                                                                                                                                                                                        *****
//*****                                                                                                                                                                                        *****
//*****                                                                                      TAAS    Storage                                                                                   *****
//*****                                                                                                                                                                                        *****
//*****                                                                                                                                                                                        *****
//**************************************************************************************************************************************************************************************************

BlockingConcurrentQueue<std::unique_ptr<zmq::message_t>> storage_listen_message_queue;
BlockingConcurrentQueue<std::unique_ptr<send_thread_params>> storage_send_message_queue;
BlockingConcurrentQueue<std::unique_ptr<proto::Message>> storage_other_message_queue;
BlockingConcurrentQueue<std::unique_ptr<proto::Message>> storage_read_queue;

std::atomic<bool> storage_init_ok_flag(false), storage_start_flag(false);
std::atomic<uint64_t> update_epoch(0), current_epoch(5), total_commit_txn_num(0);

uint64_t start_time_ll, start_physical_epoch = 1, cache_size = 10000;
struct timeval start_time;

proto::Node dest_node, src_node;

void SendPullRequest(uint64_t epoch_id) {
    auto msg = std::make_unique<proto::Message>();
    auto* request = msg->mutable_storage_pull_request();
    request->mutable_send_node()->CopyFrom(src_node);
    request->mutable_recv_node()->CopyFrom(dest_node);
    request->set_epoch_id(epoch_id);
    auto serialized_txn_str_ptr = std::make_unique<std::string>();
//    Gzip(msg.get(), serialized_txn_str_ptr.get());
    google::protobuf::io::StringOutputStream outputStream(serialized_txn_str_ptr.get());
    auto res = msg->SerializeToZeroCopyStream(&outputStream);
    storage_send_message_queue.enqueue(std::move(std::make_unique<send_thread_params>(0, 0, serialized_txn_str_ptr.release())));
    storage_send_message_queue.enqueue(std::move(std::make_unique<send_thread_params>(0, 0, nullptr)));
}

bool HandlePackTxnx(proto::Message* msg) {
    if(msg->type_case() == proto::Message::TypeCase::kStoragePullResponse) {
        auto* response = &(msg->storage_pull_response());
        if(response->result() == proto::Result::Fail) {
            //Send pull request to another server or wait a few seconds
            return true;
        }
        for(int i = 0; i < (int) response->txns_size(); i ++) {
            auto txn = std::make_unique<proto::Transaction>(std::move(std::move(response->txns(i))));
            storage_update_queue.enqueue(std::move(txn));
        }
        storage_update_queue.enqueue(std::move(std::make_unique<proto::Transaction>()));
    }
    else {
        auto* response = &(msg->storage_push_response());
        for(int i = 0; i < (int) response->txns_size(); i ++) {
            auto txn = std::make_unique<proto::Transaction>(std::move(std::move(response->txns(i))));
            storage_update_queue.enqueue(std::move(txn));
        }
        storage_update_queue.enqueue(std::move(std::make_unique<proto::Transaction>()));
    }
}

void StorageMessageManagerThreadMain(uint64_t id) { // handle result return from Txn node/ Storage Node
    SetCPU();
    MOT_LOG_INFO("线程 StorageMessageManagerThreadMain 开始工作 %llu", id);
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    std::unique_ptr<zmq::message_t> message_ptr;
    std::unique_ptr<proto::Message> msg_ptr;
    std::unique_ptr<std::string> message_string_ptr;
    while(true) {
        storage_listen_message_queue.wait_dequeue(message_ptr);
        if(message_ptr != nullptr && message_ptr->size() > 0) {
//            message_string_ptr = std::make_unique<std::string>(static_cast<const char*>(message_ptr->data()), message_ptr->size());
//            msg_ptr = std::make_unique<proto::Message>();
//            UnGzip(msg_ptr.get(), message_string_ptr.get());

            msg_ptr = std::make_unique<proto::Message>();
            assert(msg_ptr != nullptr);
//            message_string_ptr = std::make_unique<std::string>(static_cast<const char*>(message_ptr->data()), message_ptr->size());
            google::protobuf::io::ArrayInputStream inputStream(message_ptr->data(), message_ptr->size());
//            google::protobuf::io::ArrayInputStream inputStream(message_string_ptr->data(), message_string_ptr->size());
            msg_ptr->ParseFromZeroCopyStream(&inputStream);

            if(msg_ptr->type_case() == proto::Message::TypeCase::kStoragePullResponse || msg_ptr->type_case() == proto::Message::TypeCase::kStoragePushResponse) {
                if(msg_ptr->type_case() == proto::Message::TypeCase::kStoragePullResponse) {
                    auto* response = &(msg_ptr->storage_pull_response());
                    if(response->result() == proto::Result::Fail) {
                        //Send pull request to another server or wait a few seconds
                        continue;
                    }
                    for(int i = 0; i < (int) response->txns_size(); i ++) {
                        auto txn = std::make_unique<proto::Transaction>(response->txns(i));
                        storage_update_queue.enqueue(std::move(txn));
                    }
                    storage_update_queue.enqueue(std::move(std::make_unique<proto::Transaction>()));
                }
                else {
                    auto* response = &(msg_ptr->storage_push_response());
                    for(int i = 0; i < (int) response->txns_size(); i ++) {
                        auto txn = std::make_unique<proto::Transaction>(response->txns(i));
                        storage_update_queue.enqueue(std::move(txn));
                    }
                    storage_update_queue.enqueue(std::move(std::make_unique<proto::Transaction>()));
                }
            }
            else {
                storage_other_message_queue.enqueue(std::move(msg_ptr));
                storage_other_message_queue.enqueue(std::move(std::make_unique<proto::Message>()));
            }
        }
    }
}

void StorageUpdaterThreadMain(uint64_t id) {
    MOT_LOG_INFO("线程 StorageUpdaterThreadMain 开始工作 %llu", id);
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
        CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    std::map<MOT::Row*, bool> lock_map, should_lock_map;
    std::vector<MOT::Table*> vec_table;
    std::vector<MOT::Row*> vec_row;
    size_t key_length, key_id;
    std::string key_str;
    MOT::Table* table;
    MOT::Row* row;
    MOT::Key* key;
    std::shared_ptr<MOT::Key> key_ptr;
    void* buf;
    MOT::RC res;
    bool commit_res = false;
    uint64_t commit_txn_num = 0;

    auto txn_ptr = std::make_unique<proto::Transaction>();
    while(true) {
        txn_ptr.reset(nullptr);
//        while(!storage_update_queue.try_dequeue(txn_ptr)) usleep(100);
        storage_update_queue.wait_dequeue(txn_ptr);

        // handle txn read request
        if (txn_ptr->row_size() > 0) {
            //             MOT_LOG_INFO("Storage 收到一个事务");
            txn_manager->CleanTxn();
            vec_row.clear();
            vec_table.clear();
            lock_map.clear();
            should_lock_map.clear();
            commit_res = true;
            key_id = 0;

            for (int i = 0; i < txn_ptr->row_size(); i++) {
                auto *row_it = &(txn_ptr->row(i));
                MOT::Table* table = MOTAdaptor::m_engine->GetTableManager()->GetTable(row_it->table_name());
                if (table != nullptr) {
                    key_length = row_it->key().length();
                    buf = MOT::MemSessionAlloc(key_length);
                    if (buf == nullptr)
                        Assert(false);
                    key = new (buf) MOT::Key(key_length);
                    key->CpKey((uint8_t*)row_it->key().c_str(), key_length);
                    key_str = key->GetKeyStr();
                    res = table->FindRow(key, row, 0);
                    MOT::MemSessionFree(buf);
                    if (res != MOT::RC_OK) {  // an error
                        // look up fail!~
                        row = nullptr;
                    }
                    if (row != nullptr && (row_it->op_type() == proto::OpType::Update ||
                                              row_it->op_type() == proto::OpType::Delete)) {
                        should_lock_map[row] = true;
                    }
                }
                else {
                    table = nullptr, row = nullptr;
                    MOT_LOG_INFO("Taas Storage Updater 3010 table is null %s", row_it->table_name().c_str());
                }
                vec_row.push_back(row);
                vec_table.push_back(table);
            }

            for (auto row_it = should_lock_map.begin(); row_it != should_lock_map.end(); ++row_it) {
                auto row_temp = row_it->first;
                if (!lock_map[row_temp]) {
                    row_temp->LockRow();
                    lock_map[row_temp] = true;
                }
            }

            // process operation
            key_id = 0;
            //            for (auto row_it = txn_ptr->row().begin(); row_it != txn_ptr->row().end(); ++row_it) {
            for (int i = 0; i < txn_ptr->row_size(); i++) {
                auto* row_it = &(txn_ptr->row(i));
                row = vec_row[key_id];
                table = vec_table[key_id];
                key_id++;
                if (table == nullptr) {
                    continue;
                }
                if (row == nullptr) {  // insert or delete by others before this epoch
                    if (row_it->op_type() == proto::OpType::Insert) {
                        row = table->CreateNewRow();
                        row->CopyData((uint8_t*)row_it->data().c_str(), table->GetTupleSize());
                        res = table->InsertRow(row, txn_manager);
                        if ((res != MOT::RC_OK) && (res != MOT::RC_UNIQUE_VIOLATION)) {
                            MOT_REPORT_ERROR(MOT_ERROR_OOM,
                                "Insert Row ",
                                "Failed to insert new row for table %s",
                                table->GetLongTableName().c_str());
                        }
                        if (res != MOT::RC_OK) {
                            MOT_REPORT_ERROR(MOT_ERROR_OOM,
                                "Taas Insert Row ",
                                "Failed to insert new row for table %s",
                                table->GetLongTableName().c_str());
                        }
                    } else {
                        MOT_LOG_INFO("Storage Error Update/Delete a NULL row txn %llu key_id %llu op_type %llu",
                            txn_ptr->client_txn_id(),
                            key_id,
                            row_it->op_type());
                    }
                    /* code */
                } else {
                    if (row_it->op_type() == proto::OpType::Delete) {
                        row->GetPrimarySentinel()->SetDirty();
                        row->SetCSN_Delete(txn_ptr->csn());
                        // MOT_LOG_INFO("Storage Delete txn %llu key_id %llu op_type %llu", txn_ptr->client_txn_id(), key_id++, row_it->op_type());
                    } else if (row_it->op_type() == proto::OpType::Update) {
                        row->CopyData((uint8_t*)row_it->data().c_str(), table->GetTupleSize());
                        // for (auto col_it = row_it->column().begin(); col_it != row_it->column().end(); ++col_it) {
                        //     row->SetValueVariable(col_it->id(), col_it->value().c_str(), col_it->value().length()); //
                        // }
                        row->SetCSN_Update(txn_ptr->csn());
                        // MOT_LOG_INFO("Storage Update txn %llu key_id %llu op_type %llu", txn_ptr->client_txn_id(), key_id++, row_it->op_type());
                    } else if (row_it->op_type() == proto::OpType::Insert) {  /// never should be happen
                        row = table->CreateNewRow();
                        row->CopyData((uint8_t*)row_it->data().c_str(), table->GetTupleSize());
                        res = table->InsertRow(row, txn_manager);
                        if ((res != MOT::RC_OK) && (res != MOT::RC_UNIQUE_VIOLATION)) {
                            MOT_REPORT_ERROR(MOT_ERROR_OOM,
                                "Insert Row ",
                                "Failed to insert new row for table %s",
                                table->GetLongTableName().c_str());
                        }
                        if (res != MOT::RC_OK) {
                            MOT_REPORT_ERROR(MOT_ERROR_OOM,
                                "Taas Insert Row ",
                                "Failed to insert new row for table %s",
                                table->GetLongTableName().c_str());
                        }
                        // MOT_LOG_INFO("Storage Insert %llu key_id %llu op_type %llu", txn_ptr->client_txn_id(), key_id++, row_it->op_type());
                    }
                }
            }
             txn_manager->SetCommitSequenceNumber(txn_ptr->csn());
            if (txn_manager->TaasLogCommit() != MOT::RC::RC_OK) {
                // MOT_LOG_INFO("TaasLogCommit Failed");
                commit_res = false;
            }  // write change时会写入csn

            // release
            key_id = 0;
            for (auto row_it = lock_map.begin(); row_it != lock_map.end(); ++row_it) {
                auto row_temp = row_it->first;
                if (lock_map[row_temp]) {
                    row_temp->ReleaseRow();
                    lock_map[row_temp] = false;
                }
            }
//            for (int i = 0; i < txn_ptr->row_size(); i++) {
//                auto* row_it = &(txn_ptr->row(i));
//                //            for (auto row_it = txn_ptr->row().begin(); row_it != txn_ptr->row().end(); ++row_it) {
//                row = vec_row[key_id];
//                table = vec_table[key_id];
//                key_id++;
//                if (table == nullptr) {
//                    continue;
//                }
//                if (row == nullptr) {  // an error
//                    /* code */
//                }
//                if (row != nullptr &&
//                    (row_it->op_type() == proto::OpType::Update || row_it->op_type() == proto::OpType::Delete)) {
//                    if (lock_map[row]) {
//                        row->ReleaseRow();
//                        lock_map[row] = false;
//                        // MOT_LOG_INFO("txn ReleaseRow %llu key_id %llu op_type %llu", txn_ptr->client_txn_id(), key_id++, row_it->op_type());
//                    } else {
//                        // MOT_LOG_INFO("txn ReleaseRow else");
//                    }
//                }
//            }

            vec_row.clear();
            vec_table.clear();
            lock_map.clear();
            //            MOT_LOG_INFO("Storage 提交 txn num %llu", commit_txn_num);
            if (!commit_res) {
                ///
            } else {
                commit_txn_num++;
                if (commit_txn_num % 5 == 0) {
                    auto num = total_commit_txn_num.fetch_add(commit_txn_num);
                    commit_txn_num = 0;
                    MOT_LOG_INFO("共提交 txn num %llu", num);
                }
            }
            txn_ptr.release();
        }
    }
}

void StorageReaderThreadMain(uint64_t id) {
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    MOT_LOG_INFO("线程 StorageReaderThreadMain 开始工作 %llu", id);
    auto msg_ptr = std::make_unique<proto::Message>();
    while(true) {
        storage_read_queue.wait_dequeue(msg_ptr);
        //handle client read request

        //send client read response
    }
}

void StorageInit() {

}

uint64_t GetSleeptime(){
    uint64_t sleep_time;
    struct timeval current_time;
    uint64_t current_time_ll;
    gettimeofday(&current_time, NULL);
    current_time_ll = current_time.tv_sec * 1000000 + current_time.tv_usec;
    sleep_time = current_time_ll - (start_time_ll + (long)(current_epoch.load() - start_physical_epoch) * kSleepTime);
    if(sleep_time >= kSleepTime){
        MOT_LOG_INFO("start time : %llu, current time : %llu, 差值 %llu ,sleep time : %d", start_time_ll, current_time_ll, sleep_time, 0);
        return 0;
    }
    else{
        // MOT_LOG_INFO("start time : %llu, current time : %llu, 差值 %llu, sleep time : %llu", start_time_ll, current_time_ll, sleep_time, ksleeptime - sleep_time);
        return kSleepTime - sleep_time;
    }
}

void StorageManagerThreadMain(uint64_t id) { //handle other status
    MOT_LOG_INFO("线程 StorageManagerThreadMain 开始工作 %llu", id);
    StorageInit();
    storage_init_ok_flag.store(true);
    usleep(1000000);
    storage_start_flag.store(true);
//    auto msg_ptr = std::make_unique<proto::Message>();

    zmq::message_t message;
    zmq::context_t context(1);
    zmq::socket_t request_puller(context, ZMQ_PULL);
    request_puller.bind("tcp://*:5546");
    MOT_LOG_INFO("Storage 等待接受同步消息");
    request_puller.recv(&message);
    while(true) usleep(1000000000);
//    uint64_t sleep_time = static_cast<uint64_t>((((start_time.tv_sec / 60) + 1) * 60) * 1000000);
//    usleep(sleep_time - start_time_ll);
//    gettimeofday(&start_time, NULL);
//    start_time_ll = start_time.tv_sec * 1000000 + start_time.tv_usec;
//    usleep(50000);
//    auto epoch_mod = current_epoch.load() % cache_size;
//    while(true) {
//        usleep(GetSleeptime());
//        current_epoch.fetch_add(1);
//        epoch_mod = epoch_mod + 1 % cache_size;
//        shoule_update_txn_num[epoch_mod] = std::make_unique<std::atomic<uint64_t>>(0);
//        updated_txn_num[epoch_mod] = std::make_unique<std::atomic<uint64_t>>(0);
//        // SendPullRequest(current_epoch.load() - 5); //delay 5 epoch
//    }
}

void StorageWorker1ThreadMain(uint64_t id) { // Listen SUB
    SetCPU();
    MOT_LOG_INFO("线程 StorageWorker1ThreadMain Listener Subscribe Mode 开始工作 %llu", id);
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    while(storage_init_ok_flag.load() == false) usleep(200);
    zmq::context_t listen_context(1);
    zmq::socket_t socket_listen(listen_context, ZMQ_SUB);
    int queue_length = 0;
    socket_listen.setsockopt(ZMQ_SUBSCRIBE, "", 0);
    socket_listen.setsockopt(ZMQ_RCVHWM, &queue_length, sizeof(queue_length));
    for(int i = 0; i < kTxnNodeIp.size(); i ++) {
        socket_listen.connect("tcp://" + kTxnNodeIp[i] + ":5556");
        MOT_LOG_INFO("线程开始工作 ListenThread %s", ("tcp://" + kTxnNodeIp[i] + ":5556").c_str());
    }

    std::unique_ptr<zmq::message_t> message_ptr;
    for(;;) {
        message_ptr = std::make_unique<zmq::message_t>();
        socket_listen.recv(&(*message_ptr));
//        MOT_LOG_INFO("Listen SUB receive a message");
        if(!storage_listen_message_queue.enqueue(std::move(message_ptr))) assert(false);
        if(!storage_listen_message_queue.enqueue(std::move(std::make_unique<zmq::message_t>()))) assert(false);
    }
}





void StorageSendThreadMain(uint64_t id) { // PULL PUSH
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    MOT_LOG_INFO("线程 StorageSendThreadMain 开始工作 %llu", id);
    auto storage_send_context = std::make_shared<zmq::context_t>(1);
    auto storage_send_socket = std::make_shared<zmq::socket_t>(*storage_send_context, ZMQ_PUSH);
    storage_send_socket->connect("tcp://" + kTxnNodeIp[0] + ":5553");
    std::unique_ptr<send_thread_params> params;
    std::unique_ptr<zmq::message_t> msg;
    while(storage_init_ok_flag.load() == false) usleep(200);
    while(true) {
        storage_send_message_queue.wait_dequeue(params);
        if(params != nullptr && params->merge_request_ptr != nullptr) {
            msg = std::make_unique<zmq::message_t>(static_cast<void*>(const_cast<char*>(params->merge_request_ptr->data())),
                    params->merge_request_ptr->size(), string_free, static_cast<void*>(params->merge_request_ptr));
            storage_send_socket->send(*(msg));
        }
    }
}

void StorageListenThreadMain(uint64_t id) {// PULL PUSH
    MOT::SessionContext* session_context = MOT::GetSessionManager()->
                                           CreateSessionContext(IS_PGXC_COORDINATOR, 0, nullptr, INVALID_CONNECTION_ID);
    MOT::TxnManager* txn_manager = session_context->GetTxnManager();
    MOT_LOG_INFO("线程 StorageListenThreadMain 开始工作 %llu", id);
    auto storage_listen_context = std::make_shared<zmq::context_t>(2);
    auto storage_listen_socket = std::make_shared<zmq::socket_t>(*storage_listen_context, ZMQ_PULL);
    storage_listen_socket->bind("tcp://*:5554");
    while(storage_init_ok_flag.load() == false) usleep(200);
    std::unique_ptr<zmq::message_t> message_ptr;
    while(true) {
        message_ptr = std::make_unique<zmq::message_t>();
        storage_listen_socket->recv(&(*message_ptr));
        storage_listen_message_queue.enqueue(std::move(message_ptr));
        storage_listen_message_queue.enqueue(std::move(std::make_unique<zmq::message_t>()));
    }
}