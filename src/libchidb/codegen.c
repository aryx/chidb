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

/* assignment_opt.html's index point originally covered only equality;
 * extended here to every comparison operator, since a B-Tree index
 * seek naturally supports "find the boundary entry and walk from
 * there" for a range just as well as "find this one entry" for an
 * equality. `ResolvedCmp.op` is already negated (see above), so this
 * maps the negated op back to which *original* comparison it was and
 * how to seek for it. */
typedef enum { INDEX_EQ, INDEX_GT, INDEX_GE, INDEX_LT, INDEX_LE } IndexSeekKind;

static bool index_seek_kind(opcode_t negated_op, IndexSeekKind *kind)
{
    switch (negated_op)
    {
    case Op_Ne: *kind = INDEX_EQ; return true;
    case Op_Le: *kind = INDEX_GT; return true;
    case Op_Lt: *kind = INDEX_GE; return true;
    case Op_Ge: *kind = INDEX_LT; return true;
    case Op_Gt: *kind = INDEX_LE; return true;
    default: return false;
    }
}

static opcode_t index_seek_opcode(IndexSeekKind kind)
{
    switch (kind)
    {
    case INDEX_EQ: return Op_Seek;
    case INDEX_GT: return Op_SeekGt;
    case INDEX_GE: return Op_SeekGe;
    case INDEX_LT: return Op_SeekLt;
    case INDEX_LE: return Op_SeekLe;
    }
    return Op_Seek;
}

/* GT/GE seek to the first qualifying entry and walk forward (ascending);
 * LT/LE seek to the last qualifying entry and walk backward (descending).
 * Since the index is sorted and (per CREATE INDEX's own contract) unique,
 * every entry from the seek point onward in that direction automatically
 * still satisfies the original condition -- no per-row bound check
 * needed, just "did Next/Prev find another entry". EQ never walks at
 * all: with a unique index there's at most one match, and the *next*
 * entry in either direction necessarily has a different key. */
static bool index_seek_walks_forward(IndexSeekKind kind)
{
    return kind == INDEX_GT || kind == INDEX_GE;
}

/* claude: extends the single-comparison index seek above to a *two-sided*
 * bounded range on the same indexed column, e.g. `WHERE col > 10 AND col <
 * 20`. Recognizes exactly the shape "two conjuncts, same column, one a
 * lower bound (>/>=) and the other an upper bound (</<=)" -- anything else
 * (a third conjunct, both bounds on the same side, two different columns,
 * an equality mixed with a bound, ...) returns false and the caller falls
 * back to its existing single-comparison-or-scan logic unchanged. On
 * success, `*lo` and `*hi` are the indices into `cmps` of the lower/upper bound
 * (always normalized this way regardless of which order the AND put them
 * in), so the caller can always seek forward from the lower bound and stop
 * the walk once the upper bound fails, rather than needing a separate
 * descending variant too. */
static bool detect_range_pair(ResolvedCmp *cmps, int ncmp, int *lo, int *hi)
{
    if (ncmp != 2)
        return false;

    IndexSeekKind k0, k1;
    if (!index_seek_kind(cmps[0].op, &k0) || !index_seek_kind(cmps[1].op, &k1))
        return false;
    if (cmps[0].col.side != cmps[1].col.side || cmps[0].col.idx != cmps[1].col.idx)
        return false;

    bool k0_lo = (k0 == INDEX_GT || k0 == INDEX_GE), k0_hi = (k0 == INDEX_LT || k0 == INDEX_LE);
    bool k1_lo = (k1 == INDEX_GT || k1 == INDEX_GE), k1_hi = (k1 == INDEX_LT || k1 == INDEX_LE);
    if (k0_lo && k1_hi) { *lo = 0; *hi = 1; return true; }
    if (k1_lo && k0_hi) { *lo = 1; *hi = 0; return true; }
    return false;
}

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
    enum data_type coltype;
    int col_idx = column_index_by_name(columns, index->column_name, &coltype);
    /* claude: fileformat.html/assignment_opt.html: "Indexes can only be
     * created for unsigned 4-byte integer unique fields" -- without this
     * check, indexing a TEXT column silently builds a corrupt index
     * (IdxInsert reads a REG_STRING register's .value.i, which is really
     * the low bits of a pointer, as if it were the intended integer key). */
    if (col_idx < 0 || coltype != TYPE_INT)
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

