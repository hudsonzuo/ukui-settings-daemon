#include "usd-disk-space.h"

#define GIGABYTE                   1024 * 1024 * 1024

#define CHECK_EVERY_X_SECONDS      60

#define DISK_SPACE_ANALYZER        "ukui-disk-usage-analyzer"

#define SETTINGS_HOUSEKEEPING_SCHEMA      "org.ukui.SettingsDaemon.plugins.housekeeping"
#define SETTINGS_FREE_PC_NOTIFY_KEY       "free-percent-notify"
#define SETTINGS_FREE_PC_NOTIFY_AGAIN_KEY "free-percent-notify-again"
#define SETTINGS_FREE_SIZE_NO_NOTIFY      "free-size-gb-no-notify"
#define SETTINGS_MIN_NOTIFY_PERIOD        "min-notify-period"
#define SETTINGS_IGNORE_PATHS             "ignore-paths"

DIskSpace *DIskSpace::mDisk = nullptr;
static unsigned long       *time_read;

DIskSpace::DIskSpace()
{
    ldsm_notified_hash = NULL;
    ldsm_timeout_id = 0;
    ldsm_monitor = NULL;
    free_percent_notify = 0.05;
    free_percent_notify_again = 0.01;
    free_size_gb_no_notify = 2;
    min_notify_period = 10;
    ignore_paths = NULL;
    settings = new QGSettings(SETTINGS_HOUSEKEEPING_SCHEMA);
    dialog = NULL;
}

DIskSpace::~DIskSpace()
{

}

DIskSpace *DIskSpace::DiskSpaceNew()
{
    if(nullptr == mDisk)
        mDisk = new DIskSpace();
    return mDisk;
}


static gint
ldsm_ignore_path_compare (gconstpointer a,
                          gconstpointer b)
{
    return g_strcmp0 ((const gchar *)a, (const gchar *)b);
}


bool DIskSpace::ldsm_mount_is_user_ignore (const char *path)
{
    if (g_slist_find_custom (ignore_paths, path, (GCompareFunc) ldsm_ignore_path_compare) != NULL)
        return TRUE;
    else
        return FALSE;
}

static gboolean
ldsm_is_hash_item_in_ignore_paths (gpointer key,
                                   gpointer value,
                                   gpointer user_data)
{
    return DIskSpace::ldsm_mount_is_user_ignore ((char *)key);
}

void DIskSpace::usdLdsmGetConfig()
{
    gchar **settings_list;

    free_percent_notify =settings->get(SETTINGS_FREE_PC_NOTIFY_KEY).toDouble();
    if (free_percent_notify >= 1 || free_percent_notify < 0) {
        /* FIXME define min and max in gschema! */
        g_warning ("Invalid configuration of free_percent_notify: %f\n" \
                   "Using sensible default", free_percent_notify);
        free_percent_notify = 0.05;
    }

    free_percent_notify_again = settings->get(SETTINGS_FREE_PC_NOTIFY_AGAIN_KEY).toDouble();
    if (free_percent_notify_again >= 1 || free_percent_notify_again < 0) {
        /* FIXME define min and max in gschema! */
        g_warning ("Invalid configuration of free_percent_notify_again: %f\n" \
                   "Using sensible default\n", free_percent_notify_again);
        free_percent_notify_again = 0.01;
    }

    free_size_gb_no_notify = settings->get(SETTINGS_FREE_SIZE_NO_NOTIFY).toInt();
    min_notify_period = settings->get(SETTINGS_MIN_NOTIFY_PERIOD).toInt();

    if (ignore_paths != NULL) {
        g_slist_foreach (ignore_paths, (GFunc) g_free, NULL);
        g_slist_free (ignore_paths);
        ignore_paths = NULL;
    }

    settings_list =settings->getStrv(SETTINGS_IGNORE_PATHS);
    if (settings_list != NULL) {
        unsigned int i;

        for (i = 0; settings_list[i] != NULL; i++) {
                if (settings_list[i] != NULL)
                        ignore_paths = g_slist_prepend (ignore_paths, g_strdup (settings_list[i]));
        }

        /* Make sure we dont leave stale entries in ldsm_notified_hash */
        g_hash_table_foreach_remove (ldsm_notified_hash,
                                     ldsm_is_hash_item_in_ignore_paths, NULL);

        g_strfreev (settings_list);
    }
}

