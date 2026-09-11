/*
 *  chidb - a didactic relational database management system
 *
 * This module contains functions to manipulate a B-Tree file. In this context,
 * "BTree" refers not to a single B-Tree but to a "file of B-Trees" ("chidb
 * file" and "file of B-Trees" are essentially equivalent terms).
 *
 * However, this module does *not* read or write to the database file directly.
 * All read/write operations must be done through the pager module.
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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <chidb/log.h>
#include "chidbInt.h"
#include "btree.h"
#include "record.h"
#include "pager.h"
#include "util.h"

/* claude: byte offset, within the raw page buffer, of the B-Tree node
 * header. Page 1 reserves its first 100 bytes for the file header, so
 * the node itself starts right after that; every other page's node
 * starts at byte 0. */
static inline uint16_t node_header_base(npage_t npage)
{
    return npage == 1 ? 100 : 0;
}

static inline bool is_internal(uint8_t type)
{
    return type == PGTYPE_TABLE_INTERNAL || type == PGTYPE_INDEX_INTERNAL;
}

static inline bool is_index(uint8_t type)
{
    return type == PGTYPE_INDEX_INTERNAL || type == PGTYPE_INDEX_LEAF;
}

/* claude: node header is 8 bytes (type,free,ncells,cellsoffset,zero) for
 * leaf pages, or 12 bytes (+ 4-byte RightPage) for internal pages. */
static inline uint16_t node_header_size(uint8_t type)
{
    return is_internal(type) ? INTPG_CELLSOFFSET_OFFSET : LEAFPG_CELLSOFFSET_OFFSET;
}

/* claude: on-disk size a cell of the given node type would occupy. For
 * table leaf cells this depends on the (variable-length) record size
 * carried in the cell itself. */
static uint16_t cell_size(uint8_t type, BTreeCell *cell)
{
    switch (type)
    {
    case PGTYPE_TABLE_INTERNAL:
        return TABLEINTCELL_SIZE;
    case PGTYPE_TABLE_LEAF:
        return TABLELEAFCELL_SIZE_WITHOUTDATA + cell->fields.tableLeaf.data_size;
    case PGTYPE_INDEX_INTERNAL:
        return INDEXINTCELL_SIZE;
    case PGTYPE_INDEX_LEAF:
        return INDEXLEAFCELL_SIZE;
    }
    return 0;
}

static inline uint16_t node_free_space(BTreeNode *btn)
{
    return btn->cells_offset - btn->free_offset;
}

/* claude: first cell index i (0..n_cells) such that cell[i].key >= key.
 * For a leaf node this is the insertion point; for an internal node,
 * i == n_cells means "descend into right_page", otherwise descend into
 * cell[i]'s child. */
static int find_position(BTreeNode *btn, chidb_key_t key, ncell_t *pos)
{
    BTreeCell c;
    ncell_t i;

    for (i = 0; i < btn->n_cells; i++)
    {
        int rc = chidb_Btree_getCell(btn, i, &c);
        if (rc != CHIDB_OK)
            return rc;
        if (c.key >= key)
            break;
    }
    *pos = i;
    return CHIDB_OK;
}

/* claude: page number of the child covered by cell[i] (i < n_cells) or
 * right_page (i == n_cells), for an internal node of either flavor. */
static npage_t child_at(BTreeNode *btn, BTreeCell *c, ncell_t i)
{
    if (i == btn->n_cells)
        return btn->right_page;
    return is_index(btn->type) ? c->fields.indexInternal.child_page
                                : c->fields.tableInternal.child_page;
}


/* Open a B-Tree file
 *
 * This function opens a database file and verifies that the file
 * header is correct. If the file is empty (which will happen
 * if the pager is given a filename for a file that does not exist)
 * then this function will (1) initialize the file header using
 * the default page size and (2) create an empty table leaf node
 * in page 1.
 *
 * Parameters
 * - filename: Database file (might not exist)
 * - db: A chidb struct. Its bt field must be set to the newly
 *       created BTree.
 * - bt: An out parameter. Used to return a pointer to the
 *       newly created BTree.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ECORRUPTHEADER: Database file contains an invalid header
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
/* claude: exact byte values a well-formed header must carry, reverse
 * engineered from tests/check_btree_1b.c's test_1b_2 (which asserts these
 * exact bytes on a freshly-created file) combined with the "must be
 * initialized to X" fields called out in fileformat.html. Any file whose
 * header doesn't match this template on open is corrupt. */
