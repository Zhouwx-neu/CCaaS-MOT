/* -------------------------------------------------------------------------
 *
 * postgres_fdw.c
 * 		  Foreign-data wrapper for remote PostgreSQL servers
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 * 		  contrib/postgres_fdw/postgres_fdw.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "postgres_fdw.h"

#include "access/htup.h"
#include "access/sysattr.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "knl/knl_guc.h"
#include "access/tableam.h"

PG_MODULE_MAGIC;

/* Default CPU cost to start up a foreign query. */
#define DEFAULT_FDW_STARTUP_COST 100.0

/* Default CPU cost to process 1 row (above and beyond cpu_tuple_cost). */
#define DEFAULT_FDW_TUPLE_COST 0.01

/*
 * FDW-specific planner information kept in RelOptInfo.fdw_private for a
 * foreign table.  This information is collected by postgresGetForeignRelSize.
 */
typedef struct PgFdwRelationInfo {
    /* baserestrictinfo clauses, broken down into safe and unsafe subsets. */
    List *remote_conds;
    List *local_conds;

    /* Bitmap of attr numbers we need to fetch from the remote server. */
    Bitmapset *attrs_used;

    /* Cost and selectivity of local_conds. */
    QualCost local_conds_cost;
    Selectivity local_conds_sel;

    /* Estimated size and cost for a scan with baserestrictinfo quals. */
    double rows;
    int width;
    Cost startup_cost;
    Cost total_cost;

    /* Options extracted from catalogs. */
    bool use_remote_estimate;
    Cost fdw_startup_cost;
    Cost fdw_tuple_cost;

    /* Cached catalog information. */
    ForeignTable *table;
    ForeignServer *server;
    UserMapping *user; /* only set in use_remote_estimate mode */
} PgFdwRelationInfo;

/*
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * We store various information in ForeignScan.fdw_private to pass it from
 * planner to executor.  Currently we store:
 *
 * 1) SELECT statement text to be sent to the remote server
 * 2) Integer list of attribute numbers retrieved by the SELECT
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth().  For example, to get the SELECT
 * statement: sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql))
 */
enum FdwScanPrivateIndex {
    /* SQL statement to execute remotely (as a String node) */
    FdwScanPrivateSelectSql,
    /* Integer list of attribute numbers retrieved by the SELECT */
    FdwScanPrivateRetrievedAttrs
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ModifyTable node referencing a postgres_fdw foreign table.  We store:
 *
 * 1) INSERT/UPDATE/DELETE statement text to be sent to the remote server
 * 2) Integer list of target attribute numbers for INSERT/UPDATE
 * 	  (NIL for a DELETE)
 * 3) Boolean flag showing if the remote query has a RETURNING clause
 * 4) Integer list of attribute numbers retrieved by RETURNING, if any
 */
enum FdwModifyPrivateIndex {
    /* SQL statement to execute remotely (as a String node) */
    FdwModifyPrivateUpdateSql,
    /* Integer list of target attribute numbers for INSERT/UPDATE */
    FdwModifyPrivateTargetAttnums,
    /* has-returning flag (as an integer Value node) */
    FdwModifyPrivateHasReturning,
    /* Integer list of attribute numbers retrieved by RETURNING */
    FdwModifyPrivateRetrievedAttrs
};

/*
 * Execution state of a foreign scan using postgres_fdw.
 */
typedef struct PgFdwScanState {
    Relation rel;             /* relcache entry for the foreign table */
    AttInMetadata *attinmeta; /* attribute datatype conversion metadata */

    /* extracted fdw_private data */
    char *query;           /* text of SELECT command */
    List *retrieved_attrs; /* list of retrieved attribute numbers */

    /* for remote query execution */
    PGconn *conn;               /* connection for the scan */
    unsigned int cursor_number; /* quasi-unique ID for my cursor */
    bool cursor_exists;         /* have we created the cursor? */
    int numParams;              /* number of parameters passed to query */
    FmgrInfo *param_flinfo;     /* output conversion functions for them */
    List *param_exprs;          /* executable expressions for param values */
    const char **param_values;  /* textual values of query parameters */

    /* for storing result tuples */
    HeapTuple *tuples; /* array of currently-retrieved tuples */
    int num_tuples;    /* # of tuples in array */
    int next_tuple;    /* index of next one to return */

    /* batch-level state, for optimizing rewinds and avoiding useless fetch */
    int fetch_ct_2;   /* Min(# of fetches done, 2) */
    bool eof_reached; /* true if last fetch reached EOF */

    /* working memory contexts */
    MemoryContext batch_cxt; /* context holding current batch of tuples */
    MemoryContext temp_cxt;  /* context for per-tuple temporary data */
} PgFdwScanState;

/*
 * Execution state of a foreign insert/update/delete operation.
 */
typedef struct PgFdwModifyState {
    Relation rel;             /* relcache entry for the foreign table */
    AttInMetadata *attinmeta; /* attribute datatype conversion metadata */

    /* for remote query execution */
    PGconn *conn; /* connection for the scan */
    char *p_name; /* name of prepared statement, if created */

    /* extracted fdw_private data */
    char *query;           /* text of INSERT/UPDATE/DELETE command */
    List *target_attrs;    /* list of target attribute numbers */
    bool has_returning;    /* is there a RETURNING clause? */
    List *retrieved_attrs; /* attr numbers retrieved by RETURNING */

    /* info about parameters for prepared statement */
    AttrNumber ctidAttno; /* attnum of input resjunk ctid column */
    int p_nums;           /* number of parameters to transmit */
    FmgrInfo *p_flinfo;   /* output conversion functions for them */

    /* working memory context */
    MemoryContext temp_cxt; /* context for per-tuple temporary data */
} PgFdwModifyState;

/*
 * Workspace for analyzing a foreign table.
 */
typedef struct PgFdwAnalyzeState {
    Relation rel;             /* relcache entry for the foreign table */
    AttInMetadata *attinmeta; /* attribute datatype conversion metadata */
    List *retrieved_attrs;    /* attr numbers retrieved by query */

    /* collected sample rows */
    HeapTuple *rows; /* array of size targrows */
    int targrows;    /* target # of sample rows */
    int numrows;     /* # of sample rows collected */

    /* for random sampling */
    double samplerows; /* # of rows fetched */
    double rowstoskip; /* # of rows to skip before next sample */
    double rstate;     /* random state */

    /* working memory contexts */
    MemoryContext anl_cxt;  /* context for per-analyze lifespan data */
    MemoryContext temp_cxt; /* context for per-tuple temporary data */
} PgFdwAnalyzeState;

/*
 * Identify the attribute where data conversion fails.
 */
typedef struct ConversionLocation {
    Relation rel;         /* foreign table's relcache entry */
    AttrNumber cur_attno; /* attribute number being processed, or 0 */
} ConversionLocation;

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(postgres_fdw_handler);
extern "C" Datum postgres_fdw_handler(PG_FUNCTION_ARGS);

/*
 * FDW callback routines
 */
static void postgresGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static void postgresGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static ForeignScan *postgresGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
    ForeignPath *best_path, List *tlist, List *scan_clauses);
static void postgresBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *postgresIterateForeignScan(ForeignScanState *node);
static void postgresReScanForeignScan(ForeignScanState *node);
static void postgresEndForeignScan(ForeignScanState *node);
static void postgresAddForeignUpdateTargets(Query *parsetree, RangeTblEntry *target_rte, Relation target_relation);
static List *postgresPlanForeignModify(PlannerInfo *root, ModifyTable *plan, Index resultRelation, int subplan_index);
static void postgresBeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *resultRelInfo, List *fdw_private,
    int subplan_index, int eflags);
static TupleTableSlot *postgresExecForeignInsert(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
    TupleTableSlot *planSlot);
static TupleTableSlot *postgresExecForeignUpdate(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
    TupleTableSlot *planSlot);
static TupleTableSlot *postgresExecForeignDelete(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
    TupleTableSlot *planSlot);
static void postgresEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo);
static int postgresIsForeignRelUpdatable(Relation rel);
static void postgresExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void postgresExplainForeignModify(ModifyTableState *mtstate, ResultRelInfo *rinfo, List *fdw_private,
    int subplan_index, ExplainState *es);
static bool postgresAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func, BlockNumber *totalpages,
    void *additionalData, bool estimate_table_rownum);

/*
 * Helper functions
 */
static void estimate_path_cost_size(PlannerInfo *root, RelOptInfo *baserel, List *join_conds, double *p_rows,
    int *p_width, Cost *p_startup_cost, Cost *p_total_cost);
static void get_remote_estimate(const char *sql, PGconn *conn, double *rows, int *width, Cost *startup_cost,
    Cost *total_cost);
static void create_cursor(ForeignScanState *node);
static void fetch_more_data(ForeignScanState *node);
static void close_cursor(PGconn *conn, unsigned int cursor_number);
static PgFdwModifyState *createForeignModify(EState *estate, RangeTblEntry *rte, ResultRelInfo *resultRelInfo,
    CmdType operation, Plan *subplan, char *query, List *target_attrs, bool has_returning, List *retrieved_attrs);

static void prepare_foreign_modify(PgFdwModifyState *fmstate);
static const char **convert_prep_stmt_params(PgFdwModifyState *fmstate, ItemPointer tupleid, TupleTableSlot *slot);
static void store_returning_result(PgFdwModifyState *fmstate, TupleTableSlot *slot, PGresult *res);
static int postgresAcquireSampleRowsFunc(Relation relation, int elevel, HeapTuple *rows, int targrows,
    double *totalrows, double *totaldeadrows, void *additionalData, bool estimate_table_rownum);
static void analyze_row_processor(PGresult *res, int row, PgFdwAnalyzeState *astate);
static HeapTuple make_tuple_from_result_row(PGresult *res, int row, Relation rel, AttInMetadata *attinmeta,
    List *retrieved_attrs, MemoryContext temp_context);
static void conversion_error_callback(void *arg);