static void
ldsm_free_mount_info (gpointer data)
{
    LdsmMountInfo *mount = (LdsmMountInfo *)data;

    g_return_if_fail (mount != NULL);
    g_unix_mount_free (mount->mount);
    g_free (mount);
}

void DIskSpace::usdLdsmUpdateConfig(QString key)
{
    usdLdsmGetConfig();
}

static gboolean
ldsm_is_hash_item_not_in_mounts (gpointer key,
                                 gpointer value,
                                 gpointer user_data)
{
    GList *l;

    for (l = (GList *) user_data; l != NULL; l = l->next) {
        GUnixMountEntry *mount = (GUnixMountEntry *)l->data;
        const char *path;

        path = g_unix_mount_get_mount_path (mount);

        if (strcmp (path, (char *)key) == 0)
                return FALSE;
    }

    return TRUE;
}

static gboolean
is_in (const gchar *value, const gchar *set[])
{
    int i;
    for (i = 0; set[i] != NULL; i++)
    {
          if (strcmp (set[i], value) == 0)
            return TRUE;
    }
    return FALSE;
}


bool DIskSpace::ldsm_mount_should_ignore (GUnixMountEntry *mount)
{
    const gchar *fs, *device, *path;

    path = g_unix_mount_get_mount_path (mount);
    if (ldsm_mount_is_user_ignore (path))
            return TRUE;

    /* This is borrowed from GLib and used as a way to determine
     * which mounts we should ignore by default. GLib doesn't
     * expose this in a way that allows it to be used for this
     * purpose
     */

    /* We also ignore network filesystems */

    const gchar *ignore_fs[] = {
            "adfs",
            "afs",
            "auto",
            "autofs",
            "autofs4",
            "cifs",
            "cxfs",
            "devfs",
            "devpts",
            "ecryptfs",
            "fdescfs",
            "gfs",
            "gfs2",
            "kernfs",
            "linprocfs",
            "linsysfs",
            "lustre",
            "lustre_lite",
            "ncpfs",
            "nfs",
            "nfs4",
            "nfsd",
            "ocfs2",
            "proc",
            "procfs",
            "ptyfs",
            "rpc_pipefs",
            "selinuxfs",
            "smbfs",
            "sysfs",
            "tmpfs",
            "usbfs",
            "zfs",
            NULL
    };
    const gchar *ignore_devices[] = {
            "none",
            "sunrpc",
            "devpts",
            "nfsd",
            "/dev/loop",
            "/dev/vn",
            NULL
    };

    fs = g_unix_mount_get_fs_type (mount);
    device = g_unix_mount_get_device_path (mount);

    if (is_in (fs, ignore_fs))
        return TRUE;

    if (is_in (device, ignore_devices))
        return TRUE;

    return FALSE;
}

bool  DIskSpace::ldsm_mount_has_space (LdsmMountInfo *mount)
{
    double free_space;

    free_space = (double) mount->buf.f_bavail / (double) mount->buf.f_blocks;
    /* enough free space, nothing to do */
    if (free_space > free_percent_notify)
            return true;

    if (((gint64) mount->buf.f_frsize * (gint64) mount->buf.f_bavail) > ((gint64) free_size_gb_no_notify * GIGABYTE))
            return true;

    /* If we got here, then this volume is low on space */
    return false;
}

static bool
ldsm_mount_is_virtual (LdsmMountInfo *mount)
{
    if (mount->buf.f_blocks == 0) {
        /* Filesystems with zero blocks are virtual */
        return true;
    }

    return false;
}

static gchar*
ldsm_get_fs_id_for_path (const gchar *path)
{
    GFile *file;
    GFileInfo *fileinfo;
    gchar *attr_id_fs;

    file = g_file_new_for_path (path);
    fileinfo = g_file_query_info (file, G_FILE_ATTRIBUTE_ID_FILESYSTEM,
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                  NULL, NULL);
    if (fileinfo) {
        attr_id_fs = g_strdup (g_file_info_get_attribute_string (fileinfo, G_FILE_ATTRIBUTE_ID_FILESYSTEM));
        g_object_unref (fileinfo);
    } else {
        attr_id_fs = NULL;
    }

    g_object_unref (file);

    return attr_id_fs;
}