/* claude: assignment_opt.html point 3, extended from equality to every
 * comparison operator (see IndexSeekKind above). Seeks the index cursor
 * to the boundary entry for `kind`, recovers that entry's PKey, and
 * seeks the table cursor straight to it. For INDEX_EQ that's the whole
 * story -- no loop, since the index is unique (same assumption CREATE
 * INDEX itself makes) so there's at most one match and nothing else to
 * visit. For a range kind, the boundary entry is just the *first* match,
 * and the loop walks the index forward or backward (index_seek_walks_
 * forward) from there, re-doing the IdxPKey+table-seek step for each
 * entry in turn, until the index cursor itself runs out.
 *
 * `bound2`, when non-NULL (see detect_range_pair), is a second, upper
 * bound on the *same* indexed column -- `kind`/`cmp` is always the lower
 * bound in that case, so the walk always goes forward. Unlike the
 * single-bound case, the walk can't rely on the cursor's own exhaustion
 * to know when to stop (there may be plenty more index entries past the
 * upper bound); instead each visited row's column value is checked
 * against `bound2` directly, and a failure there stops the walk exactly
 * where a natural Next exhaustion would -- not "skip this row and keep
 * walking", since the index is sorted and once one row fails the upper
 * bound, every later one would too.
 *
 * Both cursors are opened before either Seek runs, so every failure
 * point -- an index miss (no qualifying entry at all), or the table seek
 * (shouldn't fail, since the PKey just came out of the index) -- can
 * share the same closing tail; a table-seek failure routes to wherever
 * the loop would otherwise continue (or, for INDEX_EQ, straight to that
 * tail), rather than aborting outright. */
