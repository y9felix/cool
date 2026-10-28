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
    char *start = s;
    while (*start && (unsigned char)*start <= ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    char *p = s + strlen(s);
    while (p > s && (unsigned char)*(p - 1) <= ' ') *--p = '\0';
}

static void put_utf8(char *dst, size_t dlen, size_t *di, unsigned int cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) cp = ' ';
    if (cp < 0x80) {
        if (*di + 1 < dlen) dst[(*di)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*di + 2 < dlen) {
            dst[(*di)++] = (char)(0xC0 | (cp >> 6));
            dst[(*di)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp < 0x10000) {
        if (*di + 3 < dlen) {
            dst[(*di)++] = (char)(0xE0 | (cp >> 12));
            dst[(*di)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[(*di)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp <= 0x10FFFF) {
        if (*di + 4 < dlen) {
            dst[(*di)++] = (char)(0xF0 | (cp >> 18));
            dst[(*di)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[(*di)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[(*di)++] = (char)(0x80 | (cp & 0x3F));
        }
    }
}

static void decode_str(char *dst, size_t dlen, const unsigned char *src, size_t slen) {
    size_t di = 0, i = 0;
    while (i < slen && src[i] != 0) {
        unsigned char c = src[i];
        unsigned int cp = c;
        size_t adv = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < slen && (src[i + 1] & 0xC0) == 0x80) {
            cp = ((c & 0x1F) << 6) | (src[i + 1] & 0x3F);
            if (cp >= 0x80) adv = 2; else cp = c;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < slen && (src[i + 1] & 0xC0) == 0x80 && (src[i + 2] & 0xC0) == 0x80) {
            cp = ((c & 0x0F) << 12) | ((src[i + 1] & 0x3F) << 6) | (src[i + 2] & 0x3F);
            if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) adv = 3; else cp = c;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < slen && (src[i + 1] & 0xC0) == 0x80 && (src[i + 2] & 0xC0) == 0x80 && (src[i + 3] & 0xC0) == 0x80) {
            cp = ((c & 0x07) << 18) | ((src[i + 1] & 0x3F) << 12) | ((src[i + 2] & 0x3F) << 6) | (src[i + 3] & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) adv = 4; else cp = c;
        }
        put_utf8(dst, dlen, &di, cp);
        i += adv;
    }
    dst[di] = '\0';
    trim(dst);
}

static void decode_utf16(char *dst, size_t dlen, const unsigned char *src, size_t slen, int enc) {
    size_t di = 0, si = 0;
    int be = (enc == 2);
    if (enc == 1 && slen >= 2) {
        if (src[0] == 0xFE && src[1] == 0xFF) { be = 1; si = 2; }
        else if (src[0] == 0xFF && src[1] == 0xFE) { be = 0; si = 2; }
    }
    for (; si + 1 < slen; si += 2) {
        unsigned int u = be ? ((src[si] << 8) | src[si + 1]) : ((src[si + 1] << 8) | src[si]);
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDBFF && si + 3 < slen) {
            unsigned int u2 = be ? ((src[si + 2] << 8) | src[si + 3]) : ((src[si + 3] << 8) | src[si + 2]);
            if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                u = 0x10000 + (((u - 0xD800) << 10) | (u2 - 0xDC00));
                si += 2;
            } else continue;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            continue;
        }
        if (u != 0xFEFF) put_utf8(dst, dlen, &di, u);
    }
    dst[di] = '\0';
    trim(dst);
}

static int valid_id(const unsigned char *p, int n) {
    for (int i = 0; i < n; i++)
        if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) return 0;
    return 1;
}

static size_t deunsync(unsigned char *p, size_t len) {
    size_t r = 0, w = 0;
    while (r < len) {
        p[w++] = p[r];
        if (p[r] == 0xFF && r + 1 < len && p[r + 1] == 0x00) r += 2;
        else r++;
    }
    return w;
}

static void get_tags(const char *path, char *title, size_t tlen, char *artist, size_t alen) {
    title[0] = '\0';
    artist[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return;

    unsigned char h[10];
    if (fread(h, 1, 10, f) == 10 && memcmp(h, "ID3", 3) == 0) {
        int ver = h[3];
        int flags = h[5];
        size_t tag_sz = ((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
        unsigned char *buf = tag_sz ? malloc(tag_sz) : NULL;
        if (buf && fread(buf, 1, tag_sz, f) == tag_sz) {
            if ((flags & 0x80) && ver < 4) tag_sz = deunsync(buf, tag_sz);
            size_t pos = 0;
            if ((flags & 0x40) && tag_sz >= 4) {
                if (ver == 4) {
                    size_t esz = ((buf[0] & 0x7f) << 21) | ((buf[1] & 0x7f) << 14) | ((buf[2] & 0x7f) << 7) | (buf[3] & 0x7f);
                    pos = (esz >= 4) ? esz : 4;
                } else if (ver == 3) {
                    size_t esz = ((size_t)buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
                    pos = 4 + esz;
                }
            }
            size_t hdr_sz = (ver == 2) ? 6 : 10;
            while (pos + hdr_sz <= tag_sz) {
                if (!valid_id(buf + pos, (ver == 2) ? 3 : 4)) break;
                char id[5] = {0};
                memcpy(id, buf + pos, (ver == 2) ? 3 : 4);
                size_t sz = 0;
                unsigned char f2 = 0;
                if (ver == 2) {
                    sz = ((size_t)buf[pos + 3] << 16) | (buf[pos + 4] << 8) | buf[pos + 5];
                } else {
                    size_t rsz = ((size_t)buf[pos + 4] << 24) | (buf[pos + 5] << 16) | (buf[pos + 6] << 8) | buf[pos + 7];
                    if (ver == 4) {
                        sz = ((buf[pos + 4] & 0x7f) << 21) | ((buf[pos + 5] & 0x7f) << 14) | ((buf[pos + 6] & 0x7f) << 7) | (buf[pos + 7] & 0x7f);
                        if ((buf[pos + 4] | buf[pos + 5] | buf[pos + 6] | buf[pos + 7]) & 0x80) {
                            sz = rsz;
                        } else if (sz != rsz && (pos + 10 + sz > tag_sz || (pos + 10 + sz < tag_sz && buf[pos + 10 + sz] != 0 && !valid_id(buf + pos + 10 + sz, 4)))) {
                            if (pos + 10 + rsz <= tag_sz && (pos + 10 + rsz == tag_sz || buf[pos + 10 + rsz] == 0 || valid_id(buf + pos + 10 + rsz, 4)))
                                sz = rsz;
                        }
                    } else {
                        sz = rsz;
                    }
                    f2 = buf[pos + 9];
                }
                pos += hdr_sz;
                if (pos + sz > tag_sz) break;

                int skip = (ver == 3 && (f2 & 0xC0)) || (ver == 4 && (f2 & 0x0C));
                size_t foff = 0;
                if (ver == 3 && (f2 & 0x20)) foff += 1;
                if (ver == 4) {
                    if (f2 & 0x40) foff += 1;
                    if (f2 & 0x01) foff += 4;
                }

                if (!skip && sz > foff + 1) {
                    char *dst = NULL;
                    size_t dlen = 0;
                    if ((strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0) && !title[0]) {
                        dst = title;
                        dlen = tlen;
                    } else if ((strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0) && !artist[0]) {
                        dst = artist;
                        dlen = alen;
                    }

                    if (dst) {
                        unsigned char *fbuf = buf + pos + foff;
                        size_t flen = sz - foff;
                        if (ver == 4 && ((flags & 0x80) || (f2 & 0x02))) flen = deunsync(fbuf, flen);
                        if (flen > 1) {
                            int enc = fbuf[0];
                            if (enc == 0 || enc == 3) {
                                decode_str(dst, dlen, fbuf + 1, flen - 1);
                            } else if (enc == 1 || enc == 2) {
                                decode_utf16(dst, dlen, fbuf + 1, flen - 1, enc);
                            }
                        }
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
                if (!title[0]) decode_str(title, tlen, v1 + 3, 30);
                if (!artist[0]) decode_str(artist, alen, v1 + 33, 30);
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

    char title[512] = {0};
    char artist[512] = {0};
    get_tags(fpath, title, sizeof(title), artist, sizeof(artist));

    if (!title[0]) {
        const char *base = fpath + ftwbuf->base;
        size_t blen = strlen(base);
        if (blen > 4) blen -= 4;
        decode_str(title, sizeof(title), (const unsigned char *)base, blen);
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
