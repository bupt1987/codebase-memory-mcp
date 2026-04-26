/*
 * pipeline_incremental.c — Disk-based incremental re-indexing.
 *
 * Operates on the existing SQLite DB directly (not RAM-first graph buffer).
 * Compares file mtime+size against stored hashes to classify changed/unchanged.
 * Deletes changed files' nodes (edges cascade via ON DELETE CASCADE),
 * re-parses only changed files through passes into a temp graph buffer,
 * then merges new nodes/edges into the disk DB. Persists updated hashes.
 *
 * Called from pipeline.c when a DB with stored hashes already exists.
 */
#include "foundation/constants.h"

enum {
    INCR_RING_BUF = 8,
    INCR_RING_MASK = 7,
    INCR_TS_BUF = 24,
    INCR_WAL_BUF = 1040,
    INCR_PATH_BUF = 4096,
};
#include "pipeline/pipeline.h"
#include "pipeline/artifact.h"
#include <stdio.h>
#include <time.h>
#include "pipeline/pipeline_internal.h"
#include "store/store.h"
#include "graph_buffer/graph_buffer.h"
#include "discover/discover.h"
#include "foundation/log.h"
#include "foundation/hash_table.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdint.h>

/* ── Constants ───────────────────────────────────────────────────── */

#define CBM_MS_PER_SEC 1000.0
#define CBM_NS_PER_MS 1000000.0
#define CBM_NS_PER_SEC 1000000000LL

/* ── Timing helper (same as pipeline.c) ──────────────────────────── */

static double elapsed_ms(struct timespec start) {
    struct timespec now;
    cbm_clock_gettime(CLOCK_MONOTONIC, &now);
    double s = (double)(now.tv_sec - start.tv_sec);
    double ns = (double)(now.tv_nsec - start.tv_nsec);
    return (s * CBM_MS_PER_SEC) + (ns / CBM_NS_PER_MS);
}

/* itoa into static buffer — matches pipeline.c helper */
static const char *itoa_buf(int v) {
    static _Thread_local char buf[INCR_RING_BUF][INCR_TS_BUF];
    static _Thread_local int idx = 0;
    idx = (idx + SKIP_ONE) & INCR_RING_MASK;
    snprintf(buf[idx], sizeof(buf[idx]), "%d", v);
    return buf[idx];
}

static bool incremental_ignore_matches_path(const cbm_gitignore_t *ignore, const char *rel_path) {
    if (!ignore || !rel_path) {
        return false;
    }
    if (cbm_gitignore_matches(ignore, rel_path, false)) {
        return true;
    }

    char dir[INCR_PATH_BUF];
    snprintf(dir, sizeof(dir), "%s", rel_path);
    for (char *slash = strchr(dir, '/'); slash; slash = strchr(slash + SKIP_ONE, '/')) {
        *slash = '\0';
        if (cbm_gitignore_matches(ignore, dir, true)) {
            return true;
        }
        *slash = '/';
    }
    return false;
}

static cbm_gitignore_t *incremental_load_cbmignore(const char *repo_path) {
    if (!repo_path) {
        return NULL;
    }
    char path[INCR_PATH_BUF];
    snprintf(path, sizeof(path), "%s/.cbmignore", repo_path);
    return cbm_gitignore_load(path);
}

/* ── Platform-portable mtime_ns ──────────────────────────────────── */

static int64_t stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return ((int64_t)st->st_mtimespec.tv_sec * CBM_NS_PER_SEC) + (int64_t)st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (int64_t)st->st_mtime * CBM_NS_PER_SEC;
#else
    return ((int64_t)st->st_mtim.tv_sec * CBM_NS_PER_SEC) + (int64_t)st->st_mtim.tv_nsec;
#endif
}

/* ── File classification ─────────────────────────────────────────── */

/* Classify discovered files against stored hashes using mtime+size.
 * Returns a boolean array: changed[i] = true if files[i] needs re-parsing.
 * Caller must free the returned array. */
static bool *classify_files(cbm_file_info_t *files, int file_count, cbm_file_hash_t *stored,
                            int stored_count, int *out_changed, int *out_unchanged) {
    bool *changed = calloc((size_t)file_count, sizeof(bool));
    if (!changed) {
        return NULL;
    }

    int n_changed = 0;
    int n_unchanged = 0;

    /* Build lookup: rel_path -> stored hash */
    CBMHashTable *ht =
        cbm_ht_create(stored_count > 0 ? (size_t)stored_count * PAIR_LEN : CBM_SZ_64);
    for (int i = 0; i < stored_count; i++) {
        cbm_ht_set(ht, stored[i].rel_path, &stored[i]);
    }

    for (int i = 0; i < file_count; i++) {
        cbm_file_hash_t *h = cbm_ht_get(ht, files[i].rel_path);
        if (!h) {
            /* New file */
            changed[i] = true;
            n_changed++;
            continue;
        }

        struct stat st;
        if (stat(files[i].path, &st) != 0) {
            changed[i] = true;
            n_changed++;
            continue;
        }

        if (stat_mtime_ns(&st) != h->mtime_ns || st.st_size != h->size) {
            changed[i] = true;
            n_changed++;
        } else {
            n_unchanged++;
        }
    }

    cbm_ht_free(ht);
    *out_changed = n_changed;
    *out_unchanged = n_unchanged;
    return changed;
}

