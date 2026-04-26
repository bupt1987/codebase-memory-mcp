/*
 * test_store_edges.c — Tests for edge CRUD operations.
 *
 * Ported from internal/store/store_test.go (TestEdgeCRUD, TestInsertEdgeBatch,
 * TestFindEdgesByURLPath, etc.)
 */
#include "test_framework.h"
#include <foundation/compat.h>
#include <store/store.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* Helper: create a store with project + N nodes (A, B, C, ...) */
static cbm_store_t *setup_store_with_nodes(int n, int64_t *ids) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    char name[8], qn[32];
    for (int i = 0; i < n; i++) {
        snprintf(name, sizeof(name), "%c", 'A' + i);
        snprintf(qn, sizeof(qn), "test.%c", 'A' + i);
        cbm_node_t node = {
            .project = "test", .label = "Function", .name = name, .qualified_name = qn};
        ids[i] = cbm_store_upsert_node(s, &node);
    }
    return s;
}

static bool string_list_contains(char **items, int count, const char *needle) {
    for (int i = 0; i < count; i++) {
        if (items[i] && strcmp(items[i], needle) == 0) {
            return true;
        }
    }
    return false;
}

static int test_process_id(void) {
#ifdef _WIN32
    return 0;
#else
    return (int)getpid();
#endif
}

static void cleanup_edge_schema_db(const char *path) {
    remove(path);
    char aux[512];
    snprintf(aux, sizeof(aux), "%s-wal", path);
    remove(aux);
    snprintf(aux, sizeof(aux), "%s-shm", path);
    remove(aux);
}

