/*
 *  chidb - a didactic relational database management system
 *
 *  SQL -> DBM Code Generator
 *
 */

/*
 *  Copyright (c) 2009-2015, The University of Chicago
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or withsend
 *  modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  - Neither the name of The University of Chicago nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software withsend specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY send OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include <chidb/chidb.h>
#include <chisql/chisql.h>
#include "dbm.h"
#include "util.h"

/* claude: covers assignment_codegen.html steps 1-5 (schema loading is in
 * util.c/api.c) plus all of assignment_opt.html: CREATE INDEX + population
 * and index-seek SELECT ("Supporting Indexes"), and sigma-pushing
 * (optimizer.c) -- which is why WHERE, for both a single-table SELECT and
 * a NATURAL JOIN, is a full conjunction of `column OP literal` comparisons
 * (see the "WHERE clauses" section below), not just one: a pushed
 * NATURAL JOIN query can leave more than one conjunct on either side, or
 * at the top. Two-way NATURAL JOIN is supported (codegen_select_join
 * below), including qualified column names and a NaturalJoin whose sides
 * are pre-wrapped in a Select (the shape sigma-pushing produces).
 * NOT implemented: using an index for either side of a join's scan (so
 * pushing a sigma only ever buys skipping a linear scan's rows early, not
 * an index seek) -- see docs/claude_notes/plan_chidb_implementation.md. */

/* Emits `opcode p1 p2 p3 p4` at *pc, then advances *pc; returns the
 * address the instruction was placed at (handy for later patching a
 * forward jump via stmt->ops[addr].p2 = ...). chidb_stmt_set_op() strdup's
 * p4 itself, so p4 here is always a borrowed pointer. */
static int emit(chidb_stmt *stmt, int *pc, opcode_t opcode, int32_t p1, int32_t p2, int32_t p3, const char *p4)
{
    chidb_dbm_op_t op = { opcode, p1, p2, p3, (char *) p4 };
    chidb_stmt_set_op(stmt, &op, *pc);
    return (*pc)++;
}

static int column_count(Column_t *columns)
{
    int n = 0;
    for (; columns; columns = columns->next)
        n++;
    return n;
}

static int column_index_by_name(Column_t *columns, const char *name, enum data_type *type_out)
{
    int i = 0;
    for (Column_t *c = columns; c; c = c->next, i++)
        if (strcasecmp(c->name, name) == 0)
        {
            if (type_out)
                *type_out = c->type;
            return i;
        }
    return -1;
}

static bool column_is_primary_key(Column_t *columns, int index)
{
    int i = 0;
    for (Column_t *c = columns; c; c = c->next, i++)
        if (i == index)
        {
            for (Constraint_t *k = c->constraints; k; k = k->next)
                if (k->t == CONS_PRIMARY_KEY)
                    return true;
            return false;
        }
    return false;
}

static const char *column_name_at(Column_t *columns, int index)
{
    int i = 0;
    for (Column_t *c = columns; c; c = c->next, i++)
        if (i == index)
            return c->name;
    return NULL;
}

static int primary_key_index(Column_t *columns)
{
    int i = 0;
    for (Column_t *c = columns; c; c = c->next, i++)
        if (column_is_primary_key(columns, i))
            return i;
    return 0; /* assignment guarantee: the first column is always the PK */
}

/* Re-parses a schema item's stored CREATE TABLE statement to recover its
 * column list, per assignment_codegen.html step 1's suggested approach.
 * *parsed_out must be free()d by the caller once the Column_t* is no
 * longer needed (this is the same shallow-free convention already used
 * for one-off reparses elsewhere in this codebase, e.g.
 * chidb_schema_find_index_on() in util.c). */
static Column_t *table_columns(chidb_schema_item_t *tbl, chisql_statement_t **parsed_out)
{
    chisql_parser(tbl->sql, parsed_out);
    return (*parsed_out)->stmt.create->table->columns;
}

/* Turns a single literal into whichever register-loading instruction is
 * appropriate for its type. A one-character STRING_LITERAL parses as a
 * TYPE_CHAR literal (sql.y's literal_value rule) rather than TYPE_TEXT,
 * so it needs turning into a 1-byte nul-terminated string first. */
static int emit_literal(chidb_stmt *stmt, int *pc, Literal_t *lit, int32_t reg)
{
    if (lit->t == TYPE_INT)
        return emit(stmt, pc, Op_Integer, lit->val.ival, reg, 0, NULL);

    char charbuf[2] = { lit->val.cval, 0 };
    const char *s = (lit->t == TYPE_CHAR) ? charbuf : lit->val.strval;
    return emit(stmt, pc, Op_String, (int32_t) strlen(s), reg, 0, s);
}

static bool literal_matches_type(Literal_t *lit, enum data_type coltype)
{
    bool lit_is_int = (lit->t == TYPE_INT);
    bool lit_is_text = (lit->t == TYPE_TEXT || lit->t == TYPE_CHAR);
    return (coltype == TYPE_INT && lit_is_int) || (coltype == TYPE_TEXT && lit_is_text);
}


/* --- WHERE clauses: a list of `column OP literal` conjuncts ------------
 *
 * A plain single-table SELECT and a NATURAL JOIN both end up needing "a
 * list of column-vs-literal comparisons, ANDed together, each resolved
 * against one or more tables" -- the only difference is how a column
 * reference gets resolved to an actual table + index, so that part is a
 * caller-supplied callback (ColResolver) and everything else is shared.
 * This is also what makes the query optimizer's sigma-pushing
 * (optimizer.c) safe to turn on: a NATURAL JOIN with a multi-conjunct
 * WHERE, once pushed, still has an AND-chain left on whichever side (or
 * both, or neither) didn't get everything pushed out of it. */

