/*
 *  chidb - a didactic relational database management system
 *
 *  Database Machine operations.
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
#include <stdlib.h>

#include "dbm.h"
#include "btree.h"
#include "record.h"


/* Function pointer for dispatch table */
typedef int (*handler_function)(chidb_stmt *stmt, chidb_dbm_op_t *op);

/* Single entry in the instruction dispatch table */
struct handler_entry
{
    opcode_t opcode;
    handler_function func;
};

/* This generates all the instruction handler prototypes. It expands to:
 *
 * int chidb_dbm_op_OpenRead(chidb_stmt *stmt, chidb_dbm_op_t *op);
 * int chidb_dbm_op_OpenWrite(chidb_stmt *stmt, chidb_dbm_op_t *op);
 * ...
 * int chidb_dbm_op_Halt(chidb_stmt *stmt, chidb_dbm_op_t *op);
 */
#define HANDLER_PROTOTYPE(OP) int chidb_dbm_op_## OP (chidb_stmt *stmt, chidb_dbm_op_t *op);
FOREACH_OP(HANDLER_PROTOTYPE)


/* Ladies and gentlemen, the dispatch table. */
#define HANDLER_ENTRY(OP) { Op_ ## OP, chidb_dbm_op_## OP},

struct handler_entry dbm_handlers[] =
{
    FOREACH_OP(HANDLER_ENTRY)
};

int chidb_dbm_op_handle (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    return dbm_handlers[op->opcode].func(stmt, op);
}


/*** claude: small helpers shared by the handlers below ***/

/* Grow the register/cursor arrays on demand, since a DBM program can
 * address any register/cursor number without a prior "declare" step. */
static int ensure_reg(chidb_stmt *stmt, uint32_t r)
{
    if (r >= stmt->nReg)
        return realloc_reg(stmt, r + 1);
    return CHIDB_OK;
}

static int ensure_cur(chidb_stmt *stmt, uint32_t c)
{
    if (c >= stmt->nCursors)
        return realloc_cur(stmt, c + 1);
    return CHIDB_OK;
}

/* <0 if a<b, 0 if equal, >0 if a>b. Only REG_INT32/REG_STRING need to be
 * supported (architecture.html: "comparison of binary registers is not
 * necessary"). */
static int reg_compare(chidb_dbm_register_t *a, chidb_dbm_register_t *b)
{
    if (a->type == REG_INT32)
        return (a->value.i > b->value.i) - (a->value.i < b->value.i);
    return strcmp(a->value.s, b->value.s);
}


/*** INSTRUCTION HANDLER IMPLEMENTATIONS ***/


int chidb_dbm_op_Noop (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    return CHIDB_OK;
}


int chidb_dbm_op_OpenRead (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int rc = ensure_cur(stmt, op->p1);
    if (rc != CHIDB_OK)
        return rc;

    npage_t root = (npage_t) stmt->reg[op->p2].value.i;
    return chidb_Cursor_open(stmt->db->bt, root, CURSOR_READ, &stmt->cursors[op->p1]);
}


int chidb_dbm_op_OpenWrite (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int rc = ensure_cur(stmt, op->p1);
    if (rc != CHIDB_OK)
        return rc;

    npage_t root = (npage_t) stmt->reg[op->p2].value.i;
    return chidb_Cursor_open(stmt->db->bt, root, CURSOR_WRITE, &stmt->cursors[op->p1]);
}


int chidb_dbm_op_Close (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    return chidb_Cursor_close(&stmt->cursors[op->p1]);
}