static bool sqlite_index_exists_at_path(const char *db_path, const char *index_name) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    sqlite3_stmt *stmt = NULL;
    bool exists = false;
    if (sqlite3_prepare_v2(db,
                           "SELECT 1 FROM sqlite_master "
                           "WHERE type='index' AND name=?1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, index_name, -1, SQLITE_TRANSIENT);
        exists = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return exists;
}

/* ── Edge CRUD ──────────────────────────────────────────────────── */

TEST(store_edge_insert_find) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    /* Insert edge */
    cbm_edge_t e = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    int64_t eid = cbm_store_insert_edge(s, &e);
    ASSERT_GT(eid, 0);

    /* Find by source */
    cbm_edge_t *edges = NULL;
    int count = 0;
    int rc = cbm_store_find_edges_by_source(s, ids[0], &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(edges[0].type, "CALLS");
    ASSERT_EQ(edges[0].source_id, ids[0]);
    ASSERT_EQ(edges[0].target_id, ids[1]);
    cbm_store_free_edges(edges, count);

    /* Find by target */
    rc = cbm_store_find_edges_by_target(s, ids[1], &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 1);
    cbm_store_free_edges(edges, count);

    /* Count */
    int ecnt = cbm_store_count_edges(s, "test");
    ASSERT_EQ(ecnt, 1);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_dedup) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    /* Insert same edge twice — should not duplicate */
    cbm_edge_t e = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_store_insert_edge(s, &e);
    cbm_store_insert_edge(s, &e);

    int ecnt = cbm_store_count_edges(s, "test");
    ASSERT_EQ(ecnt, 1);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_find_by_source_type) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    cbm_edge_t e1 = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_edge_t e2 = {
        .project = "test", .source_id = ids[0], .target_id = ids[2], .type = "IMPORTS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);

    cbm_edge_t *edges = NULL;
    int count = 0;
    int rc = cbm_store_find_edges_by_source_type(s, ids[0], "CALLS", &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(edges[0].type, "CALLS");
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_find_by_target_type) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    cbm_edge_t e1 = {.project = "test", .source_id = ids[0], .target_id = ids[2], .type = "CALLS"};
    cbm_edge_t e2 = {
        .project = "test", .source_id = ids[1], .target_id = ids[2], .type = "IMPORTS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);

    cbm_edge_t *edges = NULL;
    int count = 0;
    int rc = cbm_store_find_edges_by_target_type(s, ids[2], "CALLS", &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 1);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_find_by_type) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    cbm_edge_t e1 = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_edge_t e2 = {.project = "test", .source_id = ids[1], .target_id = ids[2], .type = "CALLS"};
    cbm_edge_t e3 = {
        .project = "test", .source_id = ids[0], .target_id = ids[2], .type = "IMPORTS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);
    cbm_store_insert_edge(s, &e3);

    cbm_edge_t *edges = NULL;
    int count = 0;
    int rc = cbm_store_find_edges_by_type(s, "test", "CALLS", &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 2);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_find_inbound_source_files_by_target_files) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t caller_a = {.project = "test",
                           .label = "Function",
                           .name = "CallerA",
                           .qualified_name = "test.CallerA",
                           .file_path = "caller_a.go"};
    cbm_node_t caller_b = {.project = "test",
                           .label = "Function",
                           .name = "CallerB",
                           .qualified_name = "test.CallerB",
                           .file_path = "caller_b.go"};
    cbm_node_t target_a = {.project = "test",
                           .label = "Function",
                           .name = "TargetA",
                           .qualified_name = "test.TargetA",
                           .file_path = "target_a.go"};
    cbm_node_t target_b = {.project = "test",
                           .label = "Function",
                           .name = "TargetB",
                           .qualified_name = "test.TargetB",
                           .file_path = "target_b.go"};
    cbm_node_t same_file = {.project = "test",
                            .label = "Function",
                            .name = "SameFile",
                            .qualified_name = "test.SameFile",
                            .file_path = "target_a.go"};

    int64_t caller_a_id = cbm_store_upsert_node(s, &caller_a);
    int64_t caller_b_id = cbm_store_upsert_node(s, &caller_b);
    int64_t target_a_id = cbm_store_upsert_node(s, &target_a);
    int64_t target_b_id = cbm_store_upsert_node(s, &target_b);
    int64_t same_file_id = cbm_store_upsert_node(s, &same_file);

    cbm_store_insert_edge(
        s, &(cbm_edge_t){.project = "test", .source_id = caller_a_id, .target_id = target_a_id,
                         .type = "CALLS"});
    cbm_store_insert_edge(
        s, &(cbm_edge_t){.project = "test", .source_id = caller_a_id, .target_id = target_b_id,
                         .type = "CALLS"});
    cbm_store_insert_edge(
        s, &(cbm_edge_t){.project = "test", .source_id = caller_b_id, .target_id = target_b_id,
                         .type = "CALLS"});
    cbm_store_insert_edge(
        s, &(cbm_edge_t){.project = "test", .source_id = same_file_id, .target_id = target_a_id,
                         .type = "CALLS"});

    const char *targets[] = {"target_a.go", "target_b.go"};
    char **sources = NULL;
    int count = 0;
    ASSERT_EQ(cbm_store_find_inbound_source_files_by_target_files(s, "test", targets, 2, &sources,
                                                                  &count),
              CBM_STORE_OK);

    ASSERT_EQ(count, 2);
    ASSERT_TRUE(string_list_contains(sources, count, "caller_a.go"));
    ASSERT_TRUE(string_list_contains(sources, count, "caller_b.go"));
    ASSERT_FALSE(string_list_contains(sources, count, "target_a.go"));

    cbm_store_free_strings(sources, count);
    cbm_store_close(s);
    PASS();
}

