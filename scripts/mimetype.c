#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#define VERSION "1.0"

static void usage(void);
static int resolve(const char *path, int brief, int follow);

static char *argv0;

static void
usage(void)
{
	fprintf(stderr,
	        "usage: %s [-bLv] [--brief] [--dereference] [file ...]\n",
	        argv0);
	exit(1);
}

static int
resolve(const char *path, int brief, int follow)
{
	GError *err;
	GFile *f;
	GFileInfo *info;
	char *guess;
	const char *type;
	int flags, ret;

	err = NULL;
	guess = NULL;
	ret = 0;
	f = g_file_new_for_path(path);
	flags = follow ? G_FILE_QUERY_INFO_NONE :
	        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS;
	if (!(info = g_file_query_info(f, "standard::content-type", flags,
	                               NULL, &err))) {
		fprintf(stderr, "%s: %s\n", argv0, err->message);
		g_error_free(err);
		g_object_unref(f);
		return 1;
	}
	type = g_file_info_get_content_type(info);
	if (!type || strcmp(type, "application/octet-stream") == 0 ||
	    strcmp(type, "application/x-zerosize") == 0) {
		guess = g_content_type_guess(path, NULL, 0, NULL);
		if (guess && strcmp(guess, "application/octet-stream") != 0)
			type = guess;
	}
	if (!type) {
		fprintf(stderr, "%s: %s: cannot determine type\n",
		        argv0, path);
		ret = 1;
		goto out;
	}
	if (brief)
		printf("%s\n", type);
	else
		printf("%s: %s\n", path, type);
out:
	g_free(guess);
	g_object_unref(info);
	g_object_unref(f);
	return ret;
}

int
main(int argc, char **argv)
{
	char *p;
	int brief, follow, i, j, ret;

	argv0 = (*argv && **argv) ? *argv : "mimetype";
	if ((p = strrchr(argv0, '/')))
		argv0 = p + 1;
	brief = 0;
	follow = 0;
	ret = 0;
	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (strcmp(argv[i], "--version") == 0) {
			puts("mimetype " VERSION);
			return 0;
		}
		if (strcmp(argv[i], "--brief") == 0) {
			brief = 1;
			continue;
		}
		if (strcmp(argv[i], "--dereference") == 0) {
			follow = 1;
			continue;
		}
		if (argv[i][1] == '-')
			usage();
		for (j = 1; argv[i][j]; j++) {
			switch (argv[i][j]) {
			case 'b':
				brief = 1;
				break;
			case 'L':
				follow = 1;
				break;
			case 'v':
				puts("mimetype " VERSION);
				return 0;
			default:
				usage();
			}
		}
	}
	if (i >= argc)
		usage();
	for (; i < argc; i++)
		if (resolve(argv[i], brief, follow))
			ret = 1;

	return ret;
}
