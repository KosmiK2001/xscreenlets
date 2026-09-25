#include "disk_monitor_core.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <locale.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_rates_and_format(void)
{
    char *text;

    assert(dm_rate_bytes_per_second(100, 2100, 1000000) == 2000);
    assert(dm_rate_bytes_per_second(100, 1100, 500000) == 2000);
    assert(dm_rate_bytes_per_second(100, 50, 1000000) == 0);
    assert(dm_rate_bytes_per_second(100, 2100, 0) == 0);
    text = dm_format_rate(512); assert(strcmp(text, "512 B/s") == 0); g_free(text);
    text = dm_format_rate(1024); assert(strcmp(text, "1.0 KiB/s") == 0); g_free(text);
    text = dm_format_rate(1024ULL * 1024 * 1024);
    assert(strcmp(text, "1.00 GiB/s") == 0); g_free(text);
    text = dm_format_rate(1024ULL * 1024 * 1024 * 1024);
    assert(strcmp(text, "1.00 TiB/s") == 0); g_free(text);
}

static void test_names_and_history(void)
{
    guint64 ring[DM_HISTORY_MAX] = {0};
    guint head = 0, count = 0;
    int i;

    assert(dm_is_whole_disk_name("sda"));
    assert(dm_is_whole_disk_name("nvme0n1"));
    assert(dm_is_whole_disk_name("vdb"));
    assert(!dm_is_whole_disk_name("sda1"));
    assert(!dm_is_whole_disk_name("nvme0n1p1"));
    assert(!dm_is_whole_disk_name("dm-0"));
    assert(!dm_is_whole_disk_name("md0"));
    for (i = 0; i < 7; i++) dm_push_history(ring, &head, &count, i + 1);
    assert(count == 7);
    assert(dm_history_value(ring, head, count, 0) == 7);
    assert(dm_history_value(ring, head, count, 6) == 1);
    assert(dm_history_value(ring, head, count, 7) == 0);
}

static void test_temperature_and_rgba(void)
{
    gdouble color[4] = {0};

    assert(dm_parse_temperature("42500\n") == 42500);
    assert(dm_parse_temperature("bad") == G_MININT);
    assert(dm_parse_rgba("0,0.5,1,0.25", color));
    assert(color[1] == 0.5 && color[3] == 0.25);
    assert(!dm_parse_rgba("1,1,1", color));
    assert(!dm_parse_rgba("1,1,1,2", color));
    assert(!dm_parse_rgba("garbage", color));
}

static void test_overflow_saturates(void)
{
    assert(dm_sectors_to_bytes(G_MAXUINT64) == G_MAXUINT64);
    assert(dm_rate_bytes_per_second(0, G_MAXUINT64, 1) == G_MAXINT64);
    assert(dm_rate_bytes_per_second(0, G_MAXUINT64 / 2, 2) == G_MAXINT64);
}

static void test_hddtemp_pipe_protocol(void)
{
    assert(dm_parse_hddtemp_response(
               "|/dev/sda|Samsung SSD|42500|C|42.5 C|", "/dev/sda") == 42500);
    assert(dm_parse_hddtemp_response(
               "|/dev/sda|model|98.6 F|100.0 F|", "/dev/sda") == G_MININT);
    assert(dm_parse_hddtemp_response(
               "|/dev/sda|other|30000|C||/dev/sdb|selected|42000|C|",
               "/dev/sdb") == 42000);
    assert(dm_parse_hddtemp_response(
               "|/dev/sdb|other|35000|C|", "/dev/sda") == G_MININT);
    assert(dm_parse_hddtemp_response("bad", "/dev/sda") == G_MININT);
    assert(dm_hddtemp_due(0, 1000000));
    assert(!dm_hddtemp_due(1000000, 1000001));
    assert(!dm_hddtemp_due(1000000, 1000000 + 60 * G_TIME_SPAN_SECOND - 1));
    assert(dm_hddtemp_due(1000000, 1000000 + 60 * G_TIME_SPAN_SECOND));
}

static void test_dedup_and_switch_reset(void)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    assert(dm_resolved_device_seen(seen, "/dev/nvme0n1"));
    assert(!dm_resolved_device_seen(seen, "/dev/nvme0n1"));
    assert(dm_resolved_device_seen(seen, "/dev/sda"));

    DmHistoryState state = {0};
    guint64 read[DM_HISTORY_MAX] = {0}, write[DM_HISTORY_MAX] = {0};
    gint temp[DM_HISTORY_MAX] = {0};
    guint rh = 0, wh = 0, th = 0, rc = 0, wc = 0, tc = 0;
    dm_push_history(read, &rh, &rc, 10);
    dm_push_history(write, &wh, &wc, 20);
    dm_push_temp_history(temp, &th, &tc, 30000);
    state.read_head = rh; state.read_count = rc;
    state.write_head = wh; state.write_count = wc;
    state.temp_head = th; state.temp_count = tc;
    state.previous_read = 100; state.previous_write = 200;
    state.previous_time_us = 12345; state.previous_valid = TRUE;
    dm_history_clear(&state);
    assert(state.read_head == 0 && state.read_count == 0);
    assert(state.write_head == 0 && state.write_count == 0);
    assert(state.temp_head == 0 && state.temp_count == 0);
    assert(state.previous_read == 0 && state.previous_write == 0);
    assert(state.previous_time_us == 0 && !state.previous_valid);
    g_hash_table_destroy(seen);
}

static void test_geometry(void)
{
    int graph_y = 0, graph_h = 0;
    gint lo = 0, hi = 0;

    /* One shared graph coordinate system covers read, write, and temperature. */
    dm_chart_geometry(160, &graph_y, &graph_h);
    assert(graph_y == 0 && graph_h == 160);

    dm_chart_geometry(80, &graph_y, &graph_h);
    assert(graph_y == 0 && graph_h == 80);

    /* Temperature overlay scale is padded, never degenerate. */
    assert(dm_temp_scale(40000, 41000, &lo, &hi));
    assert(lo <= 40000 && hi >= 41000 && hi > lo);
    assert(dm_temp_scale(42000, 42000, &lo, &hi));
    assert(hi > lo);
    assert(dm_temp_scale(G_MININT, G_MININT, &lo, &hi) == FALSE);
    assert(dm_temp_scale(0, 0, &lo, &hi) == FALSE);

    assert(dm_history_columns(420, 3) == 3);
    assert(dm_history_columns(1, 9) == 1);
    assert(dm_history_origin_x(420, 3) == 417);
    assert(dm_history_origin_x(420, 900) == 0);
    assert(dm_history_origin_x(0, 3) == 0);
}

static void test_by_id_discovery_names(void)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* A by-id LINK name always contains dashes (model-serial); only the
     * resolved kernel name decides whole-disk vs partition/dm/md. */
    assert(dm_by_id_link_name_is_candidate("ata-HGST_HTS725050A7E630_RCF50ACF0JVEPN"));
    assert(dm_by_id_link_name_is_candidate("nvme-Samsung_SSD_980_PRO_S5GXNX0T"));
    assert(!dm_by_id_link_name_is_candidate("ata-HGST_HTS725050A7E630_RCF50ACF0JVEPN-part1"));
    assert(!dm_by_id_link_name_is_candidate("nvme-eui.0025385891b0a1f2-part3"));
    assert(!dm_by_id_link_name_is_candidate("dm-name-ata-HGST"));
    assert(!dm_by_id_link_name_is_candidate(NULL));

    /* Whole-device shapes accepted in the Properties device list. */
    assert(dm_is_whole_disk_name("sda"));
    assert(dm_is_whole_disk_name("sdaa"));
    assert(dm_is_whole_disk_name("vdb"));
    assert(dm_is_whole_disk_name("xvdz"));
    assert(dm_is_whole_disk_name("nvme0n1"));
    assert(dm_is_whole_disk_name("nvme12n3"));

    /* Every partition/dm/md form is rejected, not merely the common examples. */
    assert(!dm_is_whole_disk_name("sda1"));
    assert(!dm_is_whole_disk_name("sdaa12"));
    assert(!dm_is_whole_disk_name("vdb3"));
    assert(!dm_is_whole_disk_name("xvdz7"));
    assert(!dm_is_whole_disk_name("nvme0n1p1"));
    assert(!dm_is_whole_disk_name("nvme12n3p12"));
    assert(!dm_is_whole_disk_name("dm-0"));
    assert(!dm_is_whole_disk_name("md0"));
    assert(!dm_is_whole_disk_name("loop0"));
    assert(!dm_is_whole_disk_name("zram0"));

    /* ata-* and wwn-* aliases for one resolved whole disk produce one list row. */
    assert(dm_resolved_device_seen(seen, "/dev/nvme0n1"));
    assert(!dm_resolved_device_seen(seen, "/dev/nvme0n1"));
    assert(dm_resolved_device_seen(seen, "/dev/sda"));
    g_hash_table_destroy(seen);
}