static bool header_is_valid(uint8_t *h, uint16_t *pagesize)
{
    if (memcmp(h, "SQLite format 3", 16) != 0)
        return false;
    if (h[18] != 1 || h[19] != 1 || h[20] != 0 || h[21] != 64 || h[22] != 32 || h[23] != 32)
        return false;
    if (get4byte(&h[24]) != 0 || get4byte(&h[32]) != 0 || get4byte(&h[36]) != 0)
        return false;
    if (get4byte(&h[40]) != 0 || get4byte(&h[44]) != 1)
        return false;
    if (get4byte(&h[48]) != 20000 || get4byte(&h[52]) != 0)
        return false;
    if (get4byte(&h[56]) != 1 || get4byte(&h[60]) != 0 || get4byte(&h[64]) != 0)
        return false;

    *pagesize = get2byte(&h[16]);
    return true;
}

int chidb_Btree_open(const char *filename, chidb *db, BTree **bt)
{
    Pager *pager;
    int rc = chidb_Pager_open(&pager, filename);
    if (rc != CHIDB_OK)
        return rc;

    *bt = malloc(sizeof(BTree));
    if (*bt == NULL)
        return CHIDB_ENOMEM;
    (*bt)->db = db;
    (*bt)->pager = pager;

    uint8_t header[100];
    rc = chidb_Pager_readHeader(pager, header);
    if (rc == CHIDB_NOHEADER)
    {
        /* Empty/new file: build the header and an empty table-leaf root
         * in page 1 from scratch. */
        chidb_Pager_setPageSize(pager, DEFAULT_PAGE_SIZE);

        npage_t npage1;
        chidb_Pager_allocatePage(pager, &npage1);

        MemPage *page1;
        rc = chidb_Pager_readPage(pager, 1, &page1);
        if (rc != CHIDB_OK)
            return rc;

        memset(page1->data, 0, DEFAULT_PAGE_SIZE);
        memcpy(page1->data, "SQLite format 3", 16);
        put2byte(&page1->data[16], DEFAULT_PAGE_SIZE);
        page1->data[18] = 1;
        page1->data[19] = 1;
        page1->data[21] = 64;
        page1->data[22] = 32;
        page1->data[23] = 32;
        put4byte(&page1->data[44], 1);
        put4byte(&page1->data[48], 20000);
        put4byte(&page1->data[56], 1);

        rc = chidb_Pager_writePage(pager, page1);
        chidb_Pager_releaseMemPage(pager, page1);
        if (rc != CHIDB_OK)
            return rc;

        rc = chidb_Btree_initEmptyNode(*bt, 1, PGTYPE_TABLE_LEAF);
        if (rc != CHIDB_OK)
            return rc;
    }
    else if (rc != CHIDB_OK)
    {
        return rc;
    }
    else
    {
        uint16_t pagesize;
        if (!header_is_valid(header, &pagesize))
            return CHIDB_ECORRUPTHEADER;
        chidb_Pager_setPageSize(pager, pagesize);
    }

    return CHIDB_OK;
}