TEST(store_edge_count_by_type) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    cbm_edge_t e1 = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_edge_t e2 = {.project = "test", .source_id = ids[1], .target_id = ids[2], .type = "CALLS"};
    cbm_edge_t e3 = {
        .project = "test", .source_id = ids[0], .target_id = ids[2], .type = "IMPORTS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);
    cbm_store_insert_edge(s, &e3);

    int cnt = cbm_store_count_edges_by_type(s, "test", "CALLS");
    ASSERT_EQ(cnt, 2);

    cnt = cbm_store_count_edges_by_type(s, "test", "IMPORTS");
    ASSERT_EQ(cnt, 1);

    cnt = cbm_store_count_edges_by_type(s, "test", "NONEXISTENT");
    ASSERT_EQ(cnt, 0);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_delete_by_type) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    cbm_edge_t e1 = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_edge_t e2 = {
        .project = "test", .source_id = ids[0], .target_id = ids[2], .type = "IMPORTS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);

    cbm_store_delete_edges_by_type(s, "test", "CALLS");
    int ecnt = cbm_store_count_edges(s, "test");
    ASSERT_EQ(ecnt, 1);

    cbm_store_close(s);
    PASS();
}

/* ── Edge properties ────────────────────────────────────────────── */

TEST(store_edge_properties_json) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    cbm_edge_t e = {.project = "test",
                    .source_id = ids[0],
                    .target_id = ids[1],
                    .type = "HTTP_CALLS",
                    .properties_json = "{\"url_path\":\"/api/orders/create\",\"confidence\":0.8}"};
    cbm_store_insert_edge(s, &e);

    cbm_edge_t *edges = NULL;
    int count = 0;
    cbm_store_find_edges_by_source(s, ids[0], &edges, &count);
    ASSERT_EQ(count, 1);
    ASSERT_NOT_NULL(edges[0].properties_json);
    ASSERT(strstr(edges[0].properties_json, "url_path") != NULL);
    ASSERT(strstr(edges[0].properties_json, "/api/orders/create") != NULL);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_null_properties) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    cbm_edge_t e = {.project = "test",
                    .source_id = ids[0],
                    .target_id = ids[1],
                    .type = "CALLS",
                    .properties_json = NULL};
    cbm_store_insert_edge(s, &e);

    cbm_edge_t *edges = NULL;
    int count = 0;
    cbm_store_find_edges_by_source(s, ids[0], &edges, &count);
    ASSERT_EQ(count, 1);
    ASSERT_NOT_NULL(edges[0].properties_json);
    ASSERT_STR_EQ(edges[0].properties_json, "{}");
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

/* ── Batch edge insert ──────────────────────────────────────────── */