/* A column reference resolved against one or two tables: single-table
 * SELECT only ever uses side 0; NATURAL JOIN uses side 0 = left table,
 * 1 = right table. */
typedef struct { int side; int idx; } RCol;

/* One resolved `column OP literal` comparison. `op` is already negated
 * and operand-order-adjusted so it can be emitted directly as
 * `emit(op, lit_reg, jump_target, col_reg)` -- jumping exactly when the
 * *original* condition is false. See docs/claude_notes/notes_dbm_spec.txt
 * for why Lt/Le/Gt/Ge need their operands in this particular order. */
typedef struct { RCol col; opcode_t op; Literal_t *lit; } ResolvedCmp;

typedef bool (*ColResolver)(void *ctx, ColumnReference_t *ref, RCol *out, enum data_type *type_out);

static bool resolve_comparison(Condition_t *leaf, ColResolver resolve, void *ctx, ResolvedCmp *out)
{
    if (leaf->t != RA_COND_EQ && leaf->t != RA_COND_LT && leaf->t != RA_COND_GT &&
        leaf->t != RA_COND_LEQ && leaf->t != RA_COND_GEQ)
        return false;

    Expression_t *e1 = leaf->cond.comp.expr1, *e2 = leaf->cond.comp.expr2;
    Expression_t *colExpr, *valExpr;
    bool flipped = false;
    if (e1->t == EXPR_TERM && e1->expr.term.t == TERM_COLREF)
    {
        colExpr = e1;
        valExpr = e2;
    }
    else if (e2->t == EXPR_TERM && e2->expr.term.t == TERM_COLREF)
    {
        colExpr = e2;
        valExpr = e1;
        flipped = true;
    }
    else
        return false;

    if (valExpr->t != EXPR_TERM || valExpr->expr.term.t != TERM_LITERAL)
        return false;

    enum data_type coltype;
    if (!resolve(ctx, colExpr->expr.term.ref, &out->col, &coltype))
        return false;

    out->lit = valExpr->expr.term.val;
    if (!literal_matches_type(out->lit, coltype))
        return false;

    enum CondType ct = leaf->t;
    if (flipped)
        switch (ct)
        {
        case RA_COND_LT: ct = RA_COND_GT; break;
        case RA_COND_GT: ct = RA_COND_LT; break;
        case RA_COND_LEQ: ct = RA_COND_GEQ; break;
        case RA_COND_GEQ: ct = RA_COND_LEQ; break;
        default: break;
        }
    switch (ct)
    {
    case RA_COND_EQ:  out->op = Op_Ne; break;
    case RA_COND_GT:  out->op = Op_Le; break;
    case RA_COND_GEQ: out->op = Op_Lt; break;
    case RA_COND_LT:  out->op = Op_Ge; break;
    case RA_COND_LEQ: out->op = Op_Gt; break;
    default: break;
    }
    return true;
}

/* Flattens the top-level AND-chain of `cond` into leaf Condition_t*'s (a
 * `cond` that isn't itself an AND is a length-1 chain of just itself).
 * OR/NOT/IN anywhere in the chain aren't comparisons and so simply fail
 * to resolve later, in resolve_comparison() -- this function only cares
 * about splitting AND nodes apart. */
static void flatten_conjuncts(Condition_t *cond, Condition_t ***list, int *n, int *cap)
{
    if (cond->t == RA_COND_AND)
    {
        flatten_conjuncts(cond->cond.binary.cond1, list, n, cap);
        flatten_conjuncts(cond->cond.binary.cond2, list, n, cap);
        return;
    }

    if (*n == *cap)
    {
        *cap = *cap ? *cap * 2 : 4;
        *list = realloc(*list, sizeof(Condition_t *) * *cap);
    }
    (*list)[(*n)++] = cond;
}

/* Resolves every conjunct of `cond` into a ResolvedCmp (or does nothing
 * and succeeds trivially if `cond` is NULL, i.e. there's no WHERE at
 * all). On success, *out is a malloc'd array (possibly NULL/empty) the
 * caller must free; on failure (any conjunct isn't a plain, resolvable
 * `column OP literal` comparison), returns false without allocating. */
static bool resolve_conjuncts(Condition_t *cond, ColResolver resolve, void *ctx, ResolvedCmp **out, int *nout)
{
    if (!cond)
    {
        *out = NULL;
        *nout = 0;
        return true;
    }

    Condition_t **list = NULL;
    int n = 0, cap = 0;
    flatten_conjuncts(cond, &list, &n, &cap);

    ResolvedCmp *cmps = malloc(sizeof(ResolvedCmp) * n);
    for (int i = 0; i < n; i++)
        if (!resolve_comparison(list[i], resolve, ctx, &cmps[i]))
        {
            free(list);
            free(cmps);
            return false;
        }
    free(list);
    *out = cmps;
    *nout = n;
    return true;
}

/* ColResolver for a single-table SELECT: `ctx` is that table's Column_t
 * list, and every resolved column is (necessarily) side 0. Doesn't
 * validate a table-name qualifier against the actual table name (neither
 * did the single-condition code this replaced) -- there's only one table
 * it could sensibly refer to anyway. */
static bool resolve_single_column(void *ctx, ColumnReference_t *ref, RCol *out, enum data_type *type_out)
{
    Column_t *columns = (Column_t *) ctx;
    int idx = column_index_by_name(columns, ref->columnName, type_out);
    if (idx < 0)
        return false;
    *out = (RCol){ 0, idx };
    return true;
}

/* Loads every conjunct's literal into its own register, once, starting
 * at r_lit_base -- they're compile-time constants, so there's no reason
 * to reload them on every row the way the column side has to be. */