/* Close a B-Tree file
 *
 * This function closes a database file, freeing any resource
 * used in memory, such as the pager.
 *
 * Parameters
 * - bt: B-Tree file to close
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_close(BTree *bt)
{
    int rc = chidb_Pager_close(bt->pager);
    free(bt);
    return rc;
}


/* Loads a B-Tree node from disk
 *
 * Reads a B-Tree node from a page in the disk. All the information regarding
 * the node is stored in a BTreeNode struct (see header file for more details
 * on this struct). *This is the only function that can allocate memory for
 * a BTreeNode struct*. Always use chidb_Btree_freeMemNode to free the memory
 * allocated for a BTreeNode (do not use free() directly on a BTreeNode variable)
 * Any changes made to a BTreeNode variable will not be effective in the database
 * until chidb_Btree_writeNode is called on that BTreeNode.
 *
 * Parameters
 * - bt: B-Tree file
 * - npage: Page of node to load
 * - btn: Out parameter. Used to return a pointer to newly creater BTreeNode
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EPAGENO: The provided page number is not valid
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_getNodeByPage(BTree *bt, npage_t npage, BTreeNode **btn)
{
    MemPage *page;
    int rc = chidb_Pager_readPage(bt->pager, npage, &page);
    if (rc != CHIDB_OK)
        return rc;

    *btn = malloc(sizeof(BTreeNode));
    if (*btn == NULL)
        return CHIDB_ENOMEM;

    uint16_t base = node_header_base(npage);
    uint8_t *d = page->data;

    (*btn)->page = page;
    (*btn)->type = d[base + PGHEADER_PGTYPE_OFFSET];
    (*btn)->free_offset = get2byte(&d[base + PGHEADER_FREE_OFFSET]);
    (*btn)->n_cells = get2byte(&d[base + PGHEADER_NCELLS_OFFSET]);
    (*btn)->cells_offset = get2byte(&d[base + PGHEADER_CELL_OFFSET]);

    if (is_internal((*btn)->type))
    {
        (*btn)->right_page = get4byte(&d[base + PGHEADER_RIGHTPG_OFFSET]);
        (*btn)->celloffset_array = &d[base + INTPG_CELLSOFFSET_OFFSET];
    }
    else
    {
        (*btn)->right_page = 0;
        (*btn)->celloffset_array = &d[base + LEAFPG_CELLSOFFSET_OFFSET];
    }

    return CHIDB_OK;
}


/* Frees the memory allocated to an in-memory B-Tree node
 *
 * Frees the memory allocated to an in-memory B-Tree node, and
 * the in-memory page returned by the pages (stored in the
 * "page" field of BTreeNode)
 *
 * Parameters
 * - bt: B-Tree file
 * - btn: BTreeNode to free
 *
 * Return
 * - CHIDB_OK: Operation successful
 */
int chidb_Btree_freeMemNode(BTree *bt, BTreeNode *btn)
{
    int rc = chidb_Pager_releaseMemPage(bt->pager, btn->page);
    free(btn);
    return rc;
}


/* Create a new B-Tree node
 *
 * Allocates a new page in the file and initializes it as a B-Tree node.
 *
 * Parameters
 * - bt: B-Tree file
 * - npage: Out parameter. Returns the number of the page that
 *          was allocated.
 * - type: Type of B-Tree node (PGTYPE_TABLE_INTERNAL, PGTYPE_TABLE_LEAF,
 *         PGTYPE_INDEX_INTERNAL, or PGTYPE_INDEX_LEAF)
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_newNode(BTree *bt, npage_t *npage, uint8_t type)
{
    int rc = chidb_Pager_allocatePage(bt->pager, npage);
    if (rc != CHIDB_OK)
        return rc;

    return chidb_Btree_initEmptyNode(bt, *npage, type);
}


/* Initialize a B-Tree node
 *
 * Initializes a database page to contain an empty B-Tree node. The
 * database page is assumed to exist and to have been already allocated
 * by the pager.
 *
 * Parameters
 * - bt: B-Tree file
 * - npage: Database page where the node will be created.
 * - type: Type of B-Tree node (PGTYPE_TABLE_INTERNAL, PGTYPE_TABLE_LEAF,
 *         PGTYPE_INDEX_INTERNAL, or PGTYPE_INDEX_LEAF)
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_initEmptyNode(BTree *bt, npage_t npage, uint8_t type)
{
    BTreeNode *btn;
    int rc = chidb_Btree_getNodeByPage(bt, npage, &btn);
    if (rc != CHIDB_OK)
        return rc;

    uint16_t base = node_header_base(npage);

    btn->type = type;
    btn->n_cells = 0;
    btn->cells_offset = bt->pager->page_size;
    btn->free_offset = base + node_header_size(type);
    btn->right_page = 0;
    btn->celloffset_array = &btn->page->data[base + node_header_size(type)];

    rc = chidb_Btree_writeNode(bt, btn);
    chidb_Btree_freeMemNode(bt, btn);

    return rc;
}



/* Write an in-memory B-Tree node to disk
 *
 * Writes an in-memory B-Tree node to disk. To do this, we need to update
 * the in-memory page according to the chidb page format. Since the cell
 * offset array and the cells themselves are modified directly on the
 * page, the only thing to do is to store the values of "type",
 * "free_offset", "n_cells", "cells_offset" and "right_page" in the
 * in-memory page.
 *
 * Parameters
 * - bt: B-Tree file
 * - btn: BTreeNode to write to disk
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_writeNode(BTree *bt, BTreeNode *btn)
{
    uint16_t base = node_header_base(btn->page->npage);
    uint8_t *d = btn->page->data;

    d[base + PGHEADER_PGTYPE_OFFSET] = btn->type;
    put2byte(&d[base + PGHEADER_FREE_OFFSET], btn->free_offset);
    put2byte(&d[base + PGHEADER_NCELLS_OFFSET], btn->n_cells);
    put2byte(&d[base + PGHEADER_CELL_OFFSET], btn->cells_offset);
    d[base + PGHEADER_ZERO_OFFSET] = 0;

    if (is_internal(btn->type))
        put4byte(&d[base + PGHEADER_RIGHTPG_OFFSET], btn->right_page);

    return chidb_Pager_writePage(bt->pager, btn->page);
}


/* Read the contents of a cell
 *
 * Reads the contents of a cell from a BTreeNode and stores them in a BTreeCell.
 * This involves the following:
 *  1. Find out the offset of the requested cell.
 *  2. Read the cell from the in-memory page, and parse its
 *     contents (refer to The chidb File Format document for
 *     the format of cells).
 *
 * Parameters
 * - btn: BTreeNode where cell is contained
 * - ncell: Cell number
 * - cell: BTreeCell where contents must be stored.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ECELLNO: The provided cell number is invalid
 */
