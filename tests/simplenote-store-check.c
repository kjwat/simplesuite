#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#include <assert.h>
#include <locale.h>
#include <sys/wait.h>
#include "../simplenote-store.h"

static void fixture_id(char id[33], unsigned number)
{
    snprintf(id, 33, "%032x", number);
}

static SnNote *put(SnStore *store, unsigned number, const char *date, const char *text)
{
    char id[33]; fixture_id(id, number);
    SnNote *note = NULL;
    if (!sn_store_put(store, id, date, date, text, 0, &note)) {
        fprintf(stderr, "%s\n", store->error); abort();
    }
    return note;
}

static void check_discard(void)
{
    char tmp[] = "/tmp/simplenote-discard.XXXXXX";
    assert(mkdtemp(tmp));
    SnStore store;
    assert(sn_store_open(&store, tmp, 2026));
    const char *stamp = "2026-10-05T12:00:00-0400";
    char id[33], body[64], path[PATH_MAX];
    for (unsigned i = 1; i <= 101; i++) {
        snprintf(body, sizeof body, "Keep note %u exactly", i);
        put(&store, i, stamp, body);
    }
    fixture_id(id, 101); strcpy(path, sn_find(&store, id)->batch->path);
    assert(sn_store_remove(&store, id) && !sn_find(&store, id));
    assert(store.count == 1 && access(path, F_OK) != 0);
    fixture_id(id, 50); assert(sn_store_remove(&store, id) && !sn_find(&store, id));
    assert(store.batches[0]->count == 99);
    fixture_id(id, 999); assert(sn_store_remove(&store, id));
    SnNote *note = put(&store, 102, stamp, "Fills the remaining batch slot");
    assert(store.count == 1 && note->batch->count == 100);

    fixture_id(id, 1); note = sn_find(&store, id);
    strcpy(path, note->batch->path);
    snprintf(note->batch->path, sizeof note->batch->path, "%s/missing/output.txt", tmp);
    assert(!sn_store_remove(&store, id));
    assert(sn_find(&store, id) == note && note->batch->count == 100);
    strcpy(note->batch->path, path);

    /* Recovery carries the pre-edit version, even after an autosave. */
    SnNote edit = *note;
    edit.text = "An edit that must be discardable"; edit.len = strlen(edit.text);
    strcpy(edit.updated, "2026-10-05T12:30:00-0400");
    assert(sn_save_edit_draft(&store, &edit, note));
    SnNote *draft = NULL, *original = NULL;
    int known;
    assert(sn_load_edit_draft(&store, &draft, &original, &known));
    assert(known && draft && original && !strcmp(draft->text, edit.text));
    assert(!strcmp(original->text, note->text) && !strcmp(original->updated, stamp));
    sn_free_note(draft); sn_free_note(original);
    assert(sn_save_edit_draft(&store, &edit, NULL));
    assert(sn_load_edit_draft(&store, &draft, &original, &known));
    assert(known && draft && !original); sn_free_note(draft);
    char recovery_path[PATH_MAX]; assert(sn_draft_path(&store, recovery_path));
    assert(sn_write_file(&store, recovery_path, &note, 1));
    assert(sn_load_edit_draft(&store, &draft, &original, &known));
    assert(!known && draft && !original); sn_free_note(draft);
    assert(sn_clear_draft(&store));
    sn_store_close(&store);

    assert(sn_store_open(&store, tmp, 2026));
    assert(store.count == 1 && store.batches[0]->count == 100);
    fixture_id(id, 50); assert(!sn_find(&store, id));
    fixture_id(id, 101); assert(!sn_find(&store, id));
    fixture_id(id, 49); assert(!strcmp(sn_find(&store, id)->text, "Keep note 49 exactly"));
    fixture_id(id, 51); assert(!strcmp(sn_find(&store, id)->text, "Keep note 51 exactly"));
    assert(!unlink(store.batches[0]->path)); sn_store_close(&store);
    snprintf(path, sizeof path, "%s/.lock", tmp); assert(!unlink(path));
    assert(!rmdir(tmp));
}