static void emit_filter_literals(chidb_stmt *stmt, int *pc, ResolvedCmp *cmps, int n, int32_t r_lit_base)
{
    for (int i = 0; i < n; i++)
        emit_literal(stmt, pc, cmps[i].lit, r_lit_base + i);
}

/* Reads an RCol's current value (via cursor 0 for side 0, cursor 1 for
 * side 1 -- a single-table SELECT only ever uses side 0/cursor 0, and
 * cols2 is never dereferenced in that case) into `reg`, as Key or Column
 * depending on whether it's that table's primary key. */
static void emit_rcol(chidb_stmt *stmt, int *pc, RCol rc, Column_t *cols1, Column_t *cols2, int32_t reg)
{
    Column_t *cols = rc.side == 0 ? cols1 : cols2;
    int32_t cursor = rc.side == 0 ? 0 : 1;
    if (column_is_primary_key(cols, rc.idx))
        emit(stmt, pc, Op_Key, cursor, reg, 0, NULL);
    else
        emit(stmt, pc, Op_Column, cursor, rc.idx, reg, NULL);
}

/* Emits the per-row half of a list of filter checks: for each conjunct,
 * read its column's current value and compare it against the
 * already-loaded literal at r_lit_base+i, appending the address of the
 * (negated) jump to `patch` so the caller can point every one of them at
 * wherever "this row doesn't match, skip it" actually means once that's
 * known (the two-table caller needs two different such addresses -- see
 * codegen_select_join). */
static void emit_filter_checks(chidb_stmt *stmt, int *pc, ResolvedCmp *cmps, int n,
                                Column_t *cols1, Column_t *cols2, int32_t r_lit_base, int32_t r_tmp,
                                int *patch, int *npatch)
{
    for (int i = 0; i < n; i++)
    {
        emit_rcol(stmt, pc, cmps[i].col, cols1, cols2, r_tmp);
        patch[(*npatch)++] = emit(stmt, pc, cmps[i].op, r_lit_base + i, -1, r_tmp, NULL);
    }
}


/* --- CREATE TABLE / CREATE INDEX ------------------------------------- */

/* claude: register layout mirrors tests/files/dbm-programs/create/create-table.dbmf
 * exactly: r0=schema root, r1..r5=the 5 schema-table columns (type, name,
 * assoc. table, root page, sql) in that order, with CreateTable/CreateIndex
 * writing its result straight into r4 (the "root page" slot), r6=packed
 * record, r7=this row's key. */
static int codegen_create_table(chidb_stmt *stmt, Table_t *table, const char *sql_text)
{
    chidb *db = stmt->db;
    if (chidb_schema_find_table(db, table->name))
        return CHIDB_EINVALIDSQL;

    int pc = 0;
    emit(stmt, &pc, Op_Integer, 1, 0, 0, NULL);
    emit(stmt, &pc, Op_OpenWrite, 0, 0, 5, NULL);
    emit(stmt, &pc, Op_CreateTable, 4, 0, 0, NULL);
    emit(stmt, &pc, Op_String, (int32_t) strlen("table"), 1, 0, "table");
    emit(stmt, &pc, Op_String, (int32_t) strlen(table->name), 2, 0, table->name);
    emit(stmt, &pc, Op_String, (int32_t) strlen(table->name), 3, 0, table->name);
    emit(stmt, &pc, Op_String, (int32_t) strlen(sql_text), 5, 0, sql_text);
    emit(stmt, &pc, Op_MakeRecord, 1, 5, 6, NULL);
    emit(stmt, &pc, Op_Integer, (int32_t) chidb_schema_next_key(db), 7, 0, NULL);
    emit(stmt, &pc, Op_Insert, 0, 6, 7, NULL);
    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);
    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);

    stmt->schema_change = true;
    return CHIDB_OK;
}

/* claude: after registering the index in the schema table (same shape as
 * codegen_create_table above), scans the indexed table and populates the
 * new index B-Tree with one (IdxKey,PKey) entry per row -- required by
 * assignment_opt.html point 1 ("populate the index with entries relating
 * to the current instance of the indexed table"). Cursor 0 is the schema
 * table (closed once the schema row is inserted), cursor 1 is the
 * indexed table (read), cursor 2 is the new index (write). */
