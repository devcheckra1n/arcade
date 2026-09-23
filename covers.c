#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <zlib.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "vendor/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "vendor/stb_image_resize2.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"
#pragma GCC diagnostic pop

#define PATHMAX 4096
#define META_URL "https://gamesdb.launchbox-app.com/Metadata.zip"
#define IMAGE_URL "https://images.launchbox-app.com/"
#define COVER_MAX 512

static const char *PLATFORMS[][4] = {
    {"nes", "Nintendo Entertainment System", NULL, NULL},
    {"snes", "Super Nintendo Entertainment System", NULL, NULL},
    {"n64", "Nintendo 64", NULL, NULL},
    {"gb", "Nintendo Game Boy", "Nintendo Game Boy Color", NULL},
    {"gba", "Nintendo Game Boy Advance", NULL, NULL},
    {"nds", "Nintendo DS", NULL, NULL},
    {"vb", "Nintendo Virtual Boy", NULL, NULL},
    {"psx", "Sony Playstation", NULL, NULL},
    {"psp", "Sony PSP", NULL, NULL},
    {"segaMD", "Sega Genesis", NULL, NULL},
    {"segaMS", "Sega Master System", NULL, NULL},
    {"segaGG", "Sega Game Gear", NULL, NULL},
    {"segaCD", "Sega CD", NULL, NULL},
    {"sega32x", "Sega 32X", NULL, NULL},
    {"segaSaturn", "Sega Saturn", NULL, NULL},
    {"atari2600", "Atari 2600", NULL, NULL},
    {"atari7800", "Atari 7800", NULL, NULL},
    {"lynx", "Atari Lynx", NULL, NULL},
    {"jaguar", "Atari Jaguar", NULL, NULL},
    {"pce", "NEC TurboGrafx-16", NULL, NULL},
    {"pcecd", "NEC TurboGrafx-CD", NULL, NULL},
    {"pcfx", "NEC PC-FX", NULL, NULL},
    {"ngp", "SNK Neo Geo Pocket", "SNK Neo Geo Pocket Color", NULL},
    {"ws", "WonderSwan", "WonderSwan Color", NULL},
    {"coleco", "ColecoVision", NULL, NULL},
    {"3do", "3DO Interactive Multiplayer", NULL, NULL},
    {"arcade", "Arcade", "SNK Neo Geo MVS", NULL},
    {"neogeo", "SNK Neo Geo AES", "SNK Neo Geo MVS", "Arcade"},
    {"mame", "Arcade", NULL, NULL},
    {"dos", "MS-DOS", NULL, NULL},
    {NULL, NULL, NULL, NULL},
};

static const char *COVER_EXT[] = {".png", ".jpg", ".jpeg", ".webp", NULL};

typedef struct {
    char sys[32], stem[PATHMAX], dir[PATHMAX], region[32];
    const char *plat[3];
    char *cand[4];  // normalised titles to accept
    int ncand;
    long id;        // matched launchbox game
    int how;        // 2 exact name, 1 alternate name
    char matched[512];
    char image[256];
    int iscore;
    char iregion[64];
} Rom;

static Rom *roms;
static int nroms;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

// decode the handful of xml entities launchbox uses, in place
static void unescape(char *s) {
    static const struct { const char *e; char c; } E[] = {
        {"&amp;", '&'}, {"&apos;", '\''}, {"&quot;", '"'}, {"&lt;", '<'}, {"&gt;", '>'}, {NULL, 0},
    };
    char *w = s;
    while (*s) {
        if (*s == '&') {
            int hit = 0;
            for (int i = 0; E[i].e; i++) {
                size_t n = strlen(E[i].e);
                if (!strncmp(s, E[i].e, n)) { *w++ = E[i].c; s += n; hit = 1; break; }
            }
            if (hit) continue;
            if (s[1] == '#') {
                char *end;
                long v = s[2] == 'x' ? strtol(s + 3, &end, 16) : strtol(s + 2, &end, 10);
                if (*end == ';' && v > 0 && v < 128) { *w++ = (char)v; s = end + 1; continue; }
            }
        }
        *w++ = *s++;
    }
    *w = 0;
}