/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum postgres_fdw_handler(PG_FUNCTION_ARGS)
{
    FdwRoutine *routine = makeNode(FdwRoutine);

    /* Functions for scanning foreign tables */
    routine->GetForeignRelSize = postgresGetForeignRelSize;
    routine->GetForeignPaths = postgresGetForeignPaths;
    routine->GetForeignPlan = postgresGetForeignPlan;
    routine->BeginForeignScan = postgresBeginForeignScan;
    routine->IterateForeignScan = postgresIterateForeignScan;
    routine->ReScanForeignScan = postgresReScanForeignScan;
    routine->EndForeignScan = postgresEndForeignScan;

    /* Functions for updating foreign tables */
    routine->AddForeignUpdateTargets = postgresAddForeignUpdateTargets;
    routine->PlanForeignModify = postgresPlanForeignModify;
    routine->BeginForeignModify = postgresBeginForeignModify;
    routine->ExecForeignInsert = postgresExecForeignInsert;
    routine->ExecForeignUpdate = postgresExecForeignUpdate;
    routine->ExecForeignDelete = postgresExecForeignDelete;
    routine->EndForeignModify = postgresEndForeignModify;
    routine->IsForeignRelUpdatable = postgresIsForeignRelUpdatable;

    /* Support functions for EXPLAIN */
    routine->ExplainForeignScan = postgresExplainForeignScan;
    routine->ExplainForeignModify = postgresExplainForeignModify;

    /* Support functions for ANALYZE */
    routine->AnalyzeForeignTable = postgresAnalyzeForeignTable;
    routine->AcquireSampleRows = postgresAcquireSampleRowsFunc;

    PG_RETURN_POINTER(routine);
}

/*
 * postgresGetForeignRelSize
 * 		Estimate # of rows and width of the result of the scan
 *
 * We should consider the effect of all baserestrictinfo clauses here, but
 * not any join clauses.
 */
static void postgresGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
    ListCell *lc = NULL;

    /*
     * We use PgFdwRelationInfo to pass various information to subsequent
     * functions.
     */
    PgFdwRelationInfo* fpinfo = (PgFdwRelationInfo *)palloc0(sizeof(PgFdwRelationInfo));
    baserel->fdw_private = (void *)fpinfo;

    /* Look up foreign-table catalog info. */
    fpinfo->table = GetForeignTable(foreigntableid);
    fpinfo->server = GetForeignServer(fpinfo->table->serverid);

    /*
     * Extract user-settable option values.  Note that per-table setting of
     * use_remote_estimate overrides per-server setting.
     */
    fpinfo->use_remote_estimate = false;
    fpinfo->fdw_startup_cost = DEFAULT_FDW_STARTUP_COST;
    fpinfo->fdw_tuple_cost = DEFAULT_FDW_TUPLE_COST;

    foreach (lc, fpinfo->server->options) {
        DefElem *def = (DefElem *)lfirst(lc);

        if (strcmp(def->defname, "use_remote_estimate") == 0) {
            fpinfo->use_remote_estimate = defGetBoolean(def);
        } else if (strcmp(def->defname, "fdw_startup_cost") == 0) {
            fpinfo->fdw_startup_cost = strtod(defGetString(def), NULL);
        } else if (strcmp(def->defname, "fdw_tuple_cost") == 0) {
            fpinfo->fdw_tuple_cost = strtod(defGetString(def), NULL);
        }
    }
    foreach (lc, fpinfo->table->options) {
        DefElem *def = (DefElem *)lfirst(lc);

        if (strcmp(def->defname, "use_remote_estimate") == 0) {
            fpinfo->use_remote_estimate = defGetBoolean(def);
            break; /* only need the one value */
        }
    }

    /*
     * If the table or the server is configured to use remote estimates,
     * identify which user to do remote access as during planning.  This
     * should match what ExecCheckRTEPerms() does.  If we fail due to lack of
     * permissions, the query would have failed at runtime anyway.
     */
    if (fpinfo->use_remote_estimate) {
        RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);
        Oid userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

        fpinfo->user = GetUserMapping(userid, fpinfo->server->serverid);
    } else {
        fpinfo->user = NULL;
    }

    /*
     * Identify which baserestrictinfo clauses can be sent to the remote
     * server and which can't.
     */
    classifyConditions(root, baserel, baserel->baserestrictinfo, &fpinfo->remote_conds, &fpinfo->local_conds);

    /*
     * Identify which attributes will need to be retrieved from the remote
     * server.  These include all attrs needed for joins or final output, plus
     * all attrs used in the local_conds.  (Note: if we end up using a
     * parameterized scan, it's possible that some of the join clauses will be
     * sent to the remote and thus we wouldn't really need to retrieve the
     * columns used in them.  Doesn't seem worth detecting that case though.)
     */
    fpinfo->attrs_used = NULL;
    pull_varattnos((Node *)baserel->reltargetlist, baserel->relid, &fpinfo->attrs_used);
    foreach (lc, fpinfo->local_conds) {
        RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc);

        pull_varattnos((Node *)rinfo->clause, baserel->relid, &fpinfo->attrs_used);
    }

    /*
     * Compute the selectivity and cost of the local_conds, so we don't have
     * to do it over again for each path.  The best we can do for these
     * conditions is to estimate selectivity on the basis of local statistics.
     */
    fpinfo->local_conds_sel = clauselist_selectivity(root, fpinfo->local_conds, baserel->relid, JOIN_INNER, NULL);

    cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

    /*
     * If the table or the server is configured to use remote estimates,
     * connect to the foreign server and execute EXPLAIN to estimate the
     * number of rows selected by the restriction clauses, as well as the
     * average row width.  Otherwise, estimate using whatever statistics we
     * have locally, in a way similar to ordinary tables.
     */
    if (fpinfo->use_remote_estimate) {
        /*
         * Get cost/size estimates with help of remote server.  Save the
         * values in fpinfo so we don't need to do it again to generate the
         * basic foreign path.
         */
        estimate_path_cost_size(root, baserel, NIL, &fpinfo->rows, &fpinfo->width, &fpinfo->startup_cost,
            &fpinfo->total_cost);

        /* Report estimated baserel size to planner. */
        baserel->rows = fpinfo->rows;
        baserel->width = fpinfo->width;
    } else {
        /*
         * If the foreign table has never been ANALYZEd, it will have relpages
         * and reltuples equal to zero, which most likely has nothing to do
         * with reality.  We can't do a whole lot about that if we're not
         * allowed to consult the remote server, but we can use a hack similar
         * to plancat.c's treatment of empty relations: use a minimum size
         * estimate of 10 pages, and divide by the column-datatype-based width
         * estimate to get the corresponding number of tuples.
         */
        if (baserel->pages == 0 && baserel->tuples == 0) {
            baserel->pages = 10;
            baserel->tuples = (double)(10 * BLCKSZ) / (baserel->width + sizeof(HeapTupleHeaderData));
        }

        /* Estimate baserel size as best we can with local statistics. */
        set_baserel_size_estimates(root, baserel);

        /* Fill in basically-bogus cost estimates for use later. */
        estimate_path_cost_size(root, baserel, NIL, &fpinfo->rows, &fpinfo->width, &fpinfo->startup_cost,
            &fpinfo->total_cost);
    }
}

/*
 * postgresGetForeignPaths
 * 		Create possible scan paths for a scan on the foreign table
 */
static void postgresGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
    PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *)baserel->fdw_private;
    ListCell *lc = NULL;

    /*
     * Create simplest ForeignScan path node and add it to baserel.  This path
     * corresponds to SeqScan path of regular tables (though depending on what
     * baserestrict conditions we were able to send to remote, there might
     * actually be an indexscan happening there).  We already did all the work
     * to estimate cost and size of this path.
     *
     * Although this path uses no join clauses, it could still have required
     * parameterization due to LATERAL refs in its tlist.
     */
    ForeignPath* path = create_foreignscan_path(root, baserel,
        fpinfo->startup_cost, fpinfo->total_cost, NIL, /* no pathkeys */
        NULL, NIL);                                    /* no fdw_private list */
    add_path(root, baserel, (Path *)path);

    /*
     * If we're not using remote estimates, stop here.  We have no way to
     * estimate whether any join clauses would be worth sending across, so
     * don't bother building parameterized paths.
     */
    if (!fpinfo->use_remote_estimate) {
        return;
    }

    /*
     * Thumb through all join clauses for the rel to identify which outer
     * relations could supply one or more safe-to-send-to-remote join clauses.
     * We'll build a parameterized path for each such outer relation.
     *
     * It's convenient to manage this by representing each candidate outer
     * relation by the ParamPathInfo node for it.  We can then use the
     * ppi_clauses list in the ParamPathInfo node directly as a list of the
     * interesting join clauses for that rel.  This takes care of the
     * possibility that there are multiple safe join clauses for such a rel,
     * and also ensures that we account for unsafe join clauses that we'll
     * still have to enforce locally (since the parameterized-path machinery
     * insists that we handle all movable clauses).
     */
    List *ppi_list = NIL;
    foreach (lc, baserel->joininfo) {
        RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc);

        /* Check if clause can be moved to this rel */
        if (!join_clause_is_movable_to(rinfo, baserel->relid)) {
            continue;
        }

        /* See if it is safe to send to remote */
        if (!is_foreign_expr(root, baserel, rinfo->clause)) {
            continue;
        }

        /* Calculate required outer rels for the resulting path */
        Relids required_outer = bms_union(rinfo->clause_relids, NULL);
        /* We do not want the foreign rel itself listed in required_outer */
        required_outer = bms_del_member(required_outer, baserel->relid);

        /*
         * required_outer probably can't be empty here, but if it were, we
         * couldn't make a parameterized path.
         */
        if (bms_is_empty(required_outer)) {
            continue;
        }

        /* Get the ParamPathInfo */
        ParamPathInfo* param_info = get_baserel_parampathinfo(root, baserel, required_outer);
        Assert(param_info != NULL);

        /*
         * Add it to list unless we already have it.  Testing pointer equality
         * is OK since get_baserel_parampathinfo won't make duplicates.
         */
        ppi_list = list_append_unique_ptr(ppi_list, param_info);
    }

    /*
     * Now build a path for each useful outer relation.
     */
    foreach (lc, ppi_list) {
        ParamPathInfo *param_info = (ParamPathInfo *)lfirst(lc);
        double rows;
        int width;
        Cost startup_cost;
        Cost total_cost;

        /* Get a cost estimate from the remote */
        estimate_path_cost_size(root, baserel, param_info->ppi_clauses, &rows, &width, &startup_cost, &total_cost);

        /*
         * ppi_rows currently won't get looked at by anything, but still we
         * may as well ensure that it matches our idea of the rowcount.
         */
        param_info->ppi_rows = rows;

        /* Make the path */
        path = create_foreignscan_path(root, baserel,
            startup_cost, total_cost, NIL,   /* no pathkeys */
            param_info->ppi_req_outer, NIL); /* no fdw_private list */
        add_path(root, baserel, (Path *)path);
    }
}

