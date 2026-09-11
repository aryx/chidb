/*
 *  chidb - a didactic relational database management system
 *
 *  Query Optimizer
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

#include <strings.h>

#include <chidb/chidb.h>
#include "dbm-types.h"
#include "util.h"

/* claude: assignment_opt.html "Pushing Sigmas" -- rewrites
 *
 *   Project(expr_list, Select(cond, NaturalJoin(Table(t1), Table(t2))))
 *
 * into
 *
 *   Project(expr_list, [Select(top_cond,)] NaturalJoin(
 *       [Select(cond1,)] Table(t1),
 *       [Select(cond2,)] Table(t2)
 *   ))
 *
 * by splitting cond's top-level AND-chain into three buckets: conjuncts
 * that only reference t1's columns (pushed to the left side), only t2's
 * (pushed to the right), and everything else -- a cross-table condition,
 * something not written as a plain `column OP literal` comparison, or a
 * column reference this pass can't otherwise resolve -- left at the top,
 * exactly where it already was. Leaving a conjunct at the top is always
 * safe; the classification below only ever moves a conjunct when it's
 * certain doing so doesn't change what the query means.
 *
 * Only ever touches this one exact shape. Every other statement --
 * anything except a SELECT, a SELECT without this Select-over-NaturalJoin
 * shape (including one with no WHERE at all, or a single-table query),
 * a NaturalJoin whose sides aren't both bare tables -- goes through
 * unchanged as the same shallow copy this function always produced
 * before this pass. codegen.c's codegen_select_join() is the other half
 * of this: it has to accept the pushed shape (a NaturalJoin whose sides
 * are optionally wrapped in their own Select) since this optimizer runs
 * in front of *every* chidb_prepare(), not just behind the shell's
 * `.opt` command. */

/* Returns 0 if `ref` can only refer to table1, 1 if only to table2, or -1
 * if it's ambiguous (an unqualified name shared by both tables -- a
 * natural-join column, so pushing it to either side alone would be
 * wrong) or unresolvable (a qualifier matching neither table, or an
 * unqualified name found in neither). -1 always means "leave it at the
 * top", which is always a safe fallback. */
static int classify_column_side(ColumnReference_t *ref,
                                 const char *name1, const char *alias1, Column_t *cols1,
                                 const char *name2, const char *alias2, Column_t *cols2)
{
    if (ref->tableName)
    {
        if (strcasecmp(ref->tableName, name1) == 0 || (alias1 && strcasecmp(ref->tableName, alias1) == 0))
            return 0;
        if (strcasecmp(ref->tableName, name2) == 0 || (alias2 && strcasecmp(ref->tableName, alias2) == 0))
            return 1;
        return -1;
    }

    bool in1 = false, in2 = false;
    for (Column_t *c = cols1; c; c = c->next)
        if (strcasecmp(c->name, ref->columnName) == 0) { in1 = true; break; }
    for (Column_t *c = cols2; c; c = c->next)
        if (strcasecmp(c->name, ref->columnName) == 0) { in2 = true; break; }

    if (in1 && !in2)
        return 0;
    if (in2 && !in1)
        return 1;
    return -1;
}

static int classify_leaf(Condition_t *leaf,
                          const char *name1, const char *alias1, Column_t *cols1,
                          const char *name2, const char *alias2, Column_t *cols2)
{
    if (leaf->t != RA_COND_EQ && leaf->t != RA_COND_LT && leaf->t != RA_COND_GT &&
        leaf->t != RA_COND_LEQ && leaf->t != RA_COND_GEQ)
        return -1;

    Expression_t *e1 = leaf->cond.comp.expr1, *e2 = leaf->cond.comp.expr2;
    ColumnReference_t *ref;
    if (e1->t == EXPR_TERM && e1->expr.term.t == TERM_COLREF && e2->t == EXPR_TERM && e2->expr.term.t == TERM_LITERAL)
        ref = e1->expr.term.ref;
    else if (e2->t == EXPR_TERM && e2->expr.term.t == TERM_COLREF && e1->t == EXPR_TERM &&
             e1->expr.term.t == TERM_LITERAL)
        ref = e2->expr.term.ref;
    else
        return -1; /* not a plain `column OP literal` comparison (e.g. col op col) */

    return classify_column_side(ref, name1, alias1, cols1, name2, alias2, cols2);
}