static int codegen_create_index(chidb_stmt *stmt, Index_t *index, const char *sql_text)
{
    chidb *db = stmt->db;
    chidb_schema_item_t *tbl = chidb_schema_find_table(db, index->table_name);
    if (!tbl)
        return CHIDB_EINVALIDSQL;

    chisql_statement_t *parsed;
    Column_t *columns = table_columns(tbl, &parsed);
    int col_idx = column_index_by_name(columns, index->column_name, NULL);
    if (col_idx < 0)
    {
        free(parsed);
        return CHIDB_EINVALIDSQL;
    }
    bool col_is_pk = column_is_primary_key(columns, col_idx);
    int ncols = column_count(columns);
    free(parsed);

    int pc = 0;
    emit(stmt, &pc, Op_Integer, 1, 0, 0, NULL);
    emit(stmt, &pc, Op_OpenWrite, 0, 0, 5, NULL);
    emit(stmt, &pc, Op_CreateIndex, 4, 0, 0, NULL);
    emit(stmt, &pc, Op_String, (int32_t) strlen("index"), 1, 0, "index");
    emit(stmt, &pc, Op_String, (int32_t) strlen(index->name), 2, 0, index->name);
    emit(stmt, &pc, Op_String, (int32_t) strlen(index->table_name), 3, 0, index->table_name);
    emit(stmt, &pc, Op_String, (int32_t) strlen(sql_text), 5, 0, sql_text);
    emit(stmt, &pc, Op_MakeRecord, 1, 5, 6, NULL);
    emit(stmt, &pc, Op_Integer, (int32_t) chidb_schema_next_key(db), 7, 0, NULL);
    emit(stmt, &pc, Op_Insert, 0, 6, 7, NULL);
    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);

    emit(stmt, &pc, Op_OpenWrite, 2, 4, 0, NULL); /* new index, root page still in r4 */
    emit(stmt, &pc, Op_Integer, (int32_t) tbl->root_page, 8, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 1, 8, ncols, NULL);
    int addr_rewind = emit(stmt, &pc, Op_Rewind, 1, -1, 0, NULL);
    int addr_loop = pc;

    if (col_is_pk)
        emit(stmt, &pc, Op_Key, 1, 9, 0, NULL);
    else
        emit(stmt, &pc, Op_Column, 1, col_idx, 9, NULL);
    emit(stmt, &pc, Op_Key, 1, 10, 0, NULL);
    emit(stmt, &pc, Op_IdxInsert, 2, 9, 10, NULL);
    emit(stmt, &pc, Op_Next, 1, addr_loop, 0, NULL);

    int addr_after = pc;
    stmt->ops[addr_rewind].p2 = addr_after;

    emit(stmt, &pc, Op_Close, 1, 0, 0, NULL);
    emit(stmt, &pc, Op_Close, 2, 0, 0, NULL);
    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);

    stmt->schema_change = true;
    return CHIDB_OK;
}


/* --- INSERT ----------------------------------------------------------- */

/* claude: register layout mirrors tests/files/dbm-programs/insert/insert-1.dbmf:
 * r0=table root, r1=key (the PK value), r2..r(2+ncols-1)=one register per
 * column (Null for the PK's own slot, the literal for every other one),
 * r(2+ncols)=packed record, and (assignment_opt.html point 2) one more
 * register + cursor per index on this table, from r(3+ncols)/cursor 1
 * onward, to keep each of them up to date with the new row. */
static int codegen_insert(chidb_stmt *stmt, Insert_t *insert)
{
    chidb *db = stmt->db;
    chidb_schema_item_t *tbl = chidb_schema_find_table(db, insert->table_name);
    if (!tbl)
        return CHIDB_EINVALIDSQL;

    chisql_statement_t *parsed;
    Column_t *columns = table_columns(tbl, &parsed);
    int ncols = column_count(columns);
    int pk_idx = primary_key_index(columns);

    Literal_t **vals = malloc(sizeof(Literal_t *) * ncols);
    int i = 0;
    for (Literal_t *v = insert->values; v; v = v->next, i++)
    {
        if (i >= ncols)
        {
            free(vals);
            free(parsed);
            return CHIDB_EINVALIDSQL;
        }
        vals[i] = v;
    }
    if (i != ncols)
    {
        free(vals);
        free(parsed);
        return CHIDB_EINVALIDSQL;
    }

    i = 0;
    for (Column_t *c = columns; c; c = c->next, i++)
        if (!literal_matches_type(vals[i], c->type))
        {
            free(vals);
            free(parsed);
            return CHIDB_EINVALIDSQL;
        }

    int pc = 0;
    emit(stmt, &pc, Op_Integer, (int32_t) tbl->root_page, 0, 0, NULL);
    emit(stmt, &pc, Op_OpenWrite, 0, 0, ncols, NULL);
    emit(stmt, &pc, Op_Integer, vals[pk_idx]->val.ival, 1, 0, NULL);

    for (i = 0; i < ncols; i++)
    {
        if (i == pk_idx)
            emit(stmt, &pc, Op_Null, 0, 2 + i, 0, NULL);
        else
            emit_literal(stmt, &pc, vals[i], 2 + i);
    }

    int r_record = 2 + ncols;
    emit(stmt, &pc, Op_MakeRecord, 2, ncols, r_record, NULL);
    emit(stmt, &pc, Op_Insert, 0, r_record, 1, NULL);

    int next_reg = r_record + 1;
    int cursor_num = 1;
    for (chidb_schema_item_t *item = db->schema; item; item = item->next)
    {
        if (strcmp(item->type, "index") != 0 || strcasecmp(item->table_name, insert->table_name) != 0)
            continue;

        chisql_statement_t *idx_parsed;
        chisql_parser(item->sql, &idx_parsed);
        int idx_col_idx = column_index_by_name(columns, idx_parsed->stmt.create->index->column_name, NULL);
        free(idx_parsed);
        if (idx_col_idx < 0)
            continue; /* shouldn't happen: schema is internally consistent */

        int r_idxroot = next_reg++;
        int r_idxkey = (idx_col_idx == pk_idx) ? 1 : (2 + idx_col_idx);

        emit(stmt, &pc, Op_Integer, (int32_t) item->root_page, r_idxroot, 0, NULL);
        emit(stmt, &pc, Op_OpenWrite, cursor_num, r_idxroot, 0, NULL);
        emit(stmt, &pc, Op_IdxInsert, cursor_num, r_idxkey, 1, NULL);
        emit(stmt, &pc, Op_Close, cursor_num, 0, 0, NULL);
        cursor_num++;
    }

    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);
    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);

    free(vals);
    free(parsed);
    return CHIDB_OK;
}


/* --- SELECT ------------------------------------------------------------ */

/* Shared by both codegen_select's full-scan path and its index-seek path:
 * emit one Key/Column op per projected column (reading through `cursor`,
 * which must already be positioned) into r_out0..r_out0+nout-1, followed
 * by the ResultRow that reports them. */