/*
 * postgresGetForeignPlan
 * 		Create ForeignScan plan node which implements selected best path
 */
static ForeignScan *postgresGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
    ForeignPath *best_path, List *tlist, List *scan_clauses)
{
    PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *)baserel->fdw_private;
    Index scan_relid = baserel->relid;
    List *remote_conds = NIL;
    List *local_exprs = NIL;
    List *params_list = NIL;
    List *retrieved_attrs = NIL;
    StringInfoData sql;
    ListCell *lc = NULL;

    /*
     * Separate the scan_clauses into those that can be executed remotely and
     * those that can't.  baserestrictinfo clauses that were previously
     * determined to be safe or unsafe by classifyConditions are shown in
     * fpinfo->remote_conds and fpinfo->local_conds.  Anything else in the
     * scan_clauses list will be a join clause, which we have to check for
     * remote-safety.
     *
     * Note: the join clauses we see here should be the exact same ones
     * previously examined by postgresGetForeignPaths.  Possibly it'd be worth
     * passing forward the classification work done then, rather than
     * repeating it here.
     *
     * This code must match "extract_actual_clauses(scan_clauses, false)"
     * except for the additional decision about remote versus local execution.
     * Note however that we only strip the RestrictInfo nodes from the
     * local_exprs list, since appendWhereClause expects a list of
     * RestrictInfos.
     */
    foreach (lc, scan_clauses) {
        RestrictInfo *rinfo = (RestrictInfo *)lfirst(lc);

        Assert(IsA(rinfo, RestrictInfo));

        /* Ignore any pseudoconstants, they're dealt with elsewhere */
        if (rinfo->pseudoconstant) {
            continue;
        }

        if (list_member_ptr(fpinfo->remote_conds, rinfo)) {
            remote_conds = lappend(remote_conds, rinfo);
        } else if (list_member_ptr(fpinfo->local_conds, rinfo)) {
            local_exprs = lappend(local_exprs, rinfo->clause);
        } else if (is_foreign_expr(root, baserel, rinfo->clause)) {
            remote_conds = lappend(remote_conds, rinfo);
        } else {
            local_exprs = lappend(local_exprs, rinfo->clause);
        }
    }

    /*
     * Build the query string to be sent for execution, and identify
     * expressions to be sent as parameters.
     */
    initStringInfo(&sql);
    deparseSelectSql(&sql, root, baserel, fpinfo->attrs_used, &retrieved_attrs);
    if (remote_conds) {
        appendWhereClause(&sql, root, baserel, remote_conds, true, &params_list);
    }

    /*
     * Add FOR UPDATE/SHARE if appropriate.  We apply locking during the
     * initial row fetch, rather than later on as is done for local tables.
     * The extra roundtrips involved in trying to duplicate the local
     * semantics exactly don't seem worthwhile (see also comments for
     * RowMarkType).
     *
     * Note: because we actually run the query as a cursor, this assumes that
     * DECLARE CURSOR ... FOR UPDATE is supported, which it isn't before 8.3.
     */
    if (baserel->relid == (unsigned int)root->parse->resultRelation &&
        (root->parse->commandType == CMD_UPDATE || root->parse->commandType == CMD_DELETE)) {
        /* Relation is UPDATE/DELETE target, so use FOR UPDATE */
        appendStringInfoString(&sql, " FOR UPDATE");
    } else {
        RowMarkClause *rc = get_parse_rowmark(root->parse, baserel->relid);

        if (rc) {
            /*
             * Relation is specified as a FOR UPDATE/SHARE target, so handle
             * that.
             *
             * For now, just ignore any [NO] KEY specification, since (a) it's
             * not clear what that means for a remote table that we don't have
             * complete information about, and (b) it wouldn't work anyway on
             * older remote servers.  Likewise, we don't worry about NOWAIT.
             */
            if (rc->forUpdate) {
                appendStringInfoString(&sql, " FOR UPDATE");
            } else {
                appendStringInfoString(&sql, " FOR SHARE");
            }
        }
    }

    /*
     * Build the fdw_private list that will be available to the executor.
     * Items in the list must match enum FdwScanPrivateIndex, above.
     */
    List* fdw_private = list_make2(makeString(sql.data), retrieved_attrs);

    /*
     * Create the ForeignScan node from target list, local filtering
     * expressions, remote parameter expressions, and FDW private information.
     *
     * Note that the remote parameter expressions are stored in the fdw_exprs
     * field of the finished plan node; we can't keep them in private state
     * because then they wouldn't be subject to later planner processing.
     */
    return make_foreignscan(tlist, local_exprs, scan_relid, params_list, fdw_private);
}

/*
 * postgresBeginForeignScan
 * 		Initiate an executor scan of a foreign PostgreSQL table.
 */
static void postgresBeginForeignScan(ForeignScanState *node, int eflags)
{
    ForeignScan *fsplan = (ForeignScan *)node->ss.ps.plan;
    EState *estate = node->ss.ps.state;
    Oid userid;
    int numParams;
    int i;
    ListCell *lc = NULL;

    /*
     * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
        return;
    }

    /*
     * We'll save private state in node->fdw_state.
     */
    PgFdwScanState* fsstate = (PgFdwScanState *)palloc0(sizeof(PgFdwScanState));
    node->fdw_state = (void *)fsstate;

    /*
     * Identify which user to do the remote access as.  This should match what
     * ExecCheckRTEPerms() does.
     */
    RangeTblEntry* rte = rt_fetch(fsplan->scan.scanrelid, estate->es_range_table);
    userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

    /* Get info about foreign table. */
    fsstate->rel = node->ss.ss_currentRelation;
    ForeignTable* table = GetForeignTable(RelationGetRelid(fsstate->rel));
    ForeignServer* server = GetForeignServer(table->serverid);
    UserMapping* user = GetUserMapping(userid, server->serverid);

    /*
     * Get connection to the foreign server.  Connection manager will
     * establish new connection if necessary.
     */
    fsstate->conn = GetConnection(server, user, false);

    /* Assign a unique ID for my cursor */
    fsstate->cursor_number = GetCursorNumber(fsstate->conn);
    fsstate->cursor_exists = false;

    /* Get private info created by planner functions. */
    fsstate->query = strVal(list_nth(fsplan->fdw_private, FdwScanPrivateSelectSql));
    fsstate->retrieved_attrs = (List *)list_nth(fsplan->fdw_private, FdwScanPrivateRetrievedAttrs);

    /* Create contexts for batches of tuples and per-tuple temp workspace. */
    fsstate->batch_cxt = AllocSetContextCreate(estate->es_query_cxt, "postgres_fdw tuple data",
        ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
    fsstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt, "postgres_fdw temporary data",
        ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE);

    /* Get info we'll need for input data conversion. */
    fsstate->attinmeta = TupleDescGetAttInMetadata(RelationGetDescr(fsstate->rel));

    /* Prepare for output conversion of parameters used in remote query. */
    numParams = list_length(fsplan->fdw_exprs);
    fsstate->numParams = numParams;
    fsstate->param_flinfo = (FmgrInfo *)palloc0(sizeof(FmgrInfo) * numParams);

    i = 0;
    foreach (lc, fsplan->fdw_exprs) {
        Node *param_expr = (Node *)lfirst(lc);
        Oid typefnoid;
        bool isvarlena;

        getTypeOutputInfo(exprType(param_expr), &typefnoid, &isvarlena);
        fmgr_info(typefnoid, &fsstate->param_flinfo[i]);
        i++;
    }

    /*
     * Prepare remote-parameter expressions for evaluation.  (Note: in
     * practice, we expect that all these expressions will be just Params, so
     * we could possibly do something more efficient than using the full
     * expression-eval machinery for this.  But probably there would be little
     * benefit, and it'd require postgres_fdw to know more than is desirable
     * about Param evaluation.)
     */
    fsstate->param_exprs = (List *)ExecInitExpr((Expr *)fsplan->fdw_exprs, (PlanState *)node);

    /*
     * Allocate buffer for text form of query parameters, if any.
     */
    if (numParams > 0) {
        fsstate->param_values = (const char **)palloc0(numParams * sizeof(char *));
    } else {
        fsstate->param_values = NULL;
    }
}

/*
 * postgresIterateForeignScan
 * 		Retrieve next row from the result set, or clear tuple slot to indicate
 * 		EOF.
 */
static TupleTableSlot *postgresIterateForeignScan(ForeignScanState *node)
{
    PgFdwScanState *fsstate = (PgFdwScanState *)node->fdw_state;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

    /*
     * If this is the first call after Begin or ReScan, we need to create the
     * cursor on the remote side.
     */
    if (!fsstate->cursor_exists) {
        create_cursor(node);
    }

    /*
     * Get some more tuples, if we've run out.
     */
    if (fsstate->next_tuple >= fsstate->num_tuples) {
        /* No point in another fetch if we already detected EOF, though. */
        if (!fsstate->eof_reached) {
            fetch_more_data(node);
        }
        /* If we didn't get any tuples, must be end of data. */
        if (fsstate->next_tuple >= fsstate->num_tuples) {
            return ExecClearTuple(slot);
        }
    }

    /*
     * Return the next tuple.
     */
    (void)ExecStoreTuple(fsstate->tuples[fsstate->next_tuple++], slot, InvalidBuffer, false);

    return slot;
}

/*
 * postgresReScanForeignScan
 * 		Restart the scan.
 */