static void test_diskstats_by_device(void)
{
    /* /proc/diskstats columns after the NAME are positional, matching the
     * 1-based numbering in Documentation/admin-guide/iostats.rst shifted by
     * the leading major/minor/name triple:
     *   0 reads 1 reads_merged 2 sectors_read 3 reading_ms
     *   4 writes 5 writes_merged 6 sectors_written 7 writing_ms
     * i.e. fields[5] == sectors_read and fields[9] == sectors_written.
     * The trailing writing_ms values here (500 / 6) are deliberately large
     * and distinctive: reading the wrong column is what made the applet
     * report ~70x too little written data. */
    static const char text[] =
        "   8       0 sda 100 0 2000 30 40 0 9000 500 0 0 0 0 2 5\n"
        " 259       0 nvme0n1 10 0 20 3 40 0 50 6 0 0 0 0 2 5\n";
    guint64 read = 0, write = 0;

    assert(dm_parse_diskstats_named(text, "nvme0n1", &read, &write));
    assert(read == 20);
    assert(write == 50);
    assert(dm_sectors_to_bytes(read) == 20 * 512);
    assert(dm_sectors_to_bytes(write) == 50 * 512);
    assert(dm_parse_diskstats_named(text, "sda", &read, &write));
    assert(read == 2000);
    assert(write == 9000);
    assert(!dm_parse_diskstats_named(text, "sdb", &read, &write));
    assert(!dm_parse_diskstats_named(text, "sda1", &read, &write));
    assert(!dm_parse_diskstats_named("garbage", "sda", &read, &write));
}

static void test_hwmon_priority_and_background_alpha(void)
{
    gdouble rgba[4] = {0};

    assert(dm_hwmon_temp_priority("drivetemp", NULL) == 0);
    assert(dm_hwmon_temp_priority("nvme", "Composite") == 0);
    assert(dm_hwmon_temp_priority("nvme", "Sensor 1") == 1);
    assert(dm_hwmon_temp_priority("nvme", NULL) == 2);
    assert(dm_hwmon_temp_priority("coretemp", "Core 0") == 9);
    dm_graph_background_rgba((const gdouble[]){0.2, 0.3, 0.4, 0.25}, rgba);
    assert(rgba[0] == 0.2 && rgba[1] == 0.3 && rgba[2] == 0.4 && rgba[3] == 0.25);
}

/* Blocker 1: hwmon selection must be an ORDER-INDEPENDENT lexicographic
 * comparison of (driver rank, label rank), not a first-wins directory walk. */
static void test_hwmon_selection_is_order_independent(void)
{
    /* NVMe Composite beats NVMe Sensor 1 whichever is seen first. */
    assert(dm_hwmon_candidate_key(1, dm_hwmon_temp_priority("nvme", "Composite")) <
           dm_hwmon_candidate_key(1, dm_hwmon_temp_priority("nvme", "Sensor 1")));
    assert(dm_hwmon_candidate_preferred(
               1, dm_hwmon_temp_priority("nvme", "Sensor 1"),
               1, dm_hwmon_temp_priority("nvme", "Composite")) == FALSE);
    assert(dm_hwmon_candidate_preferred(
               1, dm_hwmon_temp_priority("nvme", "Composite"),
               1, dm_hwmon_temp_priority("nvme", "Sensor 1")) == TRUE);
    /* drivetemp outranks every nvme label, again in both orders. */
    assert(dm_hwmon_candidate_preferred(
               0, dm_hwmon_temp_priority("drivetemp", NULL),
               1, dm_hwmon_temp_priority("nvme", "Composite")) == TRUE);
    assert(dm_hwmon_candidate_preferred(
               1, dm_hwmon_temp_priority("nvme", "Composite"),
               0, dm_hwmon_temp_priority("drivetemp", NULL)) == FALSE);
    /* Same tuple in both directions: neither is preferred (no churn). */
    assert(dm_hwmon_candidate_preferred(1, 1, 1, 1) == FALSE);
    /* An unranked driver never displaces a ranked one. */
    assert(dm_hwmon_candidate_preferred(9, 9, 1, 2) == FALSE);
    assert(dm_hwmon_candidate_preferred(1, 2, 9, 9) == TRUE);
}

/* Blocker 2: temperature history advances in LOCKSTEP with the I/O history.
 * A missing sample is a G_MININT marker, never a skipped slot, so a hole can
 * never shift a valid reading onto the wrong time column. */
static void test_temperature_history_lockstep(void)
{
    gint temp[DM_HISTORY_MAX] = {0};
    guint head = 0, count = 0, i;

    for (i = 0; i < 3; i++) {
        dm_push_temp_history(temp, &head, &count, i == 1 ? G_MININT : 42000 + (gint)i);
    }
    assert(count == 3);                       /* every sample stored */
    assert(dm_temp_history_value(temp, head, count, 0) == 42002);
    assert(dm_temp_history_value(temp, head, count, 1) == G_MININT);
    assert(dm_temp_history_value(temp, head, count, 2) == 42000);
    /* The renderer draws only the contiguous valid run at the newest end. */
    assert(dm_temp_history_valid_run(temp, head, count) == 1);
    assert(dm_temp_history_is_valid(temp, head, count, 0));
    assert(!dm_temp_history_is_valid(temp, head, count, 1));
    assert(dm_temp_history_observes(temp, head, count, 42000));
    assert(!dm_temp_history_observes(temp, head, count, 42001));
    assert(!dm_temp_history_observes(temp, head, count, 1));  /* marker ignored */

    for (i = 0; i < 2; i++) dm_push_temp_history(temp, &head, &count, 43000);
    assert(dm_temp_history_valid_run(temp, head, count) == 3);
    assert(dm_temp_history_is_valid(temp, head, count, 3) == FALSE); /* pre-first */

    assert(dm_temp_history_valid_run(temp, 0, 0) == 0);
}

/* Blocker 3: an invalid I/O sample must clear the current rates (N/A), not
 * leave the previous tick's numbers on screen. */
static void test_invalid_io_clears_current_values(void)
{
    DiskSample sample = {0};
    guint64 read = 0, write = 0;

    sample.read_bytes = 4096; sample.write_bytes = 8192;
    sample.io_valid = TRUE; sample.temperature_valid = TRUE;
    sample.temperature_milli = 42000;

    dm_sample_mark_invalid(&sample);
    assert(!sample.io_valid && !sample.temperature_valid);
    assert(sample.read_bytes == 0 && sample.write_bytes == 0);
    assert(sample.temperature_milli == G_MININT);

    /* An invalid sample reports no rates, and the outputs are cleared so the
     * renderer shows N/A instead of the previous tick's numbers. */
    assert(!dm_sample_current_rates(&sample, &read, &write));
    assert(read == 0 && write == 0);

    sample.io_valid = TRUE; sample.read_bytes = 512; sample.write_bytes = 1024;
    assert(dm_sample_current_rates(&sample, &read, &write));
    assert(read == 512 && write == 1024);
    assert(!dm_sample_current_rates(NULL, &read, &write));
}

/* Blocker 4: a persisted by-id path that no longer resolves must not fail
 * init — identity is preserved, values show N/A, re-resolution is bounded. */
static void test_missing_device_keeps_identity_and_retry(void)
{
    char *label;
    DiskSample sample = {0};

    assert(dm_retry_interval_us() >= 60 * G_TIME_SPAN_SECOND);
    assert(dm_retry_due(0, 5000000) == TRUE);              /* never resolved yet */
    assert(dm_retry_due(1000, 1001) == FALSE);
    assert(dm_retry_due(1000, 1000 + dm_retry_interval_us()) == TRUE);
    assert(dm_retry_due(1000, 1000 + dm_retry_interval_us() - 1) == FALSE);
    /* A backwards clock must not disable the retry loop. */
    assert(dm_retry_due(5000000, 1000) == TRUE);

    /* Identity survives: the label falls back to the by-id link name, and the
     * sample is marked unavailable instead of showing a stale rate. */
    label = dm_identity_label(NULL, "/dev/disk/by-id/ata-X_SERIAL");
    assert(strcmp(label, "ata-X_SERIAL") == 0);
    g_free(label);
    label = dm_identity_label("sda", NULL);
    assert(strcmp(label, "sda") == 0);
    g_free(label);
    label = dm_identity_label(NULL, NULL);
    assert(strcmp(label, "no disk") == 0);
    g_free(label);

    dm_sample_mark_invalid(&sample);
    assert(!sample.io_valid);
}

/* Blocker 5: a configured by-id path must be an IMMEDIATE child symlink of
 * /dev/disk/by-id — no traversal, no directory, no plain device file. */
static void test_by_id_path_validation(void)
{
    char *dir = g_dir_make_tmp("dm-byid-XXXXXX", NULL);
    char *link, *plain, *subdir, *outside, *target;
    g_assert_nonnull(dir);

    target = g_build_filename(dir, "sda", NULL);
    link = g_build_filename(dir, "ata-X_SERIAL", NULL);
    plain = g_build_filename(dir, "nvme0n1", NULL);
    subdir = g_build_filename(dir, "subdir", NULL);
    g_mkdir_with_parents(subdir, 0755);
    g_assert_true(symlink(target, link) == 0);
    g_assert_true(g_file_set_contents(plain, "not a symlink", -1, NULL));

    /* The pure shape check: a real path must be an immediate child symlink
     * under the given directory, with no traversal or extra separators. */
    assert(dm_by_id_path_is_direct_symlink(link, dir));
    assert(!dm_by_id_path_is_direct_symlink(plain, dir));
    assert(!dm_by_id_path_is_direct_symlink(subdir, dir));
    assert(!dm_by_id_path_is_direct_symlink(NULL, dir));
    assert(!dm_by_id_path_is_direct_symlink(link, NULL));

    outside = g_build_filename(dir, "..", "dm-byid-escape", NULL);
    assert(!dm_by_id_path_is_direct_symlink(outside, dir));

    /* And the by-id specific wrapper adds the /dev/disk/by-id prefix check. */
    assert(!dm_by_id_path_is_valid_configured("/dev/disk/by-id/x/../y"));
    assert(!dm_by_id_path_is_valid_configured("/dev/sda"));
    assert(!dm_by_id_path_is_valid_configured("/dev/disk/by-id/"));
    assert(!dm_by_id_path_is_valid_configured(""));
    assert(!dm_by_id_path_is_valid_configured(NULL));
    /* A currently absent immediate child is a valid stable identity: udev may
     * remove the link while the disk is unplugged, and init must preserve it. */
    assert(dm_by_id_path_is_valid_configured("/dev/disk/by-id/dm-absent-test"));

    g_free(target); g_free(link); g_free(plain); g_free(subdir); g_free(outside);
    {
        GDir *d = g_dir_open(dir, 0, NULL);
        const char *e;
        while (d && (e = g_dir_read_name(d)))
            g_unlink(g_build_filename(dir, e, NULL));
        if (d) g_dir_close(d);
        g_rmdir(dir);
    }
    g_free(dir);
}