// lowercase alphanumerics only, so "Castlevania: Symphony" == "castlevania - symphony"
static char *norm(const char *s) {
    char *o = malloc(strlen(s) + 1), *w = o;
    for (; *s; s++)
        if (isalnum((unsigned char)*s)) *w++ = (char)tolower((unsigned char)*s);
    *w = 0;
    return o;
}

// "Legend of Zelda, The (USA) (Rev 1)" -> "The Legend of Zelda"
static void title_of(const char *stem, char *out, size_t n) {
    char t[PATHMAX];
    size_t k = 0;
    int depth = 0;
    for (const char *s = stem; *s && k + 1 < sizeof t; s++) {
        if (*s == '(' || *s == '[') { depth++; continue; }
        if (*s == ')' || *s == ']') { if (depth) depth--; continue; }
        if (!depth) t[k++] = *s;
    }
    t[k] = 0;
    while (k && isspace((unsigned char)t[k - 1])) t[--k] = 0;

    static const char *arts[] = {", The", ", A", ", An", NULL};
    for (int i = 0; arts[i]; i++) {
        char *p = strstr(t, arts[i]);
        size_t al = strlen(arts[i]);
        if (p && (p[al] == 0 || !strncmp(p + al, " - ", 3))) {
            char head[PATHMAX];
            snprintf(head, sizeof head, "%.*s", (int)(p - t), t);
            snprintf(out, n, "%s %s%s", arts[i] + 2, head, p + al);
            return;
        }
    }
    snprintf(out, n, "%s", t);
}

static void region_of(const char *stem, char *out, size_t n) {
    static const struct { const char *tag, *region; } R[] = {
        {"USA", "North America"}, {"Japan", "Japan"}, {"Europe", "Europe"}, {"World", "World"},
        {"Germany", "Germany"}, {"France", "France"}, {"Korea", "Korea"}, {"Australia", "Australia"}, {NULL, NULL},
    };
    const char *p = strchr(stem, '(');
    for (; p; p = strchr(p + 1, '('))
        for (int i = 0; R[i].tag; i++)
            if (!strncmp(p + 1, R[i].tag, strlen(R[i].tag))) { snprintf(out, n, "%s", R[i].region); return; }
    snprintf(out, n, "North America");
}

static int has_cover(const char *dir, const char *stem) {
    char p[PATHMAX * 2];
    struct stat st;
    for (int i = 0; COVER_EXT[i]; i++) {
        snprintf(p, sizeof p, "%s/%s%s", dir, stem, COVER_EXT[i]);
        if (!stat(p, &st)) return 1;
    }
    return 0;
}

static int visible(const struct dirent *e) { return e->d_name[0] != '.'; }

static void add_cand(Rom *r, const char *title) {
    if (r->ncand < 4 && title[0]) r->cand[r->ncand++] = norm(title);
}