/* Find stored files that no longer exist on disk. Returns count. */
static int find_deleted_files(cbm_file_info_t *files, int file_count, cbm_file_hash_t *stored,
                              int stored_count, char ***out_deleted) {
    CBMHashTable *current = cbm_ht_create((size_t)file_count * PAIR_LEN);
    for (int i = 0; i < file_count; i++) {
        cbm_ht_set(current, files[i].rel_path, &files[i]);
    }

    int count = 0;
    int cap = CBM_SZ_64;
    char **deleted = malloc((size_t)cap * sizeof(char *));

    for (int i = 0; i < stored_count; i++) {
        if (!cbm_ht_get(current, stored[i].rel_path)) {
            if (count >= cap) {
                cap *= PAIR_LEN;
                char **tmp = realloc(deleted, (size_t)cap * sizeof(char *));
                if (!tmp) {
                    break;
                }
                deleted = tmp;
            }
            deleted[count++] = strdup(stored[i].rel_path);
        }
    }

    cbm_ht_free(current);
    *out_deleted = deleted;
    return count;
}

/* ── Persist file hashes ─────────────────────────────────────────── */

static void persist_hashes(cbm_store_t *store, const char *project, cbm_file_info_t *files,
                           int file_count) {
    for (int i = 0; i < file_count; i++) {
        struct stat st;
        if (stat(files[i].path, &st) != 0) {
            continue;
        }
        cbm_store_upsert_file_hash(store, project, files[i].rel_path, "", stat_mtime_ns(&st),
                                   st.st_size);
    }
}

static void persist_merged_hashes(cbm_store_t *store, const char *project,
                                  cbm_file_hash_t *previous, int previous_count,
                                  cbm_file_info_t *changed_files, int changed_count,
                                  char **deleted_files, int deleted_count) {
    CBMHashTable *skip = cbm_ht_create((size_t)(changed_count + deleted_count + 1) * PAIR_LEN);
    for (int i = 0; i < changed_count; i++) {
        cbm_ht_set(skip, changed_files[i].rel_path, &changed_files[i]);
    }
    for (int i = 0; i < deleted_count; i++) {
        cbm_ht_set(skip, deleted_files[i], deleted_files[i]);
    }

    for (int i = 0; i < previous_count; i++) {
        if (cbm_ht_get(skip, previous[i].rel_path)) {
            continue;
        }
        cbm_store_upsert_file_hash(store, project, previous[i].rel_path,
                                   previous[i].sha256 ? previous[i].sha256 : "",
                                   previous[i].mtime_ns, previous[i].size);
    }

    persist_hashes(store, project, changed_files, changed_count);
    cbm_ht_free(skip);
}

/* ── Registry seed visitor ────────────────────────────────────────── */

/* Callback for cbm_gbuf_foreach_node: add each node to the registry
 * so the resolver can find cross-file symbols during incremental. */
static void registry_visitor(const cbm_gbuf_node_t *node, void *userdata) {
    cbm_registry_t *r = (cbm_registry_t *)userdata;
    cbm_registry_add(r, node->name, node->qualified_name, node->label);
}

static int seed_map_set(int64_t **map, int64_t *cap, int64_t temp_id, int64_t real_id) {
    if (temp_id <= 0) {
        return 0;
    }
    if (temp_id >= *cap) {
        int64_t old_cap = *cap;
        int64_t new_cap = old_cap > 0 ? old_cap : CBM_SZ_16;
        while (temp_id >= new_cap) {
            new_cap *= PAIR_LEN;
        }
        int64_t *grown = realloc(*map, (size_t)new_cap * sizeof(int64_t));
        if (!grown) {
            return CBM_NOT_FOUND;
        }
        memset(grown + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(int64_t));
        *map = grown;
        *cap = new_cap;
    }
    (*map)[temp_id] = real_id;
    return 0;
}

static int seed_graph_from_store(cbm_gbuf_t *gbuf, cbm_registry_t *registry, cbm_node_t *nodes,
                                 int node_count, int64_t **seed_temp_to_real,
                                 int64_t *seed_temp_to_real_count) {
    if (seed_temp_to_real) {
        *seed_temp_to_real = NULL;
    }
    if (seed_temp_to_real_count) {
        *seed_temp_to_real_count = 0;
    }

    int64_t *temp_to_real = NULL;
    int64_t map_cap = node_count > 0 ? (int64_t)node_count + SKIP_ONE : 0;
    if (map_cap > 0) {
        temp_to_real = calloc((size_t)map_cap, sizeof(int64_t));
        if (!temp_to_real) {
            return CBM_NOT_FOUND;
        }
    }

    for (int i = 0; i < node_count; i++) {
        int64_t temp_id = cbm_gbuf_upsert_node(
            gbuf, nodes[i].label, nodes[i].name, nodes[i].qualified_name, nodes[i].file_path,
            nodes[i].start_line, nodes[i].end_line, nodes[i].properties_json);
        if (temp_id <= 0 || seed_map_set(&temp_to_real, &map_cap, temp_id, nodes[i].id) != 0) {
            free(temp_to_real);
            return CBM_NOT_FOUND;
        }
        cbm_registry_add(registry, nodes[i].name, nodes[i].qualified_name, nodes[i].label);
    }

    if (seed_temp_to_real) {
        *seed_temp_to_real = temp_to_real;
    } else {
        free(temp_to_real);
    }
    if (seed_temp_to_real_count) {
        *seed_temp_to_real_count = map_cap;
    }
    return 0;
}