static gboolean
ldsm_mount_has_trash (LdsmMountInfo *mount)
{
    const gchar *user_data_dir;
    gchar *user_data_attr_id_fs;
    gchar *path_attr_id_fs;
    gboolean mount_uses_user_trash = FALSE;
    gchar *trash_files_dir;
    gboolean has_trash = FALSE;
    GDir *dir;
    const gchar *path;

    user_data_dir = g_get_user_data_dir ();
    user_data_attr_id_fs = ldsm_get_fs_id_for_path (user_data_dir);

    path = g_unix_mount_get_mount_path (mount->mount);
    path_attr_id_fs = ldsm_get_fs_id_for_path (path);

    if (g_strcmp0 (user_data_attr_id_fs, path_attr_id_fs) == 0) {
        /* The volume that is low on space is on the same volume as our home
         * directory. This means the trash is at $XDG_DATA_HOME/Trash,
         * not at the root of the volume which is full.
         */
        mount_uses_user_trash = TRUE;
    }

    g_free (user_data_attr_id_fs);
    g_free (path_attr_id_fs);

    /* I can't think of a better way to find out if a volume has any trash. Any suggestions? */
    if (mount_uses_user_trash) {
        trash_files_dir = g_build_filename (g_get_user_data_dir (), "Trash", "files", NULL);
    } else {
        gchar *uid;

        uid = g_strdup_printf ("%d", getuid ());
        trash_files_dir = g_build_filename (path, ".Trash", uid, "files", NULL);
        if (!g_file_test (trash_files_dir, G_FILE_TEST_IS_DIR)) {
            gchar *trash_dir;

            g_free (trash_files_dir);
            trash_dir = g_strdup_printf (".Trash-%s", uid);
            trash_files_dir = g_build_filename (path, trash_dir, "files", NULL);
            g_free (trash_dir);
            if (!g_file_test (trash_files_dir, G_FILE_TEST_IS_DIR)) {
                g_free (trash_files_dir);
                g_free (uid);
                return has_trash;
            }
        }
        g_free (uid);
    }

    dir = g_dir_open (trash_files_dir, 0, NULL);
    if (dir) {
        if (g_dir_read_name (dir))
            has_trash = TRUE;
        g_dir_close (dir);
    }

    g_free (trash_files_dir);

    return has_trash;
}

static void
ldsm_analyze_path (const gchar *path)
{
    const gchar *argv[] = { DISK_SPACE_ANALYZER, path, NULL };

    g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH,
                    NULL, NULL, NULL, NULL);
}

bool DIskSpace::ldsm_notify_for_mount (LdsmMountInfo *mount,
                                       bool       multiple_volumes,
                                       bool       other_usable_volumes)
{
    gchar  *name, *program;
    signed long free_space;
    int   response;
    bool  has_trash;
    bool  has_disk_analyzer;
    bool  retval = TRUE;
    char  *path;

    /* Don't show a dialog if one is already displayed */
        if (dialog)
            return retval;

    name = g_unix_mount_guess_name (mount->mount);
    free_space = (gint64) mount->buf.f_frsize * (gint64) mount->buf.f_bavail;
    has_trash = ldsm_mount_has_trash (mount);
    path = g_strdup (g_unix_mount_get_mount_path (mount->mount));

    program = g_find_program_in_path (DISK_SPACE_ANALYZER);
    has_disk_analyzer = (program != NULL);
    g_free (program);

    dialog = new LdsmDialog (other_usable_volumes,
                              multiple_volumes,
                              has_disk_analyzer,
                              has_trash,
                              free_space,
                              name,
                              path);

    g_free (name);

    response = dialog->exec();
    delete dialog;

    switch (response) {
    case GTK_RESPONSE_CANCEL:
            retval = FALSE;
            break;
    case LDSM_DIALOG_RESPONSE_ANALYZE:
            retval = FALSE;
            ldsm_analyze_path (path);
            break;
    case LDSM_DIALOG_RESPONSE_EMPTY_TRASH:
            retval = TRUE;
            //usd_ldsm_trash_empty ();//调清空回收站dialog
            break;

    case GTK_RESPONSE_NONE:
    case GTK_RESPONSE_DELETE_EVENT:
            retval = TRUE;
            break;
    default:
            g_assert_not_reached ();
    }
    free (path);
    return retval;
}