TEST(store_edge_batch_insert) {
    int64_t ids[10];
    cbm_store_t *s = setup_store_with_nodes(10, ids);

    /* Create edges: each node calls the next */
    cbm_edge_t edges[9];
    for (int i = 0; i < 9; i++) {
        edges[i] = (cbm_edge_t){
            .project = "test", .source_id = ids[i], .target_id = ids[i + 1], .type = "CALLS"};
    }

    int rc = cbm_store_insert_edge_batch(s, edges, 9);
    ASSERT_EQ(rc, CBM_STORE_OK);

    int ecnt = cbm_store_count_edges(s, "test");
    ASSERT_EQ(ecnt, 9);

    /* Re-insert should not duplicate */
    rc = cbm_store_insert_edge_batch(s, edges, 9);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ecnt = cbm_store_count_edges(s, "test");
    ASSERT_EQ(ecnt, 9);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_upsert_batch_in_tx) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    cbm_edge_t edges[3] = {
        {.project = "test",
         .source_id = ids[0],
         .target_id = ids[1],
         .type = "CALLS",
         .properties_json = "{\"first\":true}"},
        {.project = "test",
         .source_id = ids[0],
         .target_id = ids[1],
         .type = "CALLS",
         .properties_json = "{\"second\":true}"},
        {.project = "test", .source_id = ids[1], .target_id = ids[2], .type = "CALLS"},
    };

    ASSERT_EQ(cbm_store_begin(s), CBM_STORE_OK);
    int inserted = -1;
    int updated = -1;
    ASSERT_EQ(cbm_store_upsert_edge_batch_stats_in_tx(s, edges, 3, &inserted, &updated),
              CBM_STORE_OK);
    ASSERT_EQ(cbm_store_commit(s), CBM_STORE_OK);
    ASSERT_EQ(inserted, 2);
    ASSERT_EQ(updated, 1);

    int ecnt = cbm_store_count_edges(s, "test");
    ASSERT_EQ(ecnt, 2);

    cbm_edge_t *found = NULL;
    int count = 0;
    ASSERT_EQ(cbm_store_find_edges_by_source(s, ids[0], &found, &count), CBM_STORE_OK);
    ASSERT_EQ(count, 1);
    ASSERT(strstr(found[0].properties_json, "\"first\"") != NULL);
    ASSERT(strstr(found[0].properties_json, "\"second\"") != NULL);
    cbm_store_free_edges(found, count);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_upsert_batch_stats_skips_empty_conflict_update) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    cbm_edge_t edges[2] = {
        {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"},
        {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"},
    };

    int inserted = -1;
    int updated = -1;
    ASSERT_EQ(cbm_store_begin(s), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_upsert_edge_batch_stats_in_tx(s, edges, 2, &inserted, &updated),
              CBM_STORE_OK);
    ASSERT_EQ(cbm_store_commit(s), CBM_STORE_OK);

    ASSERT_EQ(cbm_store_count_edges(s, "test"), 1);
    ASSERT_EQ(inserted, 1);
    ASSERT_EQ(updated, 0);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_insert_batch_stats_in_tx_counts_plain_inserts) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    cbm_edge_t edges[2] = {
        {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"},
        {.project = "test", .source_id = ids[1], .target_id = ids[2], .type = "CALLS"},
    };

    int inserted = -1;
    ASSERT_EQ(cbm_store_begin(s), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_insert_edge_batch_stats_in_tx(s, edges, 2, &inserted), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_commit(s), CBM_STORE_OK);

    ASSERT_EQ(inserted, 2);
    ASSERT_EQ(cbm_store_count_edges(s, "test"), 2);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_insert_or_upsert_batch_falls_back_after_duplicate) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    cbm_edge_t existing = {
        .project = "test",
        .source_id = ids[0],
        .target_id = ids[1],
        .type = "CALLS",
    };
    ASSERT_GT(cbm_store_insert_edge(s, &existing), 0);

    cbm_edge_t edges[2] = {
        {.project = "test", .source_id = ids[1], .target_id = ids[2], .type = "CALLS"},
        {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"},
    };

    int inserted = -1;
    int updated = -1;
    ASSERT_EQ(cbm_store_begin(s), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_insert_or_upsert_edge_batch_stats_in_tx(s, edges, 2, &inserted,
                                                                &updated),
              CBM_STORE_OK);
    ASSERT_EQ(cbm_store_commit(s), CBM_STORE_OK);

    ASSERT_EQ(inserted, 1);
    ASSERT_EQ(updated, 0);
    ASSERT_EQ(cbm_store_count_edges(s, "test"), 2);

    cbm_store_close(s);
    PASS();
}

TEST(store_edge_open_drops_unused_url_path_index) {
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/cbm_edge_schema_%d.db", cbm_tmpdir(),
             test_process_id());
    cleanup_edge_schema_db(db_path);

    cbm_store_t *s = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_exec(s,
                             "CREATE INDEX IF NOT EXISTS idx_edges_url_path "
                             "ON edges(project, url_path_gen);"),
              CBM_STORE_OK);
    cbm_store_close(s);
    ASSERT(sqlite_index_exists_at_path(db_path, "idx_edges_url_path"));

    s = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s);
    cbm_store_close(s);

    ASSERT(!sqlite_index_exists_at_path(db_path, "idx_edges_url_path"));
    cleanup_edge_schema_db(db_path);
    PASS();
}