static void emit_output_columns(chidb_stmt *stmt, int *pc, int32_t cursor, Column_t *columns,
                                 int *out_idx, int nout, int32_t r_out0)
{
    for (int i = 0; i < nout; i++)
    {
        if (column_is_primary_key(columns, out_idx[i]))
            emit(stmt, pc, Op_Key, cursor, r_out0 + i, 0, NULL);
        else
            emit(stmt, pc, Op_Column, cursor, out_idx[i], r_out0 + i, NULL);
    }
    emit(stmt, pc, Op_ResultRow, r_out0, nout, 0, NULL);
}

static void set_output_columns(chidb_stmt *stmt, Column_t *columns, int *out_idx, int nout)
{
    stmt->nCols = nout;
    /* claude: chidb_stmt_exec() asserts nRR==nCols after every run, including
     * a run that ends at Halt without ever executing ResultRow (a query
     * that legitimately matches zero rows) -- pre-set nRR here so that
     * case holds trivially; a real ResultRow later sets it to the same
     * value anyway, since its P2 is always this same `nout`. */
    stmt->nRR = nout;
    stmt->cols = malloc(sizeof(char *) * nout);
    for (int j = 0; j < nout; j++)
        stmt->cols[j] = strdup(column_name_at(columns, out_idx[j]));
}

/* claude: assignment_opt.html point 3. Compiles `WHERE indexed-col = val`
 * (or `val = indexed-col`) into an index seek instead of a full table
 * scan: seek the index cursor to the (unique) IdxKey, recover the row's
 * PKey, then seek the table cursor straight to that PKey. No loop at all,
 * since the index is assumed unique (same assumption CREATE INDEX itself
 * makes). Two independent early-exit points, each closing only the
 * cursor(s) actually open at that point: an index miss skips opening the
 * table cursor entirely; the table seek "missing" (shouldn't happen, since
 * the PKey just came out of the index) still closes both. */
static int codegen_select_indexed(chidb_stmt *stmt, chidb_schema_item_t *tbl, chidb_schema_item_t *idx,
                                   Column_t *columns, int *out_idx, int nout, Literal_t *where_lit)
{
    int r_idxroot = 0, r_lit = 1, r_pkey = 2, r_tblroot = 3, r_out0 = 4;

    int pc = 0;
    emit(stmt, &pc, Op_Integer, (int32_t) idx->root_page, r_idxroot, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 0, r_idxroot, 0, NULL);
    emit_literal(stmt, &pc, where_lit, r_lit);
    int addr_seek_idx = emit(stmt, &pc, Op_Seek, 0, -1, r_lit, NULL);
    emit(stmt, &pc, Op_IdxPKey, 0, r_pkey, 0, NULL);

    emit(stmt, &pc, Op_Integer, (int32_t) tbl->root_page, r_tblroot, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 1, r_tblroot, column_count(columns), NULL);
    int addr_seek_tbl = emit(stmt, &pc, Op_Seek, 1, -1, r_pkey, NULL);

    emit_output_columns(stmt, &pc, 1, columns, out_idx, nout, r_out0);

    int addr_close1 = pc;
    emit(stmt, &pc, Op_Close, 1, 0, 0, NULL);
    stmt->ops[addr_seek_tbl].p2 = addr_close1;

    int addr_close0 = pc;
    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);
    stmt->ops[addr_seek_idx].p2 = addr_close0;

    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);

    set_output_columns(stmt, columns, out_idx, nout);
    return CHIDB_OK;
}

/* --- SELECT ... NATURAL JOIN ------------------------------------------- */

/* Resolves a column reference against one of the two tables in a
 * two-way NATURAL JOIN: side 0 = the left table, 1 = the right. */
static bool resolve_join_column(ColumnReference_t *ref,
                                 Column_t *cols1, const char *name1, const char *alias1,
                                 Column_t *cols2, const char *name2, const char *alias2,
                                 RCol *out, enum data_type *type_out)
{
    if (ref->tableName)
    {
        if (strcasecmp(ref->tableName, name1) == 0 || (alias1 && strcasecmp(ref->tableName, alias1) == 0))
        {
            int idx = column_index_by_name(cols1, ref->columnName, type_out);
            if (idx < 0)
                return false;
            *out = (RCol){ 0, idx };
            return true;
        }
        if (strcasecmp(ref->tableName, name2) == 0 || (alias2 && strcasecmp(ref->tableName, alias2) == 0))
        {
            int idx = column_index_by_name(cols2, ref->columnName, type_out);
            if (idx < 0)
                return false;
            *out = (RCol){ 1, idx };
            return true;
        }
        return false;
    }

    /* Unqualified: a name shared by both tables is -- by the definition of
     * NATURAL JOIN -- a join column, equal on both sides in any matching
     * row, so resolving it to the left table's copy is always correct. A
     * name that appears in only one of the two tables can't collide with
     * anything in the other (again by that same definition), so it's
     * unambiguous too. */
    int idx1 = column_index_by_name(cols1, ref->columnName, type_out);
    if (idx1 >= 0)
    {
        *out = (RCol){ 0, idx1 };
        return true;
    }
    int idx2 = column_index_by_name(cols2, ref->columnName, type_out);
    if (idx2 >= 0)
    {
        *out = (RCol){ 1, idx2 };
        return true;
    }
    return false;
}

/* ColResolver adapter for resolve_join_column(), so join WHERE clauses can
 * go through the same resolve_conjuncts() as a single-table SELECT. */
typedef struct
{
    Column_t *cols1; const char *name1; const char *alias1;
    Column_t *cols2; const char *name2; const char *alias2;
} JoinResolveCtx;