static const char *basename_for_language(const char *rel_path) {
    const char *slash = rel_path ? strrchr(rel_path, '/') : NULL;
    return slash ? slash + SKIP_ONE : rel_path;
}

static void free_owned_file_infos(cbm_file_info_t *files, int count) {
    if (!files) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(files[i].path);
        free(files[i].rel_path);
    }
    free(files);
}

static int append_owned_file_info(cbm_file_info_t **files, int *count, int *cap,
                                  const cbm_file_info_t *src) {
    if (*count >= *cap) {
        int next = (*cap > 0) ? (*cap * PAIR_LEN) : CBM_SZ_16;
        cbm_file_info_t *grown = realloc(*files, (size_t)next * sizeof(cbm_file_info_t));
        if (!grown) {
            return CBM_NOT_FOUND;
        }
        *files = grown;
        *cap = next;
    }

    cbm_file_info_t *dst = &(*files)[*count];
    memset(dst, 0, sizeof(*dst));
    dst->path = strdup(src->path ? src->path : "");
    dst->rel_path = strdup(src->rel_path ? src->rel_path : "");
    if (!dst->path || !dst->rel_path) {
        free(dst->path);
        free(dst->rel_path);
        memset(dst, 0, sizeof(*dst));
        return CBM_NOT_FOUND;
    }
    dst->language = src->language;
    dst->size = src->size;
    (*count)++;
    return 0;
}

static int materialize_repo_file(const char *repo_path, const char *rel_path, cbm_file_info_t *out) {
    if (!repo_path || !rel_path || !out) {
        return CBM_NOT_FOUND;
    }

    char path[INCR_PATH_BUF];
    int n = snprintf(path, sizeof(path), "%s/%s", repo_path, rel_path);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return CBM_NOT_FOUND;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return CBM_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    out->path = strdup(path);
    out->rel_path = strdup(rel_path);
    if (!out->path || !out->rel_path) {
        free(out->path);
        free(out->rel_path);
        memset(out, 0, sizeof(*out));
        return CBM_NOT_FOUND;
    }

    out->language = cbm_language_for_filename(basename_for_language(rel_path));
    if (out->language == CBM_LANG_MATLAB) {
        out->language = cbm_disambiguate_m(path);
    }
    out->size = st.st_size;
    return 0;
}

static cbm_file_info_t *expand_changed_with_inbound_callers(
    cbm_store_t *store, cbm_pipeline_t *p, const char *project, cbm_file_info_t *changed_files,
    int changed_count, char **deleted_files, int deleted_count, int *out_count) {
    int cap = changed_count + CBM_SZ_16;
    int count = 0;
    cbm_file_info_t *expanded = cap > 0 ? calloc((size_t)cap, sizeof(cbm_file_info_t)) : NULL;
    if (cap > 0 && !expanded) {
        *out_count = 0;
        return NULL;
    }

    CBMHashTable *seen =
        cbm_ht_create((size_t)(changed_count + deleted_count + CBM_SZ_16) * PAIR_LEN);
    if (!seen) {
        free(expanded);
        *out_count = 0;
        return NULL;
    }

    for (int i = 0; i < changed_count; i++) {
        if (append_owned_file_info(&expanded, &count, &cap, &changed_files[i]) != 0) {
            free_owned_file_infos(expanded, count);
            cbm_ht_free(seen);
            *out_count = 0;
            return NULL;
        }
        cbm_ht_set(seen, expanded[count - SKIP_ONE].rel_path, &expanded[count - SKIP_ONE]);
    }
    for (int i = 0; i < deleted_count; i++) {
        cbm_ht_set(seen, deleted_files[i], deleted_files[i]);
    }

    int target_count = changed_count + deleted_count;
    const char **target_files =
        target_count > 0 ? calloc((size_t)target_count, sizeof(char *)) : NULL;
    if (target_count > 0 && !target_files) {
        free_owned_file_infos(expanded, count);
        cbm_ht_free(seen);
        *out_count = 0;
        return NULL;
    }
    for (int i = 0; i < changed_count; i++) {
        target_files[i] = changed_files[i].rel_path;
    }
    for (int i = 0; i < deleted_count; i++) {
        target_files[changed_count + i] = deleted_files[i];
    }

    char **source_files = NULL;
    int source_count = 0;
    if (cbm_store_find_inbound_source_files_by_target_files(store, project, target_files,
                                                           target_count, &source_files,
                                                           &source_count) != CBM_STORE_OK) {
        free(target_files);
        free_owned_file_infos(expanded, count);
        cbm_ht_free(seen);
        *out_count = 0;
        return NULL;
    }
    free(target_files);

    for (int i = 0; i < source_count; i++) {
        if (!source_files[i] || cbm_ht_get(seen, source_files[i])) {
            continue;
        }

        cbm_file_info_t caller = {0};
        if (materialize_repo_file(cbm_pipeline_repo_path(p), source_files[i], &caller) == 0) {
            if (append_owned_file_info(&expanded, &count, &cap, &caller) != 0) {
                free(caller.path);
                free(caller.rel_path);
                cbm_store_free_strings(source_files, source_count);
                free_owned_file_infos(expanded, count);
                cbm_ht_free(seen);
                *out_count = 0;
                return NULL;
            }
            cbm_ht_set(seen, expanded[count - SKIP_ONE].rel_path, &expanded[count - SKIP_ONE]);
        }
        free(caller.path);
        free(caller.rel_path);
    }
    cbm_store_free_strings(source_files, source_count);

    cbm_ht_free(seen);
    *out_count = count;
    return expanded;
}