void DIskSpace::ldsm_maybe_warn_mounts (GList *mounts,
                                        bool multiple_volumes,
                                        bool other_usable_volumes)
{
    GList *l;
    gboolean done = FALSE;

    for (l = mounts; l != NULL; l = l->next) {
        LdsmMountInfo *mount_info = (LdsmMountInfo  *)l->data;
        LdsmMountInfo *previous_mount_info;
        gdouble free_space;
        gdouble previous_free_space;
        time_t curr_time;
        const char *path;
        gboolean show_notify;

        if (done) {
            /* Don't show any more dialogs if the user took action with the last one. The user action
             * might free up space on multiple volumes, making the next dialog redundant.
             */
            ldsm_free_mount_info (mount_info);
            continue;
        }

        path = g_unix_mount_get_mount_path (mount_info->mount);

        previous_mount_info = (LdsmMountInfo *)g_hash_table_lookup (ldsm_notified_hash, path);
        if (previous_mount_info != NULL)
            previous_free_space = (gdouble) previous_mount_info->buf.f_bavail / (gdouble) previous_mount_info->buf.f_blocks;

        free_space = (gdouble) mount_info->buf.f_bavail / (gdouble) mount_info->buf.f_blocks;

        if (previous_mount_info == NULL) {
            /* We haven't notified for this mount yet */
            show_notify = TRUE;
            mount_info->notify_time = time (NULL);
            g_hash_table_replace (ldsm_notified_hash, g_strdup (path), mount_info);
        } else if ((previous_free_space - free_space) > free_percent_notify_again) {
            /* We've notified for this mount before and free space has decreased sufficiently since last time to notify again */
            curr_time = time (NULL);
            if (difftime (curr_time, previous_mount_info->notify_time) > (gdouble)(min_notify_period * 60)) {
                show_notify = TRUE;
                mount_info->notify_time = curr_time;
            } else {
                /* It's too soon to show the dialog again. However, we still replace the LdsmMountInfo
                 * struct in the hash table, but give it the notfiy time from the previous dialog.
                 * This will stop the notification from reappearing unnecessarily as soon as the timeout expires.
                 */
                show_notify = FALSE;
                mount_info->notify_time = previous_mount_info->notify_time;
            }
            g_hash_table_replace (ldsm_notified_hash, g_strdup (path), mount_info);
        } else {
            /* We've notified for this mount before, but the free space hasn't decreased sufficiently to notify again */
            ldsm_free_mount_info (mount_info);
            show_notify = FALSE;
        }

        if (show_notify) {
            if (ldsm_notify_for_mount (mount_info, multiple_volumes, other_usable_volumes))
                done = TRUE;
        }
    }
}

bool DIskSpace::ldsm_check_all_mounts (gpointer data)
{
    GList *mounts;
    GList *l;
    GList *check_mounts = NULL;
    GList *full_mounts = NULL;
    guint number_of_mounts;
    guint number_of_full_mounts;
    gboolean multiple_volumes = FALSE;
    gboolean other_usable_volumes = FALSE;

    /* We iterate through the static mounts in /etc/fstab first, seeing if
     * they're mounted by checking if the GUnixMountPoint has a corresponding GUnixMountEntry.
     * Iterating through the static mounts means we automatically ignore dynamically mounted media.
     */
    mounts = g_unix_mount_points_get (time_read);

    for (l = mounts; l != NULL; l = l->next) {
        GUnixMountPoint *mount_point = (GUnixMountPoint *)l->data;
        GUnixMountEntry *mount;
        LdsmMountInfo *mount_info;
        const gchar *path;

        path = g_unix_mount_point_get_mount_path (mount_point);
        mount = g_unix_mount_at (path, time_read);
        g_unix_mount_point_free (mount_point);
        if (mount == NULL) {
                /* The GUnixMountPoint is not mounted */
                continue;
        }

        mount_info = g_new0 (LdsmMountInfo, 1);
        mount_info->mount = mount;

        path = g_unix_mount_get_mount_path (mount);

        if (g_unix_mount_is_readonly (mount)) {
            ldsm_free_mount_info (mount_info);
            continue;
        }

        if (ldsm_mount_should_ignore (mount)) {
            ldsm_free_mount_info (mount_info);
            continue;
        }

        if (statvfs (path, &mount_info->buf) != 0) {
            ldsm_free_mount_info (mount_info);
            continue;
        }

        if (ldsm_mount_is_virtual (mount_info)) {
            ldsm_free_mount_info (mount_info);
            continue;
        }

        check_mounts = g_list_prepend (check_mounts, mount_info);
    }
    g_list_free (mounts);

    number_of_mounts = g_list_length (check_mounts);
    if (number_of_mounts > 1)
        multiple_volumes = TRUE;

    for (l = check_mounts; l != NULL; l = l->next) {
        LdsmMountInfo *mount_info = (LdsmMountInfo *)l->data;

        if (!ldsm_mount_has_space (mount_info)) {
            full_mounts = g_list_prepend (full_mounts, mount_info);
        } else {
            g_hash_table_remove (ldsm_notified_hash, g_unix_mount_get_mount_path (mount_info->mount));
            ldsm_free_mount_info (mount_info);
        }
    }

    number_of_full_mounts = g_list_length (full_mounts);
    if (number_of_mounts > number_of_full_mounts)
            other_usable_volumes = TRUE;

    ldsm_maybe_warn_mounts (full_mounts, multiple_volumes,
                            other_usable_volumes);

    g_list_free (check_mounts);
    g_list_free (full_mounts);

    return TRUE;
}

