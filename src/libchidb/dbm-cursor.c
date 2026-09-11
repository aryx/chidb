/*
 *  chidb - a didactic relational database management system
 *
 *  Database Machine cursors
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


#include <stdlib.h>
#include <string.h>

#include "dbm-cursor.h"

/* claude: growable array used only while collecting a B-Tree's in-order
 * entry sequence in collect() below; see dbm-cursor.h for why. */
typedef struct
{
    chidb_key_t *keys;
    uint8_t **data;
    uint16_t *sizes;
    chidb_key_t *pkeys;
    uint32_t n;
    uint32_t cap;
} collector_t;

static void collector_push(collector_t *c, chidb_key_t key, uint8_t *data, uint16_t size, chidb_key_t pkey)
{
    if (c->n == c->cap)
    {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->keys = realloc(c->keys, c->cap * sizeof(chidb_key_t));
        c->data = realloc(c->data, c->cap * sizeof(uint8_t *));
        c->sizes = realloc(c->sizes, c->cap * sizeof(uint16_t));
        c->pkeys = realloc(c->pkeys, c->cap * sizeof(chidb_key_t));
    }
    c->keys[c->n] = key;
    c->data[c->n] = data;
    c->sizes[c->n] = size;
    c->pkeys[c->n] = pkey;
    c->n++;
}

static int collect(BTree *bt, npage_t npage, collector_t *out)
{
    BTreeNode *btn;
    int rc = chidb_Btree_getNodeByPage(bt, npage, &btn);
    if (rc != CHIDB_OK)
        return rc;

    switch (btn->type)
    {
    case PGTYPE_TABLE_LEAF:
        for (ncell_t i = 0; i < btn->n_cells; i++)
        {
            BTreeCell c;
            chidb_Btree_getCell(btn, i, &c);
            uint8_t *copy = malloc(c.fields.tableLeaf.data_size);
            memcpy(copy, c.fields.tableLeaf.data, c.fields.tableLeaf.data_size);
            collector_push(out, c.key, copy, c.fields.tableLeaf.data_size, 0);
        }
        break;

    case PGTYPE_TABLE_INTERNAL:
        for (ncell_t i = 0; i < btn->n_cells; i++)
        {
            BTreeCell c;
            chidb_Btree_getCell(btn, i, &c);
            collect(bt, c.fields.tableInternal.child_page, out);
        }
        collect(bt, btn->right_page, out);
        break;

    case PGTYPE_INDEX_LEAF:
        for (ncell_t i = 0; i < btn->n_cells; i++)
        {
            BTreeCell c;
            chidb_Btree_getCell(btn, i, &c);
            collector_push(out, c.key, NULL, 0, c.fields.indexLeaf.keyPk);
        }
        break;

    case PGTYPE_INDEX_INTERNAL:
        for (ncell_t i = 0; i < btn->n_cells; i++)
        {
            BTreeCell c;
            chidb_Btree_getCell(btn, i, &c);
            collect(bt, c.fields.indexInternal.child_page, out);
            collector_push(out, c.key, NULL, 0, c.fields.indexInternal.keyPk);
        }
        collect(bt, btn->right_page, out);
        break;
    }

    chidb_Btree_freeMemNode(bt, btn);
    return CHIDB_OK;
}

int chidb_Cursor_open(BTree *bt, npage_t root, chidb_dbm_cursor_type_t type, chidb_dbm_cursor_t *cursor)
{
    collector_t out = {0};
    int rc = collect(bt, root, &out);
    if (rc != CHIDB_OK)
        return rc;

    BTreeNode *btn;
    rc = chidb_Btree_getNodeByPage(bt, root, &btn);
    if (rc != CHIDB_OK)
        return rc;
    cursor->is_index = (btn->type == PGTYPE_INDEX_INTERNAL || btn->type == PGTYPE_INDEX_LEAF);
    chidb_Btree_freeMemNode(bt, btn);

    cursor->type = type;
    cursor->bt = bt;
    cursor->root = root;
    cursor->keys = out.keys;
    cursor->data = out.data;
    cursor->sizes = out.sizes;
    cursor->pkeys = out.pkeys;
    cursor->n = out.n;
    cursor->pos = -1;

    return CHIDB_OK;
}

int chidb_Cursor_close(chidb_dbm_cursor_t *cursor)
{
    if (!cursor->is_index)
        for (uint32_t i = 0; i < cursor->n; i++)
            free(cursor->data[i]);
    free(cursor->keys);
    free(cursor->data);
    free(cursor->sizes);
    free(cursor->pkeys);
    cursor->type = CURSOR_UNSPECIFIED;

    return CHIDB_OK;
}

int chidb_Cursor_rewind(chidb_dbm_cursor_t *cursor, bool *empty)
{
    *empty = (cursor->n == 0);
    cursor->pos = *empty ? -1 : 0;
    return CHIDB_OK;
}

int chidb_Cursor_next(chidb_dbm_cursor_t *cursor, bool *moved)
{
    *moved = (cursor->pos + 1 < (int32_t) cursor->n);
    if (*moved)
        cursor->pos++;
    return CHIDB_OK;
}

int chidb_Cursor_prev(chidb_dbm_cursor_t *cursor, bool *moved)
{
    *moved = (cursor->pos - 1 >= 0);
    if (*moved)
        cursor->pos--;
    return CHIDB_OK;
}

int chidb_Cursor_seekEq(chidb_dbm_cursor_t *cursor, chidb_key_t key, bool *found)
{
    for (uint32_t i = 0; i < cursor->n; i++)
        if (cursor->keys[i] == key)
        {
            cursor->pos = i;
            *found = true;
            return CHIDB_OK;
        }
    *found = false;
    return CHIDB_OK;
}

int chidb_Cursor_seekGt(chidb_dbm_cursor_t *cursor, chidb_key_t key, bool *found)
{
    for (uint32_t i = 0; i < cursor->n; i++)
        if (cursor->keys[i] > key)
        {
            cursor->pos = i;
            *found = true;
            return CHIDB_OK;
        }
    *found = false;
    return CHIDB_OK;
}

int chidb_Cursor_seekGe(chidb_dbm_cursor_t *cursor, chidb_key_t key, bool *found)
{
    for (uint32_t i = 0; i < cursor->n; i++)
        if (cursor->keys[i] >= key)
        {
            cursor->pos = i;
            *found = true;
            return CHIDB_OK;
        }
    *found = false;
    return CHIDB_OK;
}

int chidb_Cursor_seekLt(chidb_dbm_cursor_t *cursor, chidb_key_t key, bool *found)
{
    for (int32_t i = (int32_t) cursor->n - 1; i >= 0; i--)
        if (cursor->keys[i] < key)
        {
            cursor->pos = i;
            *found = true;
            return CHIDB_OK;
        }
    *found = false;
    return CHIDB_OK;
}

int chidb_Cursor_seekLe(chidb_dbm_cursor_t *cursor, chidb_key_t key, bool *found)
{
    for (int32_t i = (int32_t) cursor->n - 1; i >= 0; i--)
        if (cursor->keys[i] <= key)
        {
            cursor->pos = i;
            *found = true;
            return CHIDB_OK;
        }
    *found = false;
    return CHIDB_OK;
}