/* Run parallel or sequential extract+resolve for changed files. */
static void run_extract_resolve(cbm_pipeline_ctx_t *ctx, cbm_file_info_t *changed_files, int ci) {
    struct timespec t;

#define MIN_FILES_FOR_PARALLEL_INCR 50
    int worker_count = cbm_default_worker_count(true);
    bool use_parallel = (worker_count > SKIP_ONE && ci > MIN_FILES_FOR_PARALLEL_INCR);

    if (use_parallel) {
        cbm_log_info("incremental.mode", "mode", "parallel", "workers", itoa_buf(worker_count),
                     "changed", itoa_buf(ci));

        _Atomic int64_t shared_ids;
        atomic_init(&shared_ids, cbm_gbuf_next_id(ctx->gbuf));

        CBMFileResult **cache = (CBMFileResult **)calloc(ci, sizeof(CBMFileResult *));
        if (cache) {
            cbm_clock_gettime(CLOCK_MONOTONIC, &t);
            cbm_parallel_extract(ctx, changed_files, ci, cache, &shared_ids, worker_count);
            cbm_gbuf_set_next_id(ctx->gbuf, atomic_load(&shared_ids));
            cbm_log_info("pass.timing", "pass", "incr_extract", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t)));

            cbm_clock_gettime(CLOCK_MONOTONIC, &t);
            cbm_build_registry_from_cache(ctx, changed_files, ci, cache);
            cbm_log_info("pass.timing", "pass", "incr_registry", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t)));

            cbm_clock_gettime(CLOCK_MONOTONIC, &t);
            cbm_parallel_resolve(ctx, changed_files, ci, cache, &shared_ids, worker_count);
            cbm_gbuf_set_next_id(ctx->gbuf, atomic_load(&shared_ids));
            cbm_log_info("pass.timing", "pass", "incr_resolve", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t)));

            for (int j = 0; j < ci; j++) {
                if (cache[j]) {
                    cbm_free_result(cache[j]);
                }
            }
            free(cache);
        }
    } else {
        cbm_log_info("incremental.mode", "mode", "sequential", "changed", itoa_buf(ci));
        cbm_pipeline_pass_definitions(ctx, changed_files, ci);
        cbm_pipeline_pass_calls(ctx, changed_files, ci);
        cbm_pipeline_pass_usages(ctx, changed_files, ci);
        cbm_pipeline_pass_semantic(ctx, changed_files, ci);
    }
}

static int delete_files_from_store(cbm_store_t *store, const char *project,
                                   cbm_file_info_t *changed_files, int changed_count,
                                   char **deleted_files, int deleted_count) {
    if (cbm_store_begin(store) != CBM_STORE_OK) {
        return CBM_NOT_FOUND;
    }

    int total_paths = changed_count + deleted_count;
    const char **paths = total_paths > 0 ? calloc((size_t)total_paths, sizeof(char *)) : NULL;
    if (total_paths > 0 && !paths) {
        cbm_store_rollback(store);
        return CBM_NOT_FOUND;
    }
    for (int i = 0; i < changed_count; i++) {
        paths[i] = changed_files[i].rel_path;
    }
    for (int i = 0; i < deleted_count; i++) {
        paths[changed_count + i] = deleted_files[i];
        cbm_store_delete_file_hash(store, project, deleted_files[i]);
    }

    if (cbm_store_delete_nodes_fts_by_files(store, project, paths, total_paths) != CBM_STORE_OK ||
        cbm_store_delete_nodes_by_files(store, project, paths, total_paths) != CBM_STORE_OK) {
        free(paths);
        cbm_store_rollback(store);
        return CBM_NOT_FOUND;
    }

    free(paths);
    return cbm_store_commit(store) == CBM_STORE_OK ? 0 : CBM_NOT_FOUND;
}