static void postgresReScanForeignScan(ForeignScanState *node)
{
    PgFdwScanState *fsstate = (PgFdwScanState *)node->fdw_state;
    char sql[64];
    int rc;

    /* If we haven't created the cursor yet, nothing to do. */
    if (!fsstate->cursor_exists) {
        return;
    }

    /*
     * If any internal parameters affecting this node have changed, we'd
     * better destroy and recreate the cursor.  Otherwise, rewinding it should
     * be good enough.  If we've only fetched zero or one batch, we needn't
     * even rewind the cursor, just rescan what we have.
     */
    if (node->ss.ps.chgParam != NULL) {
        fsstate->cursor_exists = false;
        rc = snprintf_s(sql, sizeof(sql), sizeof(sql) - 1, "CLOSE c%u", fsstate->cursor_number);
        securec_check_ss(rc, "", "");
    } else if (fsstate->fetch_ct_2 > 1) {
        rc = snprintf_s(sql, sizeof(sql), sizeof(sql) - 1, "MOVE BACKWARD ALL IN c%u", fsstate->cursor_number);
        securec_check_ss(rc, "", "");
    } else {
        /* Easy: just rescan what we already have in memory, if anything */
        fsstate->next_tuple = 0;
        return;
    }

    /*
     * We don't use a PG_TRY block here, so be careful not to throw error
     * without releasing the PGresult.
     */
    PGresult* res = pgfdw_exec_query(fsstate->conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        pgfdw_report_error(ERROR, res, fsstate->conn, true, sql);
    }
    PQclear(res);

    /* Now force a fresh FETCH. */
    fsstate->tuples = NULL;
    fsstate->num_tuples = 0;
    fsstate->next_tuple = 0;
    fsstate->fetch_ct_2 = 0;
    fsstate->eof_reached = false;
}

/*
 * postgresEndForeignScan
 * 		Finish scanning foreign table and dispose objects used for this scan
 */
static void postgresEndForeignScan(ForeignScanState *node)
{
    PgFdwScanState *fsstate = (PgFdwScanState *)node->fdw_state;

    /* if fsstate is NULL, we are in EXPLAIN; nothing to do */
    if (fsstate == NULL) {
        return;
    }

    /* Close the cursor if open, to prevent accumulation of cursors */
    if (fsstate->cursor_exists) {
        close_cursor(fsstate->conn, fsstate->cursor_number);
    }

    /* Release remote connection */
    ReleaseConnection(fsstate->conn);
    fsstate->conn = NULL;

    /* MemoryContexts will be deleted automatically. */
}

/*
 * postgresAddForeignUpdateTargets
 * 		Add resjunk column(s) needed for update/delete on a foreign table
 */
static void postgresAddForeignUpdateTargets(Query *parsetree, RangeTblEntry *target_rte, Relation target_relation)
{
    /*
     * In postgres_fdw, what we need is the ctid, same as for a regular table.
     * Make a Var representing the desired value
     */
    Var* var = makeVar((Index)parsetree->resultRelation, SelfItemPointerAttributeNumber, TIDOID, -1, InvalidOid, 0);

    /* Wrap it in a resjunk TLE with the right name ... */
    const char *attrname = "ctid";

    TargetEntry* tle = makeTargetEntry((Expr *)var, list_length(parsetree->targetList) + 1, pstrdup(attrname), true);

    /* ... and add it to the query's targetlist */
    parsetree->targetList = lappend(parsetree->targetList, tle);
}

/*
 * postgresPlanForeignModify
 * 		Plan an insert/update/delete operation on a foreign table
 *
 * Note: currently, the plan tree generated for UPDATE/DELETE will always
 * include a ForeignScan that retrieves ctids (using SELECT FOR UPDATE)
 * and then the ModifyTable node will have to execute individual remote
 * UPDATE/DELETE commands.  If there are no local conditions or joins
 * needed, it'd be better to let the scan node do UPDATE/DELETE RETURNING
 * and then do nothing at ModifyTable.  Room for future optimization ...
 */
static List *postgresPlanForeignModify(PlannerInfo *root, ModifyTable *plan, Index resultRelation, int subplan_index)
{
    CmdType operation = plan->operation;
    RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
    StringInfoData sql;
    List *targetAttrs = NIL;
    List *returningList = NIL;
    List *retrieved_attrs = NIL;

    /*
     * Core code already has some lock on each rel being planned, so we can
     * use NoLock here.
     */
    Relation rel = heap_open(rte->relid, NoLock);
    initStringInfo(&sql);

    /*
     * In an INSERT, we transmit all columns that are defined in the foreign
     * table.  In an UPDATE, if there are BEFORE ROW UPDATE triggers on the
     * foreign table, we transmit all columns like INSERT; else we transmit
     * only columns that were explicitly targets of the UPDATE, so as to avoid
     * unnecessary data transmission.  (We can't do that for INSERT since we
     * would miss sending default values for columns not listed in the source
     * statement, and for UPDATE if there are BEFORE ROW UPDATE triggers since
     * those triggers might change values for non-target columns, in which
     * case we would miss sending changed values for those columns.)
     */
    if (operation == CMD_INSERT ||
        (operation == CMD_UPDATE && rel->trigdesc && rel->trigdesc->trig_update_before_row)) {
        TupleDesc tupdesc = RelationGetDescr(rel);
        int attnum;

        for (attnum = 1; attnum <= tupdesc->natts; attnum++) {
            Form_pg_attribute attr = tupdesc->attrs[attnum - 1];

            if (!attr->attisdropped) {
                targetAttrs = lappend_int(targetAttrs, attnum);
            }
        }
    } else if (operation == CMD_UPDATE) {
        Bitmapset *tmpset = bms_copy(rte->updatedCols);
        AttrNumber col;

        while ((col = bms_first_member(tmpset)) >= 0) {
            col += FirstLowInvalidHeapAttributeNumber;
            if (col <= InvalidAttrNumber) { /* shouldn't happen */
                elog(ERROR, "system-column update is not supported");
            }
            targetAttrs = lappend_int(targetAttrs, col);
        }
    }

    /*
     * Extract the relevant RETURNING list if any.
     */
    if (plan->returningLists) {
        returningList = (List *)list_nth(plan->returningLists, subplan_index);
    }

    /*
     * Construct the SQL command string.
     */
    switch (operation) {
        case CMD_INSERT:
            deparseInsertSql(&sql, rte, resultRelation, rel, targetAttrs, returningList, &retrieved_attrs);
            break;
        case CMD_UPDATE:
            deparseUpdateSql(&sql, rte, resultRelation, rel, targetAttrs, returningList, &retrieved_attrs);
            break;
        case CMD_DELETE:
            deparseDeleteSql(&sql, rte, resultRelation, rel, returningList, &retrieved_attrs);
            break;
        default:
            elog(ERROR, "unexpected operation: %d", (int)operation);
            break;
    }

    heap_close(rel, NoLock);

    /*
     * Build the fdw_private list that will be available to the executor.
     * Items in the list must match enum FdwModifyPrivateIndex, above.
     */
    return list_make4(makeString(sql.data), targetAttrs, makeInteger((retrieved_attrs != NIL)), retrieved_attrs);
}

/*
 * postgresBeginForeignModify
 * 		Begin an insert/update/delete operation on a foreign table
 */
static void postgresBeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *resultRelInfo, List *fdw_private,
    int subplan_index, int eflags)
{
    /*
     * Do nothing in EXPLAIN (no ANALYZE) case.  resultRelInfo->ri_FdwState
     * stays NULL.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
        return;
    }

    /* Deconstruct fdw_private data. */
    char *query = strVal(list_nth(fdw_private, FdwModifyPrivateUpdateSql));
    List *target_attrs = (List *)list_nth(fdw_private, FdwModifyPrivateTargetAttnums);
    bool has_returning = (bool)intVal(list_nth(fdw_private, FdwModifyPrivateHasReturning));
    List *retrieved_attrs = (List *)list_nth(fdw_private, FdwModifyPrivateRetrievedAttrs);

    /* Find RTE. */
    RangeTblEntry *rte = rt_fetch(resultRelInfo->ri_RangeTableIndex, mtstate->ps.state->es_range_table);

    /* Construct an execution state. */
    PgFdwModifyState *fmstate = createForeignModify(mtstate->ps.state, rte, resultRelInfo, mtstate->operation,
        mtstate->mt_plans[subplan_index]->plan, query, target_attrs, has_returning, retrieved_attrs);
    resultRelInfo->ri_FdwState = fmstate;
}

/*
 * Construct an execution state of a foreign insert/update/delete operation
 */
static PgFdwModifyState *createForeignModify(EState *estate, RangeTblEntry *rte, ResultRelInfo *resultRelInfo,
    CmdType operation, Plan *subplan, char *query, List *target_attrs, bool has_returning, List *retrieved_attrs)
{
    PgFdwModifyState *fmstate;
    Relation rel = resultRelInfo->ri_RelationDesc;
    TupleDesc tupdesc = RelationGetDescr(rel);
    Oid userid;
    AttrNumber n_params;
    Oid typefnoid;
    bool isvarlena;
    ListCell *lc = NULL;

    /* Begin constructing PgFdwModifyState. */
    fmstate = (PgFdwModifyState *)palloc0(sizeof(PgFdwModifyState));
    fmstate->rel = rel;

    /*
     * Identify which user to do the remote access as.  This should match what
     * ExecCheckRTEPerms() does.
     */
    userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

    /* Get info about foreign table. */
    ForeignTable* table = GetForeignTable(RelationGetRelid(rel));
    UserMapping* user = GetUserMapping(userid, table->serverid);
    ForeignServer *server = GetForeignServer(table->serverid);

    /* Open connection; report that we'll create a prepared statement. */
    fmstate->conn = GetConnection(server, user, true);
    fmstate->p_name = NULL; /* prepared statement not made yet */

    /* Set up remote query information. */
    fmstate->query = query;
    fmstate->target_attrs = target_attrs;
    fmstate->has_returning = has_returning;
    fmstate->retrieved_attrs = retrieved_attrs;

    /* Create context for per-tuple temp workspace. */
    fmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt, "postgres_fdw temporary data",
        ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE);

    /* Prepare for input conversion of RETURNING results. */
    if (fmstate->has_returning) {
        fmstate->attinmeta = TupleDescGetAttInMetadata(tupdesc);
    }

    /* Prepare for output conversion of parameters used in prepared stmt. */
    n_params = list_length(fmstate->target_attrs) + 1;
    fmstate->p_flinfo = (FmgrInfo *)palloc0(sizeof(FmgrInfo) * n_params);
    fmstate->p_nums = 0;

    if (operation == CMD_UPDATE || operation == CMD_DELETE) {
        Assert(subplan != NULL);

        /* Find the ctid resjunk column in the subplan's result */
        fmstate->ctidAttno = ExecFindJunkAttributeInTlist(subplan->targetlist, "ctid");
        if (!AttributeNumberIsValid(fmstate->ctidAttno)) {
            elog(ERROR, "could not find junk ctid column");
        }

        /* First transmittable parameter will be ctid */
        getTypeOutputInfo(TIDOID, &typefnoid, &isvarlena);
        fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
        fmstate->p_nums++;
    }

    if (operation == CMD_INSERT || operation == CMD_UPDATE) {
        /* Set up for remaining transmittable parameters */
        foreach (lc, fmstate->target_attrs) {
            int attnum = lfirst_int(lc);
            Form_pg_attribute attr = tupdesc->attrs[attnum - 1];

            Assert(!attr->attisdropped);

            getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
            fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
            fmstate->p_nums++;
        }
    }

    Assert(fmstate->p_nums <= n_params);

    return fmstate;
}

