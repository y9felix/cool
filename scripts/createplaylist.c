#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAGSZ 512

static const char *argv0;

static void
usage(void)
{
	fprintf(stderr, "usage: %s [file ...]\n", argv0);
	exit(1);
}

static void
trim(char *s)
{
	char *p, *start;
	size_t len;

	start = s;
	while (*start && (unsigned char)*start <= ' ')
		start++;
	len = strlen(start);
	if (start != s)
		memmove(s, start, len + 1);
	p = s + len;
	while (p > s && (unsigned char)*(p - 1) <= ' ')
		*--p = '\0';
}

static size_t
put_utf8(char *dst, size_t dlen, size_t di, unsigned int cp)
{
	if (cp < 0x20 || cp == 0x7F)
		cp = ' ';
	if (cp < 0x80) {
		if (di + 1 < dlen)
			dst[di++] = cp;
	} else if (cp < 0x800) {
		if (di + 2 < dlen) {
			dst[di++] = 0xC0 | (cp >> 6);
			dst[di++] = 0x80 | (cp & 0x3F);
		}
	} else if (cp < 0x10000) {
		if (di + 3 < dlen) {
			dst[di++] = 0xE0 | (cp >> 12);
			dst[di++] = 0x80 | ((cp >> 6) & 0x3F);
			dst[di++] = 0x80 | (cp & 0x3F);
		}
	} else if (cp <= 0x10FFFF) {
		if (di + 4 < dlen) {
			dst[di++] = 0xF0 | (cp >> 18);
			dst[di++] = 0x80 | ((cp >> 12) & 0x3F);
			dst[di++] = 0x80 | ((cp >> 6) & 0x3F);
			dst[di++] = 0x80 | (cp & 0x3F);
		}
	}
	return di;
}

static void
decode_latin1(char *dst, size_t dlen, const unsigned char *src, size_t slen)
{
	size_t di, i;

	di = 0;
	for (i = 0; i < slen && src[i]; i++)
		di = put_utf8(dst, dlen, di, src[i]);
	dst[di] = '\0';
}

static void
decode_utf8(char *dst, size_t dlen, const unsigned char *src, size_t slen)
{
	size_t di, i;
	unsigned char c;

	di = 0;
	for (i = 0; i < slen && src[i] && di + 1 < dlen; i++) {
		c = src[i];
		if (c < 0x20 || c == 0x7F)
			c = ' ';
		dst[di++] = c;
	}
	if (i < slen && src[i]) {
		while (di > 0 && ((unsigned char)dst[di - 1] & 0xC0) == 0x80)
			di--;
		if (di > 0 && ((unsigned char)dst[di - 1] & 0xC0) == 0xC0)
			di--;
	}
	dst[di] = '\0';
}

static void
decode_utf16(char *dst, size_t dlen, const unsigned char *src, size_t slen,
	int enc)
{
	int be;
	size_t di, si;
	unsigned int u, u2;

	be = (enc == 2);
	di = 0;
	si = 0;
	if (enc == 1 && slen >= 2) {
		if (src[0] == 0xFE && src[1] == 0xFF) {
			be = 1;
			si = 2;
		} else if (src[0] == 0xFF && src[1] == 0xFE) {
			be = 0;
			si = 2;
		}
	}
	for (; si + 1 < slen; si += 2) {
		u = be ? ((src[si] << 8) | src[si + 1])
		       : ((src[si + 1] << 8) | src[si]);
		if (!u)
			break;
		if (u >= 0xD800 && u <= 0xDBFF && si + 3 < slen) {
			u2 = be ? ((src[si + 2] << 8) | src[si + 3])
			        : ((src[si + 3] << 8) | src[si + 2]);
			if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
				u = 0x10000 + (((u - 0xD800) << 10) |
				    (u2 - 0xDC00));
				si += 2;
			}
		}
		if (u != 0xFEFF)
			di = put_utf8(dst, dlen, di, u);
	}
	dst[di] = '\0';
}

static void
decode_tag(char *dst, size_t dlen, const unsigned char *src, size_t slen,
	int enc)
{
	switch (enc) {
	case 0:
		decode_latin1(dst, dlen, src, slen);
		break;
	case 1:
	case 2:
		decode_utf16(dst, dlen, src, slen, enc);
		break;
	case 3:
		decode_utf8(dst, dlen, src, slen);
		break;
	default:
		break;
	}
	trim(dst);
}

static int
valid_id(const unsigned char *p)
{
	int i;

	for (i = 0; i < 4; i++)
		if (!((p[i] >= 'A' && p[i] <= 'Z') ||
		      (p[i] >= '0' && p[i] <= '9')))
			return 0;
	return 1;
}

static size_t
be32(const unsigned char *p)
{
	return ((size_t)p[0] << 24) | ((size_t)p[1] << 16) |
	       ((size_t)p[2] << 8) | p[3];
}

static size_t
syncsz(const unsigned char *p)
{
	return ((size_t)(p[0] & 0x7F) << 21) |
	       ((size_t)(p[1] & 0x7F) << 14) |
	       ((size_t)(p[2] & 0x7F) << 7) |
	       (p[3] & 0x7F);
}

