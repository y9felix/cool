#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int brief = 0;
    int follow_symlinks = 1;
    int first_file = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("mimetype 1.0\n");
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: mimetype [options] [files...]\n");
            return 0;
        }
        if (strcmp(argv[i], "--brief") == 0 || strcmp(argv[i], "-b") == 0) {
            brief = 1;
            continue;
        }
        if (strcmp(argv[i], "--dereference") == 0 || strcmp(argv[i], "-L") == 0) {
            follow_symlinks = 1;
            continue;
        }
        if (strcmp(argv[i], "--no-dereference") == 0) {
            follow_symlinks = 0;
            continue;
        }
        if (argv[i][0] != '-' && first_file == -1) {
            first_file = i;
        }
    }

    if (first_file == -1) {
        return 1;
    }

    int ret = 0;
    for (int i = first_file; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        const char *path = argv[i];
        GFile *f = g_file_new_for_path(path);
        GFileQueryInfoFlags flags = follow_symlinks ? G_FILE_QUERY_INFO_NONE : G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS;
        GFileInfo *info = g_file_query_info(f, "standard::content-type,standard::fast-content-type", flags, NULL, NULL);

        const char *type = NULL;
        char *allocated_type = NULL;

        if (info) {
            type = g_file_info_get_content_type(info);
        }

        if (!type || strcmp(type, "application/octet-stream") == 0 || strcmp(type, "application/x-zerosize") == 0) {
            allocated_type = g_content_type_guess(path, NULL, 0, NULL);
            if (allocated_type && strcmp(allocated_type, "application/octet-stream") != 0) {
                type = allocated_type;
            }
        }

        if (type) {
            if (brief) {
                printf("%s\n", type);
            } else {
                printf("%s: %s\n", path, type);
            }
        } else {
            ret = 1;
        }

        if (allocated_type) g_free(allocated_type);
        if (info) g_object_unref(info);
        g_object_unref(f);
    }

    return ret;
}
