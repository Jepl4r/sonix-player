#include "../sonix-player/src/system/library/audiobookdb.c"
#include <assert.h>
#include <sys/wait.h>
// The duration probe is isolated from the database publication tests.
bool has_extension(const char *, const char *);
double durationprobe_seconds(const char *path) {
  return has_extension(path, ".mp3") ? 60 : 0;
}
static double durations[2];
static int duration_count;
static bool collect_duration(const char *path, const char *title,
                             double duration, void *user) {
  assert(duration_count < 2);
  durations[duration_count++] = duration;
  return true;
}
static bool cancel_on_thread_start;
static bool split_cyrillic_album;
static bool save_during_metadata;
static bool exit_during_metadata;
static bool remove_before_stat;
static int cancel_after_calls, metadata_calls;
static char active_book[768];
void thread_be_background(const char *name) {
  if (cancel_on_thread_start)
    scan_cancel = true;
}
void file_identity_read(const char *p, file_identity_t *out) {
  memset(out, 0, sizeof(*out));
}
bool file_identity_changed(const char *p, const file_identity_t *i) {
  return false;
}
bool has_extension(const char *n, const char *e) {
  size_t a = strlen(n), b = strlen(e);
  return a >= b && !strcasecmp(n + a - b, e);
}
bool playlist_is_junk_name(const char *n) { return false; }