int chidb_Btree_getCell(BTreeNode *btn, ncell_t ncell, BTreeCell *cell)
{
    if (ncell >= btn->n_cells)
        return CHIDB_ECELLNO;

    uint16_t offset = get2byte(&btn->celloffset_array[ncell * 2]);
    uint8_t *d = btn->page->data;

    cell->type = btn->type;

    switch (btn->type)
    {
    case PGTYPE_TABLE_INTERNAL:
        cell->fields.tableInternal.child_page = get4byte(&d[offset + TABLEINTCELL_CHILD_OFFSET]);
        getVarint32(&d[offset + TABLEINTCELL_KEY_OFFSET], &cell->key);
        break;

    case PGTYPE_TABLE_LEAF:
    {
        uint32_t size;
        getVarint32(&d[offset + TABLELEAFCELL_SIZE_OFFSET], &size);
        getVarint32(&d[offset + TABLELEAFCELL_KEY_OFFSET], &cell->key);
        cell->fields.tableLeaf.data_size = size;
        cell->fields.tableLeaf.data = &d[offset + TABLELEAFCELL_DATA_OFFSET];
        break;
    }

    case PGTYPE_INDEX_INTERNAL:
        cell->fields.indexInternal.child_page = get4byte(&d[offset + INDEXINTCELL_CHILD_OFFSET]);
        cell->key = get4byte(&d[offset + INDEXINTCELL_KEYIDX_OFFSET]);
        cell->fields.indexInternal.keyPk = get4byte(&d[offset + INDEXINTCELL_KEYPK_OFFSET]);
        break;

    case PGTYPE_INDEX_LEAF:
        cell->key = get4byte(&d[offset + INDEXLEAFCELL_KEYIDX_OFFSET]);
        cell->fields.indexLeaf.keyPk = get4byte(&d[offset + INDEXLEAFCELL_KEYPK_OFFSET]);
        break;
    }

    return CHIDB_OK;
}


/* Insert a new cell into a B-Tree node
 *
 * Inserts a new cell into a B-Tree node at a specified position ncell.
 * This involves the following:
 *  1. Add the cell at the top of the cell area. This involves "translating"
 *     the BTreeCell into the chidb format (refer to The chidb File Format
 *     document for the format of cells).
 *  2. Modify cells_offset in BTreeNode to reflect the growth in the cell area.
 *  3. Modify the cell offset array so that all values in positions >= ncell
 *     are shifted one position forward in the array. Then, set the value of
 *     position ncell to be the offset of the newly added cell.
 *
 * This function assumes that there is enough space for this cell in this node.
 *
 * Parameters
 * - btn: BTreeNode to insert cell in
 * - ncell: Cell number
 * - cell: BTreeCell to insert.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ECELLNO: The provided cell number is invalid
 */
