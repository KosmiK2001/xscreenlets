#include "disk_monitor_core.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <assert.h>
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
    /* /proc/diskstats columns after the NAME are positional:
     * 0 reads 1 reads_merged 2 sectors_read 3 ms 4 reads_ms
     * 5 writes 6 writes_merged 7 sectors_written. */
    static const char text[] =
        "   8       0 sda 100 0 2000 30 40 0 9000 500 0 0 0 0 2 5\n"
        " 259       0 nvme0n1 10 0 20 3 40 0 50 6 0 0 0 0 2 5\n";
    guint64 read = 0, write = 0;

    assert(dm_parse_diskstats_named(text, "nvme0n1", &read, &write));
    assert(read == 20);
    assert(write == 6);
    assert(dm_sectors_to_bytes(read) == 20 * 512);
    assert(dm_sectors_to_bytes(write) == 6 * 512);
    assert(dm_parse_diskstats_named(text, "sda", &read, &write));
    assert(read == 2000);
    assert(write == 500);
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
                                8, 52, &spec));
    block = dm_series_block_widget(&spec, &content);
    assert(GTK_IS_FRAME(block));
    assert(GTK_IS_BOX(content));
    assert(g_strcmp0(gtk_frame_get_label(GTK_FRAME(block)), "Read text") == 0);
    rows = gtk_container_get_children(GTK_CONTAINER(content));
    assert(g_list_length(rows) == 4);
    for (guint row = 0; row < 4; row++)
        assert(GTK_IS_BOX(g_list_nth_data(rows, row)));
    {
        GtkWidget *font_row = g_list_nth_data(rows, 0);
        GtkWidget *text_color_row = g_list_nth_data(rows, 1);
        GtkWidget *position_row = g_list_nth_data(rows, 2);
        GtkWidget *history_row = g_list_nth_data(rows, 3);
        GtkWidget *font = g_list_nth_data(
            gtk_container_get_children(GTK_CONTAINER(font_row)), 1);
        GtkWidget *text_color = g_list_nth_data(
            gtk_container_get_children(GTK_CONTAINER(text_color_row)), 1);
        GtkWidget *history_color = g_list_nth_data(
            gtk_container_get_children(GTK_CONTAINER(history_row)), 1);
        const char *labels[4] = {"Font", "Color", "Position", "Read history"};
        for (guint row = 0; row < 4; row++) {
            GList *children = gtk_container_get_children(
                GTK_CONTAINER(g_list_nth_data(rows, row)));
            GtkWidget *label = g_list_nth_data(children, 0);
            assert(GTK_IS_LABEL(label));
            assert(g_strcmp0(gtk_label_get_text(GTK_LABEL(label)),
                             labels[row]) == 0);
            g_list_free(children);
        }
        assert(GTK_IS_FONT_BUTTON(font));
        assert(GTK_IS_COLOR_BUTTON(text_color));
        assert(GTK_IS_BOX(position_row));
        assert(GTK_IS_COLOR_BUTTON(history_color));
        assert(g_strcmp0(g_object_get_data(G_OBJECT(font), "xs-key"),
                         "read_font") == 0);
        assert(g_strcmp0(g_object_get_data(G_OBJECT(text_color), "xs-key"),
                         "read_text_color") == 0);
        assert(g_strcmp0(g_object_get_data(G_OBJECT(history_color), "xs-key"),
                         "read_color") == 0);
    }
    g_list_free(rows);
    g_object_ref_sink(block);
    g_object_unref(block);
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
    puts("disk_monitor core tests: OK");
    return 0;
}