static int purge_stale_nodes_from_gbuf(cbm_gbuf_t *existing, cbm_pipeline_t *p,
                                       cbm_file_info_t *changed_files, int changed_count,
                                       char **deleted_files, int deleted_count) {
    int total_paths = changed_count + deleted_count;
    const char **paths = total_paths > 0 ? calloc((size_t)total_paths, sizeof(char *)) : NULL;
    if (total_paths > 0 && !paths) {
        return CBM_NOT_FOUND;
    }

    cbm_gitignore_t *cbmignore = incremental_load_cbmignore(cbm_pipeline_repo_path(p));
    int path_count = 0;
    int loggable_count = 0;

    for (int i = 0; i < changed_count; i++) {
        const char *rel_path = changed_files[i].rel_path;
        paths[path_count++] = rel_path;
        if (!incremental_ignore_matches_path(cbmignore, rel_path)) {
            loggable_count++;
        }
    }
    for (int i = 0; i < deleted_count; i++) {
        const char *rel_path = deleted_files[i];
        paths[path_count++] = rel_path;
        if (!incremental_ignore_matches_path(cbmignore, rel_path)) {
            loggable_count++;
        }
    }

    int deleted = cbm_gbuf_delete_by_files_logged(existing, paths, path_count, loggable_count > 0);
    cbm_gitignore_free(cbmignore);
    free(paths);
    return deleted >= 0 ? 0 : CBM_NOT_FOUND;
}

static int persist_fast_delta(cbm_store_t *store, const char *project, cbm_file_info_t *changed_files,
                              int changed_count, char **deleted_files, int deleted_count) {
    if (cbm_store_begin(store) != CBM_STORE_OK) {
        return CBM_NOT_FOUND;
    }

    for (int i = 0; i < changed_count; i++) {
        cbm_store_insert_nodes_fts_by_file(store, project, changed_files[i].rel_path);
    }
    persist_hashes(store, project, changed_files, changed_count);
    for (int i = 0; i < deleted_count; i++) {
        cbm_store_delete_file_hash(store, project, deleted_files[i]);
    }

    return cbm_store_commit(store) == CBM_STORE_OK ? 0 : CBM_NOT_FOUND;
}