static void
get_tags(FILE *f, char *title, size_t tlen, char *artist, size_t alen)
{
	char *dst;
	int ver;
	size_t dlen, pos, read_sz, sz, tag_sz;
	unsigned char buf[TAGSZ], h[10], v1[128];

	title[0] = '\0';
	artist[0] = '\0';

	if (fread(h, 1, sizeof(h), f) == sizeof(h) && !memcmp(h, "ID3", 3)) {
		ver = h[3];
		if (ver == 3 || ver == 4) {
			tag_sz = syncsz(h + 6);
			pos = 0;
			if ((h[5] & 0x40) && tag_sz >= 4) {
				if (fread(buf, 1, 4, f) == 4) {
					sz = (ver == 4) ? syncsz(buf)
					                : be32(buf);
					if (ver == 4 && sz > 4) {
						if (fseek(f, sz - 4,
						          SEEK_CUR) < 0)
							return;
					} else if (ver == 3) {
						if (fseek(f, sz,
						          SEEK_CUR) < 0)
							return;
					}
					pos = (ver == 4) ? sz : sz + 4;
				}
			}
			while (pos + 10 <= tag_sz &&
			       (!title[0] || !artist[0])) {
				if (fread(buf, 1, 10, f) != 10)
					break;
				if (!valid_id(buf))
					break;
				sz = (ver == 4) ? syncsz(buf + 4)
				                : be32(buf + 4);
				pos += 10;
				if (sz > tag_sz - pos)
					break;

				dst = NULL;
				dlen = 0;
				if (!title[0] && !memcmp(buf, "TIT2", 4)) {
					dst = title;
					dlen = tlen;
				} else if (!artist[0] &&
				           !memcmp(buf, "TPE1", 4)) {
					dst = artist;
					dlen = alen;
				}

				read_sz = 0;
				if (dst && sz > 1) {
					read_sz = sz < sizeof(buf)
					    ? sz : sizeof(buf);
					if (fread(buf, 1, read_sz, f) ==
					    read_sz)
						decode_tag(dst, dlen,
							buf + 1, read_sz - 1,
							buf[0]);
				}
				if (sz > read_sz &&
				    fseek(f, sz - read_sz, SEEK_CUR) < 0)
					break;
				pos += sz;
			}
		}
	}

	if ((title[0] && artist[0]) || fseek(f, -128, SEEK_END) < 0)
		return;
	if (fread(v1, 1, sizeof(v1), f) != sizeof(v1) || memcmp(v1, "TAG", 3))
		return;
	if (!title[0]) {
		decode_latin1(title, tlen, v1 + 3, 30);
		trim(title);
	}
	if (!artist[0]) {
		decode_latin1(artist, alen, v1 + 33, 30);
		trim(artist);
	}
}

static int
process_file(const char *path)
{
	FILE *f;
	char artist[TAGSZ], title[TAGSZ];
	const char *base, *dot;
	size_t blen;

	if (!(f = fopen(path, "rb"))) {
		fprintf(stderr, "%s: %s: %s\n", argv0, path, strerror(errno));
		return 1;
	}

	get_tags(f, title, sizeof(title), artist, sizeof(artist));
	fclose(f);

	if (!title[0]) {
		base = strrchr(path, '/');
		base = base ? base + 1 : path;
		dot = strrchr(base, '.');
		if (dot && dot != base)
			blen = (size_t)(dot - base);
		else
			blen = strlen(base);
		decode_utf8(title, sizeof(title),
			(const unsigned char *)base, blen);
		trim(title);
	}

	if (artist[0])
		printf("#EXTINF:-1,%s - %s\n%s\n", artist, title, path);
	else
		printf("#EXTINF:-1,%s\n%s\n", title, path);

	return 0;
}

static int
process_stream(FILE *fp)
{
	char line[PATH_MAX];
	int c, status;
	size_t len;

	status = 0;
	while (fgets(line, sizeof(line), fp)) {
		len = strlen(line);
		if (len > 0 && line[len - 1] != '\n' && !feof(fp)) {
			while ((c = fgetc(fp)) != EOF && c != '\n')
				;
			continue;
		}
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0])
			status |= process_file(line);
	}
	return ferror(fp) ? 1 : status;
}

int
main(int argc, char *argv[])
{
	int status;

	argv0 = *argv ? *argv : "m3u8gen";
	for (argv++, argc--; argc > 0 && **argv == '-'; argv++, argc--) {
		if (!strcmp(*argv, "--")) {
			argv++;
			argc--;
			break;
		}
		if (!strcmp(*argv, "-"))
			break;
		usage();
	}

	puts("#EXTM3U");
	if (!argc)
		return process_stream(stdin);

	status = 0;
	for (; argc > 0; argv++, argc--) {
		if (!strcmp(*argv, "-"))
			status |= process_stream(stdin);
		else
			status |= process_file(*argv);
	}
	return (ferror(stdout) || status) ? 1 : 0;
}