static void scan_roms(const char *root, int force) {
    struct dirent **sys;
    int ns = scandir(root, &sys, visible, alphasort);
    for (int i = 0; i < ns; i++) {
        const char *(*pl)[4] = NULL;
        for (int k = 0; PLATFORMS[k][0]; k++)
            if (!strcmp(PLATFORMS[k][0], sys[i]->d_name)) pl = &PLATFORMS[k];
        if (!pl) continue;
        char dir[PATHMAX];
        snprintf(dir, sizeof dir, "%s/%s", root, sys[i]->d_name);
        struct dirent **fl;
        int nf = scandir(dir, &fl, visible, alphasort);
        for (int j = 0; j < nf; j++) {
            const char *fn = fl[j]->d_name, *dot = strrchr(fn, '.');
            int skip = !dot || !strcmp(dot, ".name") || !strcasecmp(dot, ".neo");
            for (int k = 0; COVER_EXT[k] && !skip; k++) skip = !strcasecmp(dot, COVER_EXT[k]);
            if (skip) continue;
            Rom r = {0};
            snprintf(r.sys, sizeof r.sys, "%s", sys[i]->d_name);
            snprintf(r.stem, sizeof r.stem, "%.*s", (int)(dot - fn), fn);
            snprintf(r.dir, sizeof r.dir, "%s", dir);
            if (!force && has_cover(dir, r.stem)) continue;
            r.plat[0] = (*pl)[1];
            r.plat[1] = (*pl)[2];
            r.plat[2] = (*pl)[3];
            region_of(r.stem, r.region, sizeof r.region);
            char t[PATHMAX], np[PATHMAX * 2];
            title_of(r.stem, t, sizeof t);
            add_cand(&r, t);
            snprintf(np, sizeof np, "%s/%s.name", dir, r.stem);
            FILE *f = fopen(np, "r");
            if (f) {
                if (fgets(t, sizeof t, f)) { t[strcspn(t, "\r\n")] = 0; add_cand(&r, t); }
                fclose(f);
            }
            roms = realloc(roms, (size_t)(nroms + 1) * sizeof *roms);
            roms[nroms++] = r;
        }
        for (int j = 0; j < nf; j++) free(fl[j]);
        if (nf >= 0) free(fl);
    }
    for (int i = 0; i < ns; i++) free(sys[i]);
    if (ns >= 0) free(sys);
}

// ---- http ----

static size_t write_file(void *p, size_t sz, size_t n, void *f) { return fwrite(p, sz, n, (FILE *)f); }

static int fetch(const char *url, const char *path, int only_if_newer) {
    char tmp[PATHMAX + 8];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    CURL *c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "arcade-covers/1");
    struct stat st;
    if (only_if_newer && !stat(path, &st)) {
        curl_easy_setopt(c, CURLOPT_TIMECONDITION, (long)CURL_TIMECOND_IFMODSINCE);
        curl_easy_setopt(c, CURLOPT_TIMEVALUE, (long)st.st_mtime);
    }
    CURLcode rc = curl_easy_perform(c);
    long unmet = 0;
    curl_easy_getinfo(c, CURLINFO_CONDITION_UNMET, &unmet);
    curl_easy_cleanup(c);
    fclose(f);
    if (rc != CURLE_OK || unmet) {
        remove(tmp);
        return rc == CURLE_OK ? 0 : -1;
    }
    return rename(tmp, path) ? -1 : 1;
}

// ---- zip: stream one deflated member through a line callback ----