static bool resolve_join_column_cb(void *ctx0, ColumnReference_t *ref, RCol *out, enum data_type *type_out)
{
    JoinResolveCtx *ctx = (JoinResolveCtx *) ctx0;
    return resolve_join_column(ref, ctx->cols1, ctx->name1, ctx->alias1, ctx->cols2, ctx->name2, ctx->alias2,
                                out, type_out);
}

/* claude: assignment_codegen.html step 5 (two-way NATURAL JOIN, columns
 * optionally qualified with a table name/alias) combined with step 2's
 * WHERE restriction, generalized to a conjunction of comparisons (see
 * the "WHERE clauses" section above) -- needed for optimizer.c's
 * sigma-pushing to be safe to turn on, since a condition pushed to one
 * side of the join can leave more than one conjunct on the other side,
 * or at the top. There is no NATURAL-JOIN-specific test suite published
 * upstream (assignment_codegen.html: "Tests for NATURAL JOIN are not
 * currently available"), so this is validated against a fixture built
 * for this pass; see tests/files/dbm-programs/sql-select-join/.
 *
 * `table_sra` is the NaturalJoin node; either side may itself be a
 * Select wrapping a bare Table -- exactly the shape optimizer.c's
 * sigma-pushing produces, moving conditions that only touch one side
 * down next to that side's Table. `cond` is whatever's left un-pushed
 * at the top (NULL if everything was pushed, or if the optimizer never
 * touched this query in the first place).
 *
 * Compiles to a nested-loop join: cursor 0 = left table, cursor 1 =
 * right table. A condition pushed to the LEFT side is checked once per
 * outer-loop row, skipping straight past the entire inner loop when it
 * fails -- the actual performance point of pushing, in this executor.
 * A condition pushed to the RIGHT side, the natural-join equality tests
 * themselves, and whatever's left at the top are all checked once per
 * (outer,inner) pair, same as before pushing existed. Every kind of
 * check is a negated jump straight to the relevant Next instruction,
 * same technique as the single-table scan uses. Using an index for
 * either side's scan is not implemented -- see
 * docs/claude_notes/plan_chidb_implementation.md. */