/*
 * postgresExecForeignInsert
 * 		Insert one row into a foreign table
 */
static TupleTableSlot *postgresExecForeignInsert(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
    TupleTableSlot *planSlot)
{
    PgFdwModifyState *fmstate = (PgFdwModifyState *)resultRelInfo->ri_FdwState;
    int n_rows;

    if (fmstate == NULL) {
        StringInfoData sql;
        Index resultRelation = resultRelInfo->ri_RangeTableIndex;
        Relation rel = resultRelInfo->ri_RelationDesc;
        TupleDesc tupdesc = RelationGetDescr(rel);
        List *targetAttrs = NIL;
        List *retrieved_attrs = NIL;
        RangeTblEntry *rte = rt_fetch(resultRelation, estate->es_range_table);

        initStringInfo(&sql);
        /* We transmit all columns that are defined in the foreign table. */
        for (int attnum = 1; attnum <= tupdesc->natts; attnum++) {
            Form_pg_attribute attr = tupdesc->attrs[attnum - 1];

            if (!attr->attisdropped) {
                targetAttrs = lappend_int(targetAttrs, attnum);
            }
        }
        deparseInsertSql(&sql, rte, resultRelation, rel, targetAttrs, NULL, &retrieved_attrs);

        fmstate = createForeignModify(estate, rte, resultRelInfo, CMD_INSERT, NULL, sql.data, targetAttrs,
            retrieved_attrs != NIL, retrieved_attrs);
        resultRelInfo->ri_FdwState = fmstate;
    }

    /* Set up the prepared statement on the remote server, if we didn't yet */
    if (!fmstate->p_name) {
        prepare_foreign_modify(fmstate);
    }

    /* Convert parameters needed by prepared statement to text form */
    const char **p_values = convert_prep_stmt_params(fmstate, NULL, slot);

    /*
     * Execute the prepared statement.
     */
    if (!PQsendQueryPrepared(fmstate->conn, fmstate->p_name, fmstate->p_nums, p_values, NULL, NULL, 0)) {
        pgfdw_report_error(ERROR, NULL, fmstate->conn, false, fmstate->query);
    }

    /*
     * Get the result, and check for success.
     *
     * We don't use a PG_TRY block here, so be careful not to throw error
     * without releasing the PGresult.
     */
    PGresult* res = pgfdw_get_result(fmstate->conn, fmstate->query);
    if (PQresultStatus(res) != (fmstate->has_returning ? PGRES_TUPLES_OK : PGRES_COMMAND_OK)) {
        pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);
    }

    /* Check number of rows affected, and fetch RETURNING tuple if any */
    if (fmstate->has_returning) {
        n_rows = PQntuples(res);
        if (n_rows > 0) {
            store_returning_result(fmstate, slot, res);
        }
    } else {
        n_rows = atoi(PQcmdTuples(res));
    }

    /* And clean up */
    PQclear(res);

    MemoryContextReset(fmstate->temp_cxt);

    /* Return NULL if nothing was inserted on the remote end */
    return (n_rows > 0) ? slot : NULL;
}

/*
 * postgresExecForeignUpdate
 * 		Update one row in a foreign table
 */
static TupleTableSlot *postgresExecForeignUpdate(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
    TupleTableSlot *planSlot)
{
    PgFdwModifyState *fmstate = (PgFdwModifyState *)resultRelInfo->ri_FdwState;
    Datum datum;
    bool isNull = false;
    int n_rows;

    /* Set up the prepared statement on the remote server, if we didn't yet */
    if (!fmstate->p_name) {
        prepare_foreign_modify(fmstate);
    }

    /* Get the ctid that was passed up as a resjunk column */
    datum = ExecGetJunkAttribute(planSlot, fmstate->ctidAttno, &isNull);
    /* shouldn't ever get a null result... */
    if (isNull) {
        elog(ERROR, "ctid is NULL");
    }

    /* Convert parameters needed by prepared statement to text form */
    const char **p_values = convert_prep_stmt_params(fmstate, (ItemPointer)DatumGetPointer(datum), slot);

    /*
     * Execute the prepared statement.
     */
    if (!PQsendQueryPrepared(fmstate->conn, fmstate->p_name, fmstate->p_nums, p_values, NULL, NULL, 0)) {
        pgfdw_report_error(ERROR, NULL, fmstate->conn, false, fmstate->query);
    }

    /*
     * Get the result, and check for success.
     *
     * We don't use a PG_TRY block here, so be careful not to throw error
     * without releasing the PGresult.
     */
    PGresult* res = pgfdw_get_result(fmstate->conn, fmstate->query);
    if (PQresultStatus(res) != (fmstate->has_returning ? PGRES_TUPLES_OK : PGRES_COMMAND_OK)) {
        pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);
    }

    /* Check number of rows affected, and fetch RETURNING tuple if any */
    if (fmstate->has_returning) {
        n_rows = PQntuples(res);
        if (n_rows > 0) {
            store_returning_result(fmstate, slot, res);
        }
    } else {
        n_rows = atoi(PQcmdTuples(res));
    }

    /* And clean up */
    PQclear(res);

    MemoryContextReset(fmstate->temp_cxt);

    /* Return NULL if nothing was updated on the remote end */
    return (n_rows > 0) ? slot : NULL;
}

/*
 * postgresExecForeignDelete
 * 		Delete one row from a foreign table
 */
static TupleTableSlot *postgresExecForeignDelete(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
    TupleTableSlot *planSlot)
{
    PgFdwModifyState *fmstate = (PgFdwModifyState *)resultRelInfo->ri_FdwState;
    Datum datum;
    bool isNull = false;
    int n_rows;

    /* Set up the prepared statement on the remote server, if we didn't yet */
    if (!fmstate->p_name) {
        prepare_foreign_modify(fmstate);
    }

    /* Get the ctid that was passed up as a resjunk column */
    datum = ExecGetJunkAttribute(planSlot, fmstate->ctidAttno, &isNull);
    /* shouldn't ever get a null result... */
    if (isNull) {
        elog(ERROR, "ctid is NULL");
    }

    /* Convert parameters needed by prepared statement to text form */
    const char **p_values = convert_prep_stmt_params(fmstate, (ItemPointer)DatumGetPointer(datum), NULL);

    /*
     * Execute the prepared statement.
     */
    if (!PQsendQueryPrepared(fmstate->conn, fmstate->p_name, fmstate->p_nums, p_values, NULL, NULL, 0)) {
        pgfdw_report_error(ERROR, NULL, fmstate->conn, false, fmstate->query);
    }

    /*
     * Get the result, and check for success.
     *
     * We don't use a PG_TRY block here, so be careful not to throw error
     * without releasing the PGresult.
     */
    PGresult *res = pgfdw_get_result(fmstate->conn, fmstate->query);
    if (PQresultStatus(res) != (fmstate->has_returning ? PGRES_TUPLES_OK : PGRES_COMMAND_OK)) {
        pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);
    }

    /* Check number of rows affected, and fetch RETURNING tuple if any */
    if (fmstate->has_returning) {
        n_rows = PQntuples(res);
        if (n_rows > 0) {
            store_returning_result(fmstate, slot, res);
        }
    } else {
        n_rows = atoi(PQcmdTuples(res));
    }

    /* And clean up */
    PQclear(res);

    MemoryContextReset(fmstate->temp_cxt);

    /* Return NULL if nothing was deleted on the remote end */
    return (n_rows > 0) ? slot : NULL;
}

/*
 * postgresEndForeignModify
 * 		Finish an insert/update/delete operation on a foreign table
 */
static void postgresEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo)
{
    PgFdwModifyState *fmstate = (PgFdwModifyState *)resultRelInfo->ri_FdwState;

    /* If fmstate is NULL, we are in EXPLAIN; nothing to do */
    if (fmstate == NULL) {
        return;
    }

    /* If we created a prepared statement, destroy it */
    if (fmstate->p_name) {
        char sql[64];

        int rc = snprintf_s(sql, sizeof(sql), sizeof(sql) - 1, "DEALLOCATE %s", fmstate->p_name);
        securec_check_ss(rc, "", "");

        /*
         * We don't use a PG_TRY block here, so be careful not to throw error
         * without releasing the PGresult.
         */
        PGresult* res = pgfdw_exec_query(fmstate->conn, sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            pgfdw_report_error(ERROR, res, fmstate->conn, true, sql);
        }
        PQclear(res);
        fmstate->p_name = NULL;
    }

    /* Release remote connection */
    ReleaseConnection(fmstate->conn);
    fmstate->conn = NULL;
}

/*
 * postgresIsForeignRelUpdatable
 * 		Determine whether a foreign table supports INSERT, UPDATE and/or
 * 		DELETE.
 */
static int postgresIsForeignRelUpdatable(Relation rel)
{
    ListCell *lc = NULL;

    /*
     * By default, all postgres_fdw foreign tables are assumed updatable. This
     * can be overridden by a per-server setting, which in turn can be
     * overridden by a per-table setting.
     */
    bool updatable = true;

    ForeignTable* table = GetForeignTable(RelationGetRelid(rel));
    ForeignServer* server = GetForeignServer(table->serverid);

    foreach (lc, server->options) {
        DefElem *def = (DefElem *)lfirst(lc);

        if (strcmp(def->defname, "updatable") == 0) {
            updatable = defGetBoolean(def);
        }
    }
    foreach (lc, table->options) {
        DefElem *def = (DefElem *)lfirst(lc);

        if (strcmp(def->defname, "updatable") == 0) {
            updatable = defGetBoolean(def);
        }
    }

    /*
     * Currently "updatable" means support for INSERT, UPDATE and DELETE.
     */
    return updatable ? (1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE) : 0;
}