TEST(store_edge_batch_empty) {
    cbm_store_t *s = cbm_store_open_memory();
    int rc = cbm_store_insert_edge_batch(s, NULL, 0);
    ASSERT_EQ(rc, CBM_STORE_OK);
    cbm_store_close(s);
    PASS();
}

/* ── Edge cascade on node delete ────────────────────────────────── */

TEST(store_edge_cascade_on_node_delete) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    /* A→B, A→C */
    cbm_edge_t e1 = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_edge_t e2 = {.project = "test", .source_id = ids[0], .target_id = ids[2], .type = "CALLS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);

    /* Delete node A — edges should cascade */
    cbm_store_delete_nodes_by_project(s, "test");
    int ecnt = cbm_store_count_edges(s, "test");
    ASSERT_EQ(ecnt, 0);

    cbm_store_close(s);
    PASS();
}

/* ── Batch insert with count=0 (non-NULL array) ────────────────── */

TEST(store_edge_batch_insert_zero_count) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_edge_t dummy = {.project = "test", .source_id = 1, .target_id = 2, .type = "CALLS"};
    int rc = cbm_store_insert_edge_batch(s, &dummy, 0);
    ASSERT_EQ(rc, CBM_STORE_OK);

    int ecnt = cbm_store_count_edges(s, "test");
    ASSERT_EQ(ecnt, 0);

    cbm_store_close(s);
    PASS();
}

/* ── Batch insert stress: 50 edges ─────────────────────────────── */

TEST(store_edge_batch_insert_50) {
    /* Create 51 nodes so we can have 50 A→B edges with distinct targets */
    int64_t ids[51];
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    for (int i = 0; i < 51; i++) {
        char name[8], qn[32];
        snprintf(name, sizeof(name), "N%d", i);
        snprintf(qn, sizeof(qn), "test.N%d", i);
        cbm_node_t node = {
            .project = "test", .label = "Function", .name = name, .qualified_name = qn};
        ids[i] = cbm_store_upsert_node(s, &node);
    }

    /* 50 edges: N0→N1, N1→N2, ..., N49→N50 */
    cbm_edge_t edges[50];
    for (int i = 0; i < 50; i++) {
        edges[i] = (cbm_edge_t){
            .project = "test", .source_id = ids[i], .target_id = ids[i + 1], .type = "CALLS"};
    }

    int rc = cbm_store_insert_edge_batch(s, edges, 50);
    ASSERT_EQ(rc, CBM_STORE_OK);

    int ecnt = cbm_store_count_edges(s, "test");
    ASSERT_EQ(ecnt, 50);

    cbm_store_close(s);
    PASS();
}

/* ── find_edges_by_source with non-existent source ─────────────── */

TEST(store_edge_find_source_nonexistent) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_edge_t *edges = NULL;
    int count = 0;
    int rc = cbm_store_find_edges_by_source(s, 999999, &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 0);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

/* ── find_edges_by_target with non-existent target ─────────────── */

TEST(store_edge_find_target_nonexistent) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_edge_t *edges = NULL;
    int count = 0;
    int rc = cbm_store_find_edges_by_target(s, 999999, &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 0);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

/* ── find_edges_by_type for non-existent type ──────────────────── */

TEST(store_edge_find_type_nonexistent) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    cbm_edge_t e = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_store_insert_edge(s, &e);

    cbm_edge_t *edges = NULL;
    int count = 0;
    int rc = cbm_store_find_edges_by_type(s, "test", "DOES_NOT_EXIST", &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 0);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

/* ── count_edges on empty project ──────────────────────────────── */

TEST(store_edge_count_empty_project) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "empty", "/tmp/empty");

    int ecnt = cbm_store_count_edges(s, "empty");
    ASSERT_EQ(ecnt, 0);

    cbm_store_close(s);
    PASS();
}

/* ── count_edges_by_type for missing type ──────────────────────── */