static int codegen_select_join(chidb_stmt *stmt, SRA_t *table_sra, Expression_t *expr_list, Condition_t *cond)
{
    chidb *db = stmt->db;
    SRA_t *sra1 = table_sra->binary.sra1, *sra2 = table_sra->binary.sra2;

    Condition_t *side1_cond = NULL, *side2_cond = NULL;
    if (sra1->t == SRA_SELECT)
    {
        side1_cond = sra1->select.cond;
        sra1 = sra1->select.sra;
    }
    if (sra2->t == SRA_SELECT)
    {
        side2_cond = sra2->select.cond;
        sra2 = sra2->select.sra;
    }
    if (sra1->t != SRA_TABLE || sra2->t != SRA_TABLE)
        return CHIDB_EINVALIDSQL; /* only a two-way join of base tables */

    const char *name1 = sra1->table.ref->table_name, *alias1 = sra1->table.ref->alias;
    const char *name2 = sra2->table.ref->table_name, *alias2 = sra2->table.ref->alias;

    chidb_schema_item_t *tbl1 = chidb_schema_find_table(db, name1);
    chidb_schema_item_t *tbl2 = chidb_schema_find_table(db, name2);
    if (!tbl1 || !tbl2)
        return CHIDB_EINVALIDSQL;

    chisql_statement_t *parsed1, *parsed2;
    Column_t *cols1 = table_columns(tbl1, &parsed1);
    Column_t *cols2 = table_columns(tbl2, &parsed2);
    int ncols1 = column_count(cols1), ncols2 = column_count(cols2);
    JoinResolveCtx jctx = { cols1, name1, alias1, cols2, name2, alias2 };

    /* Natural-join column pairs: every name shared by both tables. */
    int *join1 = malloc(sizeof(int) * ncols1);
    int *join2 = malloc(sizeof(int) * ncols1);
    int npairs = 0;
    {
        int i = 0;
        for (Column_t *c = cols1; c; c = c->next, i++)
        {
            int j = column_index_by_name(cols2, c->name, NULL);
            if (j >= 0)
            {
                join1[npairs] = i;
                join2[npairs] = j;
                npairs++;
            }
        }
    }

    bool star = (expr_list->next == NULL && expr_list->t == EXPR_TERM &&
                 expr_list->expr.term.t == TERM_COLREF &&
                 strcmp(expr_list->expr.term.ref->columnName, "*") == 0);

    int nout;
    RCol *out;
    if (star)
    {
        /* `*` coalesces the natural-join columns, same as real SQL: every
         * column of the left table, then only the right table's columns
         * that AREN'T one of the join columns. */
        nout = ncols1;
        for (int i = 0; i < ncols2; i++)
        {
            bool isjoin = false;
            for (int p = 0; p < npairs; p++)
                if (join2[p] == i) { isjoin = true; break; }
            if (!isjoin)
                nout++;
        }
        out = malloc(sizeof(RCol) * nout);
        int k = 0;
        for (int i = 0; i < ncols1; i++)
            out[k++] = (RCol){ 0, i };
        for (int i = 0; i < ncols2; i++)
        {
            bool isjoin = false;
            for (int p = 0; p < npairs; p++)
                if (join2[p] == i) { isjoin = true; break; }
            if (!isjoin)
                out[k++] = (RCol){ 1, i };
        }
    }
    else
    {
        nout = 0;
        for (Expression_t *e = expr_list; e; e = e->next)
            nout++;
        out = malloc(sizeof(RCol) * nout);
        int k = 0;
        for (Expression_t *e = expr_list; e; e = e->next, k++)
        {
            if (e->t != EXPR_TERM || e->expr.term.t != TERM_COLREF ||
                !resolve_join_column_cb(&jctx, e->expr.term.ref, &out[k], NULL))
            {
                free(out);
                free(join1);
                free(join2);
                free(parsed1);
                free(parsed2);
                return CHIDB_EINVALIDSQL;
            }
        }
    }

    ResolvedCmp *side1_cmps = NULL, *side2_cmps = NULL, *top_cmps = NULL;
    int n1 = 0, n2 = 0, nt = 0;
    if (!resolve_conjuncts(side1_cond, resolve_join_column_cb, &jctx, &side1_cmps, &n1) ||
        !resolve_conjuncts(side2_cond, resolve_join_column_cb, &jctx, &side2_cmps, &n2) ||
        !resolve_conjuncts(cond, resolve_join_column_cb, &jctx, &top_cmps, &nt))
    {
        free(side1_cmps);
        free(side2_cmps);
        free(top_cmps);
        free(out);
        free(join1);
        free(join2);
        free(parsed1);
        free(parsed2);
        return CHIDB_EINVALIDSQL;
    }

    /* From here on nothing else can fail, so it's safe to start emitting. */
    int32_t r_root1 = 0, r_root2 = 1;
    int32_t r_lit_base = 2;
    int32_t r_tmp_a = r_lit_base + n1 + n2 + nt;
    int32_t r_tmp_b = r_tmp_a + 1;
    int32_t r_out0 = r_tmp_b + 1;

    int pc = 0;
    emit(stmt, &pc, Op_Integer, (int32_t) tbl1->root_page, r_root1, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 0, r_root1, ncols1, NULL);
    emit(stmt, &pc, Op_Integer, (int32_t) tbl2->root_page, r_root2, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 1, r_root2, ncols2, NULL);

    emit_filter_literals(stmt, &pc, side1_cmps, n1, r_lit_base);
    emit_filter_literals(stmt, &pc, side2_cmps, n2, r_lit_base + n1);
    emit_filter_literals(stmt, &pc, top_cmps, nt, r_lit_base + n1 + n2);

    int npatch_max = npairs + n1 + n2 + nt + 1;
    int *patch_outer = malloc(sizeof(int) * npatch_max);
    int *patch_inner = malloc(sizeof(int) * npatch_max);
    int npatch_outer = 0, npatch_inner = 0;

    int addr_rewind0 = emit(stmt, &pc, Op_Rewind, 0, -1, 0, NULL);
    int addr_loop1 = pc;

    /* Conditions pushed to the left (outer) table: checked once per
     * outer row, before we even bother rewinding the inner cursor. */
    emit_filter_checks(stmt, &pc, side1_cmps, n1, cols1, cols2, r_lit_base, r_tmp_a, patch_outer, &npatch_outer);

    int addr_rewind1 = emit(stmt, &pc, Op_Rewind, 1, -1, 0, NULL);
    int addr_loop2 = pc;

    for (int p = 0; p < npairs; p++)
    {
        emit_rcol(stmt, &pc, (RCol){ 0, join1[p] }, cols1, cols2, r_tmp_a);
        emit_rcol(stmt, &pc, (RCol){ 1, join2[p] }, cols1, cols2, r_tmp_b);
        patch_inner[npatch_inner++] = emit(stmt, &pc, Op_Ne, r_tmp_a, -1, r_tmp_b, NULL);
    }
    emit_filter_checks(stmt, &pc, side2_cmps, n2, cols1, cols2, r_lit_base + n1, r_tmp_a, patch_inner, &npatch_inner);
    emit_filter_checks(stmt, &pc, top_cmps, nt, cols1, cols2, r_lit_base + n1 + n2, r_tmp_a, patch_inner,
                        &npatch_inner);

    for (int k = 0; k < nout; k++)
        emit_rcol(stmt, &pc, out[k], cols1, cols2, r_out0 + k);
    emit(stmt, &pc, Op_ResultRow, r_out0, nout, 0, NULL);

    int addr_skip_inner = emit(stmt, &pc, Op_Next, 1, addr_loop2, 0, NULL);
    for (int k = 0; k < npatch_inner; k++)
        stmt->ops[patch_inner[k]].p2 = addr_skip_inner;

    int addr_next0 = pc;
    stmt->ops[addr_rewind1].p2 = addr_next0;
    for (int k = 0; k < npatch_outer; k++)
        stmt->ops[patch_outer[k]].p2 = addr_next0;
    emit(stmt, &pc, Op_Next, 0, addr_loop1, 0, NULL);

    int addr_end = pc;
    stmt->ops[addr_rewind0].p2 = addr_end;
    emit(stmt, &pc, Op_Close, 1, 0, 0, NULL);
    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);
    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);

    stmt->nCols = nout;
    stmt->nRR = nout;
    stmt->cols = malloc(sizeof(char *) * nout);
    for (int k = 0; k < nout; k++)
        stmt->cols[k] = strdup(column_name_at(out[k].side == 0 ? cols1 : cols2, out[k].idx));

    free(patch_outer);
    free(patch_inner);
    free(side1_cmps);
    free(side2_cmps);
    free(top_cmps);
    free(out);
    free(join1);
    free(join2);
    free(parsed1);
    free(parsed2);
    return CHIDB_OK;
}


/* claude: register layout mirrors testing.html's worked example
 * (`SELECT * FROM courses`) in the no-WHERE case: r0=table root,
 * r1..r(nout)=output columns. A WHERE clause (any number of `column OP
 * literal` conjuncts ANDed together -- see the "WHERE clauses" section
 * above) shifts the output columns up to make room for one literal
 * register per conjunct plus one shared scratch register, and each
 * conjunct is compiled as its own *negation*, jumping straight to the
 * Next instruction (skipping this row) when the negation holds -- e.g.
 * `col > val` becomes `Le val, addr_next, col` ("if col <= val, skip").
 * See docs/claude_notes/notes_dbm_spec.txt for why the comparison
 * opcodes' operand order has to be mirrored like this. */