int chidb_Btree_insertCell(BTreeNode *btn, ncell_t ncell, BTreeCell *cell)
{
    if (ncell > btn->n_cells)
        return CHIDB_ECELLNO;

    uint16_t size = cell_size(btn->type, cell);
    uint16_t offset = btn->cells_offset - size;
    uint8_t *d = btn->page->data;

    switch (btn->type)
    {
    case PGTYPE_TABLE_INTERNAL:
        put4byte(&d[offset + TABLEINTCELL_CHILD_OFFSET], cell->fields.tableInternal.child_page);
        putVarint32(&d[offset + TABLEINTCELL_KEY_OFFSET], cell->key);
        break;

    case PGTYPE_TABLE_LEAF:
        putVarint32(&d[offset + TABLELEAFCELL_SIZE_OFFSET], cell->fields.tableLeaf.data_size);
        putVarint32(&d[offset + TABLELEAFCELL_KEY_OFFSET], cell->key);
        memcpy(&d[offset + TABLELEAFCELL_DATA_OFFSET], cell->fields.tableLeaf.data, cell->fields.tableLeaf.data_size);
        break;

    case PGTYPE_INDEX_INTERNAL:
        put4byte(&d[offset + INDEXINTCELL_CHILD_OFFSET], cell->fields.indexInternal.child_page);
        put4byte(&d[offset + INDEXINTCELL_KEYIDX_OFFSET], cell->key);
        put4byte(&d[offset + INDEXINTCELL_KEYPK_OFFSET], cell->fields.indexInternal.keyPk);
        break;

    case PGTYPE_INDEX_LEAF:
        put4byte(&d[offset + INDEXLEAFCELL_KEYIDX_OFFSET], cell->key);
        put4byte(&d[offset + INDEXLEAFCELL_KEYPK_OFFSET], cell->fields.indexLeaf.keyPk);
        break;
    }

    btn->cells_offset = offset;

    for (ncell_t i = btn->n_cells; i > ncell; i--)
        put2byte(&btn->celloffset_array[i * 2], get2byte(&btn->celloffset_array[(i - 1) * 2]));
    put2byte(&btn->celloffset_array[ncell * 2], offset);

    btn->n_cells++;
    btn->free_offset += 2;

    return CHIDB_OK;
}