int main(void)
{
    setlocale(LC_ALL, "");
    check_discard();
    char tmp[] = "/tmp/simplenote-store.XXXXXX";
    assert(mkdtemp(tmp));
    SnStore store;
    assert(sn_store_open(&store, tmp, 2026));
    assert(store.count == 0);
    const char *stamp = "2026-10-05T12:00:00-0400";
    const char *text = "First line\n\nCaf\xc3\xa9 / \xe4\xb8\x96\xe7\x95\x8c / \xf0\x9f\x8e\xad\n\tIndented\n--- Note deadbeef ---\n--- End ---\nBytes: 999\n";
    for (unsigned i = 1; i <= 203; i++) put(&store, i, stamp, text);
    assert(store.count == 3);
    assert(store.batches[0]->count == 100 && store.batches[1]->count == 100 && store.batches[2]->count == 3);
    struct stat st;
    assert(!stat(store.batches[0]->path, &st) && (st.st_mode & 0777) == 0600);
    char first_path[PATH_MAX]; strcpy(first_path, store.batches[0]->path);
    SnNote *edited = put(&store, 50, stamp, "Edited existing note\nwithout making a new record.");
    assert(edited->batch == store.batches[0] && store.count == 3);
    char id[33]; fixture_id(id, 5);
    SnNote *trashed = sn_find(&store, id);
    assert(sn_store_put(&store, id, stamp, stamp, trashed->text, 1, NULL));
    assert(trashed->deleted && store.batches[0]->count == 100);

    /* A second process cannot overwrite the first process's loaded batches. */
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        SnStore other;
        int ok = sn_store_open(&other, tmp, 2026);
        sn_store_close(&other);
        _exit(ok ? 1 : 0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    sn_store_close(&store);

    assert(sn_store_open(&store, tmp, 2026));
    fixture_id(id, 50); edited = sn_find(&store, id);
    assert(edited && !strcmp(edited->text, "Edited existing note\nwithout making a new record."));
    fixture_id(id, 1); assert(!strcmp(sn_find(&store, id)->text, text));
    fixture_id(id, 5); trashed = sn_find(&store, id); assert(trashed->deleted);
    assert(sn_store_put(&store, id, stamp, stamp, text, 0, NULL) && !trashed->deleted);

    /* Failed atomic replacement keeps the previous note in memory and on disk. */
    char saved_path[PATH_MAX]; strcpy(saved_path, edited->batch->path);
    snprintf(edited->batch->path, sizeof edited->batch->path, "%s/missing/output.txt", tmp);
    fixture_id(id, 50);
    assert(!sn_store_put(&store, id, stamp, stamp, "Must not replace the old text", 0, NULL));
    assert(!strcmp(edited->text, "Edited existing note\nwithout making a new record."));
    strcpy(edited->batch->path, saved_path);

    SnNote draft = {0}; fixture_id(draft.id, 204);
    strcpy(draft.created, stamp); strcpy(draft.updated, stamp);
    draft.text = (char *)text; draft.len = strlen(text);
    assert(sn_save_draft(&store, &draft));
    SnNote *recovered = NULL;
    assert(sn_load_draft(&store, &recovered) && recovered && !strcmp(recovered->text, text));
    sn_free_note(recovered);
    sn_store_close(&store);

    /* Rollover moves all complete and partial batches, leaving other files alone. */
    char unrelated[PATH_MAX]; snprintf(unrelated, sizeof unrelated, "%s/README.txt", tmp);
    FILE *file = fopen(unrelated, "w"); assert(file); fputs("leave this alone\n", file); fclose(file);
    assert(sn_store_open(&store, tmp, 2027));
    assert(access(first_path, F_OK) != 0 && access(unrelated, F_OK) == 0);
    for (size_t i = 0; i < store.count; i++) assert(strstr(store.batches[i]->path, "/2026/"));
    put(&store, 204, stamp, "Recovered note still belongs to 2026");
    assert(store.count == 3 && store.batches[2]->count <= 100);
    put(&store, 205, "2027-01-01T09:15:00-0500", "First note of 2027");
    assert(store.count == 4);
    fixture_id(id, 205); SnNote *next_year = sn_find(&store, id);
    assert(next_year && !strstr(next_year->batch->path, "/2027/"));
    assert(sn_clear_draft(&store));
    assert(sn_load_draft(&store, &recovered) && !recovered);
    sn_store_close(&store);

    assert(sn_store_open(&store, tmp, 2028));
    fixture_id(id, 205); assert(strstr(sn_find(&store, id)->batch->path, "/2027/"));
    fixture_id(id, 50); edited = sn_find(&store, id);
    assert(sn_store_put(&store, id, stamp, "2028-01-01T12:00:00-0500", "Edit in the archived year", 0, NULL));
    assert(strstr(edited->batch->path, "/2026/"));
    sn_store_close(&store);

    /* A truncated managed file must stop loading before anything is rewritten. */
    char corrupt[PATH_MAX]; snprintf(corrupt, sizeof corrupt, "%s/2028-01-01-001.txt", tmp);
    file = fopen(corrupt, "w"); assert(file);
    fputs(SN_FILE_HEADER "\n--- Note 00000000000000000000000000000100 ---\nCreated: 2028-01-01T12:00:00-0500\nUpdated: 2028-01-01T12:00:00-0500\nDeleted: 0\nBytes: 50\n\nshort", file);
    long original_size = ftell(file); fclose(file);
    assert(!sn_store_open(&store, tmp, 2028));
    assert(strstr(store.error, "incomplete or invalid"));
    assert(!stat(corrupt, &st) && st.st_size == original_size);
    sn_store_close(&store);
    assert(!unlink(corrupt));

    /* Cleanup only the fixture files, including archived batches. */
    assert(sn_store_open(&store, tmp, 2028));
    for (size_t i = 0; i < store.count; i++) assert(!unlink(store.batches[i]->path));
    sn_store_close(&store);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.lock", tmp); unlink(path);
    snprintf(path, sizeof path, "%s/2026", tmp); rmdir(path);
    snprintf(path, sizeof path, "%s/2027", tmp); rmdir(path);
    unlink(unrelated); assert(!rmdir(tmp));
    puts("OK simplenote storage: 100-note batches, discard removal, Unicode, edits, trash, recovery baselines, locking, annual archives, and failure preservation");
    return 0;
}