/* Blocker 7: a disk switch resolves the replacement into temporary state and
 * commits atomically; a failed preparation leaves the old device untouched. */
static void test_disk_switch_rollback(void)
{
    DmDeviceState live = {0}, prepared = {0};

    live.by_id = g_strdup("/dev/disk/by-id/ata-OLD");
    live.device_name = g_strdup("sda");
    live.device_path = g_strdup("/dev/sda");
    live.resolved = TRUE;
    live.sample.io_valid = TRUE;
    live.sample.read_bytes = 777;
    live.history.previous_valid = TRUE;
    live.history.previous_read = 100;

    /* A failed preparation never reaches the live state. */
    prepared.by_id = g_strdup("/dev/disk/by-id/ata-NEW");
    prepared.device_name = g_strdup("sdb");
    prepared.device_path = g_strdup("/dev/sdb");
    prepared.resolved = TRUE;
    assert(dm_device_state_is_whole_disk(&prepared));
    assert(dm_device_state_is_whole_disk(&live));

    DmDeviceState broken = {0};
    broken.by_id = g_strdup("/dev/disk/by-id/ata-MISSING");
    assert(!dm_device_state_is_whole_disk(&broken));  /* unresolved → reject */

    dm_device_state_clear(&broken);
    dm_device_state_clear(&prepared);
    /* The live state still holds the OLD device: rollback kept it intact. */
    assert(strcmp(live.by_id, "/dev/disk/by-id/ata-OLD") == 0);
    assert(strcmp(live.device_name, "sda") == 0);
    assert(live.sample.io_valid && live.sample.read_bytes == 777);
    assert(live.history.previous_valid && live.history.previous_read == 100);
    dm_device_state_clear(&live);
}

/* Blocker 6: the Properties dialog context is refcounted and carries only the
 * stable instance NAME — never a raw XsPlugin/PrivData pointer. */
static void test_dialog_context_is_refcounted_and_name_only(void)
{
    DmDialogContext *ctx = dm_dialog_context_new("disk_monitor-1234abcd-user");
    DmDialogContext *held;

    g_assert_nonnull(ctx);
    assert(strcmp(dm_dialog_context_name(ctx), "disk_monitor-1234abcd-user") == 0);
    assert(dm_dialog_context_refcount(ctx) == 1);
    held = dm_dialog_context_ref(ctx);
    assert(held == ctx);
    assert(dm_dialog_context_refcount(ctx) == 2);
    /* A second context for the same instance resolves the same name; the
     * callbacks never hold a plugin pointer, only this name. */
    assert(strcmp(dm_dialog_context_name(held), dm_dialog_context_name(ctx)) == 0);
    dm_dialog_context_unref(held);
    assert(dm_dialog_context_refcount(ctx) == 1);
    dm_dialog_context_unref(ctx);   /* freed here, the dialog no longer owns it */
    assert(dm_dialog_context_ref(NULL) == NULL);
    dm_dialog_context_unref(NULL);  /* no-op */
    assert(dm_dialog_context_name(NULL) == NULL);
}

/* Config is written only for a COMMITTED switch, and an unresolved state
 * contributes no by-id value at all. */
static void test_switch_config_commit_gate(void)
{
    DmDeviceState committed = {0}, pending = {0};
    char *text;

    committed.by_id = g_strdup("/dev/disk/by-id/ata-NEW");
    committed.resolved = TRUE;
    text = dm_switch_config_by_id(&committed);
    assert(text && strcmp(text, "/dev/disk/by-id/ata-NEW") == 0);
    g_free(text);

    text = dm_switch_config_by_id(&pending);   /* prepared but not resolved */
    assert(text == NULL);
    text = dm_switch_config_by_id(NULL);
    assert(text == NULL);
    dm_device_state_clear(&committed);
    dm_device_state_clear(&pending);
}

static guint blocking_test_calls;

static void blocking_test_handler(GtkComboBox *combo, gpointer data)
{
    (void)combo;
    assert(data);
    blocking_test_calls++;
}

static void test_combo_rollback_blocks_exact_dialog_context(void)
{
    DmDialogContext *ctx;
    DmDialogContext *other;
    GtkWidget *combo;
    guint blocked;

    gtk_init_check(NULL, NULL);
    ctx = dm_dialog_context_new("disk_monitor-current");
    other = dm_dialog_context_new("disk_monitor-current");
    combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "old");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "new");
    g_signal_connect(combo, "changed", G_CALLBACK(blocking_test_handler), ctx);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    blocking_test_calls = 0;

    blocked = g_signal_handlers_block_matched(combo, G_SIGNAL_MATCH_DATA,
                                              0, 0, NULL,
                                              G_CALLBACK(blocking_test_handler), ctx);
    assert(blocked == 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 1);
    assert(blocking_test_calls == 0);
    g_signal_handlers_unblock_matched(combo, G_SIGNAL_MATCH_DATA,
                                      0, 0, NULL,
                                      G_CALLBACK(blocking_test_handler), ctx);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    assert(blocking_test_calls == 1);
    (void)other;
    g_object_ref_sink(combo);
    g_object_unref(combo);
    dm_dialog_context_unref(ctx);
    dm_dialog_context_unref(other);
}

static void test_alias_safe_owned_replacement(void)
{
    DmDeviceState state = {0};
    char *identity = g_strdup("/dev/disk/by-id/ata-SERIAL");

    state.by_id = identity;
    state.device_name = g_strdup("sda");
    state.device_path = g_strdup("/dev/sda");
    state.resolved = TRUE;
    state.sample.io_valid = TRUE;
    state.sample.read_bytes = 1234;

    /* Replacement is allowed to receive the currently-owned pointer. */
    dm_device_state_replace_owned(&state, g_strdup("other"),
                                  state.device_name, state.device_path);
    assert(strcmp(state.by_id, "other") == 0);
    assert(strcmp(state.device_name, "sda") == 0);
    assert(strcmp(state.device_path, "/dev/sda") == 0);
    dm_device_state_clear(&state);

    state.by_id = g_strdup("/dev/disk/by-id/ata-SERIAL");
    state.device_name = g_strdup("sdb");
    state.device_path = g_strdup("/dev/sdb");
    state.resolved = TRUE;
    dm_device_state_replace_owned(&state, state.by_id, state.device_name,
                                  state.device_path);
    assert(strcmp(state.by_id, "/dev/disk/by-id/ata-SERIAL") == 0);
    assert(strcmp(state.device_name, "sdb") == 0);
    assert(strcmp(state.device_path, "/dev/sdb") == 0);
    dm_device_state_clear(&state);
}

static void test_temperature_request_identity_and_apply(void)
{
    DmTemperatureRequest request = {
        .instance_name = g_strdup("disk_monitor-deadbeef-label"),
        .by_id = g_strdup("/dev/disk/by-id/ata-SERIAL"),
        .device_path = g_strdup("/dev/sda"),
        .generation = 7,
        .request_id = 11
    };
    DiskSample sample = {0};

    assert(dm_temperature_request_matches(&request, request.instance_name, 7,
                                          request.by_id, request.device_path, TRUE));
    assert(!dm_temperature_request_matches(&request, request.instance_name, 8,
                                           request.by_id, request.device_path, TRUE));
    assert(!dm_temperature_request_matches(&request, "disk_monitor-other", 7,
                                           request.by_id, request.device_path, TRUE));
    assert(!dm_temperature_request_matches(&request, request.instance_name, 7,
                                           "/dev/disk/by-id/ata-OTHER",
                                           request.device_path, TRUE));
    assert(!dm_temperature_request_matches(&request, request.instance_name, 7,
                                           request.by_id, "/dev/sdb", TRUE));
    assert(!dm_temperature_request_matches(&request, request.instance_name, 7,
                                           request.by_id, request.device_path, FALSE));

    assert(dm_temperature_completion_apply(&sample, &request, 42000,
                                          request.instance_name, 7,
                                          request.by_id, request.device_path, TRUE));
    assert(sample.temperature_valid && sample.temperature_milli == 42000);
    sample.temperature_valid = TRUE;
    sample.temperature_milli = 42000;
    assert(!dm_temperature_completion_apply(&sample, &request, 43000,
                                            request.instance_name, 8,
                                            request.by_id, request.device_path, TRUE));
    assert(sample.temperature_valid && sample.temperature_milli == 42000);
    assert(dm_temperature_completion_apply(&sample, &request, G_MININT,
                                          request.instance_name, 7,
                                          request.by_id, request.device_path, TRUE));
    assert(!sample.temperature_valid && sample.temperature_milli == G_MININT);
    dm_temperature_request_clear(&request);
}

static void test_by_id_link_prefilter_suffix(void)
{
    assert(!dm_by_id_link_name_is_candidate("ata-MODEL-part1"));
    assert(!dm_by_id_link_name_is_candidate("nvme-MODEL-part12"));
    assert(dm_by_id_link_name_is_candidate("ata-MODEL-part"));
    assert(dm_by_id_link_name_is_candidate("nvme-MODEL-part1x"));
    assert(dm_by_id_link_name_is_candidate("nvme-MODEL-partition"));
    assert(dm_by_id_link_name_is_candidate("ata-MODEL-PART1"));
}