typedef void (*LineFn)(char *line);

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void stream_member(const char *zip, const char *member, LineFn fn) {
    FILE *f = fopen(zip, "rb");
    if (!f) die("can't open %s", zip);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tail = size < 65557 ? size : 65557;
    unsigned char *end = malloc((size_t)tail);
    fseek(f, size - tail, SEEK_SET);
    if (fread(end, 1, (size_t)tail, f) != (size_t)tail) die("short read on %s", zip);
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(end + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) die("%s isn't a zip", zip);
    uint32_t cdsize = rd32(end + eocd + 12), cdoff = rd32(end + eocd + 16);
    free(end);

    unsigned char *cd = malloc(cdsize);
    fseek(f, cdoff, SEEK_SET);
    if (fread(cd, 1, cdsize, f) != cdsize) die("short read on %s", zip);
    long loff = -1;
    uint32_t csize = 0;
    uint16_t method = 0;
    for (uint32_t p = 0; p + 46 <= cdsize;) {
        uint16_t nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        if (nl == strlen(member) && !memcmp(cd + p + 46, member, nl)) {
            method = rd16(cd + p + 10);
            csize = rd32(cd + p + 20);
            loff = rd32(cd + p + 42);
            break;
        }
        p += 46 + nl + xl + cl;
    }
    free(cd);
    if (loff < 0) die("%s has no %s", zip, member);

    unsigned char lh[30];
    fseek(f, loff, SEEK_SET);
    if (fread(lh, 1, 30, f) != 30) die("short read on %s", zip);
    fseek(f, loff + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
    if (method != 8) die("%s: unsupported compression %u", member, method);

    z_stream z = {0};
    inflateInit2(&z, -MAX_WBITS);
    enum { IN = 1 << 16, OUT = 1 << 18 };
    unsigned char *in = malloc(IN);
    char *line = malloc(OUT * 2 + 1);
    size_t have = 0, left = csize;
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        if (have >= OUT * 2) have = 0;  // pathological line, drop it
        if (!z.avail_in) {
            size_t want = left < IN ? left : IN;
            if (!want) break;
            z.avail_in = (uInt)fread(in, 1, want, f);
            if (!z.avail_in) break;
            left -= z.avail_in;
            z.next_in = in;
        }
        z.next_out = (unsigned char *)line + have;
        z.avail_out = (uInt)(OUT * 2 - have);
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) die("%s: inflate failed (%d)", member, rc);
        have = OUT * 2 - z.avail_out;
        size_t start = 0;
        for (size_t i = 0; i < have; i++)
            if (line[i] == '\n') {
                line[i] = 0;
                fn(line + start);
                start = i + 1;
            }
        memmove(line, line + start, have - start);
        have -= start;
    }
    if (have) { line[have] = 0; fn(line); }
    inflateEnd(&z);
    free(in);
    free(line);
    fclose(f);
}

// pull the text of <tag>...</tag> from a single-line element
static int field(const char *line, const char *tag, char *out, size_t n) {
    char open[64];
    snprintf(open, sizeof open, "<%s>", tag);
    const char *p = strstr(line, open);
    if (!p) return 0;
    p += strlen(open);
    const char *e = strstr(p, "</");
    if (!e) return 0;
    snprintf(out, n, "%.*s", (int)(e - p), p);
    unescape(out);
    return 1;
}

// ---- mame.xml: romset name -> title ----

static char mfile[256], mname[512];

static void on_mame(char *l) {
    if (strstr(l, "<MameFile>")) { mfile[0] = mname[0] = 0; return; }
    if (!mfile[0]) field(l, "FileName", mfile, sizeof mfile);
    if (!mname[0]) field(l, "Name", mname, sizeof mname);
    if (strstr(l, "</MameFile>") && mfile[0] && mname[0])
        for (int i = 0; i < nroms; i++)
            if ((!strcmp(roms[i].sys, "arcade") || !strcmp(roms[i].sys, "neogeo") || !strcmp(roms[i].sys, "mame")) &&
                !strcasecmp(roms[i].stem, mfile))
                add_cand(&roms[i], mname);
}

// ---- metadata.xml ----

enum { NONE, GAME, ALT, IMAGE };
static int state;
static char g_name[512], g_plat[128], g_id[32], g_file[256], g_type[128], g_region[128];

// launchbox ids on the platforms we care about, for alternate-name matches
static long *pid;
static const char **pplat;
static size_t npid, cappid;

static const char *plat_of(long id) {
    for (size_t i = 0; i < npid; i++)
        if (pid[i] == id) return pplat[i];
    return NULL;
}

static int plat_ok(const Rom *r, const char *plat) {
    for (int k = 0; k < 3; k++)
        if (r->plat[k] && !strcmp(r->plat[k], plat)) return 1;
    return 0;
}

static int name_hit(const Rom *r, const char *name) {
    char *n = norm(name);
    int hit = 0;
    for (int k = 0; k < r->ncand && !hit; k++) hit = !strcmp(n, r->cand[k]);
    free(n);
    return hit;
}