/* Find an entry in a table B-Tree
 *
 * Finds the data associated for a given key in a table B-Tree
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want search in
 * - key: Entry key
 * - data: Out-parameter where a copy of the data must be stored
 * - size: Out-parameter where the number of bytes of data must be stored
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ENOTFOUND: No entry with the given key way found
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_find(BTree *bt, npage_t nroot, chidb_key_t key, uint8_t **data, uint16_t *size)
{
    BTreeNode *btn;
    int rc = chidb_Btree_getNodeByPage(bt, nroot, &btn);
    if (rc != CHIDB_OK)
        return rc;

    if (btn->type == PGTYPE_TABLE_LEAF)
    {
        for (ncell_t i = 0; i < btn->n_cells; i++)
        {
            BTreeCell c;
            chidb_Btree_getCell(btn, i, &c);
            if (c.key == key)
            {
                *size = c.fields.tableLeaf.data_size;
                *data = malloc(*size);
                if (*data == NULL)
                {
                    chidb_Btree_freeMemNode(bt, btn);
                    return CHIDB_ENOMEM;
                }
                memcpy(*data, c.fields.tableLeaf.data, *size);
                chidb_Btree_freeMemNode(bt, btn);
                return CHIDB_OK;
            }
        }
        chidb_Btree_freeMemNode(bt, btn);
        return CHIDB_ENOTFOUND;
    }
    else
    {
        ncell_t pos = 0;
        BTreeCell c;
        rc = find_position(btn, key, &pos);
        if (rc != CHIDB_OK)
        {
            chidb_Btree_freeMemNode(bt, btn);
            return rc;
        }
        if (pos < btn->n_cells)
            chidb_Btree_getCell(btn, pos, &c);
        npage_t child = child_at(btn, &c, pos);
        chidb_Btree_freeMemNode(bt, btn);
        return chidb_Btree_find(bt, child, key, data, size);
    }
}



/* Insert an entry into a table B-Tree
 *
 * This is a convenience function that wraps around chidb_Btree_insert.
 * It takes a key and data, and creates a BTreeCell that can be passed
 * along to chidb_Btree_insert.
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want to insert
 *          this entry in.
 * - key: Entry key
 * - data: Pointer to data we want to insert
 * - size: Number of bytes of data
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EDUPLICATE: An entry with that key already exists
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_insertInTable(BTree *bt, npage_t nroot, chidb_key_t key, uint8_t *data, uint16_t size)
{
    BTreeCell cell;
    cell.type = PGTYPE_TABLE_LEAF;
    cell.key = key;
    cell.fields.tableLeaf.data_size = size;
    cell.fields.tableLeaf.data = data;

    return chidb_Btree_insert(bt, nroot, &cell);
}


/* Insert an entry into an index B-Tree
 *
 * This is a convenience function that wraps around chidb_Btree_insert.
 * It takes a KeyIdx and a KeyPk, and creates a BTreeCell that can be passed
 * along to chidb_Btree_insert.
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want to insert
 *          this entry in.
 * - keyIdx: See The chidb File Format.
 * - keyPk: See The chidb File Format.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EDUPLICATE: An entry with that key already exists
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_insertInIndex(BTree *bt, npage_t nroot, chidb_key_t keyIdx, chidb_key_t keyPk)
{
    BTreeCell cell;
    cell.type = PGTYPE_INDEX_LEAF;
    cell.key = keyIdx;
    cell.fields.indexLeaf.keyPk = keyPk;

    return chidb_Btree_insert(bt, nroot, &cell);
}


/* Insert a BTreeCell into a B-Tree
 *
 * The chidb_Btree_insert and chidb_Btree_insertNonFull functions
 * are responsible for inserting new entries into a B-Tree, although
 * chidb_Btree_insertNonFull is the one that actually does the
 * insertion. chidb_Btree_insert, however, first checks if the root
 * has to be split (a splitting operation that is different from
 * splitting any other node). If so, chidb_Btree_split is called
 * before calling chidb_Btree_insertNonFull.
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want to insert
 *          this cell in.
 * - btc: BTreeCell to insert into B-Tree
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EDUPLICATE: An entry with that key already exists
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_insert(BTree *bt, npage_t nroot, BTreeCell *btc)
{
    BTreeNode *root;
    int rc = chidb_Btree_getNodeByPage(bt, nroot, &root);
    if (rc != CHIDB_OK)
        return rc;

    uint16_t needed = cell_size(root->type, btc);
    bool full = node_free_space(root) < (uint16_t)(needed + 2);
    uint8_t root_type = root->type;
    chidb_Btree_freeMemNode(bt, root);

    if (full)
    {
        /* claude: the root's page NUMBER can never change (it's the value
         * recorded in the schema table / passed in as nroot by every
         * caller), so a full root is split by cloning its entire content
         * into a brand-new page C, turning the root into an empty
         * internal node with right_page=C in place, and then splitting C
         * for real -- promoting C's median into the now-internal root. */
        npage_t nC;
        rc = chidb_Btree_newNode(bt, &nC, root_type);
        if (rc != CHIDB_OK)
            return rc;

        BTreeNode *rootN, *C;
        chidb_Btree_getNodeByPage(bt, nroot, &rootN);
        chidb_Btree_getNodeByPage(bt, nC, &C);
        for (ncell_t i = 0; i < rootN->n_cells; i++)
        {
            BTreeCell c;
            chidb_Btree_getCell(rootN, i, &c);
            chidb_Btree_insertCell(C, i, &c);
        }
        C->right_page = rootN->right_page;
        chidb_Btree_writeNode(bt, C);
        chidb_Btree_freeMemNode(bt, C);
        chidb_Btree_freeMemNode(bt, rootN);

        uint8_t newRootType = is_index(root_type) ? PGTYPE_INDEX_INTERNAL : PGTYPE_TABLE_INTERNAL;
        chidb_Btree_initEmptyNode(bt, nroot, newRootType);
        chidb_Btree_getNodeByPage(bt, nroot, &rootN);
        rootN->right_page = nC;
        chidb_Btree_writeNode(bt, rootN);
        chidb_Btree_freeMemNode(bt, rootN);

        npage_t nC2;
        rc = chidb_Btree_split(bt, nroot, nC, 0, &nC2);
        if (rc != CHIDB_OK)
            return rc;
    }

    return chidb_Btree_insertNonFull(bt, nroot, btc);
}