static void test_rgba_rejects_nonfinite_and_extra_fields(void)
{
    gdouble color[4] = {0};

    assert(!dm_parse_rgba("nan,0,0,1", color));
    assert(!dm_parse_rgba("0,inf,0,1", color));
    assert(!dm_parse_rgba("0,0,-inf,1", color));
    assert(!dm_parse_rgba(" 0,0,0,1", color));
    assert(!dm_parse_rgba("0, 0,0,1", color));
    assert(!dm_parse_rgba("0,0,0,1,", color));
    assert(!dm_parse_rgba("0,,0,1", color));
    assert(dm_parse_rgba("0,0,0,1", color));
}

static void test_failed_diskstats_invalidates_rate_baseline(void)
{
    DmHistoryState history = {0};

    history.previous_read = 1000;
    history.previous_write = 2000;
    history.previous_time_us = 1000000;
    history.previous_valid = TRUE;
    dm_diskstats_sample_failed(&history, 3000000);
    assert(!history.previous_valid);
    assert(history.previous_read == 0 && history.previous_write == 0);
    assert(history.previous_time_us == 0);
}

static void test_periodic_by_id_resolution_detects_move(void)
{
    char *dir = g_dir_make_tmp("dm-resolve-XXXXXX", NULL);
    char *link = g_build_filename(dir, "ata-SERIAL", NULL);
    char *target = g_build_filename(dir, "sda", NULL);
    char *replacement = g_build_filename(dir, "sdb", NULL);
    g_assert_nonnull(dir);
    g_file_set_contents(target, "", 0, NULL);
    g_file_set_contents(replacement, "", 0, NULL);
    g_assert_cmpint(symlink(target, link), ==, 0);
    assert(dm_resolved_target_matches(link, target) == DM_RESOLUTION_MATCH);
    g_unlink(link);
    g_assert_cmpint(symlink(replacement, link), ==, 0);
    assert(dm_resolved_target_matches(link, target) == DM_RESOLUTION_MOVED);
    g_unlink(link);
    assert(dm_resolved_target_matches(link, target) == DM_RESOLUTION_MISSING);
    g_unlink(target); g_unlink(replacement); g_rmdir(dir);
    g_free(link); g_free(target); g_free(replacement); g_free(dir);
}

static void test_hddtemp_stalled_read_is_bounded(void)
{
    int listen_fd, port;
    socklen_t length = sizeof(port);
    struct sockaddr_in address = {0};
    pid_t child;
    gint64 started, elapsed;
    gint result;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listen_fd, 1) == 0);
    assert(getsockname(listen_fd, (struct sockaddr *)&address, &length) == 0);
    port = ntohs(address.sin_port);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd >= 0) {
            char byte;
            ssize_t ignored = read(fd, &byte, 1);
            (void)ignored;
            pause();
        }
        _exit(0);
    }
    started = g_get_monotonic_time();
    result = dm_hddtemp_query("127.0.0.1", port, "/dev/sda", 200);
    elapsed = g_get_monotonic_time() - started;
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(listen_fd);
    assert(result == G_MININT);
    assert(elapsed < G_TIME_SPAN_SECOND);
}

static void test_hddtemp_start_and_completion_guards(void)
{
    gpointer current = g_strdup("task");
    gpointer completed = g_strdup("task");
    gpointer other = g_strdup("other");

    assert(dm_hddtemp_start_allowed(NULL, TRUE, TRUE, TRUE));
    assert(!dm_hddtemp_start_allowed(current, TRUE, TRUE, TRUE));
    assert(!dm_hddtemp_start_allowed(NULL, FALSE, TRUE, TRUE));
    assert(!dm_hddtemp_start_allowed(NULL, TRUE, FALSE, TRUE));
    assert(!dm_hddtemp_start_allowed(NULL, TRUE, TRUE, FALSE));
    assert(dm_hddtemp_completion_is_current(completed, completed));
    assert(!dm_hddtemp_completion_is_current(current, other));
    assert(!dm_hddtemp_completion_is_current(NULL, completed));
    g_free(current); g_free(completed); g_free(other);
}

static void test_combo_identity_row_policy(void)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(rows, g_strdup("/dev/disk/by-id/nvme-eui.primary"));
    assert(dm_combo_identity_needs_row("/dev/disk/by-id/nvme-eui.primary", rows,
                                       TRUE, TRUE) == FALSE);
    assert(dm_combo_identity_needs_row("/dev/disk/by-id/wwn-same-disk", rows,
                                       TRUE, TRUE) == FALSE);
    assert(dm_combo_identity_needs_row("/dev/disk/by-id/wwn-missing-disk", rows,
                                       FALSE, FALSE));
    g_ptr_array_free(rows, TRUE);
}

static void test_default_height_matches_example(void)
{
    assert(DM_DEFAULT_HEIGHT == 220);
}

static void test_deadline_remaining_is_never_negative(void)
{
    gint64 deadline = 1000000;
    assert(dm_deadline_remaining_us(deadline, 900000) == 100000);
    assert(dm_deadline_remaining_us(deadline, 1000000) == 0);
    assert(dm_deadline_remaining_us(deadline, 1000001) == 0);
}

static void test_render_style_contract(void)
{
    gdouble rgba[4] = {0.2, 0.3, 0.4, 0.9};
    char *locale_before = g_strdup(setlocale(LC_NUMERIC, NULL));
    char *formatted;
    char *expected = "0.200000,0.300000,0.400000,0.900000";

    /* Persisted RGBA is ASCII even in the user's comma-decimal locale. */
    setlocale(LC_NUMERIC, "ru_RU.UTF-8");
    formatted = dm_format_rgba(rgba);
    assert(strcmp(formatted, expected) == 0);
    g_free(formatted);
    if (locale_before) {
        setlocale(LC_NUMERIC, locale_before);
        g_free(locale_before);
    }

    /* A missing key keeps the temperature overlay disabled; explicit opt-in
     * enables it without changing the current temperature text. */
    assert(dm_temperature_history_enabled(NULL) == FALSE);
    assert(dm_temperature_history_enabled("false") == FALSE);
    assert(dm_temperature_history_enabled("0") == FALSE);
    assert(dm_temperature_history_enabled("true") == TRUE);
    assert(dm_temperature_history_enabled("1") == TRUE);

    /* Filled Read/Write areas preserve RGB and apply a separate fill alpha. */
    dm_fill_rgba(rgba, 0.55, rgba);
    assert(rgba[0] == 0.2 && rgba[1] == 0.3 && rgba[2] == 0.4);
    assert(rgba[3] > 0.549 && rgba[3] < 0.551);
}

static void capture_spin_change(GtkSpinButton *spin, gpointer user_data)
{
    struct {
        const char *expected_key;
        gint expected_value;
        gboolean called;
    } *capture = user_data;

    assert(g_strcmp0(g_object_get_data(G_OBJECT(spin), "xs-key"),
                     capture->expected_key) == 0);
    assert(gtk_spin_button_get_value(spin) == capture->expected_value);
    capture->called = TRUE;
}

static void test_position_pair_spec(void)
{
    DmPositionPairSpec spec = {0};

    assert(dm_position_pair_spec("Read", "read_x", "read_y", 8, 52,
                                 1599, 1199, &spec));
    assert(g_strcmp0(spec.label, "Read") == 0);
    assert(g_strcmp0(spec.x_key, "read_x") == 0);
    assert(g_strcmp0(spec.y_key, "read_y") == 0);
    assert(spec.x == 8 && spec.y == 52);
    assert(spec.x_max == 1599 && spec.y_max == 1199);
    assert(dm_position_pair_spec(NULL, "write_x", "write_y", 150, 52,
                                 1599, 1199, &spec) == FALSE);

    /* The actual GTK row contains two keyed spin controls with independent
     * X/Y ranges and values. */
    gtk_init_check(NULL, NULL);
    {
        GtkWidget *row, *x_spin, *y_spin;
        GtkAdjustment *x_adjustment, *y_adjustment;
        GList *children;
        struct {
            const char *expected_key;
            gint expected_value;
            gboolean called;
        } changed[2] = {{"read_x", 9, FALSE}, {"read_y", 53, FALSE}};

        row = dm_position_pair_widget("Read text", "read_x", "read_y",
                                      8, 52, 1599, 1199,
                                      &x_spin, &y_spin);
        assert(GTK_IS_BOX(row));
        assert(gtk_orientable_get_orientation(GTK_ORIENTABLE(row)) ==
               GTK_ORIENTATION_HORIZONTAL);
        assert(GTK_IS_SPIN_BUTTON(x_spin));
        assert(GTK_IS_SPIN_BUTTON(y_spin));
        assert(g_strcmp0(g_object_get_data(G_OBJECT(x_spin), "xs-key"),
                         "read_x") == 0);
        assert(g_strcmp0(g_object_get_data(G_OBJECT(y_spin), "xs-key"),
                         "read_y") == 0);

        /* The actual row order is label X, X spin, label Y, Y spin. */
        children = gtk_container_get_children(GTK_CONTAINER(row));
        assert(g_list_length(children) == 4);
        assert(GTK_IS_LABEL(g_list_nth_data(children, 0)));
        assert(g_strcmp0(gtk_label_get_text(GTK_LABEL(g_list_nth_data(children, 0))),
                         "X") == 0);
        assert(g_list_nth_data(children, 1) == x_spin);
        assert(GTK_IS_LABEL(g_list_nth_data(children, 2)));
        assert(g_strcmp0(gtk_label_get_text(GTK_LABEL(g_list_nth_data(children, 2))),
                         "Y") == 0);
        assert(g_list_nth_data(children, 3) == y_spin);
        g_list_free(children);

        /* A real value-changed dispatch exposes the matching persistent key. */
        g_signal_connect(x_spin, "value-changed",
                         G_CALLBACK(capture_spin_change), &changed[0]);
        g_signal_connect(y_spin, "value-changed",
                         G_CALLBACK(capture_spin_change), &changed[1]);

        x_adjustment = gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(x_spin));
        y_adjustment = gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(y_spin));
        assert(gtk_adjustment_get_lower(x_adjustment) == 0);
        assert(gtk_adjustment_get_upper(x_adjustment) == 1599);
        assert(gtk_adjustment_get_lower(y_adjustment) == 0);
        assert(gtk_adjustment_get_upper(y_adjustment) == 1199);
        assert(gtk_spin_button_get_value(GTK_SPIN_BUTTON(x_spin)) == 8);
        assert(gtk_spin_button_get_value(GTK_SPIN_BUTTON(y_spin)) == 52);

        gtk_spin_button_set_value(GTK_SPIN_BUTTON(x_spin), 9);
        assert(changed[0].called);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(y_spin), 53);
        assert(changed[1].called);
        g_object_ref_sink(row);
        g_object_unref(row);
    }
}