/*
 * postgresExplainForeignScan
 * 		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 */
static void postgresExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
    if (es->verbose) {
        List* fdw_private = ((ForeignScan *)node->ss.ps.plan)->fdw_private;
        char* sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
        ExplainPropertyText("Remote SQL", sql, es);
    }
}

/*
 * postgresExplainForeignModify
 * 		Produce extra output for EXPLAIN of a ModifyTable on a foreign table
 */
static void postgresExplainForeignModify(ModifyTableState *mtstate, ResultRelInfo *rinfo, List *fdw_private,
    int subplan_index, ExplainState *es)
{
    if (es->verbose) {
        char *sql = strVal(list_nth(fdw_private, FdwModifyPrivateUpdateSql));

        ExplainPropertyText("Remote SQL", sql, es);
    }
}


/*
 * estimate_path_cost_size
 * 		Get cost and size estimates for a foreign scan
 *
 * We assume that all the baserestrictinfo clauses will be applied, plus
 * any join clauses listed in join_conds.
 */
static void estimate_path_cost_size(PlannerInfo *root, RelOptInfo *baserel, List *join_conds, double *p_rows,
    int *p_width, Cost *p_startup_cost, Cost *p_total_cost)
{
    PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *)baserel->fdw_private;
    double rows;
    double retrieved_rows;
    int width = 0;
    Cost startup_cost;
    Cost total_cost;
    Cost run_cost;
    Cost cpu_per_tuple;

    /*
     * If the table or the server is configured to use remote estimates,
     * connect to the foreign server and execute EXPLAIN to estimate the
     * number of rows selected by the restriction+join clauses.  Otherwise,
     * estimate rows using whatever statistics we have locally, in a way
     * similar to ordinary tables.
     */
    if (fpinfo->use_remote_estimate) {
        List *remote_join_conds = NIL;
        List *local_join_conds = NIL;
        StringInfoData sql;
        List *retrieved_attrs = NIL;
        Selectivity local_sel;
        QualCost local_cost;

        /*
         * join_conds might contain both clauses that are safe to send across,
         * and clauses that aren't.
         */
        classifyConditions(root, baserel, join_conds, &remote_join_conds, &local_join_conds);

        /*
         * Construct EXPLAIN query including the desired SELECT, FROM, and
         * WHERE clauses.  Params and other-relation Vars are replaced by
         * dummy values.
         */
        initStringInfo(&sql);
        appendStringInfoString(&sql, "EXPLAIN ");
        deparseSelectSql(&sql, root, baserel, fpinfo->attrs_used, &retrieved_attrs);
        if (fpinfo->remote_conds) {
            appendWhereClause(&sql, root, baserel, fpinfo->remote_conds, true, NULL);
        }
        if (remote_join_conds) {
            appendWhereClause(&sql, root, baserel, remote_join_conds, (fpinfo->remote_conds == NIL), NULL);
        }

        /* Get the remote estimate */
        PGconn* conn = GetConnection(fpinfo->server, fpinfo->user, false);
        get_remote_estimate(sql.data, conn, &rows, &width, &startup_cost, &total_cost);
        ReleaseConnection(conn);

        retrieved_rows = rows;

        /* Factor in the selectivity of the locally-checked quals */
        local_sel = clauselist_selectivity(root, local_join_conds, baserel->relid, JOIN_INNER, NULL);
        local_sel *= fpinfo->local_conds_sel;

        rows = clamp_row_est(rows * local_sel);

        /* Add in the eval cost of the locally-checked quals */
        startup_cost += fpinfo->local_conds_cost.startup;
        total_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;
        cost_qual_eval(&local_cost, local_join_conds, root);
        startup_cost += local_cost.startup;
        total_cost += local_cost.per_tuple * retrieved_rows;
    } else {
        /*
         * We don't support join conditions in this mode (hence, no
         * parameterized paths can be made).
         */
        Assert(join_conds == NIL);

        /* Use rows/width estimates made by set_baserel_size_estimates. */
        rows = baserel->rows;
        width = baserel->width;

        /*
         * Back into an estimate of the number of retrieved rows.  Just in
         * case this is nuts, clamp to at most baserel->tuples.
         */
        retrieved_rows = clamp_row_est(rows / fpinfo->local_conds_sel);
        retrieved_rows = Min(retrieved_rows, baserel->tuples);

        /*
         * Cost as though this were a seqscan, which is pessimistic.  We
         * effectively imagine the local_conds are being evaluated remotely,
         * too.
         */
        startup_cost = 0;
        run_cost = 0;
        run_cost += u_sess->attr.attr_sql.seq_page_cost * baserel->pages;

        startup_cost += baserel->baserestrictcost.startup;
        cpu_per_tuple = u_sess->attr.attr_sql.cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
        run_cost += cpu_per_tuple * baserel->tuples;

        total_cost = startup_cost + run_cost;
    }

    /*
     * Add some additional cost factors to account for connection overhead
     * (fdw_startup_cost), transferring data across the network
     * (fdw_tuple_cost per retrieved row), and local manipulation of the data
     * (cpu_tuple_cost per retrieved row).
     */
    startup_cost += fpinfo->fdw_startup_cost;
    total_cost += fpinfo->fdw_startup_cost;
    total_cost += fpinfo->fdw_tuple_cost * retrieved_rows;
    total_cost += u_sess->attr.attr_sql.cpu_tuple_cost * retrieved_rows;

    /* Return results. */
    *p_rows = rows;
    *p_width = width;
    *p_startup_cost = startup_cost;
    *p_total_cost = total_cost;
}

/*
 * Estimate costs of executing a SQL statement remotely.
 * The given "sql" must be an EXPLAIN command.
 */
static void get_remote_estimate(const char *sql, PGconn *conn, double *rows, int *width, Cost *startup_cost,
    Cost *total_cost)
{
    PGresult *volatile res = NULL;

    /* PGresult must be released before leaving this function. */
    PG_TRY();
    {
        int n;

        /*
         * Execute EXPLAIN remotely.
         */
        res = pgfdw_exec_query(conn, sql);
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            pgfdw_report_error(ERROR, res, conn, false, sql);
        }

        /*
         * Extract cost numbers for topmost plan node.  Note we search for a
         * left paren from the end of the line to avoid being confused by
         * other uses of parentheses.
         */
        char *line = PQgetvalue(res, 0, 0);
        if (strstr(line, "Bypass") != NULL) {
            line = PQgetvalue(res, 1, 0);
        }
        char *p = strrchr(line, '(');
        if (p == NULL) {
            elog(ERROR, "could not interpret EXPLAIN output: \"%s\"", line);
        }
        n = sscanf_s(p, "(cost=%lf..%lf rows=%lf width=%d)", startup_cost, total_cost, rows, width);
        if (n != 4) {
            elog(ERROR, "could not interpret EXPLAIN output: \"%s\"", line);
        }

        PQclear(res);
        res = NULL;
    }
    PG_CATCH();
    {
        if (res) {
            PQclear(res);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
}


/*
 * Create cursor for node's query with current parameter values.
 */
static void create_cursor(ForeignScanState *node)
{
    PgFdwScanState *fsstate = (PgFdwScanState *)node->fdw_state;
    ExprContext *econtext = node->ss.ps.ps_ExprContext;
    int numParams = fsstate->numParams;
    const char **values = fsstate->param_values;
    PGconn *conn = fsstate->conn;
    StringInfoData buf;

    /*
     * Construct array of query parameter values in text format.  We do the
     * conversions in the short-lived per-tuple context, so as not to cause a
     * memory leak over repeated scans.
     */
    if (numParams > 0) {
        int nestlevel;
        int i;
        ListCell *lc = NULL;

        MemoryContext oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

        nestlevel = set_transmission_modes();

        i = 0;
        foreach (lc, fsstate->param_exprs) {
            ExprState *expr_state = (ExprState *)lfirst(lc);
            Datum expr_value;
            bool isNull = false;

            /* Evaluate the parameter expression */
            expr_value = ExecEvalExpr(expr_state, econtext, &isNull, NULL);

            /*
             * Get string representation of each parameter value by invoking
             * type-specific output function, unless the value is null.
             */
            if (isNull) {
                values[i] = NULL;
            } else {
                values[i] = OutputFunctionCall(&fsstate->param_flinfo[i], expr_value);
            }
            i++;
        }

        reset_transmission_modes(nestlevel);

        (void)MemoryContextSwitchTo(oldcontext);
    }

    /* Construct the DECLARE CURSOR command */
    initStringInfo(&buf);
    appendStringInfo(&buf, "DECLARE c%u CURSOR FOR\n%s", fsstate->cursor_number, fsstate->query);

    /*
     * Notice that we pass NULL for paramTypes, thus forcing the remote server
     * to infer types for all parameters.  Since we explicitly cast every
     * parameter (see deparse.c), the "inference" is trivial and will produce
     * the desired result.  This allows us to avoid assuming that the remote
     * server has the same OIDs we do for the parameters' types.
     */
    if (!PQsendQueryParams(conn, buf.data, numParams, NULL, values, NULL, NULL, 0)) {
        pgfdw_report_error(ERROR, NULL, conn, false, buf.data);
    }

    /*
     * Get the result, and check for success.
     *
     * We don't use a PG_TRY block here, so be careful not to throw error
     * without releasing the PGresult.
     */
    PGresult* res = pgfdw_get_result(conn, buf.data);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        pgfdw_report_error(ERROR, res, conn, true, fsstate->query);
    }
    PQclear(res);

    /* Mark the cursor as created, and show no tuples have been retrieved */
    fsstate->cursor_exists = true;
    fsstate->tuples = NULL;
    fsstate->num_tuples = 0;
    fsstate->next_tuple = 0;
    fsstate->fetch_ct_2 = 0;
    fsstate->eof_reached = false;

    /* Clean up */
    pfree(buf.data);
}

/*
 * Fetch some more rows from the node's cursor.
 */