mp4_file_t *mp4_open(const char *p) { return NULL; }
int mp4_chapter_count(const mp4_file_t *m) { return 0; }
void mp4_close(mp4_file_t *m) {}
bool id3chap_present(const char *p) {
  if (remove_before_stat)
    unlink(p);
  return false;
}
bool vorbischap_present(const char *p) { return false; }
void metadata_read(const char *p, song_metadata_t *out) {
  if (cancel_after_calls && ++metadata_calls >= cancel_after_calls)
    scan_cancel = true;
  if (exit_during_metadata)
    _exit(42);
  if (save_during_metadata) {
    save_during_metadata = false;
    audiobookdb_save_position(active_book, "current.mp3", 1900);
  }
  memset(out, 0, sizeof(*out));
  snprintf(out->album, sizeof(out->album), "%s",
           split_cyrillic_album && strstr(p, "02.mp3") ? "книга" : "Книга");
}
char *metadata_read_with_description(const char *p, song_metadata_t *out) {
  metadata_read(p, out);
  return NULL;
}
static int scalar(const char *sql) {
  sqlite3_stmt *s;
  assert(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
  assert(sqlite3_step(s) == SQLITE_ROW);
  int v = sqlite3_column_int(s, 0);
  sqlite3_finalize(s);
  return v;
}
static void make_file(const char *p) {
  FILE *f = fopen(p, "wb");
  assert(f);
  fputs("fixture", f);
  fclose(f);
}

static void scan_now(const char *root) {
  snprintf(scan_root, sizeof(scan_root), "%s", root);
  scan_cancel = false;
  scan_thread_func(NULL);
}
static void seed(void) {
  exec("INSERT INTO "
       "AUDIOBOOK_TABLE(path,name,last_played,resume_file,resume_pos) "
       "VALUES('A','A',1,'A.mp3',1800),('B','B',1,'B.mp3',2700)");
}
static bool names_count(const char *name, int count, void *user) {
  *(int *)user += count;
  return true;
}
static int deny_publish(void *u, int action, const char *table, const char *col,
                        const char *database, const char *trigger) {
  return action == SQLITE_INSERT && !strcmp(table, "AUDIOBOOK_PARTS") &&
                 !strcmp(database, "main")
             ? SQLITE_DENY
             : SQLITE_OK;
}
int main(int argc, char **argv) {
  assert(argc == 3);
  if (!strcmp(argv[1], "crash-child")) {
    char path[768];
    snprintf(path, sizeof(path), "%s/test.db", argv[2]);
    assert(audiobookdb_open(path));
    exit_during_metadata = true;
    scan_now(argv[2]);
    return 43;
  }
  char dbpath[768], book[512], a[768], b[768];
  snprintf(dbpath, sizeof(dbpath), "%s/test.db", argv[2]);
  if (!strcmp(argv[1], "speed-column")) {
    // A card indexed before there was a speed column, as version 4 wrote it.
    sqlite3 *old;
    assert(sqlite3_open(dbpath, &old) == SQLITE_OK);
    assert(sqlite3_exec(
               old,
               "CREATE TABLE AUDIOBOOK_TABLE("
               "path TEXT PRIMARY KEY, name TEXT COLLATE NOCASE, size INT, "
               "mtime INT, last_played INT DEFAULT 0, resume_file TEXT, "
               "resume_pos REAL, author TEXT DEFAULT '', series TEXT DEFAULT "
               "'', series_part REAL, added INT DEFAULT 0, folder INT DEFAULT "
               "0, summary TEXT);"
               "PRAGMA user_version=4;"
               "INSERT INTO "
               "AUDIOBOOK_TABLE(path,name,last_played,resume_file,resume_pos) "
               "VALUES('A','A',1,'A.mp3',1900)",
               NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(old);
  }
  if (!strcmp(argv[1], "duration-column") ||
      !strcmp(argv[1], "duration-card")) {
    sqlite3 *old;
    assert(sqlite3_open(dbpath, &old) == SQLITE_OK);
    assert(
        sqlite3_exec(old,
                     "CREATE TABLE AUDIOBOOK_PARTS(path TEXT PRIMARY KEY,book "
                     "TEXT,idx INT,title TEXT); INSERT INTO AUDIOBOOK_PARTS "
                     "VALUES('p','b',0,'t'); PRAGMA user_version=4",
                     NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(old);
  }
  assert(audiobookdb_open(dbpath));
  if (!strcmp(argv[1], "duration-card")) {
    unsigned epoch = audiobookdb_card_epoch();
    assert(audiobookdb_set_part_duration_on_card("p", 12, epoch));
    assert(scalar("SELECT duration FROM AUDIOBOOK_PARTS") == 12);
    audiobookdb_close();
    assert(!audiobookdb_set_part_duration_on_card("p", 24, epoch));
    assert(audiobookdb_open(dbpath));
    assert(!audiobookdb_set_part_duration_on_card("p", 24, epoch));
    assert(scalar("SELECT duration FROM AUDIOBOOK_PARTS") == 12);
    assert(audiobookdb_set_part_duration_on_card("p", 36,
                                                 audiobookdb_card_epoch()));
    assert(scalar("SELECT duration FROM AUDIOBOOK_PARTS") == 36);
  } else if (!strcmp(argv[1], "duration-column")) {
    assert(scalar("SELECT count(*) FROM AUDIOBOOK_PARTS") == 1);
    assert(scalar("SELECT COALESCE(duration,0) FROM AUDIOBOOK_PARTS") == 0);
    assert(scalar("PRAGMA user_version") == 4);
  } else if (!strcmp(argv[1], "cancel")) {
    seed();
    cancel_on_thread_start = true;
    scan_now(argv[2]);
    assert(audiobookdb_count() == 2);
    audiobookdb_close();
    assert(audiobookdb_open(dbpath));
    assert(audiobookdb_count() == 2);
  } else if (!strcmp(argv[1], "stat-error")) {
    seed();
    snprintf(a, sizeof(a), "%s/01.mp3", argv[2]);
    make_file(a);
    remove_before_stat = true;
    scan_now(argv[2]);
    assert(audiobookdb_count() == 2);
  } else if (!strcmp(argv[1], "partial-cancel")) {
    seed();
    for (int i = 0; i < 60; i++) {
      snprintf(a, sizeof(a), "%s/%03d.mp3", argv[2], i);
      make_file(a);
    }
    cancel_after_calls = 55;
    scan_now(argv[2]);
    assert(metadata_calls == 55);
    assert(audiobookdb_count() == 2);
  } else if (!strcmp(argv[1], "stale")) {
    seed();
    write_position_locked("A", "A.mp3", 200, 2);
    write_position_locked("B", "B.mp3", 300, 3);
    write_position_locked("A", "A.mp3", 100, 1);
    assert(scalar("SELECT resume_pos FROM AUDIOBOOK_TABLE WHERE path='A'") ==
           200);
  } else if (!strcmp(argv[1], "write-retry")) {
    write_position_locked("A", "A.mp3", 100, 1);
    seed();
    write_position_locked("A", "A.mp3", 100, 1);
    assert(scalar("SELECT resume_pos FROM AUDIOBOOK_TABLE WHERE path='A'") ==
           100);
  } else if (!strcmp(argv[1], "late-other-book")) {
    seed();
    write_position_locked("B", "B.mp3", 300, 3);
    write_position_locked("A", "A.mp3", 100, 1);
    assert(scalar("SELECT resume_pos FROM AUDIOBOOK_TABLE WHERE path='A'") ==
           100);
  } else if (!strcmp(argv[1], "authors")) {
    seed();
    exec("UPDATE AUDIOBOOK_TABLE SET author=CASE path WHEN 'A' THEN 'Пушкин' "
         "ELSE 'ПУШКИН' END");
    int n = 0;
    assert(audiobookdb_names_for_each(AUDIOBOOK_NAMES_AUTHORS, false, false,
                                      names_count, &n) == 1);
    assert(n == 2);
    audiobookdb_index_t *ix = audiobookdb_index_open(
        AUDIOBOOK_LIST_AUTHOR, "пушкин", AUDIOBOOK_ORDER_NAME, false);
    assert(ix);
    assert(audiobookdb_index_count(ix) == 2);
    audiobookdb_index_close(ix);
  } else if (!strcmp(argv[1], "natural")) {
    assert(natural_compare("Глава 2", "глава 10") < 0);
    assert(natural_compare("ГЛАВА 12", "глава 2") > 0);
    assert(natural_compare("Глава 02", "глава 2") == 0);
  } else if (!strcmp(argv[1], "disc")) {
    assert(is_disc_folder("Диск 1"));
    assert(is_disc_folder("диск02"));
    assert(is_disc_folder("ДИСК-3"));
    assert(!is_disc_folder("Дискуссия 1"));
    assert(is_disc_folder("CD 1"));
  } else if (!strcmp(argv[1], "speed-column")) {
    // The column is there, empty, and nothing else about the card has moved:
    // no rescan asked for, the position where it was.
    assert(scalar("SELECT COALESCE(speed,0) FROM AUDIOBOOK_TABLE WHERE "
                  "path='A'") == 0);
    assert(audiobookdb_get_speed("A") == 0);
    assert(scalar("SELECT resume_pos FROM AUDIOBOOK_TABLE WHERE path='A'") ==
           1900);
    assert(scalar("PRAGMA user_version") == 4);
    assert(!audiobookdb_needs_rescan());
    assert(audiobookdb_count() == 1);
  } else if (!strcmp(argv[1], "scan-error")) {
    seed();
    scan_now("/does/not/exist/sonix-test");
    assert(audiobookdb_count() == 2);
  } else {
    snprintf(book, sizeof(book), "%s/Book", argv[2]);
    assert(mkdir(book, 0700) == 0);
    snprintf(a, sizeof(a), "%s/01.mp3", book);
    snprintf(b, sizeof(b), "%s/02.mp3", book);
    make_file(a);
    make_file(b);
    if (!strcmp(argv[1], "duration-kept") ||
        !strcmp(argv[1], "duration-scan")) {
      scan_now(argv[2]);
      if (!strcmp(argv[1], "duration-kept")) {
        audiobookdb_set_part_duration(b, 123.5);
        scan_now(argv[2]);
      }
      assert(audiobookdb_parts_for_each(book, collect_duration, NULL) == 2);
      assert(durations[0] == 60);
      assert(durations[1] == (!strcmp(argv[1], "duration-kept") ? 123.5 : 60));
    } else if (!strcmp(argv[1], "cyrillic")) {
      split_cyrillic_album = true;
      scan_now(argv[2]);
      assert(audiobookdb_count() == 1);
      assert(scalar("SELECT count(*) FROM AUDIOBOOK_TABLE WHERE folder=1") ==
             1);
    } else if (!strcmp(argv[1], "live-save")) {
      sqlite3_stmt *st;
      assert(sqlite3_prepare_v2(
                 db,
                 "INSERT INTO "
                 "AUDIOBOOK_TABLE(path,name,last_played,resume_file,resume_pos)"
                 " VALUES(?,'Book',1,'current.mp3',1800)",
                 -1, &st, NULL) == SQLITE_OK);
      sqlite3_bind_text(st, 1, book, -1, SQLITE_TRANSIENT);
      assert(sqlite3_step(st) == SQLITE_DONE);
      sqlite3_finalize(st);
      snprintf(active_book, sizeof(active_book), "%s", book);
      save_during_metadata = true;
      scan_now(argv[2]);
      char file[512];
      double seconds = 0;
      assert(audiobookdb_get_position(book, file, sizeof(file), &seconds));
      assert(seconds == 1900);
      assert(audiobookdb_count() == 1);
    } else if (!strcmp(argv[1], "crash")) {
      seed();
      audiobookdb_close();
      pid_t pid = fork();
      assert(pid >= 0);
      if (pid == 0) {
        execl(argv[0], argv[0], "crash-child", argv[2], (char *)NULL);
        _exit(44);
      }
      int status;
      assert(waitpid(pid, &status, 0) == pid);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 42);
      assert(audiobookdb_open(dbpath));
      assert(audiobookdb_count() == 2);
    } else if (!strcmp(argv[1], "publish-failure")) {
      seed();
      audiobookdb_set_speed("A", 1250);
      exec("INSERT INTO AUDIOBOOK_PARTS(path,book,idx,title) "
           "VALUES('A.mp3','A',0,'A')");
      sqlite3_set_authorizer(db, deny_publish, NULL);
      scan_now(argv[2]);
      sqlite3_set_authorizer(db, NULL, NULL);
      assert(audiobookdb_count() == 2);
      assert(scalar("SELECT resume_pos FROM AUDIOBOOK_TABLE WHERE path='A'") ==
             1800);
      assert(scalar("SELECT count(*) FROM AUDIOBOOK_PARTS") == 1);
      assert(audiobookdb_get_speed("A") == 1250);
    } else if (!strcmp(argv[1], "speed-kept")) {
      scan_now(argv[2]);
      assert(audiobookdb_get_speed(book) == 0);
      audiobookdb_save_position(book, b, 1900);
      audiobookdb_set_speed(book, 1250);
      assert(audiobookdb_get_speed(book) == 1250);
      scan_now(argv[2]);
      assert(audiobookdb_count() == 1);
      assert(audiobookdb_get_speed(book) == 1250);
      char file[512];
      double seconds = 0;
      assert(audiobookdb_get_position(book, file, sizeof(file), &seconds));
      assert(seconds == 1900 && !strcmp(file, b));
    } else if (!strcmp(argv[1], "prune")) {
      seed();
      scan_now(argv[2]);
      assert(audiobookdb_count() == 1);
      assert(
          scalar(
              "SELECT count(*) FROM AUDIOBOOK_TABLE WHERE path IN ('A','B')") ==
          0);
      assert(scalar("SELECT count(*) FROM AUDIOBOOK_PARTS") == 2);
    } else {
      fprintf(stderr, "Unknown case %s\n", argv[1]);
      return 2;
    }
  }
  audiobookdb_close();
  printf("PASS %s\n", argv[1]);
  return 0;
}