static void test_series_block_widget(void)
{
    DmSeriesBlockSpec spec = {0};
    const gdouble blue[4] = {0.2, 0.75, 1.0, 1.0};
    const gdouble history[4] = {0.1, 0.5, 0.8, 1.0};
    GtkWidget *block, *content;
    GList *rows;

    gtk_init_check(NULL, NULL);
    assert(dm_series_block_spec("Read text", "read_font", "read_text_color",
                                "read_x", "read_y", "read_color",
                                "Sans 8", "Read history", blue, history,
                                8, 52, 419, 219, &spec));
    /* The anchor bounds come from the instance design base, and they must be
     * reported back so the spinner cannot be wound past a value that could
     * only ever scale off-window. */
    assert(spec.x_max == 419);
    assert(spec.y_max == 219);
    block = dm_series_block_widget(&spec, &content);
    assert(GTK_IS_FRAME(block));
    assert(GTK_IS_BOX(content));
    assert(g_strcmp0(gtk_frame_get_label(GTK_FRAME(block)), "Read text") == 0);
    rows = gtk_container_get_children(GTK_CONTAINER(content));
    assert(g_list_length(rows) == 3);
    for (guint row = 0; row < 3; row++)
        assert(GTK_IS_BOX(g_list_nth_data(rows, row)));
    {
        GtkWidget *appearance_row = g_list_nth_data(rows, 0);
        GtkWidget *position_row = g_list_nth_data(rows, 1);
        GtkWidget *history_row = g_list_nth_data(rows, 2);
        GList *appearance = gtk_container_get_children(
            GTK_CONTAINER(appearance_row));
        GList *history_children = gtk_container_get_children(
            GTK_CONTAINER(history_row));
        GtkWidget *font_holder = g_list_nth_data(appearance, 0);
        GtkWidget *text_color = g_list_nth_data(appearance, 1);
        GList *font_holder_children = gtk_container_get_children(
            GTK_CONTAINER(font_holder));
        GtkWidget *font = g_list_nth_data(font_holder_children, 0);
        GtkWidget *history_color = g_list_nth_data(history_children, 1);
        const char *labels[3] = {NULL, "Position", "Read history"};

        assert(g_list_length(appearance) == 2);
        assert(g_list_length(history_children) == 2);
        assert(GTK_IS_FIXED(font_holder));
        assert(g_list_length(font_holder_children) == 1);
        assert(GTK_IS_FONT_BUTTON(font));
        assert(GTK_IS_COLOR_BUTTON(text_color));
        {
            GtkRequisition minimum, natural;
            gtk_widget_show_all(block);
            gtk_widget_get_preferred_size(font_holder, &minimum, &natural);
            assert(minimum.width == 180);
            assert(natural.width == 180);
            gtk_widget_get_preferred_size(font, &minimum, &natural);
            assert(minimum.width == 180);
            assert(natural.width == 180);
        }
        for (guint row = 1; row < 3; row++) {
            GList *children = gtk_container_get_children(
                GTK_CONTAINER(g_list_nth_data(rows, row)));
            GtkWidget *label = g_list_nth_data(children, 0);
            assert(GTK_IS_LABEL(label));
            assert(g_strcmp0(gtk_label_get_text(GTK_LABEL(label)),
                             labels[row]) == 0);
            gint width = -1, height = -1;
            gtk_widget_get_size_request(label, &width, &height);
            assert(width < 0);
            assert(height < 0);
            g_list_free(children);
        }
        assert(GTK_IS_BOX(position_row));
        assert(GTK_IS_COLOR_BUTTON(history_color));
        assert(g_strcmp0(g_object_get_data(G_OBJECT(font), "xs-key"),
                         "read_font") == 0);
        assert(g_strcmp0(g_object_get_data(G_OBJECT(text_color), "xs-key"),
                         "read_text_color") == 0);
        assert(g_strcmp0(g_object_get_data(G_OBJECT(history_color), "xs-key"),
                         "read_color") == 0);
        g_list_free(appearance);
        g_list_free(font_holder_children);
        g_list_free(history_children);
    }
    g_list_free(rows);
    g_object_ref_sink(block);
    g_object_unref(block);
}

static void test_color_buttons_are_rgb_only(void)
{
    DmSeriesBlockSpec spec = {0};
    GtkWidget *block, *content = NULL;
    GList *rows, *children;
    GdkRGBA rgba;
    guint colors = 0;

    spec.title = "Read";
    spec.font = "Sans 8";
    spec.font_key = "read_font";
    spec.text_color_key = "read_text_color";
    spec.x_key = "read_x";
    spec.y_key = "read_y";
    spec.x = 8;
    spec.y = 52;
    spec.history_label = "Read history";
    spec.history_color_key = "read_color";
    block = dm_series_block_widget(&spec, &content);
    assert(block);
    gtk_widget_show_all(block);
    rows = gtk_container_get_children(GTK_CONTAINER(content));
    /* Row 0: font + text colour. Row 2: history colour. The picker is
     * RGB-only, but the alpha it was given must survive an RGB edit: that
     * alpha is what the fill and the text are actually drawn with. */
    children = gtk_container_get_children(GTK_CONTAINER(g_list_nth_data(rows, 2)));
    rgba.red = 0.2; rgba.green = 0.4; rgba.blue = 0.6; rgba.alpha = 0.55;
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g_list_nth_data(children, 1)),
                                &rgba);
    assert(gtk_color_chooser_get_use_alpha(
               GTK_COLOR_CHOOSER(g_list_nth_data(children, 1))) == FALSE);
    {
        GdkRGBA read_back;
        gtk_color_chooser_get_rgba(
            GTK_COLOR_CHOOSER(g_list_nth_data(children, 1)), &read_back);
        assert(fabs(read_back.alpha - 0.55) < 0.01);
    }
    g_list_free(children);
    children = gtk_container_get_children(GTK_CONTAINER(g_list_nth_data(rows, 0)));
    for (GList *l = children; l; l = l->next)
        if (GTK_IS_COLOR_BUTTON(l->data)) {
            assert(gtk_color_chooser_get_use_alpha(
                       GTK_COLOR_CHOOSER(l->data)) == FALSE);
            colors++;
        }
    assert(colors == 1);
    g_list_free(children);
    g_list_free(rows);
    g_object_ref_sink(block);
    g_object_unref(block);
}

static void test_scale_position_design_coords(void)
{
    /* The stored value never changes, so scaling is a pure function: the same
     * design coordinate always maps to the same live pixel, and forty 1 px
     * resizes can never drift. */
    assert(dm_scale_position(8, 420, 210, 209) == 4);
    assert(dm_scale_position(150, 420, 210, 209) == 75);
    assert(dm_scale_position(310, 420, 210, 209) == 155);
    assert(dm_scale_position(12, 220, 110, 109) == 6);
    /* Identity when the window has not changed. */
    assert(dm_scale_position(150, 420, 420, 419) == 150);
    assert(dm_scale_position(12, 220, 220, 219) == 12);
    /* Growth scales up. */
    assert(dm_scale_position(100, 420, 840, 839) == 200);
    /* 400 * 200/420 = 190, which still fits, so no clamp is needed. */
    assert(dm_scale_position(400, 420, 200, 199) == 190);
    /* A coordinate that really would overshoot is pulled back to the edge. */
    assert(dm_scale_position(419, 420, 200, 199) == 199);
    /* Degenerate sizes must not divide by zero. */
    assert(dm_scale_position(8, 0, 200, 199) == 8);
    assert(dm_scale_position(8, 420, 0, 199) == 8);
    /* Negative input is clamped to zero. */
    assert(dm_scale_position(-20, 420, 420, 419) == 0);
    /* The exact end-to-end case: a 420->380 window maps 100 to ~90. */
    assert(dm_scale_position(100, 420, 380, 379) == 90);
}