static int codegen_select(chidb_stmt *stmt, SRA_t *sra)
{
    chidb *db = stmt->db;

    if (sra->t != SRA_PROJECT)
        return CHIDB_EINVALIDSQL;
    Expression_t *expr_list = sra->project.expr_list;
    SRA_t *inner = sra->project.sra;

    Condition_t *cond = NULL;
    SRA_t *table_sra = inner;
    if (inner->t == SRA_SELECT)
    {
        cond = inner->select.cond;
        table_sra = inner->select.sra;
    }
    if (table_sra->t == SRA_NATURAL_JOIN)
        return codegen_select_join(stmt, table_sra, expr_list, cond);
    if (table_sra->t != SRA_TABLE)
        return CHIDB_EINVALIDSQL; /* outer/theta joins, unions, etc.: not implemented */

    const char *table_name = table_sra->table.ref->table_name;
    chidb_schema_item_t *tbl = chidb_schema_find_table(db, table_name);
    if (!tbl)
        return CHIDB_EINVALIDSQL;

    chisql_statement_t *parsed;
    Column_t *columns = table_columns(tbl, &parsed);
    int ncols = column_count(columns);

    bool star = (expr_list->next == NULL && expr_list->t == EXPR_TERM &&
                 expr_list->expr.term.t == TERM_COLREF &&
                 strcmp(expr_list->expr.term.ref->columnName, "*") == 0);

    int nout;
    int *out_idx;
    if (star)
    {
        nout = ncols;
        out_idx = malloc(sizeof(int) * nout);
        for (int i = 0; i < nout; i++)
            out_idx[i] = i;
    }
    else
    {
        nout = 0;
        for (Expression_t *e = expr_list; e; e = e->next)
            nout++;
        out_idx = malloc(sizeof(int) * nout);
        int i = 0;
        for (Expression_t *e = expr_list; e; e = e->next, i++)
        {
            if (e->t != EXPR_TERM || e->expr.term.t != TERM_COLREF)
            {
                free(out_idx);
                free(parsed);
                return CHIDB_EINVALIDSQL;
            }
            int idx = column_index_by_name(columns, e->expr.term.ref->columnName, NULL);
            if (idx < 0)
            {
                free(out_idx);
                free(parsed);
                return CHIDB_EINVALIDSQL;
            }
            out_idx[i] = idx;
        }
    }

    ResolvedCmp *cmps;
    int ncmp;
    if (!resolve_conjuncts(cond, resolve_single_column, columns, &cmps, &ncmp))
    {
        free(out_idx);
        free(parsed);
        return CHIDB_EINVALIDSQL;
    }

    /* assignment_opt.html point 3: a single top-level `indexed-col = val`
     * (or `val = indexed-col`) compiles to an index seek instead of a
     * full scan -- doesn't extend to a multi-conjunct WHERE. */
    if (ncmp == 1 && cmps[0].op == Op_Ne)
    {
        chidb_schema_item_t *idx = chidb_schema_find_index_on(db, table_name, column_name_at(columns, cmps[0].col.idx));
        if (idx)
        {
            int rc = codegen_select_indexed(stmt, tbl, idx, columns, out_idx, nout, cmps[0].lit);
            free(cmps);
            free(out_idx);
            free(parsed);
            return rc;
        }
    }

    int32_t r_root = 0;
    int32_t r_lit_base = 1;
    int32_t r_tmp = r_lit_base + ncmp;
    int32_t r_out0 = (ncmp == 0) ? r_lit_base : (r_tmp + 1);

    int pc = 0;
    emit(stmt, &pc, Op_Integer, (int32_t) tbl->root_page, r_root, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 0, r_root, ncols, NULL);
    emit_filter_literals(stmt, &pc, cmps, ncmp, r_lit_base);

    int addr_rewind = emit(stmt, &pc, Op_Rewind, 0, -1, 0, NULL);
    int addr_loop = pc;

    int *patch = malloc(sizeof(int) * (ncmp + 1));
    int npatch = 0;
    emit_filter_checks(stmt, &pc, cmps, ncmp, columns, NULL, r_lit_base, r_tmp, patch, &npatch);

    emit_output_columns(stmt, &pc, 0, columns, out_idx, nout, r_out0);

    int addr_next = emit(stmt, &pc, Op_Next, 0, addr_loop, 0, NULL);
    for (int k = 0; k < npatch; k++)
        stmt->ops[patch[k]].p2 = addr_next;

    int addr_after = pc;
    stmt->ops[addr_rewind].p2 = addr_after;

    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);
    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);

    set_output_columns(stmt, columns, out_idx, nout);

    free(patch);
    free(cmps);
    free(out_idx);
    free(parsed);
    return CHIDB_OK;
}


int chidb_stmt_codegen(chidb_stmt *stmt, chisql_statement_t *sql_stmt)
{
    switch (sql_stmt->type)
    {
    case STMT_CREATE:
        if (sql_stmt->stmt.create->t == CREATE_TABLE)
            return codegen_create_table(stmt, sql_stmt->stmt.create->table, sql_stmt->text);
        else
            return codegen_create_index(stmt, sql_stmt->stmt.create->index, sql_stmt->text);

    case STMT_INSERT:
        return codegen_insert(stmt, sql_stmt->stmt.insert);

    case STMT_SELECT:
        return codegen_select(stmt, sql_stmt->stmt.select);

    default:
        /* DELETE, and anything else the parser accepts: out of scope. */
        return CHIDB_EINVALIDSQL;
    }
}