static int image_score(const char *type) {
    static const struct { const char *t; int s; } T[] = {
        {"Box - Front", 100}, {"Box - Front - Reconstructed", 90}, {"Fanart - Box - Front", 60},
        {"Cart - Front", 40}, {"Disc", 20}, {NULL, 0},
    };
    for (int i = 0; T[i].t; i++)
        if (!strcmp(T[i].t, type)) return T[i].s;
    return 0;
}

static void on_meta(char *l) {
    if (strstr(l, "<Game>")) { state = GAME; g_name[0] = g_plat[0] = g_id[0] = 0; return; }
    if (strstr(l, "<GameAlternateName>")) { state = ALT; g_name[0] = g_id[0] = 0; return; }
    if (strstr(l, "<GameImage>")) { state = IMAGE; g_id[0] = g_file[0] = g_type[0] = g_region[0] = 0; return; }

    if (state == GAME) {
        if (!g_name[0]) field(l, "Name", g_name, sizeof g_name);
        if (!g_plat[0]) field(l, "Platform", g_plat, sizeof g_plat);
        if (!g_id[0]) field(l, "DatabaseID", g_id, sizeof g_id);
        if (!strstr(l, "</Game>")) return;
        state = NONE;
        long id = atol(g_id);
        int wanted = 0;
        for (int i = 0; i < nroms; i++) {
            if (!plat_ok(&roms[i], g_plat)) continue;
            wanted = 1;
            if (roms[i].how < 2 && name_hit(&roms[i], g_name)) {
                roms[i].id = id;
                roms[i].how = 2;
                snprintf(roms[i].matched, sizeof roms[i].matched, "%s", g_name);
            }
        }
        if (wanted) {
            if (npid == cappid) {
                cappid = cappid ? cappid * 2 : 4096;
                pid = realloc(pid, cappid * sizeof *pid);
                pplat = realloc(pplat, cappid * sizeof *pplat);
            }
            for (int k = 0; PLATFORMS[k][0]; k++)
                for (int m = 1; m < 4; m++)
                    if (PLATFORMS[k][m] && !strcmp(PLATFORMS[k][m], g_plat)) pplat[npid] = PLATFORMS[k][m];
            pid[npid++] = id;
        }
    } else if (state == ALT) {
        if (!g_name[0]) field(l, "AlternateName", g_name, sizeof g_name);
        if (!g_id[0]) field(l, "DatabaseID", g_id, sizeof g_id);
        if (!strstr(l, "</GameAlternateName>")) return;
        state = NONE;
        long id = atol(g_id);
        const char *plat = NULL;
        for (int i = 0; i < nroms; i++) {
            if (roms[i].how || !name_hit(&roms[i], g_name)) continue;
            if (!plat) plat = plat_of(id);
            if (plat && plat_ok(&roms[i], plat)) {
                roms[i].id = id;
                roms[i].how = 1;
                snprintf(roms[i].matched, sizeof roms[i].matched, "%s (alt name)", g_name);
            }
        }
    } else if (state == IMAGE) {
        if (!g_id[0]) field(l, "DatabaseID", g_id, sizeof g_id);
        if (!g_file[0]) field(l, "FileName", g_file, sizeof g_file);
        if (!g_type[0]) field(l, "Type", g_type, sizeof g_type);
        if (!g_region[0]) field(l, "Region", g_region, sizeof g_region);
        if (!strstr(l, "</GameImage>")) return;
        state = NONE;
        long id = atol(g_id);
        int ts = image_score(g_type);
        if (!ts) return;
        for (int i = 0; i < nroms; i++) {
            if (!roms[i].how || roms[i].id != id) continue;
            // the rom's own region wins, then north america, then anything
            int s = ts * 10 + (!strcmp(g_region, roms[i].region) ? 5 : !strcmp(g_region, "North America") ? 3 :
                               !g_region[0] || !strcmp(g_region, "World") ? 2 : 0);
            if (s > roms[i].iscore) {
                roms[i].iscore = s;
                snprintf(roms[i].image, sizeof roms[i].image, "%s", g_file);
                snprintf(roms[i].iregion, sizeof roms[i].iregion, "%s", g_region[0] ? g_region : "no region");
            }
        }
    }
}