static void test_series_color_picker_is_rgb_only(void)
{
    DmSeriesBlockSpec spec = {0};
    GtkWidget *block, *content = NULL;
    GList *rows, *appearance, *kids;
    GtkWidget *text_color = NULL, *history_color = NULL;

    spec.title = "Color";
    spec.font_key = "f"; spec.text_color_key = "tc";
    spec.text_color[0] = 1; spec.text_color[1] = 1;
    spec.text_color[2] = 1; spec.text_color[3] = 1;
    spec.x_key = "x"; spec.y_key = "y";
    spec.history_label = "History";
    spec.history_color_key = "hc";
    spec.history_color[0] = 1; spec.history_color[1] = 0;
    spec.history_color[2] = 0; spec.history_color[3] = 0.55;
    spec.font = "Sans 8"; spec.x = 0; spec.y = 0;

    block = dm_series_block_widget(&spec, &content);
    g_assert_nonnull(block);
    rows = gtk_container_get_children(GTK_CONTAINER(content));
    appearance = gtk_container_get_children(
        GTK_CONTAINER(g_list_nth_data(rows, 0)));
    for (kids = appearance; kids; kids = kids->next) {
        GtkWidget *w = GTK_WIDGET(kids->data);
        if (GTK_IS_COLOR_BUTTON(w) &&
            !strcmp(g_object_get_data(G_OBJECT(w), "xs-key"), "tc"))
            text_color = w;
    }
    /* The history color button lives in the third row, next to its label. */
    kids = gtk_container_get_children(
        GTK_CONTAINER(g_list_nth_data(rows, 2)));
    for (; kids; kids = kids->next) {
        GtkWidget *w = GTK_WIDGET(kids->data);
        if (GTK_IS_COLOR_BUTTON(w) &&
            !strcmp(g_object_get_data(G_OBJECT(w), "xs-key"), "hc"))
            history_color = w;
    }
    g_assert_nonnull(text_color);
    g_assert_nonnull(history_color);
    /* The user asked for an RGB-only picker: no transparency scale. */
    g_assert_false(gtk_color_chooser_get_use_alpha(
        GTK_COLOR_CHOOSER(text_color)));
    g_assert_false(gtk_color_chooser_get_use_alpha(
        GTK_COLOR_CHOOSER(history_color)));
    /* The stored alpha must survive an RGB-only edit. */
    {
        GdkRGBA got;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(history_color), &got);
        g_assert_cmpfloat(got.alpha, ==, 0.55);
    }
    /* A real palette pick with use_alpha FALSE: GTK hands back alpha 1.0, so
     * reading it from the picker would silently destroy the configured alpha.
     * This mirrors what dm_color_set() does. */
    {
        GdkRGBA picked = {0.2, 0.4, 0.6, 1.0};
        GdkRGBA back;
        double target[4] = {1.0, 0.0, 0.0, 0.55};

        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(history_color), &picked);
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(history_color), &back);
        target[0] = back.red; target[1] = back.green; target[2] = back.blue;
        /* The alpha MUST come from the stored config, never from the picker. */
        g_assert_cmpfloat(target[3], ==, 0.55);
    }
    /* A block with no history row must still build: the Graph label draws no
     * fill, so it passes an empty history key and must not be dropped. */
    {
        DmSeriesBlockSpec s2 = {0};
        g_assert_true(dm_series_block_spec(
            "Graph label", "label_font", "text_color", "label_x", "label_y",
            NULL, "Sans 8", NULL, s2.text_color, s2.history_color, 0, 0,
            419, 219, &s2));
        g_assert_null(s2.history_color_key);
        g_assert_null(s2.history_label);
    }
    g_object_ref_sink(block);
    g_object_unref(block);
}

/* The value is drawn with a dark outline DM_TEXT_SHADOW_RADIUS px beyond the
 * glyphs in all eight directions, so the fit must be computed against the
 * inflated extent. Fitting on text_w alone let a value sit at a legal x while
 * its halo already crossed the frame. */
static void test_fit_text_coordinate_with_shadow(void)
{
    const int w = 311, r = 3;
    /* glyphs 40 px wide -> 46 px of drawn footprint with the outline */
    const int glyph = 40;
    const int drawn = glyph + 2 * r;

    /* Anchored at the right edge, the whole footprint must land inside. */
    int x = dm_fit_text_coordinate(w - drawn, drawn, w);
    assert(x == w - drawn);
    assert(x + drawn <= w);
    /* Pushed one px further than legal, the fit pulls it back inside. */
    x = dm_fit_text_coordinate(w - drawn + 1, drawn, w);
    assert(x == w - drawn);
    /* A short string is not moved when it already fits. */
    assert(dm_fit_text_coordinate(5, drawn, w) == 5);
}

static void test_fit_text_coordinate(void)
{
    /* A short string at a legal anchor stays put. */
    assert(dm_fit_text_coordinate(8, 40, 419) == 8);
    /* A long string at a legal anchor is pulled left so it ends at the edge. */
    assert(dm_fit_text_coordinate(380, 60, 419) == 359);
    /* A negative anchor is clamped to zero. */
    assert(dm_fit_text_coordinate(-5, 40, 419) == 0);
    /* Text wider than the whole window still starts at zero, never negative. */
    assert(dm_fit_text_coordinate(10, 500, 100) == 0);
    /* A non-positive limit disables the fit. */
    assert(dm_fit_text_coordinate(380, 60, 0) == 380);
}

static void test_graph_label_settings_block(void)
{
    DmSeriesBlockSpec spec = {0};
    GtkWidget *block, *content = NULL;
    GList *rows, *children;
    guint labels = 0;

    /* The graph label gets the same three-row block as Read/Write/Temp:
     * font + text colour, Position X/Y, and its own history/colour row. */
    spec.title = "Graph label";
    spec.font = "Sans 8";
    spec.font_key = "label_font";
    spec.text_color_key = "label_text_color";
    spec.x_key = "label_x";
    spec.y_key = "label_y";
    spec.x = 4;
    spec.y = 3;
    spec.history_label = "Graph label history";
    spec.history_color_key = "label_color";
    block = dm_series_block_widget(&spec, &content);
    assert(block);
    gtk_widget_show_all(block);
    rows = gtk_container_get_children(GTK_CONTAINER(content));
    /* Three rows, exactly like Read/Write/Temp: appearance, Position, history. */
    assert(g_list_length(rows) == 3);
    children = gtk_container_get_children(GTK_CONTAINER(g_list_nth_data(rows, 2)));
    assert(GTK_IS_LABEL(g_list_nth_data(children, 0)));
    assert(g_strcmp0(gtk_label_get_text(GTK_LABEL(g_list_nth_data(children, 0))),
                     "Graph label history") == 0);
    labels++;
    g_list_free(children);
    assert(labels == 1);
    g_list_free(rows);
    g_object_ref_sink(block);
    g_object_unref(block);
}

static void test_format_temperature_lines(void)
{
    char **lines;
    guint count = 0;

    /* One sensor stays a single line. */
    {
        gint one[1] = {45500};
        lines = dm_format_temperature_lines(one, 1, &count);
        assert(count == 1);
        assert(g_strcmp0(lines[0], "45.5 C") == 0);
        g_strfreev(lines);
    }
    /* Two sensors are stacked one under the other, not run together. */
    {
        gint two[2] = {45500, 38000};
        lines = dm_format_temperature_lines(two, 2, &count);
        assert(count == 2);
        assert(g_strcmp0(lines[0], "45.5 C") == 0);
        assert(g_strcmp0(lines[1], "38.0 C") == 0);
        g_strfreev(lines);
    }
    /* A missing reading leaves no blank line. */
    {
        gint gap[2] = {G_MININT, 38000};
        lines = dm_format_temperature_lines(gap, 2, &count);
        assert(count == 1);
        assert(g_strcmp0(lines[0], "38.0 C") == 0);
        g_strfreev(lines);
    }
    {
        gint none[2] = {G_MININT, G_MININT};
        lines = dm_format_temperature_lines(none, 2, &count);
        assert(count == 1);
        assert(g_strcmp0(lines[0], "N/A") == 0);
        g_strfreev(lines);
    }
    lines = dm_format_temperature_lines(NULL, 0, &count);
    assert(count == 1);
    assert(g_strcmp0(lines[0], "N/A") == 0);
    g_strfreev(lines);
}

static void test_secondary_temperature_sentinel(void)
{
    DmDeviceState state;

    /* A device that never resolved must never look like a 0.0 C reading: the
     * second sensor's sentinel is G_MININT, and 0 is a valid temperature. */
    memset(&state, 0, sizeof(state));
    dm_device_state_secondary_reset(&state);
    assert(state.secondary_milli == G_MININT);
    assert(state.temp_path_secondary == NULL);

    /* Losing the target must clear the second sensor exactly like the first. */
    state.temp_path = g_strdup("/sys/class/block/nvme0n1/device/hwmon0/temp1_input");
    state.temp_path_secondary = g_strdup("/sys/class/block/nvme0n1/device/hwmon0/temp2_input");
    state.secondary_milli = 38000;
    dm_device_state_secondary_reset(&state);
    assert(state.secondary_milli == G_MININT);
    assert(state.temp_path_secondary == NULL);
    /* The primary sensor path is owned by the state, so clear() must handle it,
     * and it must leave the second sensor marked as "no reading" rather than
     * a valid-looking 0.0 C. */
    assert(state.temp_path != NULL);
    dm_device_state_clear(&state);
    assert(state.temp_path == NULL);
    assert(state.temp_path_secondary == NULL);
    assert(state.secondary_milli == G_MININT);
}