/* Flattens cond's top-level AND-chain into leaf Condition_t*'s (a `cond`
 * that isn't itself an AND is a length-1 chain of just itself). */
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

static Condition_t *fold_and(Condition_t **list, int n)
{
    Condition_t *result = list[0];
    for (int i = 1; i < n; i++)
        result = And(result, list[i]);
    return result;
}

int chidb_stmt_optimize(chidb *db, chisql_statement_t *sql_stmt, chisql_statement_t **sql_stmt_opt)
{
    *sql_stmt_opt = malloc(sizeof(chisql_statement_t));
    memcpy(*sql_stmt_opt, sql_stmt, sizeof(chisql_statement_t));

    if (sql_stmt->type != STMT_SELECT)
        return CHIDB_OK;

    SRA_t *proj = sql_stmt->stmt.select;
    if (proj->t != SRA_PROJECT || proj->project.sra->t != SRA_SELECT)
        return CHIDB_OK;

    SRA_t *sel = proj->project.sra;
    SRA_t *join = sel->select.sra;
    if (join->t != SRA_NATURAL_JOIN)
        return CHIDB_OK;

    SRA_t *t1 = join->binary.sra1, *t2 = join->binary.sra2;
    if (t1->t != SRA_TABLE || t2->t != SRA_TABLE)
        return CHIDB_OK; /* only the exact two-base-table shape */

    const char *name1 = t1->table.ref->table_name, *alias1 = t1->table.ref->alias;
    const char *name2 = t2->table.ref->table_name, *alias2 = t2->table.ref->alias;

    chidb_schema_item_t *tbl1 = chidb_schema_find_table(db, name1);
    chidb_schema_item_t *tbl2 = chidb_schema_find_table(db, name2);
    if (!tbl1 || !tbl2)
        return CHIDB_OK; /* unknown table: leave it for codegen to reject */

    chisql_statement_t *parsed1, *parsed2;
    chisql_parser(tbl1->sql, &parsed1);
    chisql_parser(tbl2->sql, &parsed2);
    Column_t *cols1 = parsed1->stmt.create->table->columns;
    Column_t *cols2 = parsed2->stmt.create->table->columns;

    Condition_t **list = NULL;
    int n = 0, cap = 0;
    flatten_conjuncts(sel->select.cond, &list, &n, &cap);

    Condition_t **side1 = malloc(sizeof(Condition_t *) * n);
    Condition_t **side2 = malloc(sizeof(Condition_t *) * n);
    Condition_t **top = malloc(sizeof(Condition_t *) * n);
    int n1 = 0, n2 = 0, nt = 0;

    for (int i = 0; i < n; i++)
    {
        int side = classify_leaf(list[i], name1, alias1, cols1, name2, alias2, cols2);
        if (side == 0)
            side1[n1++] = list[i];
        else if (side == 1)
            side2[n2++] = list[i];
        else
            top[nt++] = list[i];
    }

    free(list);
    free(parsed1);
    free(parsed2);

    if (n1 == 0 && n2 == 0)
    {
        /* Nothing pushable: leave the trivial copy from the top of this
         * function untouched. */
        free(side1);
        free(side2);
        free(top);
        return CHIDB_OK;
    }

    SRA_t *left = n1 > 0 ? SRASelect(t1, fold_and(side1, n1)) : t1;
    SRA_t *right = n2 > 0 ? SRASelect(t2, fold_and(side2, n2)) : t2;
    SRA_t *newjoin = SRANaturalJoin(left, right);
    SRA_t *newtop = nt > 0 ? SRASelect(newjoin, fold_and(top, nt)) : newjoin;
    SRA_t *newproj = SRAProject(newtop, proj->project.expr_list);
    newproj->project.distinct = proj->project.distinct;
    newproj->project.order_by = proj->project.order_by;
    newproj->project.asc_desc = proj->project.asc_desc;
    newproj->project.group_by = proj->project.group_by;

    (*sql_stmt_opt)->stmt.select = newproj;

    free(side1);
    free(side2);
    free(top);
    return CHIDB_OK;
}
