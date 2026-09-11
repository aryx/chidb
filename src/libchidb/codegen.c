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

/* claude: covers assignment_codegen.html steps 1-4 (schema loading is in
 * util.c/api.c) plus CREATE INDEX + index population from assignment_opt.html.
 * NOT implemented: NATURAL JOIN (step 5) and index-based SELECT lookups
 * (the other half of assignment_opt.html) -- see
 * docs/claude_notes/plan_chidb_implementation.md. Every SELECT here is a
 * single-table scan with at most one `column OP literal` WHERE clause. */

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
 * r(2+ncols)=packed record. */
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
    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);
    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);

    free(vals);
    free(parsed);
    return CHIDB_OK;
}


/* --- SELECT ------------------------------------------------------------ */

/* claude: register layout mirrors testing.html's worked example
 * (`SELECT * FROM courses`): r0=table root, r1..r(nout)=output columns
 * when there's no WHERE clause. With a WHERE clause, r1=the literal
 * (loaded once, before the loop), r2=the row's current value for the
 * WHERE column (reloaded every iteration), and the output columns shift
 * up to r3..r(2+nout) to make room.
 *
 * The WHERE test is compiled as its own *negation*, jumping straight to
 * the Next instruction (skipping this row) when the negation holds --
 * e.g. `col > val` becomes `Le val, addr_next, col` ("if col <= val, skip").
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
    if (table_sra->t != SRA_TABLE)
        return CHIDB_EINVALIDSQL; /* joins: not implemented, see file header */

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

    bool has_where = false;
    int where_col_idx = -1;
    opcode_t where_op = Op_Ne;
    Literal_t *where_lit = NULL;

    if (cond)
    {
        if (cond->t != RA_COND_EQ && cond->t != RA_COND_LT && cond->t != RA_COND_GT &&
            cond->t != RA_COND_LEQ && cond->t != RA_COND_GEQ)
        {
            free(out_idx);
            free(parsed);
            return CHIDB_EINVALIDSQL;
        }

        Expression_t *e1 = cond->cond.comp.expr1, *e2 = cond->cond.comp.expr2;
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
        {
            free(out_idx);
            free(parsed);
            return CHIDB_EINVALIDSQL;
        }

        if (valExpr->t != EXPR_TERM || valExpr->expr.term.t != TERM_LITERAL)
        {
            free(out_idx);
            free(parsed);
            return CHIDB_EINVALIDSQL;
        }

        enum data_type coltype;
        where_col_idx = column_index_by_name(columns, colExpr->expr.term.ref->columnName, &coltype);
        where_lit = valExpr->expr.term.val;
        if (where_col_idx < 0 || !literal_matches_type(where_lit, coltype))
        {
            free(out_idx);
            free(parsed);
            return CHIDB_EINVALIDSQL;
        }

        enum CondType ct = cond->t;
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
        case RA_COND_EQ:  where_op = Op_Ne; break;
        case RA_COND_GT:  where_op = Op_Le; break;
        case RA_COND_GEQ: where_op = Op_Lt; break;
        case RA_COND_LT:  where_op = Op_Ge; break;
        case RA_COND_LEQ: where_op = Op_Gt; break;
        default: break;
        }
        has_where = true;
    }

    bool where_col_is_pk = has_where && column_is_primary_key(columns, where_col_idx);

    int r_root = 0;
    int r_lit = 1;
    int r_col = 2;
    int r_out0 = has_where ? 3 : 1;

    int pc = 0;
    emit(stmt, &pc, Op_Integer, (int32_t) tbl->root_page, r_root, 0, NULL);
    emit(stmt, &pc, Op_OpenRead, 0, r_root, ncols, NULL);

    if (has_where)
        emit_literal(stmt, &pc, where_lit, r_lit);

    int addr_rewind = emit(stmt, &pc, Op_Rewind, 0, -1, 0, NULL);
    int addr_loop = pc;

    int addr_skip = -1;
    if (has_where)
    {
        if (where_col_is_pk)
            emit(stmt, &pc, Op_Key, 0, r_col, 0, NULL);
        else
            emit(stmt, &pc, Op_Column, 0, where_col_idx, r_col, NULL);
        addr_skip = emit(stmt, &pc, where_op, r_lit, -1, r_col, NULL);
    }

    for (int i = 0; i < nout; i++)
    {
        if (column_is_primary_key(columns, out_idx[i]))
            emit(stmt, &pc, Op_Key, 0, r_out0 + i, 0, NULL);
        else
            emit(stmt, &pc, Op_Column, 0, out_idx[i], r_out0 + i, NULL);
    }
    emit(stmt, &pc, Op_ResultRow, r_out0, nout, 0, NULL);

    int addr_next = emit(stmt, &pc, Op_Next, 0, addr_loop, 0, NULL);
    if (has_where)
        stmt->ops[addr_skip].p2 = addr_next;

    int addr_after = pc;
    stmt->ops[addr_rewind].p2 = addr_after;

    emit(stmt, &pc, Op_Close, 0, 0, 0, NULL);
    emit(stmt, &pc, Op_Halt, 0, 0, 0, NULL);

    stmt->nCols = nout;
    /* claude: chidb_stmt_exec() asserts nRR==nCols after every run, including
     * a run that ends at Halt without ever executing ResultRow (a query
     * that legitimately matches zero rows) -- pre-set nRR here so that
     * case holds trivially; a real ResultRow later sets it to the same
     * value anyway, since its P2 is always this same `nout`. */
    stmt->nRR = nout;
    stmt->cols = malloc(sizeof(char *) * nout);
    for (int j = 0; j < nout; j++)
    {
        int i = 0;
        for (Column_t *c = columns; c; c = c->next, i++)
            if (i == out_idx[j])
                stmt->cols[j] = strdup(c->name);
    }

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