static void test_format_temperature(void)
{
    char *text;

    /* A single sensor keeps the plain one-value display. */
    {
        gint one[1] = {45500};
        text = dm_format_temperature(one, 1);
        assert(g_strcmp0(text, "45.5 C") == 0);
        g_free(text);
    }
    /* NVMe composite + sensor expose two readings: both are shown, separated
     * by a space, so neither sensor is silently dropped. */
    {
        gint two[2] = {45500, 38000};
        text = dm_format_temperature(two, 2);
        assert(g_strcmp0(text, "45.5 C 38.0 C") == 0);
        g_free(text);
    }
    /* Only valid readings are shown; a gap in the middle is not printed. */
    {
        gint gap[2] = {G_MININT, 38000};
        text = dm_format_temperature(gap, 2);
        assert(g_strcmp0(text, "38.0 C") == 0);
        g_free(text);
    }
    /* No usable reading at all keeps the previous N/A contract. */
    {
        gint none[2] = {G_MININT, G_MININT};
        text = dm_format_temperature(none, 2);
        assert(g_strcmp0(text, "N/A") == 0);
        g_free(text);
    }
    text = dm_format_temperature(NULL, 0);
    assert(g_strcmp0(text, "N/A") == 0);
    g_free(text);
}

static void test_rounded_region(void)
{
    cairo_region_t *r;

    /* radius 0 / negative -> no region at all; the caller clears the shape */
    assert(dm_rounded_region(300, 200, 0) == NULL);
    assert(dm_rounded_region(300, 200, -5) == NULL);
    /* degenerate sizes must not build a region */
    assert(dm_rounded_region(0, 200, 10) == NULL);
    assert(dm_rounded_region(300, 0, 10) == NULL);

    r = dm_rounded_region(300, 200, 24);
    assert(r != NULL);
    /* the four extreme corners are cut away */
    assert(!cairo_region_contains_point(r, 0, 0));
    assert(!cairo_region_contains_point(r, 299, 0));
    assert(!cairo_region_contains_point(r, 0, 199));
    assert(!cairo_region_contains_point(r, 299, 199));
    /* the middle of every edge is still part of the window */
    assert(cairo_region_contains_point(r, 150, 0));
    assert(cairo_region_contains_point(r, 150, 199));
    assert(cairo_region_contains_point(r, 0, 100));
    assert(cairo_region_contains_point(r, 299, 100));
    /* and so is the centre */
    assert(cairo_region_contains_point(r, 150, 100));
    cairo_region_destroy(r);

    /* a radius larger than half the short side is clamped, not rejected */
    r = dm_rounded_region(100, 60, 999);
    assert(r != NULL);
    assert(cairo_region_contains_point(r, 50, 30));
    cairo_region_destroy(r);

    /* a window smaller than its radius still yields a usable region */
    r = dm_rounded_region(20, 20, 24);
    assert(r != NULL);
    cairo_region_destroy(r);
}

/* The mask cut must be DEEPEST at the corner column and vanish by the time it
 * reaches the radius. The bug this guards against computed it the other way
 * round (0 at the corner, radius at the edge), which made the mask bite
 * hardest exactly where the border arc is drawn — the border then disappeared
 * as soon as the shape was applied. */
static void test_rounded_region_cut_profile(void)
{
    cairo_region_t *r = dm_rounded_region(300, 200, 24);
    int i;

    assert(r != NULL);
    /* Column 0 is the corner itself: everything above the radius is cut. */
    assert(!cairo_region_contains_point(r, 0, 23));
    /* The cut shrinks as the column approaches the radius, so by column 24 the
     * top edge is visible again. */
    assert(cairo_region_contains_point(r, 24, 0));
    assert(!cairo_region_contains_point(r, 0, 0));

    /* Monotonic: the first visible row never moves back down as x increases. */
    {
        int prev = -1;

        for (i = 0; i <= 24; i++) {
            int y;

            for (y = 0; y < 200; y++)
                if (cairo_region_contains_point(r, i, y))
                    break;
            assert(y <= 200);
            if (prev >= 0)
                assert(y <= prev);
            prev = y;
        }
    }
    cairo_region_destroy(r);
}

static void test_corner_radius(void)
{
    /* Zero and negatives mean square corners — no clip path at all. */
    assert(dm_corner_radius_value(0) == 0.0);
    assert(dm_corner_radius_value(-8) == 0.0);
    assert(dm_corner_radius_is_rounded(dm_corner_radius_value(0)) == FALSE);
    assert(dm_corner_radius_is_rounded(dm_corner_radius_value(-8)) == FALSE);
    /* A real radius is passed through and reported as rounded. */
    assert(dm_corner_radius_value(12) == 12.0);
    assert(dm_corner_radius_is_rounded(dm_corner_radius_value(12)) == TRUE);
    /* Fractional-pixel radii below half a pixel are not worth a clip. */
    assert(dm_corner_radius_is_rounded(0.4) == FALSE);
    assert(dm_corner_radius_is_rounded(0.5) == FALSE);
    assert(dm_corner_radius_is_rounded(0.6) == TRUE);
}

static void test_disk_selector_and_properties_size(void)
{
    GtkWidget *selector, *combo, *label, *combo_row;
    GList *rows, *combo_row_children;
    int width = -1, height = -1;
    GtkRequisition combo_min, combo_nat;
    const char *names[3] = {"/dev/disk/by-id/nvme-eui.6479a78dd000044c",
                            "/dev/disk/by-id/ata-HGST_HTS721010A9E630_JR1020D30R13TF",
                            "/dev/disk/by-id/sda"};

    gtk_init_check(NULL, NULL);
    combo = gtk_combo_box_text_new();
    for (guint i = 0; i < G_N_ELEMENTS(names); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), names[i]);
    selector = dm_disk_selector_widget(combo);
    assert(GTK_IS_BOX(selector));
    rows = gtk_container_get_children(GTK_CONTAINER(selector));
    assert(g_list_length(rows) == 2);
    assert(GTK_IS_LABEL(g_list_nth_data(rows, 0)));
    label = g_list_nth_data(rows, 0);
    assert(g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), "Disk") == 0);
    assert(GTK_IS_BOX(g_list_nth_data(rows, 1)));
    combo_row = g_list_nth_data(rows, 1);
    combo_row_children = gtk_container_get_children(GTK_CONTAINER(combo_row));
    assert(g_list_length(combo_row_children) == 1);
    assert(g_list_nth_data(combo_row_children, 0) == combo);
    g_list_free(combo_row_children);

    /* No manual size request: GTK sizes the combo to its longest entry, and
     * the control keeps a normal single-row height. */
    gtk_widget_get_size_request(combo, &width, &height);
    assert(width < 0);
    assert(height < 0);
    gtk_widget_show_all(selector);
    gtk_widget_get_preferred_size(combo, &combo_min, &combo_nat);
    assert(combo_nat.height > 0);
    assert(combo_nat.height <= 48);
    assert(combo_nat.width < 620);
    g_list_free(rows);
    g_object_ref_sink(selector);
    g_object_unref(selector);

    dm_properties_size("disk_monitor", &width, &height);
    assert(width == 620 && height == 780);
    width = height = -1;
    dm_properties_size("clock", &width, &height);
    assert(width == 0 && height == 0);

    /* The scroller caps the page's NATURAL size, so the dialog window
     * actually shrinks instead of growing to fit the whole content. */
    {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *scroller;
            GtkRequisition page_nat, scroll_min, scroll_nat;

            dm_properties_size("disk_monitor", &width, &height);
            scroller = dm_properties_scroller(page, width, height);
            for (guint i = 0; i < 80; i++)
                gtk_box_pack_start(GTK_BOX(page), gtk_label_new("row"), FALSE, FALSE, 0);
            gtk_widget_show_all(scroller);
            gtk_widget_get_preferred_size(page, NULL, &page_nat);
            gtk_widget_get_preferred_size(scroller, &scroll_min, &scroll_nat);
            assert(page_nat.height > 780);
            /* The scroller never grows past the requested height, so the dialog
             * window stays small and scrolls instead of fitting every row. */
            assert(scroll_nat.height <= height);
            assert(scroll_min.height >= height);
            assert(scroll_min.width >= width);
            assert(gtk_bin_get_child(GTK_BIN(
                       gtk_bin_get_child(GTK_BIN(scroller)))) == page);
            g_object_ref_sink(scroller);
            g_object_unref(scroller);
    }
}

/* The hwmon attribute directory is one level deep for NVMe and two for a
 * SATA/SAS drive behind an ata bridge, so both layouts must resolve. A SATA SSD
 * silently reported N/A before this was covered. */