TEST(store_edge_count_by_type_missing) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    cbm_edge_t e = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_store_insert_edge(s, &e);

    int cnt = cbm_store_count_edges_by_type(s, "test", "NEVER_EXISTS");
    ASSERT_EQ(cnt, 0);

    cbm_store_close(s);
    PASS();
}

/* ── delete_edges_by_type preserves other types ────────────────── */

TEST(store_edge_delete_by_type_preserves_others) {
    int64_t ids[3];
    cbm_store_t *s = setup_store_with_nodes(3, ids);

    cbm_edge_t e1 = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_edge_t e2 = {
        .project = "test", .source_id = ids[0], .target_id = ids[2], .type = "IMPORTS"};
    cbm_edge_t e3 = {
        .project = "test", .source_id = ids[1], .target_id = ids[2], .type = "HTTP_CALLS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);
    cbm_store_insert_edge(s, &e3);
    ASSERT_EQ(cbm_store_count_edges(s, "test"), 3);

    /* Delete only CALLS */
    cbm_store_delete_edges_by_type(s, "test", "CALLS");
    ASSERT_EQ(cbm_store_count_edges(s, "test"), 2);

    /* IMPORTS and HTTP_CALLS still exist */
    ASSERT_EQ(cbm_store_count_edges_by_type(s, "test", "IMPORTS"), 1);
    ASSERT_EQ(cbm_store_count_edges_by_type(s, "test", "HTTP_CALLS"), 1);
    ASSERT_EQ(cbm_store_count_edges_by_type(s, "test", "CALLS"), 0);

    cbm_store_close(s);
    PASS();
}

/* ── delete_edges_by_project preserves other projects ──────────── */

TEST(store_edge_delete_by_project_preserves_others) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "alpha", "/tmp/alpha");
    cbm_store_upsert_project(s, "beta", "/tmp/beta");

    /* Create nodes in both projects */
    cbm_node_t na = {
        .project = "alpha", .label = "Function", .name = "A", .qualified_name = "alpha.A"};
    cbm_node_t nb = {
        .project = "alpha", .label = "Function", .name = "B", .qualified_name = "alpha.B"};
    cbm_node_t nc = {
        .project = "beta", .label = "Function", .name = "C", .qualified_name = "beta.C"};
    cbm_node_t nd = {
        .project = "beta", .label = "Function", .name = "D", .qualified_name = "beta.D"};
    int64_t idA = cbm_store_upsert_node(s, &na);
    int64_t idB = cbm_store_upsert_node(s, &nb);
    int64_t idC = cbm_store_upsert_node(s, &nc);
    int64_t idD = cbm_store_upsert_node(s, &nd);

    cbm_edge_t e1 = {
        .project = "alpha", .source_id = idA, .target_id = idB, .type = "CALLS"};
    cbm_edge_t e2 = {
        .project = "beta", .source_id = idC, .target_id = idD, .type = "CALLS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);

    ASSERT_EQ(cbm_store_count_edges(s, "alpha"), 1);
    ASSERT_EQ(cbm_store_count_edges(s, "beta"), 1);

    /* Delete alpha edges only */
    cbm_store_delete_edges_by_project(s, "alpha");
    ASSERT_EQ(cbm_store_count_edges(s, "alpha"), 0);
    ASSERT_EQ(cbm_store_count_edges(s, "beta"), 1);

    cbm_store_close(s);
    PASS();
}

/* ── Edge with special chars in properties_json ────────────────── */

TEST(store_edge_properties_special_chars) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    /* JSON with unicode, quotes, backslashes */
    const char *json = "{\"desc\":\"line1\\nline2\",\"path\":\"/api/v1/users?q=foo&bar=baz\","
                       "\"emoji\":\"\\u2603\"}";
    cbm_edge_t e = {.project = "test",
                    .source_id = ids[0],
                    .target_id = ids[1],
                    .type = "HTTP_CALLS",
                    .properties_json = json};
    cbm_store_insert_edge(s, &e);

    cbm_edge_t *edges = NULL;
    int count = 0;
    cbm_store_find_edges_by_source(s, ids[0], &edges, &count);
    ASSERT_EQ(count, 1);
    ASSERT_NOT_NULL(edges[0].properties_json);
    /* Round-trip should preserve the JSON string */
    ASSERT(strstr(edges[0].properties_json, "line1\\nline2") != NULL);
    ASSERT(strstr(edges[0].properties_json, "/api/v1/users") != NULL);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