static int run_incremental_file_set_db_merge(cbm_pipeline_t *p, const char *db_path,
                                             cbm_file_info_t *changed_files, int changed_count,
                                             char **deleted_files, int deleted_count) {
    struct timespec t0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t0);

    const char *project = cbm_pipeline_project_name(p);
    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_log_error("incremental.err", "msg", "open_db_failed", "path", db_path);
        return CBM_NOT_FOUND;
    }

    if (changed_count == 0 && deleted_count == 0) {
        cbm_log_info("incremental.noop", "reason", "no_changes");
        cbm_store_close(store);
        return 0;
    }

    struct timespec t;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    int expanded_count = 0;
    cbm_file_info_t *expanded_files = expand_changed_with_inbound_callers(
        store, p, project, changed_files, changed_count, deleted_files, deleted_count,
        &expanded_count);
    if ((changed_count > 0 || deleted_count > 0) && !expanded_files && expanded_count == 0) {
        cbm_store_close(store);
        return CBM_NOT_FOUND;
    }
    cbm_log_info("incremental.expand", "callers", itoa_buf(expanded_count - changed_count),
                 "total_changed", itoa_buf(expanded_count), "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    if (delete_files_from_store(store, project, expanded_files, expanded_count, deleted_files,
                                deleted_count) != 0) {
        free_owned_file_infos(expanded_files, expanded_count);
        cbm_store_close(store);
        return CBM_NOT_FOUND;
    }
    cbm_log_info("incremental.db_purge", "changed", itoa_buf(expanded_count), "deleted",
                 itoa_buf(deleted_count), "elapsed_ms", itoa_buf((int)elapsed_ms(t)));

    if (expanded_count == 0) {
        if (cbm_pipeline_repo_path(p) && cbm_artifact_exists(cbm_pipeline_repo_path(p))) {
            cbm_artifact_export(db_path, cbm_pipeline_repo_path(p), project, CBM_ARTIFACT_FAST);
        }
        free_owned_file_infos(expanded_files, expanded_count);
        cbm_store_close(store);
        cbm_log_info("incremental.done", "elapsed_ms", itoa_buf((int)elapsed_ms(t0)));
        return 0;
    }

    cbm_node_t *nodes = NULL;
    int node_count = 0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    if (cbm_store_find_nodes_by_project(store, project, &nodes, &node_count) != CBM_STORE_OK) {
        free_owned_file_infos(expanded_files, expanded_count);
        cbm_store_close(store);
        return CBM_NOT_FOUND;
    }

    cbm_gbuf_t *delta = cbm_gbuf_new(project, cbm_pipeline_repo_path(p));
    cbm_registry_t *registry = cbm_registry_new();
    if (!delta || !registry) {
        cbm_registry_free(registry);
        cbm_gbuf_free(delta);
        cbm_store_free_nodes(nodes, node_count);
        free_owned_file_infos(expanded_files, expanded_count);
        cbm_store_close(store);
        return CBM_NOT_FOUND;
    }
    int64_t *seed_temp_to_real = NULL;
    int64_t seed_temp_to_real_count = 0;
    if (seed_graph_from_store(delta, registry, nodes, node_count, &seed_temp_to_real,
                              &seed_temp_to_real_count) != 0) {
        cbm_registry_free(registry);
        cbm_gbuf_free(delta);
        cbm_store_free_nodes(nodes, node_count);
        free_owned_file_infos(expanded_files, expanded_count);
        cbm_store_close(store);
        return CBM_NOT_FOUND;
    }
    cbm_store_free_nodes(nodes, node_count);
    cbm_log_info("incremental.db_seed", "nodes", itoa_buf(cbm_gbuf_node_count(delta)),
                 "elapsed_ms", itoa_buf((int)elapsed_ms(t)));

    cbm_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = cbm_pipeline_repo_path(p),
        .gbuf = delta,
        .registry = registry,
        .cancelled = cbm_pipeline_cancelled_ptr(p),
        .mode = cbm_pipeline_get_mode(p),
    };

    for (int i = 0; i < expanded_count; i++) {
        char *file_qn = cbm_pipeline_fqn_compute(project, expanded_files[i].rel_path, "__file__");
        if (file_qn) {
            cbm_gbuf_upsert_node(delta, "File", expanded_files[i].rel_path, file_qn,
                                 expanded_files[i].rel_path, 0, 0, "{}");
            free(file_qn);
        }
    }

    run_extract_resolve(&ctx, expanded_files, expanded_count);
    cbm_pipeline_pass_k8s(&ctx, expanded_files, expanded_count);
    cbm_pipeline_pass_tests(&ctx, expanded_files, expanded_count);

    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    const char **dirty_paths = calloc((size_t)expanded_count, sizeof(char *));
    if (!dirty_paths) {
        free(seed_temp_to_real);
        cbm_registry_free(registry);
        cbm_gbuf_free(delta);
        free_owned_file_infos(expanded_files, expanded_count);
        cbm_store_close(store);
        return CBM_NOT_FOUND;
    }
    for (int i = 0; i < expanded_count; i++) {
        dirty_paths[i] = expanded_files[i].rel_path;
    }
    int merged_nodes = 0;
    int merged_edges = 0;
    int merge_rc = cbm_gbuf_merge_delta_into_store(
        delta, store, dirty_paths, expanded_count, seed_temp_to_real, seed_temp_to_real_count,
        &merged_nodes, &merged_edges);
    cbm_log_info("incremental.db_merge", "rc", itoa_buf(merge_rc), "nodes",
                 itoa_buf(merged_nodes), "edges", itoa_buf(merged_edges), "seeded",
                 itoa_buf(node_count), "elapsed_ms", itoa_buf((int)elapsed_ms(t)));
    free(dirty_paths);
    free(seed_temp_to_real);
    cbm_registry_free(registry);
    cbm_gbuf_free(delta);
    if (merge_rc != 0) {
        free_owned_file_infos(expanded_files, expanded_count);
        cbm_store_close(store);
        return CBM_NOT_FOUND;
    }

    int persist_rc =
        persist_fast_delta(store, project, expanded_files, expanded_count, deleted_files,
                           deleted_count);
    if (persist_rc != 0) {
        free_owned_file_infos(expanded_files, expanded_count);
        cbm_store_close(store);
        return CBM_NOT_FOUND;
    }

    if (cbm_pipeline_repo_path(p) && cbm_artifact_exists(cbm_pipeline_repo_path(p))) {
        cbm_artifact_export(db_path, cbm_pipeline_repo_path(p), project, CBM_ARTIFACT_FAST);
    }
    free_owned_file_infos(expanded_files, expanded_count);
    cbm_store_close(store);
    cbm_log_info("incremental.done", "elapsed_ms", itoa_buf((int)elapsed_ms(t0)));
    return 0;
}