/* Insert a BTreeCell into a non-full B-Tree node
 *
 * chidb_Btree_insertNonFull inserts a BTreeCell into a node that is
 * assumed not to be full (i.e., does not require splitting). If the
 * node is a leaf node, the cell is directly added in the appropriate
 * position according to its key. If the node is an internal node, the
 * function will determine what child node it must insert it in, and
 * calls itself recursively on that child node. However, before doing so
 * it will check if the child node is full or not. If it is, then it will
 * have to be split first.
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want to insert
 *          this cell in.
 * - btc: BTreeCell to insert into B-Tree
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EDUPLICATE: An entry with that key already exists
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_insertNonFull(BTree *bt, npage_t npage, BTreeCell *btc)
{
    BTreeNode *btn;
    int rc = chidb_Btree_getNodeByPage(bt, npage, &btn);
    if (rc != CHIDB_OK)
        return rc;

    if (!is_internal(btn->type))
    {
        ncell_t pos = 0;
        rc = find_position(btn, btc->key, &pos);
        if (rc == CHIDB_OK && pos < btn->n_cells)
        {
            BTreeCell existing;
            chidb_Btree_getCell(btn, pos, &existing);
            if (existing.key == btc->key)
            {
                chidb_Btree_freeMemNode(bt, btn);
                return CHIDB_EDUPLICATE;
            }
        }
        rc = chidb_Btree_insertCell(btn, pos, btc);
        if (rc == CHIDB_OK)
            rc = chidb_Btree_writeNode(bt, btn);
        chidb_Btree_freeMemNode(bt, btn);
        return rc;
    }
    else
    {
        ncell_t pos = 0;
        BTreeCell c;
        find_position(btn, btc->key, &pos);
        if (pos < btn->n_cells)
            chidb_Btree_getCell(btn, pos, &c);
        npage_t childPage = child_at(btn, &c, pos);
        chidb_Btree_freeMemNode(bt, btn);

        BTreeNode *child;
        rc = chidb_Btree_getNodeByPage(bt, childPage, &child);
        if (rc != CHIDB_OK)
            return rc;
        uint16_t needed = cell_size(child->type, btc);
        bool full = node_free_space(child) < (uint16_t)(needed + 2);
        chidb_Btree_freeMemNode(bt, child);

        if (full)
        {
            npage_t nchild2;
            rc = chidb_Btree_split(bt, npage, childPage, pos, &nchild2);
            if (rc != CHIDB_OK)
                return rc;

            /* claude: re-descend from scratch -- the split just changed
             * the parent's cells, so recompute which of the two halves
             * (childPage, now the upper half, or nchild2, the new lower
             * half) actually covers btc->key. */
            rc = chidb_Btree_getNodeByPage(bt, npage, &btn);
            if (rc != CHIDB_OK)
                return rc;
            find_position(btn, btc->key, &pos);
            if (pos < btn->n_cells)
                chidb_Btree_getCell(btn, pos, &c);
            childPage = child_at(btn, &c, pos);
            chidb_Btree_freeMemNode(bt, btn);
        }

        return chidb_Btree_insertNonFull(bt, childPage, btc);
    }
}