/* ── Edge with very long type string ───────────────────────────── */

TEST(store_edge_long_type_string) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    /* 200-char type string */
    char long_type[201];
    memset(long_type, 'X', 200);
    long_type[200] = '\0';

    cbm_edge_t e = {.project = "test",
                    .source_id = ids[0],
                    .target_id = ids[1],
                    .type = long_type};
    int64_t eid = cbm_store_insert_edge(s, &e);
    ASSERT_GT(eid, 0);

    /* Verify it round-trips */
    cbm_edge_t *edges = NULL;
    int count = 0;
    int rc = cbm_store_find_edges_by_source(s, ids[0], &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(edges[0].type, long_type);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

/* ── find_edges_by_source_type with non-existent type ──────────── */

TEST(store_edge_find_source_type_nonexistent) {
    int64_t ids[2];
    cbm_store_t *s = setup_store_with_nodes(2, ids);

    cbm_edge_t e = {.project = "test", .source_id = ids[0], .target_id = ids[1], .type = "CALLS"};
    cbm_store_insert_edge(s, &e);

    cbm_edge_t *edges = NULL;
    int count = 0;
    int rc = cbm_store_find_edges_by_source_type(s, ids[0], "NOPE", &edges, &count);
    ASSERT_EQ(rc, CBM_STORE_OK);
    ASSERT_EQ(count, 0);
    cbm_store_free_edges(edges, count);

    cbm_store_close(s);
    PASS();
}

SUITE(store_edges) {
    RUN_TEST(store_edge_insert_find);
    RUN_TEST(store_edge_dedup);
    RUN_TEST(store_edge_find_by_source_type);
    RUN_TEST(store_edge_find_by_target_type);
    RUN_TEST(store_edge_find_by_type);
    RUN_TEST(store_edge_find_inbound_source_files_by_target_files);
    RUN_TEST(store_edge_count_by_type);
    RUN_TEST(store_edge_delete_by_type);
    RUN_TEST(store_edge_properties_json);
    RUN_TEST(store_edge_null_properties);
    RUN_TEST(store_edge_batch_insert);
    RUN_TEST(store_edge_upsert_batch_in_tx);
    RUN_TEST(store_edge_upsert_batch_stats_skips_empty_conflict_update);
    RUN_TEST(store_edge_insert_batch_stats_in_tx_counts_plain_inserts);
    RUN_TEST(store_edge_insert_or_upsert_batch_falls_back_after_duplicate);
    RUN_TEST(store_edge_open_drops_unused_url_path_index);
    RUN_TEST(store_edge_batch_empty);
    RUN_TEST(store_edge_cascade_on_node_delete);
    /* Edge case tests */
    RUN_TEST(store_edge_batch_insert_zero_count);
    RUN_TEST(store_edge_batch_insert_50);
    RUN_TEST(store_edge_find_source_nonexistent);
    RUN_TEST(store_edge_find_target_nonexistent);
    RUN_TEST(store_edge_find_type_nonexistent);
    RUN_TEST(store_edge_count_empty_project);
    RUN_TEST(store_edge_count_by_type_missing);
    RUN_TEST(store_edge_delete_by_type_preserves_others);
    RUN_TEST(store_edge_delete_by_project_preserves_others);
    RUN_TEST(store_edge_properties_special_chars);
    RUN_TEST(store_edge_long_type_string);
    RUN_TEST(store_edge_find_source_type_nonexistent);
}