int chidb_dbm_op_Rewind (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    bool empty;
    int rc = chidb_Cursor_rewind(&stmt->cursors[op->p1], &empty);
    if (rc != CHIDB_OK)
        return rc;
    if (empty)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_Next (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    bool moved;
    int rc = chidb_Cursor_next(&stmt->cursors[op->p1], &moved);
    if (rc != CHIDB_OK)
        return rc;
    if (moved)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_Prev (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    bool moved;
    int rc = chidb_Cursor_prev(&stmt->cursors[op->p1], &moved);
    if (rc != CHIDB_OK)
        return rc;
    if (moved)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_Seek (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    bool found;
    chidb_key_t k = (chidb_key_t) stmt->reg[op->p3].value.i;
    int rc = chidb_Cursor_seekEq(&stmt->cursors[op->p1], k, &found);
    if (rc != CHIDB_OK)
        return rc;
    if (!found)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_SeekGt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    bool found;
    chidb_key_t k = (chidb_key_t) stmt->reg[op->p3].value.i;
    int rc = chidb_Cursor_seekGt(&stmt->cursors[op->p1], k, &found);
    if (rc != CHIDB_OK)
        return rc;
    if (!found)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_SeekGe (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    bool found;
    chidb_key_t k = (chidb_key_t) stmt->reg[op->p3].value.i;
    int rc = chidb_Cursor_seekGe(&stmt->cursors[op->p1], k, &found);
    if (rc != CHIDB_OK)
        return rc;
    if (!found)
        stmt->pc = op->p2;
    return CHIDB_OK;
}

int chidb_dbm_op_SeekLt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    bool found;
    chidb_key_t k = (chidb_key_t) stmt->reg[op->p3].value.i;
    int rc = chidb_Cursor_seekLt(&stmt->cursors[op->p1], k, &found);
    if (rc != CHIDB_OK)
        return rc;
    if (!found)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_SeekLe (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    bool found;
    chidb_key_t k = (chidb_key_t) stmt->reg[op->p3].value.i;
    int rc = chidb_Cursor_seekLe(&stmt->cursors[op->p1], k, &found);
    if (rc != CHIDB_OK)
        return rc;
    if (!found)
        stmt->pc = op->p2;
    return CHIDB_OK;
}

int chidb_dbm_op_Column (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    chidb_dbm_cursor_t *cur = &stmt->cursors[op->p1];
    DBRecord *dbr;
    int rc = chidb_DBRecord_unpack(&dbr, cur->data[cur->pos]);
    if (rc != CHIDB_OK)
        return rc;

    rc = ensure_reg(stmt, op->p3);
    if (rc != CHIDB_OK)
    {
        chidb_DBRecord_destroy(dbr);
        return rc;
    }

    chidb_dbm_register_t *r = &stmt->reg[op->p3];
    int type = chidb_DBRecord_getType(dbr, op->p2);

    switch (type)
    {
    case SQL_NULL:
        r->type = REG_NULL;
        break;
    case SQL_INTEGER_1BYTE:
    {
        int8_t v;
        chidb_DBRecord_getInt8(dbr, op->p2, &v);
        r->type = REG_INT32;
        r->value.i = v;
        break;
    }
    case SQL_INTEGER_2BYTE:
    {
        int16_t v;
        chidb_DBRecord_getInt16(dbr, op->p2, &v);
        r->type = REG_INT32;
        r->value.i = v;
        break;
    }
    case SQL_INTEGER_4BYTE:
    {
        int32_t v;
        chidb_DBRecord_getInt32(dbr, op->p2, &v);
        r->type = REG_INT32;
        r->value.i = v;
        break;
    }
    case SQL_TEXT:
    {
        char *s;
        chidb_DBRecord_getString(dbr, op->p2, &s);
        r->type = REG_STRING;
        r->value.s = s;
        break;
    }
    }

    chidb_DBRecord_destroy(dbr);
    return CHIDB_OK;
}


int chidb_dbm_op_Key (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    chidb_dbm_cursor_t *cur = &stmt->cursors[op->p1];
    int rc = ensure_reg(stmt, op->p2);
    if (rc != CHIDB_OK)
        return rc;

    stmt->reg[op->p2].type = REG_INT32;
    stmt->reg[op->p2].value.i = cur->keys[cur->pos];
    return CHIDB_OK;
}


int chidb_dbm_op_Integer (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int rc = ensure_reg(stmt, op->p2);
    if (rc != CHIDB_OK)
        return rc;

    stmt->reg[op->p2].type = REG_INT32;
    stmt->reg[op->p2].value.i = op->p1;
    return CHIDB_OK;
}


int chidb_dbm_op_String (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int rc = ensure_reg(stmt, op->p2);
    if (rc != CHIDB_OK)
        return rc;

    stmt->reg[op->p2].type = REG_STRING;
    stmt->reg[op->p2].value.s = strdup(op->p4);
    return CHIDB_OK;
}


int chidb_dbm_op_Null (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int rc = ensure_reg(stmt, op->p2);
    if (rc != CHIDB_OK)
        return rc;

    stmt->reg[op->p2].type = REG_NULL;
    return CHIDB_OK;
}


int chidb_dbm_op_ResultRow (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    stmt->startRR = op->p1;
    stmt->nRR = op->p2;

    /* claude: chidb_stmt_exec only ever forwards CHIDB_ROW up to the
     * caller if an opcode handler itself returns CHIDB_ROW (any other
     * value is either propagated as an error, or -- for CHIDB_OK/DONE --
     * folded into CHIDB_DONE at the end of the fetch loop), so this is
     * the one place that has to return it. */
    return CHIDB_ROW;
}


int chidb_dbm_op_MakeRecord (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    DBRecordBuffer dbrb;
    chidb_DBRecord_create_empty(&dbrb, (uint8_t) op->p2);

    for (int32_t i = 0; i < op->p2; i++)
    {
        chidb_dbm_register_t *r = &stmt->reg[op->p1 + i];
        switch (r->type)
        {
        case REG_NULL:
            chidb_DBRecord_appendNull(&dbrb);
            break;
        case REG_INT32:
            chidb_DBRecord_appendInt32(&dbrb, r->value.i);
            break;
        case REG_STRING:
            chidb_DBRecord_appendString(&dbrb, r->value.s);
            break;
        default:
            break;
        }
    }

    DBRecord *dbr;
    chidb_DBRecord_finalize(&dbrb, &dbr);

    uint8_t *packed;
    chidb_DBRecord_pack(dbr, &packed);

    int rc = ensure_reg(stmt, op->p3);
    if (rc != CHIDB_OK)
    {
        free(packed);
        chidb_DBRecord_destroy(dbr);
        return rc;
    }

    stmt->reg[op->p3].type = REG_BINARY;
    stmt->reg[op->p3].value.bin.bytes = packed;
    stmt->reg[op->p3].value.bin.nbytes = dbr->packed_len;

    chidb_DBRecord_destroy(dbr);
    return CHIDB_OK;
}


int chidb_dbm_op_Insert (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    chidb_dbm_cursor_t *cur = &stmt->cursors[op->p1];
    chidb_dbm_register_t *rec = &stmt->reg[op->p2];
    chidb_dbm_register_t *key = &stmt->reg[op->p3];

    int rc = chidb_Btree_insertInTable(cur->bt, cur->root, (chidb_key_t) key->value.i,
                                        rec->value.bin.bytes, (uint16_t) rec->value.bin.nbytes);
    /* claude: CHIDB_EDUPLICATE is a private code (chidbInt.h) that must not
     * leak out as a chidb_step() return value -- translate to the public,
     * documented CHIDB_ECONSTRAINT here, at the DBM/API boundary. */
    return rc == CHIDB_EDUPLICATE ? CHIDB_ECONSTRAINT : rc;
}


int chidb_dbm_op_Eq (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    if (reg_compare(&stmt->reg[op->p1], &stmt->reg[op->p3]) == 0)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_Ne (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    if (reg_compare(&stmt->reg[op->p1], &stmt->reg[op->p3]) != 0)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_Lt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* jump if value in r2 (p3) is less than value in r1 (p1) */
    if (reg_compare(&stmt->reg[op->p3], &stmt->reg[op->p1]) < 0)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_Le (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    if (reg_compare(&stmt->reg[op->p3], &stmt->reg[op->p1]) <= 0)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_Gt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    if (reg_compare(&stmt->reg[op->p3], &stmt->reg[op->p1]) > 0)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


int chidb_dbm_op_Ge (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    if (reg_compare(&stmt->reg[op->p3], &stmt->reg[op->p1]) >= 0)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


/* IdxGt p1 p2 p3 *
 *
 * p1: cursor
 * p2: jump addr
 * p3: register containing value k
 *
 * if (idxkey at cursor p1) > k, jump
 */
int chidb_dbm_op_IdxGt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    chidb_dbm_cursor_t *cur = &stmt->cursors[op->p1];
    chidb_key_t k = (chidb_key_t) stmt->reg[op->p3].value.i;
    if (cur->keys[cur->pos] > k)
        stmt->pc = op->p2;
    return CHIDB_OK;
}

/* IdxGe p1 p2 p3 *
 *
 * p1: cursor
 * p2: jump addr
 * p3: register containing value k
 *
 * if (idxkey at cursor p1) >= k, jump
 */
int chidb_dbm_op_IdxGe (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    chidb_dbm_cursor_t *cur = &stmt->cursors[op->p1];
    chidb_key_t k = (chidb_key_t) stmt->reg[op->p3].value.i;
    if (cur->keys[cur->pos] >= k)
        stmt->pc = op->p2;
    return CHIDB_OK;
}

/* IdxLt p1 p2 p3 *
 *
 * p1: cursor
 * p2: jump addr
 * p3: register containing value k
 *
 * if (idxkey at cursor p1) < k, jump
 */
int chidb_dbm_op_IdxLt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    chidb_dbm_cursor_t *cur = &stmt->cursors[op->p1];
    chidb_key_t k = (chidb_key_t) stmt->reg[op->p3].value.i;
    if (cur->keys[cur->pos] < k)
        stmt->pc = op->p2;
    return CHIDB_OK;
}

/* IdxLe p1 p2 p3 *
 *
 * p1: cursor
 * p2: jump addr
 * p3: register containing value k
 *
 * if (idxkey at cursor p1) <= k, jump
 */
int chidb_dbm_op_IdxLe (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    chidb_dbm_cursor_t *cur = &stmt->cursors[op->p1];
    chidb_key_t k = (chidb_key_t) stmt->reg[op->p3].value.i;
    if (cur->keys[cur->pos] <= k)
        stmt->pc = op->p2;
    return CHIDB_OK;
}


/* IdxPKey p1 p2 * *
 *
 * p1: cursor
 * p2: register
 *
 * store pkey from (cell at cursor p1) in (register at p2)
 */
int chidb_dbm_op_IdxPKey (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    chidb_dbm_cursor_t *cur = &stmt->cursors[op->p1];
    int rc = ensure_reg(stmt, op->p2);
    if (rc != CHIDB_OK)
        return rc;

    stmt->reg[op->p2].type = REG_INT32;
    stmt->reg[op->p2].value.i = cur->pkeys[cur->pos];
    return CHIDB_OK;
}

/* IdxInsert p1 p2 p3 *
 *
 * p1: cursor
 * p2: register containing IdxKey
 * p3: register containing PKey
 *
 * add new (IdkKey,PKey) entry in index BTree pointed at by cursor at p1
 */
int chidb_dbm_op_IdxInsert (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    chidb_dbm_cursor_t *cur = &stmt->cursors[op->p1];
    chidb_key_t idxKey = (chidb_key_t) stmt->reg[op->p2].value.i;
    chidb_key_t pKey = (chidb_key_t) stmt->reg[op->p3].value.i;

    int rc = chidb_Btree_insertInIndex(cur->bt, cur->root, idxKey, pKey);
    /* claude: see chidb_dbm_op_Insert above -- same private->public translation. */
    return rc == CHIDB_EDUPLICATE ? CHIDB_ECONSTRAINT : rc;
}


int chidb_dbm_op_CreateTable (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    npage_t root;
    int rc = chidb_Btree_newNode(stmt->db->bt, &root, PGTYPE_TABLE_LEAF);
    if (rc != CHIDB_OK)
        return rc;

    rc = ensure_reg(stmt, op->p1);
    if (rc != CHIDB_OK)
        return rc;

    stmt->reg[op->p1].type = REG_INT32;
    stmt->reg[op->p1].value.i = root;
    return CHIDB_OK;
}


int chidb_dbm_op_CreateIndex (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    npage_t root;
    int rc = chidb_Btree_newNode(stmt->db->bt, &root, PGTYPE_INDEX_LEAF);
    if (rc != CHIDB_OK)
        return rc;

    rc = ensure_reg(stmt, op->p1);
    if (rc != CHIDB_OK)
        return rc;

    stmt->reg[op->p1].type = REG_INT32;
    stmt->reg[op->p1].value.i = root;
    return CHIDB_OK;
}


/* claude: not documented in architecture.html (only SCopy is), but
 * present in the opcode enum -- implemented with the obvious SQLite-style
 * meaning: a deep copy (own string storage), as opposed to SCopy's
 * shallow/shared-pointer copy. Unused by any codegen output emitted by
 * this implementation; kept correct in case a test exercises it directly. */
int chidb_dbm_op_Copy (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int rc = ensure_reg(stmt, op->p1);
    if (rc != CHIDB_OK)
        return rc;
    rc = ensure_reg(stmt, op->p2);
    if (rc != CHIDB_OK)
        return rc;

    chidb_dbm_register_t *src = &stmt->reg[op->p1];
    chidb_dbm_register_t *dst = &stmt->reg[op->p2];

    dst->type = src->type;
    switch (src->type)
    {
    case REG_INT32:
        dst->value.i = src->value.i;
        break;
    case REG_STRING:
        dst->value.s = strdup(src->value.s);
        break;
    case REG_BINARY:
        dst->value.bin.nbytes = src->value.bin.nbytes;
        dst->value.bin.bytes = malloc(src->value.bin.nbytes);
        memcpy(dst->value.bin.bytes, src->value.bin.bytes, src->value.bin.nbytes);
        break;
    default:
        break;
    }

    return CHIDB_OK;
}


int chidb_dbm_op_SCopy (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int rc = ensure_reg(stmt, op->p1);
    if (rc != CHIDB_OK)
        return rc;
    rc = ensure_reg(stmt, op->p2);
    if (rc != CHIDB_OK)
        return rc;

    stmt->reg[op->p2] = stmt->reg[op->p1];
    return CHIDB_OK;
}


int chidb_dbm_op_Halt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* claude: no error-code/message plumbing needed per assignment_dbm.html
     * ("You are not required to support error codes or messages in Halt").
     * Advancing pc past endOp is exactly what the fetch loop in
     * chidb_stmt_exec() treats as a clean stop. */
    stmt->pc = stmt->endOp;
    return CHIDB_OK;
}