static int codegen_select_indexed(chidb_stmt *stmt, chidb_schema_item_t *tbl, chidb_schema_item_t *idx,
                                   Column_t *columns, int *out_idx, int nout, ResolvedCmp *cmp, IndexSeekKind kind,
                                   ResolvedCmp *bound2)
{
    int r_idxroot = 0, r_root = 1, r_lit = 2, r_pkey = 3;
    int32_t r_lit2 = bound2 ? 4 : -1;
    int32_t r_tmp2 = bound2 ? 5 : -1;
    int32_t r_out0 = bound2 ? 6 : 4;

    int pc = 0;
    emit(stmt, &pc, Op_Integer, (int32_t) idx->root_page, r_idxroot, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 0, r_idxroot, 0, NULL);
    emit(stmt, &pc, Op_Integer, (int32_t) tbl->root_page, r_root, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 1, r_root, column_count(columns), NULL);
    emit_literal(stmt, &pc, cmp->lit, r_lit);
    if (bound2)
        emit_literal(stmt, &pc, bound2->lit, r_lit2);

    int addr_seek_idx = emit(stmt, &pc, index_seek_opcode(kind), 0, -1, r_lit, NULL);
    int addr_loop = pc;
    emit(stmt, &pc, Op_IdxPKey, 0, r_pkey, 0, NULL);
    int addr_seek_tbl = emit(stmt, &pc, Op_Seek, 1, -1, r_pkey, NULL);

    int addr_bound2_fail = -1;
    if (bound2)
    {
        /* cursor 1 is the table cursor here (cursor 0 is the index), so
         * this can't reuse emit_rcol -- it hardcodes cursor 0 for side 0. */
        if (column_is_primary_key(columns, bound2->col.idx))
            emit(stmt, &pc, Op_Key, 1, r_tmp2, 0, NULL);
        else
            emit(stmt, &pc, Op_Column, 1, bound2->col.idx, r_tmp2, NULL);
        addr_bound2_fail = emit(stmt, &pc, bound2->op, r_lit2, -1, r_tmp2, NULL);
    }

    emit_output_columns(stmt, &pc, 1, columns, out_idx, nout, r_out0);

    int addr_after = (kind == INDEX_EQ)
                          ? pc
                          : emit(stmt, &pc, index_seek_walks_forward(kind) ? Op_Next : Op_Prev, 0, addr_loop, 0, NULL);
    stmt->ops[addr_seek_tbl].p2 = addr_after;

    int addr_tail = pc;
    if (bound2)
        stmt->ops[addr_bound2_fail].p2 = addr_tail;
    emit(stmt, &pc, Op_Close, 1, 0, 0, NULL);
    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);
    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);
    stmt->ops[addr_seek_idx].p2 = addr_tail;

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
 * Cursor 0 = left table, cursor 1 = right table, cursor 2/3 = an index
 * cursor used only while a side is index-driven (see below). Each side
 * is independently planned (plan_side_access) as one of:
 *   - ACC_SCAN: the side's whole pushed condition (0 or more conjuncts)
 *     is checked as a per-row filter inside a Rewind/Next loop over the
 *     table cursor, same as a single-table scan.
 *   - ACC_EQSEEK: the side's pushed condition is exactly one equality on
 *     an indexed column -- seek that index for the (unique) matching
 *     row instead of scanning. No loop at all for that side, since
 *     there's at most one row.
 *   - ACC_RANGESEEK: the side's pushed condition is exactly one `<`/`<=`/
 *     `>`/`>=` on an indexed column (assignment_opt.html's index point,
 *     extended from equality to any comparison, same as
 *     codegen_select_indexed), or exactly two conjuncts forming a
 *     lower+upper bound on the same indexed column (detect_range_pair,
 *     same extension as codegen_select's single-table path) -- seek the
 *     index to the boundary entry and then walk it forward or backward
 *     (index_seek_walks_forward), re-deriving the matching table row
 *     (IdxPKey + a table Seek) at each step, until the index cursor
 *     itself runs out *or*, in the two-sided case, until the second
 *     bound fails on a visited row (checked directly, since sorted order
 *     means one failure implies every later one too -- see cmp2 below).
 *     Loops, same as ACC_SCAN, just driven by the index cursor instead
 *     of the table cursor.
 * An index-driven side's boundary is a compile-time literal, so it
 * never depends on the other side's current row: an ACC_EQSEEK side is
 * therefore positioned once, up front, before any loop runs at all, and
 * its cursor is simply never Rewind/Next again -- the natural-join
 * equality check against it (and the other side's own filter/output
 * reads) just keeps reading whatever row it landed on. An ACC_RANGESEEK
 * side still has to loop, but starts that loop already filtered down to
 * just its own matching rows, rather than the whole table.
 *
 * Every cursor actually needed is opened up front, before any Seek or
 * Rewind runs, specifically so that every possible "no more rows" event
 * -- an index miss, an empty table on a SCAN side -- can share one
 * cursor-closing tail at the very end: by construction, nothing that
 * tail closes is ever still unopened at the point something jumps to
 * it. Every check (natural-join equality, an ACC_SCAN side's filters,
 * the top-level leftover conjuncts) is a negated jump, same technique
 * as the single-table scan uses. */

/* Which of the three access strategies (see above) one side of a join
 * uses, and the details codegen_select_join needs to emit it. */
typedef struct
{
    enum { ACC_SCAN, ACC_EQSEEK, ACC_RANGESEEK } kind;
    chidb_schema_item_t *idx; /* set iff kind != ACC_SCAN */
    ResolvedCmp *cmp;         /* the driving (seek) conjunct, iff kind != ACC_SCAN */
    ResolvedCmp *cmp2;        /* the upper bound of a two-sided range, iff set (kind == ACC_RANGESEEK only) */
    opcode_t seek_op;         /* Op_Seek/SeekGt/SeekGe/SeekLt/SeekLe, iff kind != ACC_SCAN */
    bool forward;             /* Next (true) vs Prev (false), iff kind == ACC_RANGESEEK */
} SideAccess;

static SideAccess plan_side_access(chidb *db, const char *table_name, Column_t *columns, ResolvedCmp *cmps, int ncmp)
{
    SideAccess sa = { ACC_SCAN, NULL, NULL, NULL, Op_Seek, false };

    IndexSeekKind kind;
    ResolvedCmp *primary, *bound2 = NULL;
    if (ncmp == 1 && index_seek_kind(cmps[0].op, &kind))
        primary = &cmps[0];
    else
    {
        int lo, hi;
        if (!detect_range_pair(cmps, ncmp, &lo, &hi))
            return sa;
        primary = &cmps[lo];
        bound2 = &cmps[hi];
        index_seek_kind(primary->op, &kind);
    }

    chidb_schema_item_t *idx = chidb_schema_find_index_on(db, table_name, column_name_at(columns, primary->col.idx));
    if (!idx)
        return sa;

    sa.idx = idx;
    sa.cmp = primary;
    sa.cmp2 = bound2;
    sa.seek_op = index_seek_opcode(kind);
    if (kind == INDEX_EQ)
        sa.kind = ACC_EQSEEK;
    else
    {
        sa.kind = ACC_RANGESEEK;
        sa.forward = index_seek_walks_forward(kind);
    }
    return sa;
}

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

    SideAccess side1acc = plan_side_access(db, name1, cols1, side1_cmps, n1);
    SideAccess side2acc = plan_side_access(db, name2, cols2, side2_cmps, n2);
    bool use_index1 = (side1acc.kind != ACC_SCAN), use_index2 = (side2acc.kind != ACC_SCAN);
    int n1_scan = (side1acc.kind == ACC_SCAN) ? n1 : 0; /* a seeking side's one
                                                          * conjunct is fully
                                                          * consumed by the seek */
    int n2_scan = (side2acc.kind == ACC_SCAN) ? n2 : 0;

    int next_reg = 0;
    int32_t r_idxroot1 = use_index1 ? next_reg++ : -1;
    int32_t r_root1 = next_reg++;
    int32_t r_idxroot2 = use_index2 ? next_reg++ : -1;
    int32_t r_root2 = next_reg++;
    int32_t r_lit1 = use_index1 ? next_reg++ : -1;
    int32_t r_pkey1 = use_index1 ? next_reg++ : -1;
    int32_t r_lit1b = side1acc.cmp2 ? next_reg++ : -1;
    int32_t r_lit2 = use_index2 ? next_reg++ : -1;
    int32_t r_pkey2 = use_index2 ? next_reg++ : -1;
    int32_t r_lit2b = side2acc.cmp2 ? next_reg++ : -1;
    int32_t r_lit_base = next_reg;
    next_reg += n1_scan + n2_scan + nt;
    int32_t r_tmp_a = next_reg++;
    int32_t r_tmp_b = next_reg++;
    int32_t r_out0 = next_reg;
    next_reg += nout;

    int pc = 0;

    /* Open every cursor that will be used, before any Seek/Rewind --
     * see the file comment for why. */
    if (use_index1)
    {
        emit(stmt, &pc, Op_Integer, (int32_t) side1acc.idx->root_page, r_idxroot1, 0, NULL);
        emit(stmt, &pc, Op_OpenRead, 2, r_idxroot1, 0, NULL);
    }
    emit(stmt, &pc, Op_Integer, (int32_t) tbl1->root_page, r_root1, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 0, r_root1, ncols1, NULL);
    if (use_index2)
    {
        emit(stmt, &pc, Op_Integer, (int32_t) side2acc.idx->root_page, r_idxroot2, 0, NULL);
        emit(stmt, &pc, Op_OpenRead, 3, r_idxroot2, 0, NULL);
    }
    emit(stmt, &pc, Op_Integer, (int32_t) tbl2->root_page, r_root2, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 1, r_root2, ncols2, NULL);

    if (use_index1)
        emit_literal(stmt, &pc, side1acc.cmp->lit, r_lit1);
    if (side1acc.cmp2)
        emit_literal(stmt, &pc, side1acc.cmp2->lit, r_lit1b);
    if (use_index2)
        emit_literal(stmt, &pc, side2acc.cmp->lit, r_lit2);
    if (side2acc.cmp2)
        emit_literal(stmt, &pc, side2acc.cmp2->lit, r_lit2b);

    /* Addresses whose failure means "zero rows, close everything opened
     * above, and stop" -- always safe, since every cursor is open by now. */
    int *patch_die = malloc(sizeof(int) * 4);
    int npatch_die = 0;

    /* ACC_EQSEEK sides are fully positioned here, once, before any loop:
     * their result can't depend on where a loop elsewhere currently is.
     * ACC_RANGESEEK sides still need their *own* loop (over the index
     * cursor), so their initial Seek is emitted later, at the point
     * where that side's loop begins -- exactly where an ACC_SCAN side's
     * Rewind would go. */
    if (side1acc.kind == ACC_EQSEEK)
    {
        patch_die[npatch_die++] = emit(stmt, &pc, Op_Seek, 2, -1, r_lit1, NULL);
        emit(stmt, &pc, Op_IdxPKey, 2, r_pkey1, 0, NULL);
        patch_die[npatch_die++] = emit(stmt, &pc, Op_Seek, 0, -1, r_pkey1, NULL);
    }
    if (side2acc.kind == ACC_EQSEEK)
    {
        patch_die[npatch_die++] = emit(stmt, &pc, Op_Seek, 3, -1, r_lit2, NULL);
        emit(stmt, &pc, Op_IdxPKey, 3, r_pkey2, 0, NULL);
        patch_die[npatch_die++] = emit(stmt, &pc, Op_Seek, 1, -1, r_pkey2, NULL);
    }

    emit_filter_literals(stmt, &pc, side1_cmps, n1_scan, r_lit_base);
    emit_filter_literals(stmt, &pc, side2_cmps, n2_scan, r_lit_base + n1_scan);
    emit_filter_literals(stmt, &pc, top_cmps, nt, r_lit_base + n1_scan + n2_scan);

    int npatch_max = npairs + n1_scan + n2_scan + nt + 1;
    int *patch_outer = malloc(sizeof(int) * npatch_max);
    int *patch_inner = malloc(sizeof(int) * npatch_max);
    int npatch_outer = 0, npatch_inner = 0;

    bool outer_loops = (side1acc.kind != ACC_EQSEEK);
    int addr_pos0 = -1;
    int addr_loop1 = pc;
    if (outer_loops)
    {
        addr_pos0 = (side1acc.kind == ACC_SCAN) ? emit(stmt, &pc, Op_Rewind, 0, -1, 0, NULL)
                                                 : emit(stmt, &pc, side1acc.seek_op, 2, -1, r_lit1, NULL);
        addr_loop1 = pc;
        if (side1acc.kind == ACC_SCAN)
            emit_filter_checks(stmt, &pc, side1_cmps, n1_scan, cols1, cols2, r_lit_base, r_tmp_a, patch_outer,
                                &npatch_outer);
        else /* ACC_RANGESEEK: re-derive the matching table row for this index entry */
        {
            emit(stmt, &pc, Op_IdxPKey, 2, r_pkey1, 0, NULL);
            patch_die[npatch_die++] = emit(stmt, &pc, Op_Seek, 0, -1, r_pkey1, NULL);
            /* Two-sided range: check the upper bound directly against this
             * row (cursor 0 is side1's table cursor). A failure here means
             * the whole outer walk is exhausted (sorted order: no later
             * entry can pass either), same as any other patch_die case --
             * unlike an ACC_SCAN filter failure, this must NOT retry via
             * the outer's own advance instruction. */
            if (side1acc.cmp2)
            {
                if (column_is_primary_key(cols1, side1acc.cmp2->col.idx))
                    emit(stmt, &pc, Op_Key, 0, r_tmp_a, 0, NULL);
                else
                    emit(stmt, &pc, Op_Column, 0, side1acc.cmp2->col.idx, r_tmp_a, NULL);
                patch_die[npatch_die++] = emit(stmt, &pc, side1acc.cmp2->op, r_lit1b, -1, r_tmp_a, NULL);
            }
        }
    }

    /* Unlike patch_die (routes straight to addr_tail), a side2 two-sided
     * range's upper-bound failure must only end *this outer row's* inner
     * walk, not the whole query -- there may be more outer rows left. It
     * needs the same "skip the retry, land right after the inner advance
     * instruction" treatment addr_pos1 itself gets below (see the comment
     * further down), so it's patched once addr_done_inner is known. */
    int *patch_stop_inner = malloc(sizeof(int) * 1);
    int npatch_stop_inner = 0;

    bool inner_loops = (side2acc.kind != ACC_EQSEEK);
    int addr_pos1 = -1;
    int addr_loop2 = pc;
    if (inner_loops)
    {
        addr_pos1 = (side2acc.kind == ACC_SCAN) ? emit(stmt, &pc, Op_Rewind, 1, -1, 0, NULL)
                                                 : emit(stmt, &pc, side2acc.seek_op, 3, -1, r_lit2, NULL);
        addr_loop2 = pc;
        if (side2acc.kind == ACC_RANGESEEK)
        {
            emit(stmt, &pc, Op_IdxPKey, 3, r_pkey2, 0, NULL);
            patch_die[npatch_die++] = emit(stmt, &pc, Op_Seek, 1, -1, r_pkey2, NULL);
            if (side2acc.cmp2)
            {
                if (column_is_primary_key(cols2, side2acc.cmp2->col.idx))
                    emit(stmt, &pc, Op_Key, 1, r_tmp_a, 0, NULL);
                else
                    emit(stmt, &pc, Op_Column, 1, side2acc.cmp2->col.idx, r_tmp_a, NULL);
                patch_stop_inner[npatch_stop_inner++] = emit(stmt, &pc, side2acc.cmp2->op, r_lit2b, -1, r_tmp_a, NULL);
            }
        }
    }

    for (int p = 0; p < npairs; p++)
    {
        emit_rcol(stmt, &pc, (RCol){ 0, join1[p] }, cols1, cols2, r_tmp_a);
        emit_rcol(stmt, &pc, (RCol){ 1, join2[p] }, cols1, cols2, r_tmp_b);
        patch_inner[npatch_inner++] = emit(stmt, &pc, Op_Ne, r_tmp_a, -1, r_tmp_b, NULL);
    }
    emit_filter_checks(stmt, &pc, side2_cmps, n2_scan, cols1, cols2, r_lit_base + n1_scan, r_tmp_a, patch_inner,
                        &npatch_inner);
    emit_filter_checks(stmt, &pc, top_cmps, nt, cols1, cols2, r_lit_base + n1_scan + n2_scan, r_tmp_a, patch_inner,
                        &npatch_inner);

    for (int k = 0; k < nout; k++)
        emit_rcol(stmt, &pc, out[k], cols1, cols2, r_out0 + k);
    emit(stmt, &pc, Op_ResultRow, r_out0, nout, 0, NULL);

    /* A join-pair mismatch or an ACC_SCAN side2/top filter failing
     * retries: jump straight to the advance instruction to try the next
     * candidate. addr_pos1 failing outright (its side never had any
     * candidate at all: Rewind1 found the table empty, or a range Seek
     * found nothing past/before the boundary) is different -- it must
     * NOT retry via the advance instruction, since that assumes the
     * cursor is already validly positioned somewhere and just needs to
     * move on. Next()/Prev() only degrade to a safe no-op for a never-
     * positioned cursor when the underlying array is empty (n==0, true
     * for Rewind1's failure case); a range Seek can fail on a NON-empty
     * index (there just isn't an entry past/before the boundary), and
     * jumping into Next/Prev from that unpositioned state (pos still -1
     * from open) would incorrectly treat "no next-until-boundary" as if
     * pos+1 were still in bounds and step onto whatever entry happens to
     * sit at index 0 (or n-1) of the *unfiltered* array -- silently
     * resuming as if the range walk were already underway, picking up a
     * row that never should have matched the range at all. Route it to
     * addr_done_inner instead: the address right after the advance
     * instruction, i.e. exactly where control would land anyway once a
     * genuine loop legitimately runs out. */
    int addr_retry_inner, addr_done_inner;
    if (inner_loops)
    {
        opcode_t adv2 = (side2acc.kind == ACC_SCAN) ? Op_Next : (side2acc.forward ? Op_Next : Op_Prev);
        int32_t cur2 = (side2acc.kind == ACC_SCAN) ? 1 : 3;
        addr_retry_inner = emit(stmt, &pc, adv2, cur2, addr_loop2, 0, NULL);
        addr_done_inner = pc;
    }
    else
        addr_retry_inner = addr_done_inner = pc;
    for (int k = 0; k < npatch_inner; k++)
        stmt->ops[patch_inner[k]].p2 = addr_retry_inner;
    if (addr_pos1 >= 0)
        stmt->ops[addr_pos1].p2 = addr_done_inner;
    for (int k = 0; k < npatch_stop_inner; k++)
        stmt->ops[patch_stop_inner[k]].p2 = addr_done_inner;

    /* And the outer side: an ACC_SCAN side1 filter failing skips
     * straight to the outer's advance (the whole inner stage never ran
     * for this row); if side1 doesn't loop at all, everything just
     * falls through to the closing tail. */
    int addr_tail;
    if (outer_loops)
    {
        opcode_t adv1 = (side1acc.kind == ACC_SCAN) ? Op_Next : (side1acc.forward ? Op_Next : Op_Prev);
        int32_t cur1 = (side1acc.kind == ACC_SCAN) ? 0 : 2;
        int addr_advance0 = emit(stmt, &pc, adv1, cur1, addr_loop1, 0, NULL);
        for (int k = 0; k < npatch_outer; k++)
            stmt->ops[patch_outer[k]].p2 = addr_advance0;
        addr_tail = pc;
        stmt->ops[addr_pos0].p2 = addr_tail;
    }
    else
    {
        addr_tail = pc;
    }
    for (int k = 0; k < npatch_die; k++)
        stmt->ops[patch_die[k]].p2 = addr_tail;

    emit(stmt, &pc, Op_Close, 1, 0, 0, NULL);
    if (use_index2)
        emit(stmt, &pc, Op_Close, 3, 0, 0, NULL);
    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);
    if (use_index1)
        emit(stmt, &pc, Op_Close, 2, 0, 0, NULL);
    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);

    stmt->nCols = nout;
    stmt->nRR = nout;
    stmt->cols = malloc(sizeof(char *) * nout);
    for (int k = 0; k < nout; k++)
        stmt->cols[k] = strdup(column_name_at(out[k].side == 0 ? cols1 : cols2, out[k].idx));

    free(patch_die);
    free(patch_outer);
    free(patch_inner);
    free(patch_stop_inner);
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

    /* assignment_opt.html point 3, extended from equality to any
     * comparison (see IndexSeekKind), and further extended to a two-sided
     * bounded range on the same column (see detect_range_pair): a single
     * top-level `indexed-col OP val`, or exactly two conjuncts forming a
     * lower+upper bound on the same indexed column, compiles to an index
     * seek instead of a full scan -- anything else falls through to the
     * scan below unchanged. */
    IndexSeekKind seek_kind;
    ResolvedCmp *primary = NULL, *bound2 = NULL;
    int lo, hi;
    if (ncmp == 1 && index_seek_kind(cmps[0].op, &seek_kind))
        primary = &cmps[0];
    else if (detect_range_pair(cmps, ncmp, &lo, &hi))
    {
        primary = &cmps[lo];
        bound2 = &cmps[hi];
        index_seek_kind(primary->op, &seek_kind);
    }

    if (primary)
    {
        chidb_schema_item_t *idx = chidb_schema_find_index_on(db, table_name, column_name_at(columns, primary->col.idx));
        if (idx)
        {
            int rc = codegen_select_indexed(stmt, tbl, idx, columns, out_idx, nout, primary, seek_kind, bound2);
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