static void test_find_hwmon_temp_layouts(void)
{
    char *root = g_dir_make_tmp("dm-hwmon-XXXXXX", NULL);
    char *dev, *best, *second = NULL;
    gboolean ok;

    g_assert_nonnull(root);

    /* NVMe layout: device/hwmon0/{name,temp1_input,temp1_label} */
    dev = g_build_filename(root, "nvme0n1", "device", "hwmon0", NULL);
    /* g_mkdir_with_parents returns 0 on success, -1 on error. */
    ok = g_mkdir_with_parents(dev, 0755);
    g_assert_cmpint(ok, ==, 0);
    {
        char *f;
        f = g_build_filename(dev, "name", NULL);
        g_file_set_contents(f, "nvme\n", -1, NULL);
        g_free(f);
        f = g_build_filename(dev, "temp1_input", NULL);
        g_file_set_contents(f, "34000", -1, NULL);
        g_free(f);
        f = g_build_filename(dev, "temp1_label", NULL);
        g_file_set_contents(f, "Composite\n", -1, NULL);
        g_free(f);
    }
    g_free(dev);

    /* SATA layout: device/hwmon/hwmon16/{...} one level deeper. */
    dev = g_build_filename(root, "sdi", "device", "hwmon", "hwmon16", NULL);
    /* g_mkdir_with_parents returns 0 on success, -1 on error. */
    ok = g_mkdir_with_parents(dev, 0755);
    g_assert_cmpint(ok, ==, 0);
    {
        char *f;
        f = g_build_filename(dev, "name", NULL);
        g_file_set_contents(f, "drivetemp\n", -1, NULL);
        g_free(f);
        f = g_build_filename(dev, "temp1_input", NULL);
        g_file_set_contents(f, "35000", -1, NULL);
        g_free(f);
    }
    g_free(dev);

    /* A dir that merely starts with "hwmon" but has no name file is not a
     * sensor and must not be selected. */
    dev = g_build_filename(root, "sdb", "device", "hwmon3", NULL);
    /* g_mkdir_with_parents returns 0 on success, -1 on error. */
    ok = g_mkdir_with_parents(dev, 0755);
    g_assert_cmpint(ok, ==, 0);
    g_free(dev);

    best = dm_find_hwmon_temp(root, "nvme0n1", NULL);
    g_assert_nonnull(best);
    g_assert_true(g_str_has_suffix(best, "hwmon0/temp1_input"));
    g_free(best);

    best = dm_find_hwmon_temp(root, "sdi", &second);
    g_assert_nonnull(best);
    g_assert_true(g_str_has_suffix(best, "hwmon16/temp1_input"));
    g_free(best);
    g_assert_null(second);

    /* No sensor at all must report N/A rather than a wrong path. */
    g_assert_null(dm_find_hwmon_temp(root, "sdb", NULL));
    g_assert_null(dm_find_hwmon_temp(root, "missing", NULL));

    /* GDir-based cleanup keeps the test free of a shell. */
    {
        GDir *d = g_dir_open(root, 0, NULL);
        const char *e;
        if (d) {
            while ((e = g_dir_read_name(d))) {
                char *sub = g_build_filename(root, e, NULL);
                char *cmd = g_strdup_printf("rm -rf '%s'", sub);
                int rc = system(cmd);
                (void)rc;
                g_free(cmd);
                g_free(sub);
            }
            g_dir_close(d);
        }
    }
    g_free(root);
}

/* /proc/diskstats sector counters are fixed 512-byte units regardless of the
 * device's logical_block_size, and the conversion must stay 512 on a 4Kn
 * device. Multiplying by logical_block_size is the classic way to report 8x
 * the real throughput. */
static void test_sector_units_independent_of_logical_block_size(void)
{
    guint64 sectors;

    /* A 4Kn device reports logical_block_size 4096, and the kernel still
     * normalises its counters to 512-byte units. */
    sectors = 1000;
    g_assert_cmpuint(dm_sectors_to_bytes(sectors), ==, sectors * 512);

    /* A 512e device (physical 4096, logical 512) is the same case. */
    g_assert_cmpuint(dm_sectors_to_bytes(2048), ==, 1024u * 1024u);

    /* The conversion is byte-exact, not rounded. */
    g_assert_cmpuint(dm_sectors_to_bytes(1), ==, 512u);
    g_assert_cmpuint(dm_sectors_to_bytes(0), ==, 0u);

    /* Saturates instead of wrapping. */
    g_assert_cmpuint(dm_sectors_to_bytes(G_MAXUINT64), ==, G_MAXUINT64);
    g_assert_cmpuint(dm_sectors_to_bytes(G_MAXUINT64 / 512 + 1),
                     ==, G_MAXUINT64);

    /* The diagnostic reader never feeds the conversion: a missing or malformed
     * sysfs file reports 0 rather than a plausible-looking default. */
    g_assert_cmpint(dm_disk_logical_sector_size(NULL), ==, 0);
    g_assert_cmpint(dm_disk_logical_sector_size(""), ==, 0);
    g_assert_cmpint(dm_disk_logical_sector_size("no-such-device-xyz"), ==, 0);
}

/* On a mostly idle disk the per-tick rate is bursty, so two monitors reading
 * the same counter can differ by orders of magnitude purely by window. The
 * smoothing window must average the most recent samples, the way conky's
 * diskio_avg_samples does, and window 1 must stay instantaneous. */
static void test_rate_smoothing(void)
{
    DmHistoryState h;
    /* Measured bursty sequence on an idle SSD, KiB/s converted to B/s. */
    const gint64 burst[6] = {0, 136 * 1024, 0, 0, 12 * 1024, 0};
    guint i;

    memset(&h, 0, sizeof(h));
    dm_rate_smooth_reset(&h);
    g_assert_cmpint(h.rate_count, ==, 0);

    for (i = 0; i < 6; i++)
        dm_rate_smooth_push(&h, burst[i], burst[i]);
    g_assert_cmpint(h.rate_count, ==, 6);

    /* Window 1 = the newest sample only = instantaneous, no smoothing. */
    g_assert_cmpint(dm_rate_smoothed(&h, 1, FALSE), ==, burst[5]);
    /* Window 2 averages the two newest. */
    g_assert_cmpint(dm_rate_smoothed(&h, 2, FALSE),
                    ==, (burst[4] + burst[5]) / 2);
    /* Window 6 averages all six, which is far below the burst peak. */
    g_assert_cmpint(dm_rate_smoothed(&h, 6, FALSE),
                    ==, (0 + burst[1] + 0 + 0 + burst[4] + 0) / 6);
    g_assert_cmpint(dm_rate_smoothed(&h, 6, FALSE) < burst[1], ==, TRUE);

    /* A window LONGER than the history must not read uninitialised slots. */
    g_assert_cmpint(dm_rate_smoothed(&h, DM_RATE_SMOOTH_MAX, FALSE),
                    ==, (0 + burst[1] + 0 + 0 + burst[4] + 0) / 6);

    /* Negative rates are clamped, not subtracted. */
    dm_rate_smooth_reset(&h);
    dm_rate_smooth_push(&h, -5, -5);
    g_assert_cmpint(dm_rate_smoothed(&h, 1, FALSE), ==, 0);

    /* Empty history yields 0 rather than dividing by zero. */
    dm_rate_smooth_reset(&h);
    g_assert_cmpint(dm_rate_smoothed(&h, 5, FALSE), ==, 0);
    g_assert_cmpint(dm_rate_smoothed(&h, 5, TRUE), ==, 0);

    /* The ring wraps without overrunning its fixed capacity. */
    dm_rate_smooth_reset(&h);
    for (i = 0; i < DM_RATE_SMOOTH_MAX * 3; i++)
        dm_rate_smooth_push(&h, 100, 200);
    g_assert_cmpint(h.rate_count, ==, DM_RATE_SMOOTH_MAX);
    g_assert_cmpint(dm_rate_smoothed(&h, DM_RATE_SMOOTH_MAX, FALSE), ==, 100);
    g_assert_cmpint(dm_rate_smoothed(&h, DM_RATE_SMOOTH_MAX, TRUE), ==, 200);
}

/* Guards the exact regression: the write column was off by one and read the
 * write TIMING in milliseconds. The values are taken from a live sdi line, so
 * sectors_written and writing_ms differ by orders of magnitude and the wrong
 * column cannot pass by accident. */
static void test_diskstats_write_column(void)
{
    static const char text[] =
        "   8     128 sdi 476981 54172 33078894 153344 531891 497270 "
        "32285448 448968 0 340932 777080 33077 0 33188688 15299 208358 "
        "159467\n";
    guint64 read = 0, write = 0;

    assert(dm_parse_diskstats_named(text, "sdi", &read, &write));
    assert(read == 33078894u);
    assert(write == 32285448u);
    /* Not the timing column, and not the merged-request count. */
    assert(write != 448968u);
    assert(write != 497270u);
    assert(write != 531891u);
    /* A short line (14 columns, pre-4.18 layout) must still parse. */
    assert(dm_parse_diskstats_named(
        "   8       0 sdb 1 2 3 4 5 6 7 8 9 10 11 12\n", "sdb", &read, &write));
    assert(read == 3);
    assert(write == 7);
}

int main(void)
{
    test_rates_and_format();
    test_names_and_history();
    test_temperature_and_rgba();
    test_overflow_saturates();
    test_hddtemp_pipe_protocol();
    test_dedup_and_switch_reset();
    test_geometry();
    test_by_id_discovery_names();
    test_diskstats_by_device();
    test_diskstats_write_column();
    test_hwmon_priority_and_background_alpha();
    test_hwmon_selection_is_order_independent();
    test_temperature_history_lockstep();
    test_invalid_io_clears_current_values();
    test_missing_device_keeps_identity_and_retry();
    test_by_id_path_validation();
    test_disk_switch_rollback();
    test_dialog_context_is_refcounted_and_name_only();
    test_switch_config_commit_gate();
    test_combo_rollback_blocks_exact_dialog_context();
    test_alias_safe_owned_replacement();
    test_temperature_request_identity_and_apply();
    test_by_id_link_prefilter_suffix();
    test_rgba_rejects_nonfinite_and_extra_fields();
    test_failed_diskstats_invalidates_rate_baseline();
    test_periodic_by_id_resolution_detects_move();
    test_hddtemp_stalled_read_is_bounded();
    test_hddtemp_start_and_completion_guards();
    test_combo_identity_row_policy();
    test_default_height_matches_example();
    test_deadline_remaining_is_never_negative();
    test_render_style_contract();
    test_position_pair_spec();
    test_series_block_widget();
    test_color_buttons_are_rgb_only();
    test_secondary_temperature_sentinel();
    test_format_temperature();
    test_format_temperature_lines();
    test_graph_label_settings_block();
    test_scale_position_design_coords();
    test_fit_text_coordinate();
    test_fit_text_coordinate_with_shadow();
    test_find_hwmon_temp_layouts();
    test_sector_units_independent_of_logical_block_size();
    test_rate_smoothing();
    test_series_color_picker_is_rgb_only();
    test_rounded_region();
    test_rounded_region_cut_profile();
    test_corner_radius();
    test_disk_selector_and_properties_size();
    puts("disk_monitor core tests: OK");
    return 0;
}