static void fetch_more_data(ForeignScanState *node)
{
    PgFdwScanState *fsstate = (PgFdwScanState *)node->fdw_state;
    PGresult *volatile res = NULL;

    /*
     * We'll store the tuples in the batch_cxt.  First, flush the previous
     * batch.
     */
    fsstate->tuples = NULL;
    MemoryContextReset(fsstate->batch_cxt);
    MemoryContext oldcontext = MemoryContextSwitchTo(fsstate->batch_cxt);

    /* PGresult must be released before leaving this function. */
    PG_TRY();
    {
        PGconn *conn = fsstate->conn;
        char sql[64];
        int fetch_size;
        int numrows;
        int i;

        /* The fetch size is arbitrary, but shouldn't be enormous. */
        fetch_size = 100;

        int rc = snprintf_s(sql, sizeof(sql), sizeof(sql) - 1, "FETCH %d FROM c%u",
            fetch_size, fsstate->cursor_number);
        securec_check_ss(rc, "", "");

        res = pgfdw_exec_query(conn, sql);
        /* On error, report the original query, not the FETCH. */
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            pgfdw_report_error(ERROR, res, conn, false, fsstate->query);
        }

        /* Convert the data into HeapTuples */
        numrows = PQntuples(res);
        fsstate->tuples = (HeapTuple *)palloc0(numrows * sizeof(HeapTuple));
        fsstate->num_tuples = numrows;
        fsstate->next_tuple = 0;

        for (i = 0; i < numrows; i++) {
            fsstate->tuples[i] = make_tuple_from_result_row(res, i, fsstate->rel, fsstate->attinmeta,
                fsstate->retrieved_attrs, fsstate->temp_cxt);
        }

        /* Update fetch_ct_2 */
        if (fsstate->fetch_ct_2 < 2) {
            fsstate->fetch_ct_2++;
        }

        /* Must be EOF if we didn't get as many tuples as we asked for. */
        fsstate->eof_reached = (numrows < fetch_size);

        PQclear(res);
        res = NULL;
    }
    PG_CATCH();
    {
        if (res) {
            PQclear(res);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();

    (void)MemoryContextSwitchTo(oldcontext);
}

/*
 * Force assorted GUC parameters to settings that ensure that we'll output
 * data values in a form that is unambiguous to the remote server.
 *
 * This is rather expensive and annoying to do once per row, but there's
 * little choice if we want to be sure values are transmitted accurately;
 * we can't leave the settings in place between rows for fear of affecting
 * user-visible computations.
 *
 * We use the equivalent of a function SET option to allow the settings to
 * persist only until the caller calls reset_transmission_modes().  If an
 * error is thrown in between, guc.c will take care of undoing the settings.
 *
 * The return value is the nestlevel that must be passed to
 * reset_transmission_modes() to undo things.
 */
int set_transmission_modes(void)
{
    int nestlevel = NewGUCNestLevel();

    /*
     * The values set here should match what pg_dump does.  See also
     * configure_remote_session in connection.c.
     */
    if (u_sess->time_cxt.DateStyle != USE_ISO_DATES) {
        (void)set_config_option("datestyle", "ISO", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0);
    }
    if (u_sess->attr.attr_common.IntervalStyle != INTSTYLE_POSTGRES) {
        (void)set_config_option("intervalstyle", "postgres", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0);
    }
    if (u_sess->attr.attr_common.extra_float_digits < 3) {
        (void)set_config_option("extra_float_digits", "3", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0);
    }
    return nestlevel;
}

/*
 * Undo the effects of set_transmission_modes().
 */
void reset_transmission_modes(int nestlevel)
{
    AtEOXact_GUC(true, nestlevel);
}

/*
 * Utility routine to close a cursor.
 */
static void close_cursor(PGconn *conn, unsigned int cursor_number)
{
    char sql[64];

    int rc = snprintf_s(sql, sizeof(sql), sizeof(sql) - 1, "CLOSE c%u", cursor_number);
    securec_check_ss(rc, "", "");

    /*
     * We don't use a PG_TRY block here, so be careful not to throw error
     * without releasing the PGresult.
     */
    PGresult* res = pgfdw_exec_query(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        pgfdw_report_error(ERROR, res, conn, true, sql);
    }
    PQclear(res);
}

/*
 * prepare_foreign_modify
 * 		Establish a prepared statement for execution of INSERT/UPDATE/DELETE
 */
static void prepare_foreign_modify(PgFdwModifyState *fmstate)
{
    char prep_name[NAMEDATALEN];

    /* Construct name we'll use for the prepared statement. */
    int rc = snprintf_s(prep_name, sizeof(prep_name), sizeof(prep_name) - 1,
                        "pgsql_fdw_prep_%u", GetPrepStmtNumber(fmstate->conn));
    securec_check_ss(rc, "", "");
    char* p_name = pstrdup(prep_name);

    /*
     * We intentionally do not specify parameter types here, but leave the
     * remote server to derive them by default.  This avoids possible problems
     * with the remote server using different type OIDs than we do.  All of
     * the prepared statements we use in this module are simple enough that
     * the remote server will make the right choices.
     */
    if (!PQsendPrepare(fmstate->conn, p_name, fmstate->query, 0, NULL)) {
        pgfdw_report_error(ERROR, NULL, fmstate->conn, false, fmstate->query);
    }

    /*
     * Get the result, and check for success.
     *
     * We don't use a PG_TRY block here, so be careful not to throw error
     * without releasing the PGresult.
     */
    PGresult* res = pgfdw_get_result(fmstate->conn, fmstate->query);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);
    }
    PQclear(res);

    /* This action shows that the prepare has been done. */
    fmstate->p_name = p_name;
}

/*
 * convert_prep_stmt_params
 * 		Create array of text strings representing parameter values
 *
 * tupleid is ctid to send, or NULL if none
 * slot is slot to get remaining parameters from, or NULL if none
 *
 * Data is constructed in temp_cxt; caller should reset that after use.
 */
static const char **convert_prep_stmt_params(PgFdwModifyState *fmstate, ItemPointer tupleid, TupleTableSlot *slot)
{
    int pindex = 0;

    MemoryContext oldcontext = MemoryContextSwitchTo(fmstate->temp_cxt);

    const char **p_values = (const char **)palloc(sizeof(char *) * fmstate->p_nums);

    /* 1st parameter should be ctid, if it's in use */
    if (tupleid != NULL) {
        /* don't need set_transmission_modes for TID output */
        p_values[pindex] = OutputFunctionCall(&fmstate->p_flinfo[pindex], PointerGetDatum(tupleid));
        pindex++;
    }

    /* get following parameters from slot */
    if (slot != NULL && fmstate->target_attrs != NIL) {
        int nestlevel;
        ListCell *lc = NULL;

        nestlevel = set_transmission_modes();

        foreach (lc, fmstate->target_attrs) {
            int attnum = lfirst_int(lc);
            Datum value;
            bool isnull = false;

            value = tableam_tslot_getattr(slot, attnum, &isnull);
            if (isnull) {
                p_values[pindex] = NULL;
            } else {
                p_values[pindex] = OutputFunctionCall(&fmstate->p_flinfo[pindex], value);
            }
            pindex++;
        }

        reset_transmission_modes(nestlevel);
    }

    Assert(pindex == fmstate->p_nums);

    (void)MemoryContextSwitchTo(oldcontext);

    return p_values;
}

/*
 * store_returning_result
 * 		Store the result of a RETURNING clause
 *
 * On error, be sure to release the PGresult on the way out.  Callers do not
 * have PG_TRY blocks to ensure this happens.
 */
