#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>

static FILE *out_fp;

static void trim(char *s) {
    char *p = s + strlen(s);
    while (p > s && (unsigned char)*(p - 1) <= ' ') *--p = '\0';
}

static void get_tags(const char *path, char *title, size_t tlen, char *artist, size_t alen) {
    title[0] = '\0';
    artist[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return;

    unsigned char h[10];
    if (fread(h, 1, 10, f) == 10 && memcmp(h, "ID3", 3) == 0) {
        int ver = h[3];
        size_t tag_sz = ((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
        unsigned char *buf = malloc(tag_sz);
        if (buf && fread(buf, 1, tag_sz, f) == tag_sz) {
            size_t pos = 0;
            while (pos + 10 <= tag_sz) {
                if (buf[pos] == 0) break;
                char id[5] = {0};
                memcpy(id, buf + pos, 4);
                size_t sz = 0;
                if (ver == 4) {
                    sz = ((buf[pos + 4] & 0x7f) << 21) | ((buf[pos + 5] & 0x7f) << 14) |
                         ((buf[pos + 6] & 0x7f) << 7) | (buf[pos + 7] & 0x7f);
                } else {
                    sz = ((unsigned char)buf[pos + 4] << 24) | ((unsigned char)buf[pos + 5] << 16) |
                         ((unsigned char)buf[pos + 6] << 8) | (unsigned char)buf[pos + 7];
                }
                pos += 10;
                if (pos + sz > tag_sz) break;

                if (sz > 1) {
                    int enc = buf[pos];
                    char *dst = NULL;
                    size_t dlen = 0;
                    if (strcmp(id, "TIT2") == 0 && !title[0]) {
                        dst = title;
                        dlen = tlen;
                    } else if (strcmp(id, "TPE1") == 0 && !artist[0]) {
                        dst = artist;
                        dlen = alen;
                    }

                    if (dst) {
                        if (enc == 0 || enc == 3) {
                            size_t n = (sz - 1 < dlen - 1) ? sz - 1 : dlen - 1;
                            memcpy(dst, buf + pos + 1, n);
                            dst[n] = '\0';
                        } else if (enc == 1 || enc == 2) {
                            size_t di = 0;
                            int be = (enc == 2 || (sz >= 3 && buf[pos + 1] == 0xFE && buf[pos + 2] == 0xFF));
                            size_t si = (enc == 1 && sz >= 3) ? 3 : 1;
                            for (; si + 1 < sz && di < dlen - 1; si += 2) {
                                char c = be ? buf[pos + si + 1] : buf[pos + si];
                                if (c) dst[di++] = c;
                            }
                            dst[di] = '\0';
                        }
                        trim(dst);
                    }
                }
                pos += sz;
            }
        }
        free(buf);
    }

    if (!title[0] || !artist[0]) {
        if (fseek(f, -128, SEEK_END) == 0) {
            unsigned char v1[128];
            if (fread(v1, 1, 128, f) == 128 && memcmp(v1, "TAG", 3) == 0) {
                if (!title[0]) {
                    snprintf(title, tlen, "%.30s", (char *)(v1 + 3));
                    trim(title);
                }
                if (!artist[0]) {
                    snprintf(artist, alen, "%.30s", (char *)(v1 + 33));
                    trim(artist);
                }
            }
        }
    }
    fclose(f);
}

static int process_file(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    if (typeflag != FTW_F) return 0;

    size_t len = strlen(fpath);
    if (len < 4 || strcasecmp(fpath + len - 4, ".mp3") != 0) return 0;

    char title[256] = {0};
    char artist[256] = {0};
    get_tags(fpath, title, sizeof(title), artist, sizeof(artist));

    if (!title[0]) {
        const char *base = fpath + ftwbuf->base;
        size_t blen = strlen(base);
        if (blen > 4) blen -= 4;
        snprintf(title, sizeof(title), "%.*s", (int)blen, base);
    }
    if (!artist[0]) {
        strncpy(artist, "Unknown", sizeof(artist) - 1);
    }

    fprintf(out_fp, "#EXTINF:-1,%s - %s\n%s\n", artist, title, fpath);
    return 0;
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home) return 1;

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/music", home);
    if (access(dir, F_OK) != 0) {
        snprintf(dir, sizeof(dir), "%s/Music", home);
    }

    out_fp = fopen("playlist.m3u8", "w");
    if (!out_fp) return 1;

    fprintf(out_fp, "#EXTM3U\n");
    nftw(dir, process_file, 64, FTW_PHYS);
    fclose(out_fp);

    return 0;
}
