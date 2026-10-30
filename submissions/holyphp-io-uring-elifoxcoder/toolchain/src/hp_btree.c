/* ---- HolyPHP generic B-tree cursor binding ----
 * Appended to sqlite3.c by build.sh so it can see SQLite's internals. This is
 * a driver primitive, like sqlite3_prepare: it exposes cursors on a table or
 * index, their payload bytes and the rowid. No challenge logic lives here. */

typedef struct HpBt {
    sqlite3 *db;
    Btree *pBt;
    BtCursor cur;
    KeyInfo *pKey;      /* owned by this cursor when it is an index cursor */
} HpBt;

unsigned int hp_bt_root_impl(sqlite3 *db, const char *zName, int isIndex) {
    char *zErr = 0;
    if (!db || !zName) return 0;
    /* sqlite3_open() does not read the schema; do it before looking up a
     * table or index by name. */
    {
        int rc = sqlite3Init(db, &zErr);
        if (rc != SQLITE_OK) { sqlite3_free(zErr); return 0; }
    }
    if (isIndex) {
        HashElem *k;
        for (k = sqliteHashFirst(&db->aDb[0].pSchema->idxHash); k; k = sqliteHashNext(k)) {
            Index *pIdx = (Index *)sqliteHashData(k);
            if (sqlite3StrICmp(pIdx->zName, zName) == 0) return pIdx->tnum;
        }
        return 0;
    } else {
        Table *pTab = sqlite3FindTable(db, zName, "main");
        return pTab ? pTab->tnum : 0;
    }
}

static Index *hp_find_index(sqlite3 *db, const char *zName) {
    HashElem *k;
    for (k = sqliteHashFirst(&db->aDb[0].pSchema->idxHash); k; k = sqliteHashNext(k)) {
        Index *pIdx = (Index *)sqliteHashData(k);
        if (sqlite3StrICmp(pIdx->zName, zName) == 0) return pIdx;
    }
    return 0;
}

/* Open a cursor on a table (isIndex == 0) or on an index by name. The index
 * cursor needs the index's KeyInfo, otherwise the btree cannot interpret the
 * keys and sqlite3BtreeLast() fails. */
void *hp_bt_open_impl(sqlite3 *db, const char *zName, int isIndex) {
    HpBt *h;
    Pgno root;
    KeyInfo *pKey = 0;
    char *zErr = 0;
    if (!db || !zName) return 0;
    if (sqlite3Init(db, &zErr) != SQLITE_OK) { sqlite3_free(zErr); return 0; }
    if (isIndex) {
        Index *pIdx = hp_find_index(db, zName);
        Parse sParse;
        if (!pIdx) return 0;
        root = pIdx->tnum;
        /* The index cursor needs the index's KeyInfo; without it the btree
         * cannot compare keys and sqlite3BtreeLast() fails. sqlite3KeyInfoOfIndex
         * wants a Parse, but only reads db/nErr from it. */
        memset(&sParse, 0, sizeof sParse);
        sParse.db = db;
        pKey = sqlite3KeyInfoOfIndex(&sParse, pIdx);
        if (!pKey) return 0;
    } else {
        Table *pTab = sqlite3FindTable(db, zName, "main");
        if (!pTab) return 0;
        root = pTab->tnum;
    }
    h = (HpBt *)sqlite3_malloc(sizeof *h);
    if (!h) return 0;
    memset(h, 0, sizeof *h);
    h->db = db;
    h->pBt = db->aDb[0].pBt;
    h->pKey = pKey;
    if (sqlite3BtreeCursor(h->pBt, root, 0, pKey, &h->cur) != SQLITE_OK) {
        if (pKey) sqlite3KeyInfoUnref(pKey);
        sqlite3_free(h);
        return 0;
    }
    return h;
}

void hp_bt_close_impl(void *v) {
    HpBt *h = (HpBt *)v;
    if (!h) return;
    sqlite3BtreeCloseCursor(&h->cur);
    if (h->pKey) sqlite3KeyInfoUnref(h->pKey);
    sqlite3_free(h);
}

/* 1 = on a row, 0 = empty / past the end, -1 = error */
int hp_bt_first_impl(void *v) {
    HpBt *h = (HpBt *)v;
    int res = 1;
    if (!h) return -1;
    return sqlite3BtreeFirst(&h->cur, &res) == SQLITE_OK ? (res ? 0 : 1) : -1;
}
int hp_bt_last_impl(void *v) {
    HpBt *h = (HpBt *)v;
    int res = 1;
    if (!h) return -1;
    return sqlite3BtreeLast(&h->cur, &res) == SQLITE_OK ? (res ? 0 : 1) : -1;
}
int hp_bt_next_impl(void *v) {
    HpBt *h = (HpBt *)v;
    if (!h) return 0;
    return sqlite3BtreeNext(&h->cur, 0) == SQLITE_OK ? 1 : 0;
}
int hp_bt_prev_impl(void *v) {
    HpBt *h = (HpBt *)v;
    if (!h) return 0;
    return sqlite3BtreePrevious(&h->cur, 0) == SQLITE_OK ? 1 : 0;
}
int hp_bt_seek_impl(void *v, sqlite3_int64 rowid) {
    HpBt *h = (HpBt *)v;
    int res = 1;
    if (!h) return -1;
    return sqlite3BtreeTableMoveto(&h->cur, (i64)rowid, 0, &res) == SQLITE_OK ? (res ? 0 : 1) : -1;
}
sqlite3_int64 hp_bt_rowid_impl(void *v) {
    HpBt *h = (HpBt *)v;
    if (!h) return -1;
    return (sqlite3_int64)sqlite3BtreeIntegerKey(&h->cur);
}
const unsigned char *hp_bt_payload_impl(void *v, int *pn) {
    HpBt *h = (HpBt *)v;
    u32 amt = 0;
    const void *p;
    if (!h || !pn) return 0;
    *pn = 0;
    if (h->cur.eState != CURSOR_VALID) return 0;
    /* getCellInfo() runs here: fetchPayload() asserts info.nSize != 0, and a
     * cursor that just moved has it cleared. */
    (void)sqlite3BtreePayloadSize(&h->cur);
    p = sqlite3BtreePayloadFetch(&h->cur, &amt);
    *pn = (int)amt;
    return (const unsigned char *)p;
}