static void store_returning_result(PgFdwModifyState *fmstate, TupleTableSlot *slot, PGresult *res)
{
    /* PGresult must be released before leaving this function. */
    PG_TRY();
    {
        HeapTuple newtup;

        newtup = make_tuple_from_result_row(res, 0, fmstate->rel, fmstate->attinmeta, fmstate->retrieved_attrs,
            fmstate->temp_cxt);
        /* tuple will be deleted when it is cleared from the slot */
        (void)ExecStoreTuple(newtup, slot, InvalidBuffer, true);
    }
    PG_CATCH();
    {
        if (res) {
            PQclear(res);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/*
 * postgresAnalyzeForeignTable
 * 		Test whether analyzing this foreign table is supported
 */
static bool postgresAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func, BlockNumber *totalpages,
    void *additionalData, bool estimate_table_rownum)
{
    StringInfoData sql;
    PGresult *volatile res = NULL;

    /* Return the row-analysis function pointer */
    *func = postgresAcquireSampleRowsFunc;

    /*
     * Now we have to get the number of pages.  It's annoying that the ANALYZE
     * API requires us to return that now, because it forces some duplication
     * of effort between this routine and postgresAcquireSampleRowsFunc.  But
     * it's probably not worth redefining that API at this point.
     * Get the connection to use.  We do the remote access as the table's
     * owner, even if the ANALYZE was started by some other user.
     */
    ForeignTable* table = GetForeignTable(RelationGetRelid(relation));
    ForeignServer* server = GetForeignServer(table->serverid);
    UserMapping* user = GetUserMapping(relation->rd_rel->relowner, server->serverid);
    PGconn* conn = GetConnection(server, user, false);

    /*
     * Construct command to get page count for relation.
     */
    initStringInfo(&sql);
    deparseAnalyzeSizeSql(&sql, relation);

    /* In what follows, do not risk leaking any PGresults. */
    PG_TRY();
    {
        res = pgfdw_exec_query(conn, sql.data);
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            pgfdw_report_error(ERROR, res, conn, false, sql.data);
        }

        if (PQntuples(res) != 1 || PQnfields(res) != 1) {
            elog(ERROR, "unexpected result from deparseAnalyzeSizeSql query");
        }
        *totalpages = strtoul(PQgetvalue(res, 0, 0), NULL, 10);

        PQclear(res);
        res = NULL;
    }
    PG_CATCH();
    {
        if (res) {
            PQclear(res);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();

    ReleaseConnection(conn);

    return true;
}

/*
 * Acquire a random sample of rows from foreign table managed by postgres_fdw.
 *
 * We fetch the whole table from the remote side and pick out some sample rows.
 *
 * Selected rows are returned in the caller-allocated array rows[],
 * which must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also count the total number of rows in the table and return it into
 * *totalrows.  Note that *totaldeadrows is always set to 0.
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the table.  Therefore, correlation estimates derived later
 * may be meaningless, but it's OK because we don't use the estimates
 * currently (the planner only pays attention to correlation for indexscans).
 */
static int postgresAcquireSampleRowsFunc(Relation relation, int elevel, HeapTuple *rows, int targrows,
    double *totalrows, double *totaldeadrows, void *additionalData, bool estimate_table_rownum)
{
    PgFdwAnalyzeState astate;
    unsigned int cursor_number;
    StringInfoData sql;
    PGresult *volatile res = NULL;

    /* Initialize workspace state */
    astate.rel = relation;
    astate.attinmeta = TupleDescGetAttInMetadata(RelationGetDescr(relation));

    astate.rows = rows;
    astate.targrows = targrows;
    astate.numrows = 0;
    astate.samplerows = 0;
    astate.rowstoskip = -1; /* -1 means not set yet */
    astate.rstate = anl_init_selection_state(targrows);

    /* Remember ANALYZE context, and create a per-tuple temp context */
    astate.anl_cxt = CurrentMemoryContext;
    astate.temp_cxt = AllocSetContextCreate(CurrentMemoryContext, "postgres_fdw temporary data", ALLOCSET_SMALL_MINSIZE,
        ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE);

    /*
     * Get the connection to use.  We do the remote access as the table's
     * owner, even if the ANALYZE was started by some other user.
     */
    ForeignTable* table = GetForeignTable(RelationGetRelid(relation));
    ForeignServer* server = GetForeignServer(table->serverid);
    UserMapping* user = GetUserMapping(relation->rd_rel->relowner, server->serverid);
    PGconn* conn = GetConnection(server, user, false);

    /*
     * Construct cursor that retrieves whole rows from remote.
     */
    cursor_number = GetCursorNumber(conn);
    initStringInfo(&sql);
    appendStringInfo(&sql, "DECLARE c%u CURSOR FOR ", cursor_number);
    deparseAnalyzeSql(&sql, relation, &astate.retrieved_attrs);

    /* In what follows, do not risk leaking any PGresults. */
    PG_TRY();
    {
        res = pgfdw_exec_query(conn, sql.data);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            pgfdw_report_error(ERROR, res, conn, false, sql.data);
        }
        PQclear(res);
        res = NULL;

        /* Retrieve and process rows a batch at a time. */
        for (;;) {
            char fetch_sql[64];
            int fetch_size;
            int numrows;
            int i;

            /* Allow users to cancel long query */
            CHECK_FOR_INTERRUPTS();

            /*
             * XXX possible future improvement: if rowstoskip is large, we
             * could issue a MOVE rather than physically fetching the rows,
             * then just adjust rowstoskip and samplerows appropriately.
             * The fetch size is arbitrary, but shouldn't be enormous.
             */
            fetch_size = 100;

            /* Fetch some rows */
            int rc = snprintf_s(fetch_sql, sizeof(fetch_sql), sizeof(fetch_sql) - 1,
                                "FETCH %d FROM c%u", fetch_size, cursor_number);
            securec_check_ss(rc, "", "");

            res = pgfdw_exec_query(conn, fetch_sql);
            /* On error, report the original query, not the FETCH. */
            if (PQresultStatus(res) != PGRES_TUPLES_OK) {
                pgfdw_report_error(ERROR, res, conn, false, sql.data);
            }

            /* Process whatever we got. */
            numrows = PQntuples(res);
            for (i = 0; i < numrows; i++) {
                analyze_row_processor(res, i, &astate);
            }

            PQclear(res);
            res = NULL;

            /* Must be EOF if we didn't get all the rows requested. */
            if (numrows < fetch_size) {
                break;
            }
        }

        /* Close the cursor, just to be tidy. */
        close_cursor(conn, cursor_number);
    }
    PG_CATCH();
    {
        if (res) {
            PQclear(res);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();

    ReleaseConnection(conn);

    /* We assume that we have no dead tuple. */
    *totaldeadrows = 0.0;

    /* We've retrieved all living tuples from foreign server. */
    *totalrows = astate.samplerows;

    /*
     * Emit some interesting relation info
     */
    ereport(elevel, (errmsg("\"%s\": table contains %.0f rows, %d rows in sample", RelationGetRelationName(relation),
        astate.samplerows, astate.numrows)));

    return astate.numrows;
}

/*
 * Collect sample rows from the result of query.
 * 	 - Use all tuples in sample until target # of samples are collected.
 * 	 - Subsequently, replace already-sampled tuples randomly.
 */
static void analyze_row_processor(PGresult *res, int row, PgFdwAnalyzeState *astate)
{
    int targrows = astate->targrows;
    int pos; /* array index to store tuple in */

    /* Always increment sample row counter. */
    astate->samplerows += 1;

    /*
     * Determine the slot where this sample row should be stored.  Set pos to
     * negative value to indicate the row should be skipped.
     */
    if (astate->numrows < targrows) {
        /* First targrows rows are always included into the sample */
        pos = astate->numrows++;
    } else {
        /*
         * Now we start replacing tuples in the sample until we reach the end
         * of the relation.  Same algorithm as in acquire_sample_rows in
         * analyze.c; see Jeff Vitter's paper.
         */
        if (astate->rowstoskip < 0) {
            astate->rowstoskip = anl_get_next_S(astate->samplerows, targrows, &astate->rstate);
        }

        if (astate->rowstoskip <= 0) {
            /* Choose a random reservoir element to replace. */
            pos = (int)(targrows * anl_random_fract());
            Assert(pos >= 0 && pos < targrows);
            heap_freetuple(astate->rows[pos]);
        } else {
            /* Skip this tuple. */
            pos = -1;
        }

        astate->rowstoskip -= 1;
    }

    if (pos >= 0) {
        /*
         * Create sample tuple from current result row, and store it in the
         * position determined above.  The tuple has to be created in anl_cxt.
         */
        MemoryContext oldcontext = MemoryContextSwitchTo(astate->anl_cxt);

        astate->rows[pos] = make_tuple_from_result_row(res, row, astate->rel, astate->attinmeta,
            astate->retrieved_attrs, astate->temp_cxt);

        (void)MemoryContextSwitchTo(oldcontext);
    }
}

/*
 * Create a tuple from the specified row of the PGresult.
 *
 * rel is the local representation of the foreign table, attinmeta is
 * conversion data for the rel's tupdesc, and retrieved_attrs is an
 * integer list of the table column numbers present in the PGresult.
 * temp_context is a working context that can be reset after each tuple.
 */
static HeapTuple make_tuple_from_result_row(PGresult *res, int row, Relation rel, AttInMetadata *attinmeta,
    List *retrieved_attrs, MemoryContext temp_context)
{
    TupleDesc tupdesc = RelationGetDescr(rel);
    ItemPointer ctid = NULL;
    ConversionLocation errpos;
    ErrorContextCallback errcallback;
    ListCell *lc = NULL;
    int j;

    Assert(row < PQntuples(res));

    /*
     * Do the following work in a temp context that we reset after each tuple.
     * This cleans up not only the data we have direct access to, but any
     * cruft the I/O functions might leak.
     */
    MemoryContext oldcontext = MemoryContextSwitchTo(temp_context);

    Datum* values = (Datum *)palloc0(tupdesc->natts * sizeof(Datum));
    bool* nulls = (bool *)palloc(tupdesc->natts * sizeof(bool));
    /* Initialize to nulls for any columns not present in result */
    int rc = memset_s(nulls, tupdesc->natts * sizeof(bool), true, tupdesc->natts * sizeof(bool));
    securec_check(rc, "", "");

    /*
     * Set up and install callback to report where conversion error occurs.
     */
    errpos.rel = rel;
    errpos.cur_attno = 0;
    errcallback.callback = conversion_error_callback;
    errcallback.arg = (void *)&errpos;
    errcallback.previous = t_thrd.log_cxt.error_context_stack;
    t_thrd.log_cxt.error_context_stack = &errcallback;

    /*
     * i indexes columns in the relation, j indexes columns in the PGresult.
     */
    j = 0;
    foreach (lc, retrieved_attrs) {
        int i = lfirst_int(lc);
        char *valstr = NULL;

        /* fetch next column's textual value */
        if (PQgetisnull(res, row, j)) {
            valstr = NULL;
        } else {
            valstr = PQgetvalue(res, row, j);
        }

        /* convert value to internal representation */
        if (i > 0) {
            /* ordinary column */
            Assert(i <= tupdesc->natts);
            nulls[i - 1] = (valstr == NULL);
            /* Apply the input function even to nulls, to support domains */
            errpos.cur_attno = i;
            values[i - 1] = InputFunctionCall(&attinmeta->attinfuncs[i - 1], valstr, attinmeta->attioparams[i - 1],
                attinmeta->atttypmods[i - 1]);
            errpos.cur_attno = 0;
        } else if (i == SelfItemPointerAttributeNumber) {
            /* ctid --- note we ignore any other system column in result */
            if (valstr != NULL) {
                Datum datum = DirectFunctionCall1(tidin, CStringGetDatum(valstr));
                ctid = (ItemPointer)DatumGetPointer(datum);
            }
        }

        j++;
    }

    /* Uninstall error context callback. */
    t_thrd.log_cxt.error_context_stack = errcallback.previous;

    /*
     * Check we got the expected number of columns.  Note: j == 0 and
     * PQnfields == 1 is expected, since deparse emits a NULL if no columns.
     */
    if (j > 0 && j != PQnfields(res)) {
        elog(ERROR, "remote query result does not match the foreign table");
    }

    /*
     * Build the result tuple in caller's memory context.
     */
    (void)MemoryContextSwitchTo(oldcontext);

    HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);

    if (ctid) {
        tuple->t_self = *ctid;
    }

    /* Clean up */
    MemoryContextReset(temp_context);

    return tuple;
}

/*
 * Callback function which is called when error occurs during column value
 * conversion.  Print names of column and relation.
 */
static void conversion_error_callback(void *arg)
{
    ConversionLocation *errpos = (ConversionLocation *)arg;
    TupleDesc tupdesc = RelationGetDescr(errpos->rel);
    if (errpos->cur_attno > 0 && errpos->cur_attno <= tupdesc->natts) {
        errcontext("column \"%s\" of foreign table \"%s\"", NameStr(tupdesc->attrs[errpos->cur_attno - 1]->attname),
            RelationGetRelationName(errpos->rel));
    }
}