/* Run post-extraction passes (tests, decorator tags, configlink). */
static void run_postpasses(cbm_pipeline_ctx_t *ctx, cbm_file_info_t *changed_files, int ci,
                           const char *project) {
    struct timespec t;

    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_pipeline_pass_tests(ctx, changed_files, ci);
    cbm_log_info("pass.timing", "pass", "incr_tests", "elapsed_ms", itoa_buf((int)elapsed_ms(t)));

    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_pipeline_pass_decorator_tags(ctx->gbuf, project);
    cbm_log_info("pass.timing", "pass", "incr_decorator_tags", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_pipeline_pass_configlink(ctx);
    cbm_log_info("pass.timing", "pass", "incr_configlink", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    /* SIMILAR_TO + SEMANTICALLY_RELATED edges only in moderate/full modes */
    if (ctx->mode <= CBM_MODE_MODERATE) {
        cbm_clock_gettime(CLOCK_MONOTONIC, &t);
        cbm_pipeline_pass_similarity(ctx);
        cbm_log_info("pass.timing", "pass", "incr_similarity", "elapsed_ms",
                     itoa_buf((int)elapsed_ms(t)));

        cbm_clock_gettime(CLOCK_MONOTONIC, &t);
        cbm_pipeline_pass_semantic_edges(ctx);
        cbm_log_info("pass.timing", "pass", "incr_semantic_edges", "elapsed_ms",
                     itoa_buf((int)elapsed_ms(t)));
    }
}
/* Delete old DB and dump merged graph + hashes to disk. */
static void dump_and_persist(cbm_gbuf_t *gbuf, const char *db_path, const char *project,
                             cbm_file_hash_t *previous_hashes, int previous_hash_count,
                             cbm_file_info_t *changed_files, int changed_count,
                             char **deleted_files, int deleted_count, const char *repo_path) {
    struct timespec t;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);

    cbm_unlink(db_path);
    char wal[INCR_WAL_BUF];
    char shm[INCR_WAL_BUF];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    snprintf(shm, sizeof(shm), "%s-shm", db_path);
    cbm_unlink(wal);
    cbm_unlink(shm);

    int dump_rc = cbm_gbuf_dump_to_sqlite(gbuf, db_path);
    cbm_log_info("incremental.dump", "rc", itoa_buf(dump_rc), "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    cbm_store_t *hash_store = cbm_store_open_path(db_path);
    if (hash_store) {
        persist_merged_hashes(hash_store, project, previous_hashes, previous_hash_count,
                              changed_files, changed_count, deleted_files, deleted_count);

        /* FTS5 rebuild after incremental dump.  The btree dump path bypasses
         * any triggers that could have kept nodes_fts synchronized, so we
         * rebuild from the nodes table here.  See the full-dump path in
         * pipeline.c for the matching logic. */
        cbm_store_exec(hash_store, "INSERT INTO nodes_fts(nodes_fts) VALUES('delete-all');");
        if (cbm_store_exec(hash_store,
                           "INSERT INTO nodes_fts(rowid, name, qualified_name, label, file_path) "
                           "SELECT id, cbm_camel_split(name), qualified_name, label, file_path "
                           "FROM nodes;") != CBM_STORE_OK) {
            cbm_store_exec(hash_store,
                           "INSERT INTO nodes_fts(rowid, name, qualified_name, label, file_path) "
                           "SELECT id, name, qualified_name, label, file_path FROM nodes;");
        }

        cbm_store_close(hash_store);
    }

    /* Auto-update artifact if one already exists (persistence was enabled previously) */
    if (repo_path && cbm_artifact_exists(repo_path)) {
        cbm_artifact_export(db_path, repo_path, project, CBM_ARTIFACT_FAST);
    }
}

/* Apply an already-classified incremental file set. */
static int run_incremental_file_set(cbm_pipeline_t *p, const char *db_path,
                                    cbm_file_info_t *changed_files, int changed_count,
                                    char **deleted_files, int deleted_count,
                                    cbm_file_info_t *hash_files, int hash_file_count) {
    struct timespec t0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t0);

    const char *project = cbm_pipeline_project_name(p);

    /* Open existing disk DB */
    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_log_error("incremental.err", "msg", "open_db_failed", "path", db_path);
        return CBM_NOT_FOUND;
    }

    cbm_file_hash_t *previous_hashes = NULL;
    int previous_hash_count = 0;
    cbm_store_get_file_hashes(store, project, &previous_hashes, &previous_hash_count);

    /* Fast path: nothing changed → skip */
    if (changed_count == 0 && deleted_count == 0) {
        cbm_log_info("incremental.noop", "reason", "no_changes");
        cbm_store_close(store);
        cbm_store_free_file_hashes(previous_hashes, previous_hash_count);
        return 0;
    }

    cbm_log_info("incremental.reparse", "files", itoa_buf(changed_count));

    struct timespec t;

    /* Step 1: Load existing graph into RAM */
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_gbuf_t *existing = cbm_gbuf_new(project, cbm_pipeline_repo_path(p));
    int load_rc = cbm_gbuf_load_from_db(existing, db_path, project);
    cbm_log_info("incremental.load_db", "rc", itoa_buf(load_rc), "nodes",
                 itoa_buf(cbm_gbuf_node_count(existing)), "edges",
                 itoa_buf(cbm_gbuf_edge_count(existing)), "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t)));

    if (load_rc != 0) {
        cbm_log_error("incremental.err", "msg", "load_db_failed");
        cbm_gbuf_free(existing);
        cbm_store_close(store);
        cbm_store_free_file_hashes(previous_hashes, previous_hash_count);
        return CBM_NOT_FOUND;
    }

    cbm_store_close(store);

    /* Step 2: Purge stale nodes */
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    if (purge_stale_nodes_from_gbuf(existing, p, changed_files, changed_count, deleted_files,
                                    deleted_count) != 0) {
        cbm_gbuf_free(existing);
        cbm_store_free_file_hashes(previous_hashes, previous_hash_count);
        return CBM_NOT_FOUND;
    }
    cbm_log_info("incremental.purge", "elapsed_ms", itoa_buf((int)elapsed_ms(t)));

    /* Step 3-5: Registry + extract + resolve */
    cbm_registry_t *registry = cbm_registry_new();
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    cbm_gbuf_foreach_node(existing, registry_visitor, registry);
    cbm_log_info("incremental.registry_seed", "symbols", itoa_buf(cbm_registry_size(registry)),
                 "elapsed_ms", itoa_buf((int)elapsed_ms(t)));

    cbm_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = cbm_pipeline_repo_path(p),
        .gbuf = existing,
        .registry = registry,
        .cancelled = cbm_pipeline_cancelled_ptr(p),
        .mode = cbm_pipeline_get_mode(p),
    };

    for (int i = 0; i < changed_count; i++) {
        char *file_qn = cbm_pipeline_fqn_compute(project, changed_files[i].rel_path, "__file__");
        if (file_qn) {
            cbm_gbuf_upsert_node(existing, "File", changed_files[i].rel_path, file_qn,
                                 changed_files[i].rel_path, 0, 0, "{}");
            free(file_qn);
        }
    }

    run_extract_resolve(&ctx, changed_files, changed_count);
    cbm_pipeline_pass_k8s(&ctx, changed_files, changed_count);
    run_postpasses(&ctx, changed_files, changed_count, project);

    cbm_registry_free(registry);

    /* Step 7: Dump to disk */
    dump_and_persist(existing, db_path, project, previous_hashes, previous_hash_count, hash_files,
                     hash_file_count, deleted_files, deleted_count, cbm_pipeline_repo_path(p));
    cbm_gbuf_free(existing);
    cbm_store_free_file_hashes(previous_hashes, previous_hash_count);

    cbm_log_info("incremental.done", "elapsed_ms", itoa_buf((int)elapsed_ms(t0)));
    return 0;
}