/* Split a B-Tree node
 *
 * Splits a B-Tree node N. This involves the following:
 * - Find the median cell in N.
 * - Create a new B-Tree node M.
 * - Move the cells before the median cell to M (if the
 *   cell is a table leaf cell, the median cell is moved too)
 * - Add a cell to the parent (which, by definition, will be an
 *   internal page) with the median key and the page number of M.
 *
 * Parameters
 * - bt: B-Tree file
 * - npage_parent: Page number of the parent node
 * - npage_child: Page number of the node to split
 * - parent_ncell: Position in the parent where the new cell will
 *                 be inserted.
 * - npage_child2: Out parameter. Used to return the page of the new child node.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_split(BTree *bt, npage_t npage_parent, npage_t npage_child, ncell_t parent_ncell, npage_t *npage_child2)
{
    BTreeNode *child;
    int rc = chidb_Btree_getNodeByPage(bt, npage_child, &child);
    if (rc != CHIDB_OK)
        return rc;

    uint8_t type = child->type;
    bool leaf = !is_internal(type);
    bool idx = is_index(type);
    ncell_t ncells = child->n_cells;
    ncell_t m = ncells / 2;

    /* claude: snapshot every cell out of `child` before we start
     * overwriting its page -- table leaf cells point directly into the
     * page buffer (see chidb_Btree_getCell), so those payloads must be
     * deep-copied now or they'd be clobbered by initEmptyNode below. */
    BTreeCell *cells = malloc(sizeof(BTreeCell) * ncells);
    for (ncell_t i = 0; i < ncells; i++)
    {
        chidb_Btree_getCell(child, i, &cells[i]);
        if (type == PGTYPE_TABLE_LEAF)
        {
            uint8_t *copy = malloc(cells[i].fields.tableLeaf.data_size);
            memcpy(copy, cells[i].fields.tableLeaf.data, cells[i].fields.tableLeaf.data_size);
            cells[i].fields.tableLeaf.data = copy;
        }
    }
    chidb_key_t median_key = cells[m].key;
    chidb_key_t median_keyPk = idx ? (leaf ? cells[m].fields.indexLeaf.keyPk : cells[m].fields.indexInternal.keyPk) : 0;
    npage_t old_right_page = child->right_page;
    chidb_Btree_freeMemNode(bt, child);

    /* M: brand-new page holding the lower half (median cell included, for
     * a leaf split -- see fileformat/architecture docs on splitting). */
    npage_t nM;
    rc = chidb_Btree_newNode(bt, &nM, type);
    if (rc != CHIDB_OK)
        return rc;

    BTreeNode *M;
    chidb_Btree_getNodeByPage(bt, nM, &M);
    ncell_t mCount = leaf ? (m + 1) : m;
    for (ncell_t i = 0; i < mCount; i++)
        chidb_Btree_insertCell(M, i, &cells[i]);
    if (!leaf)
        M->right_page = idx ? cells[m].fields.indexInternal.child_page : cells[m].fields.tableInternal.child_page;
    chidb_Btree_writeNode(bt, M);
    chidb_Btree_freeMemNode(bt, M);

    /* npage_child (N) is reinitialized in place and keeps the upper half,
     * so its page number -- the one every existing parent pointer already
     * references -- never changes. */
    chidb_Btree_initEmptyNode(bt, npage_child, type);
    BTreeNode *N;
    chidb_Btree_getNodeByPage(bt, npage_child, &N);
    ncell_t j = 0;
    for (ncell_t i = m + 1; i < ncells; i++)
        chidb_Btree_insertCell(N, j++, &cells[i]);
    if (!leaf)
        N->right_page = old_right_page;
    chidb_Btree_writeNode(bt, N);
    chidb_Btree_freeMemNode(bt, N);

    for (ncell_t i = 0; i < ncells; i++)
        if (type == PGTYPE_TABLE_LEAF)
            free(cells[i].fields.tableLeaf.data);
    free(cells);

    /* Promote (median_key, nM) into the parent. */
    BTreeCell promoted;
    promoted.type = idx ? PGTYPE_INDEX_INTERNAL : PGTYPE_TABLE_INTERNAL;
    promoted.key = median_key;
    if (idx)
    {
        promoted.fields.indexInternal.child_page = nM;
        promoted.fields.indexInternal.keyPk = median_keyPk;
    }
    else
    {
        promoted.fields.tableInternal.child_page = nM;
    }

    BTreeNode *parent;
    rc = chidb_Btree_getNodeByPage(bt, npage_parent, &parent);
    if (rc != CHIDB_OK)
        return rc;
    rc = chidb_Btree_insertCell(parent, parent_ncell, &promoted);
    chidb_Btree_writeNode(bt, parent);
    chidb_Btree_freeMemNode(bt, parent);
    if (rc != CHIDB_OK)
        return rc;

    *npage_child2 = nM;
    return CHIDB_OK;
}