// box art scans run to several MB; the launcher only needs a card-sized jpeg
static int shrink(const char *in, const char *out) {
    int w, h, c;
    unsigned char *px = stbi_load(in, &w, &h, &c, 3);
    if (!px) return -1;
    int m = w > h ? w : h, nw = w, nh = h;
    if (m > COVER_MAX) {
        nw = (int)((long)w * COVER_MAX / m);
        nh = (int)((long)h * COVER_MAX / m);
    }
    unsigned char *o = px;
    if (nw != w || nh != h) {
        o = stbir_resize_uint8_srgb(px, w, h, 0, NULL, nw, nh, 0, STBIR_RGB);
        if (!o) { stbi_image_free(px); return -1; }
    }
    int ok = stbi_write_jpg(out, nw, nh, 3, o, 85);
    if (o != px) free(o);
    stbi_image_free(px);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    int force = argc > 1 && (!strcmp(argv[1], "-f") || !strcmp(argv[1], "--force"));
    char *self = realpath(argv[0], NULL);
    if (!self) die("can't resolve %s", argv[0]);
    *strrchr(self, '/') = 0;
    char romdir[PATHMAX], cache[PATHMAX], zip[PATHMAX + 16];
    snprintf(romdir, sizeof romdir, "%s/roms", self);
    free(self);

    scan_roms(romdir, force);
    if (!nroms) {
        puts("every game already has a cover (use -f to replace them)");
        return 0;
    }

    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cache, sizeof cache, "%s/arcade", xdg);
    else snprintf(cache, sizeof cache, "%s/.cache/arcade", home ? home : ".");
    mkdir(cache, 0755);
    snprintf(zip, sizeof zip, "%s/Metadata.zip", cache);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("checking launchbox games database...\n");
    int got = fetch(META_URL, zip, 1);
    if (got < 0 && access(zip, R_OK)) die("couldn't download %s", META_URL);
    if (got > 0) printf("downloaded a fresh copy\n");

    stream_member(zip, "Mame.xml", on_mame);
    stream_member(zip, "Metadata.xml", on_meta);

    int ok = 0;
    for (int i = 0; i < nroms; i++) {
        Rom *r = &roms[i];
        if (!r->how || !r->image[0]) {
            printf("miss  %s/%s%s\n", r->sys, r->stem, r->how ? " (found the game, but it has no box art)" : "");
            continue;
        }
        char url[512], out[PATHMAX * 2], raw[PATHMAX + 32];
        snprintf(url, sizeof url, "%s%s", IMAGE_URL, r->image);
        snprintf(out, sizeof out, "%s/%s.jpg", r->dir, r->stem);
        snprintf(raw, sizeof raw, "%s/cover.tmp", cache);
        if (force)
            for (int k = 0; COVER_EXT[k]; k++) {
                char old[PATHMAX * 2];
                snprintf(old, sizeof old, "%s/%s%s", r->dir, r->stem, COVER_EXT[k]);
                remove(old);
            }
        if (fetch(url, raw, 0) < 0 || shrink(raw, out)) {
            remove(raw);
            printf("fail  %s/%s (download or decode error)\n", r->sys, r->stem);
            continue;
        }
        remove(raw);
        struct stat st;
        stat(out, &st);
        printf("ok    %s/%s <- %s [%s] %lld KB\n", r->sys, r->stem, r->matched, r->iregion, (long long)st.st_size / 1024);
        ok++;
    }
    printf("\n%d of %d covers saved\n", ok, nroms);
    curl_global_cleanup();
    return 0;
}