/* ── Incremental pipeline entry points ───────────────────────────── */

int cbm_pipeline_run_incremental_files(cbm_pipeline_t *p, const char *db_path,
                                       cbm_file_info_t *changed_files, int changed_count,
                                       char **deleted_files, int deleted_count,
                                       cbm_file_info_t *all_files, int all_file_count) {
    if (!p || !db_path || changed_count < 0 || deleted_count < 0 || all_file_count < 0) {
        return CBM_NOT_FOUND;
    }
    if ((changed_count > 0 && !changed_files) || (deleted_count > 0 && !deleted_files) ||
        (all_file_count > 0 && !all_files)) {
        return CBM_NOT_FOUND;
    }
    cbm_log_info("incremental.explicit", "changed", itoa_buf(changed_count), "deleted",
                 itoa_buf(deleted_count), "current_files", itoa_buf(all_file_count));
    if (cbm_pipeline_get_mode(p) == CBM_MODE_FAST) {
        return run_incremental_file_set_db_merge(p, db_path, changed_files, changed_count,
                                                 deleted_files, deleted_count);
    }
    return run_incremental_file_set(p, db_path, changed_files, changed_count, deleted_files,
                                    deleted_count, all_files, all_file_count);
}

int cbm_pipeline_run_incremental(cbm_pipeline_t *p, const char *db_path, cbm_file_info_t *files,
                                 int file_count) {
    const char *project = cbm_pipeline_project_name(p);

    /* Open existing disk DB */
    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_log_error("incremental.err", "msg", "open_db_failed", "path", db_path);
        return CBM_NOT_FOUND;
    }

    /* Load stored file hashes */
    cbm_file_hash_t *stored = NULL;
    int stored_count = 0;
    cbm_store_get_file_hashes(store, project, &stored, &stored_count);
    cbm_store_close(store);

    /* Classify files */
    int n_changed = 0;
    int n_unchanged = 0;
    bool *is_changed =
        classify_files(files, file_count, stored, stored_count, &n_changed, &n_unchanged);

    /* Find deleted files */
    char **deleted = NULL;
    int deleted_count = find_deleted_files(files, file_count, stored, stored_count, &deleted);

    cbm_log_info("incremental.classify", "changed", itoa_buf(n_changed), "unchanged",
                 itoa_buf(n_unchanged), "deleted", itoa_buf(deleted_count));

    cbm_store_free_file_hashes(stored, stored_count);

    if (!is_changed) {
        for (int i = 0; i < deleted_count; i++) {
            free(deleted[i]);
        }
        free(deleted);
        return CBM_NOT_FOUND;
    }

    /* Build list of changed files */
    cbm_file_info_t *changed_files =
        (n_changed > 0) ? malloc((size_t)n_changed * sizeof(cbm_file_info_t)) : NULL;
    int ci = 0;
    for (int i = 0; i < file_count; i++) {
        if (is_changed[i]) {
            changed_files[ci++] = files[i];
        }
    }
    free(is_changed);

    int rc = cbm_pipeline_get_mode(p) == CBM_MODE_FAST
                 ? run_incremental_file_set_db_merge(p, db_path, changed_files, ci, deleted,
                                                     deleted_count)
                 : run_incremental_file_set(p, db_path, changed_files, ci, deleted, deleted_count,
                                            files, file_count);

    free(changed_files);
    for (int i = 0; i < deleted_count; i++) {
        free(deleted[i]);
    }
    free(deleted);
    return rc;
}