void DIskSpace::ldsm_mounts_changed (GObject  *monitor,gpointer  data)
{
    GList *mounts;

    /* remove the saved data for mounts that got removed */
    mounts = g_unix_mounts_get (time_read);
    g_hash_table_foreach_remove (ldsm_notified_hash,
                                 ldsm_is_hash_item_not_in_mounts, mounts);
    g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);

    /* check the status now, for the new mounts */
    ldsm_check_all_mounts (NULL);

    /* and reset the timeout */
    if (ldsm_timeout_id)
            g_source_remove (ldsm_timeout_id);
    ldsm_timeout_id = g_timeout_add_seconds (CHECK_EVERY_X_SECONDS,
                                             (GSourceFunc)ldsm_check_all_mounts, NULL);
}

void DIskSpace::UsdLdsmSetup(bool check_now)
{
    if (ldsm_notified_hash || ldsm_timeout_id || ldsm_monitor) {
        qWarning ("Low disk space monitor already initialized.");
        return;
    }
    ldsm_notified_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free,
                                                ldsm_free_mount_info);
    usdLdsmGetConfig();
    connect(settings,SIGNAL(changes(QString)),this,SLOT(usdLdsmUpdateConfig(QString)));
#if GLIB_CHECK_VERSION (2, 44, 0)
    ldsm_monitor = g_unix_mount_monitor_get ();
#else
    ldsm_monitor = g_unix_mount_monitor_new ();
    g_unix_mount_monitor_set_rate_limit (ldsm_monitor, 1000);
#endif
    g_signal_connect (ldsm_monitor, "mounts-changed",
                      G_CALLBACK (ldsm_mounts_changed), NULL);

    if (check_now)
        ldsm_check_all_mounts (NULL);

    ldsm_timeout_id = g_timeout_add_seconds (CHECK_EVERY_X_SECONDS,
                                             (GSourceFunc)ldsm_check_all_mounts, NULL);


}

void DIskSpace::UsdLdsmClean()
{
    if (ldsm_timeout_id)
            g_source_remove (ldsm_timeout_id);
    ldsm_timeout_id = 0;

    if (ldsm_notified_hash)
            g_hash_table_destroy (ldsm_notified_hash);
    ldsm_notified_hash = NULL;

    if (ldsm_monitor)
            g_object_unref (ldsm_monitor);
    ldsm_monitor = NULL;

    if (settings) {
            g_object_unref (settings);
    }

    /*if (dialog) {
            gtk_widget_destroy (GTK_WIDGET (dialog));
            dialog = NULL;
    }*/

    if (ignore_paths) {
            g_slist_foreach (ignore_paths, (GFunc) g_free, NULL);
            g_slist_free (ignore_paths);
    }
}

#ifdef TEST
int main (int    argc, char **argv)
{
    GMainLoop *loop;
    DIskSpace *mDisk;
    mDisk = DIskSpace::DiskSpaceNew();
    gtk_init (&argc, &argv);

    loop = g_main_loop_new (NULL, FALSE);

    mDisk->UsdLdsmSetup (true);

    g_main_loop_run (loop);

    mDisk->UsdLdsmClean ();

    g_main_loop_unref (loop);

    delete mDisk;
    return 0;
}
#endif
