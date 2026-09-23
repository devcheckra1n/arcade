#define _GNU_SOURCE
#include <dirent.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#define ITERATIONS 600000
#ifndef CHUNK
#define CHUNK (16 * 1024 * 1024)
#endif
#define PATHMAX 4096

static const char *SYSTEMS[] = {
    "nes", "snes", "n64", "gb", "gba", "nds", "vb", "psx", "psp", "segaMD", "segaMS", "segaGG",
    "segaCD", "sega32x", "segaSaturn", "atari2600", "atari7800", "lynx", "jaguar", "pce", "pcfx",
    "pcecd", "ngp", "ws", "coleco", "3do", "arcade", "neogeo", "mame", "dos", NULL,
};

static const char *COVERS[][2] = {
    {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".webp", "image/webp"}, {NULL, NULL},
};

static char root[PATHMAX], roms[PATHMAX], lib[PATHMAX], data[PATHMAX];
static unsigned char enc[32], mac[32];
static unsigned char *inbuf, *outbuf;

typedef struct { char *s; size_t n, cap; } Buf;

typedef struct {
    char *path;
    long long size, mtime;
    char sha[65];
    int hit;
} CacheEntry;

static CacheEntry *cache;
static size_t ncache, capcache;
static char **used;
static size_t nused, capused;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}

static void b_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if (!b->s) die("out of memory");
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void b_str(Buf *b, const char *s) { b_add(b, s, strlen(s)); }

static void b_fmt(Buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    b_add(b, tmp, (size_t)n);
}

static void b_json(Buf *b, const char *s) {
    b_add(b, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            b_add(b, e, 2);
        } else if (c < 0x20) {
            b_fmt(b, "\\u%04x", c);
        } else {
            b_add(b, (const char *)&c, 1);
        }
    }
    b_add(b, "\"", 1);
}

static void hex(const unsigned char *in, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 15];
    }
    out[n * 2] = 0;
}

static int unhex(const char *in, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(in + i * 2, "%2x", &v) != 1) return -1;
        out[i] = (unsigned char)v;
    }
    return 0;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = xmalloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) die("read failed: %s", path);
    fclose(f);
    s[n] = 0;
    if (len) *len = (size_t)n;
    return s;
}

static void spit(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f)) die("write failed: %s", path);
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static size_t seal(const unsigned char *in, size_t n, const char *aad, unsigned char *out) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int len;
    if (RAND_bytes(out, 12) != 1) die("no randomness");
    EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, enc, out);
    EVP_EncryptUpdate(c, NULL, &len, (const unsigned char *)aad, (int)strlen(aad));
    EVP_EncryptUpdate(c, out + 12, &len, in, (int)n);
    EVP_EncryptFinal_ex(c, out + 12 + len, &len);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out + 12 + n);
    EVP_CIPHER_CTX_free(c);
    return 12 + n + 16;
}

static int unseal(const unsigned char *in, size_t n, const char *aad) {
    if (n < 28) return -1;
    size_t ct = n - 28;
    unsigned char *out = xmalloc(ct + 16);
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int len, ok;
    EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, enc, in);
    EVP_DecryptUpdate(c, NULL, &len, (const unsigned char *)aad, (int)strlen(aad));
    EVP_DecryptUpdate(c, out, &len, in + 12, (int)ct);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void *)(in + 12 + ct));
    ok = EVP_DecryptFinal_ex(c, out + len, &len);
    EVP_CIPHER_CTX_free(c);
    free(out);
    return ok > 0 ? 0 : -1;
}

static void ask(const char *prompt, char *buf, size_t n) {
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) die("no terminal to ask for a password (set ARCADE_PASSWORD)");
    struct termios old, off;
    tcgetattr(fileno(tty), &old);
    off = old;
    off.c_lflag &= ~(tcflag_t)ECHO;
    fputs(prompt, tty);
    fflush(tty);
    tcsetattr(fileno(tty), TCSAFLUSH, &off);
    if (!fgets(buf, (int)n, tty)) buf[0] = 0;
    tcsetattr(fileno(tty), TCSAFLUSH, &old);
    fputc('\n', tty);
    fclose(tty);
    buf[strcspn(buf, "\r\n")] = 0;
}

static void load_key(void) {
    char meta[PATHMAX], man[PATHMAX], pw[1024], again[1024];
    unsigned char salt[16], out[64];
    long iterations = ITERATIONS;
    snprintf(meta, sizeof meta, "%s/meta.json", lib);
    snprintf(man, sizeof man, "%s/m", lib);
    int fresh = !exists(meta);

    if (fresh) {
        if (RAND_bytes(salt, 16) != 1) die("no randomness");
    } else {
        char *m = slurp(meta, NULL), *p;
        if (!(p = strstr(m, "\"salt\"")) || !(p = strchr(p + 6, '"')) || unhex(p + 1, salt, 16)) die("bad %s", meta);
        if ((p = strstr(m, "\"iterations\"")) && (p = strchr(p, ':'))) iterations = strtol(p + 1, NULL, 10);
        free(m);
    }

    const char *env = getenv("ARCADE_PASSWORD");
    if (env) {
        snprintf(pw, sizeof pw, "%s", env);
    } else {
        ask("password: ", pw, sizeof pw);
        if (fresh) {
            ask("again: ", again, sizeof again);
            if (strcmp(pw, again)) die("passwords don't match");
        }
    }
    if (!pw[0]) die("empty password");

    PKCS5_PBKDF2_HMAC(pw, (int)strlen(pw), salt, 16, (int)iterations, EVP_sha256(), 64, out);
    memcpy(enc, out, 32);
    memcpy(mac, out + 32, 32);
    OPENSSL_cleanse(pw, sizeof pw);

    if (!fresh && exists(man)) {
        size_t n;
        char *m = slurp(man, &n);
        if (unseal((unsigned char *)m, n, "manifest")) die("wrong password for this library");
        free(m);
    }
    if (fresh) {
        char s[33], body[160];
        mkdir(lib, 0755);
        hex(salt, 16, s);
        int n = snprintf(body, sizeof body, "{\"v\": 1, \"kdf\": \"PBKDF2-SHA256\", \"iterations\": %d, \"salt\": \"%s\"}\n", ITERATIONS, s);
        spit(meta, body, (size_t)n);
    }
}

static void cache_load(void) {
    char p[PATHMAX], line[PATHMAX + 128];
    snprintf(p, sizeof p, "%s/.cache", roms);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *tab[3], *s = line;
        int k = 0;
        for (; *s && k < 3; s++)
            if (*s == '\t') { *s = 0; tab[k++] = s + 1; }
        if (k < 3) continue;
        char *sha = tab[2];
        sha[strcspn(sha, "\r\n")] = 0;
        if (strlen(sha) != 64) continue;
        if (ncache == capcache) cache = realloc(cache, (capcache = capcache ? capcache * 2 : 64) * sizeof *cache);
        CacheEntry *e = &cache[ncache++];
        e->path = strdup(line);
        e->size = atoll(tab[0]);
        e->mtime = atoll(tab[1]);
        memcpy(e->sha, sha, 65);
        e->hit = 0;
    }
    fclose(f);
}

static void cache_save(void) {
    char p[PATHMAX];
    snprintf(p, sizeof p, "%s/.cache", roms);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (size_t i = 0; i < ncache; i++)
        if (cache[i].hit) fprintf(f, "%s\t%lld\t%lld\t%s\n", cache[i].path, cache[i].size, cache[i].mtime, cache[i].sha);
    fclose(f);
}

static void sha_file(const char *path, const char *rel, unsigned char out[32]) {
    struct stat st;
    if (stat(path, &st)) die("stat failed: %s", path);
    long long mt = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    for (size_t i = 0; i < ncache; i++) {
        CacheEntry *e = &cache[i];
        if (!strcmp(e->path, rel) && e->size == st.st_size && e->mtime == mt) {
            e->hit = 1;
            unhex(e->sha, out, 32);
            return;
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) die("can't read %s", path);
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    size_t n;
    while ((n = fread(inbuf, 1, CHUNK, f)) > 0) EVP_DigestUpdate(c, inbuf, n);
    EVP_DigestFinal_ex(c, out, NULL);
    EVP_MD_CTX_free(c);
    fclose(f);
    if (ncache == capcache) cache = realloc(cache, (capcache = capcache ? capcache * 2 : 64) * sizeof *cache);
    CacheEntry *e = &cache[ncache++];
    e->path = strdup(rel);
    e->size = st.st_size;
    e->mtime = mt;
    hex(out, 32, e->sha);
    e->hit = 1;
}

static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(p);
}

static void rmtree(const char *p) { nftw(p, rm_cb, 16, FTW_DEPTH | FTW_PHYS); }

static int count_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static void mark_used(const char *id) {
    if (nused == capused) used = realloc(used, (capused = capused ? capused * 2 : 64) * sizeof *used);
    used[nused++] = strdup(id);
}

// reader: either a file or an in-memory buffer
typedef struct { FILE *f; const unsigned char *mem; size_t left; } Src;

static size_t src_read(Src *s, unsigned char *dst, size_t n) {
    if (s->f) return fread(dst, 1, n, s->f);
    if (n > s->left) n = s->left;
    memcpy(dst, s->mem, n);
    s->mem += n;
    s->left -= n;
    return n;
}

// ids are keyed so the public repo can't be matched against known rom hashes
static int store(const unsigned char sha[32], Src *src, long long size, char id[25]) {
    unsigned char h[32];
    unsigned int hl;
    char full[65], dir[PATHMAX], tmp[PATHMAX], part[PATHMAX + 16], aad[64];
    HMAC(EVP_sha256(), mac, 32, sha, 32, h, &hl);
    hex(h, 32, full);
    memcpy(id, full, 24);
    id[24] = 0;
    int parts = size <= 0 ? 1 : (int)((size + CHUNK - 1) / CHUNK);
    mark_used(id);

    snprintf(dir, sizeof dir, "%s/%s", data, id);
    if (count_files(dir) == parts) return parts;

    snprintf(tmp, sizeof tmp, "%s/%s.tmp", data, id);
    rmtree(tmp);
    mkdir(lib, 0755);
    mkdir(data, 0755);
    if (mkdir(tmp, 0755)) die("mkdir failed: %s", tmp);
    for (int i = 0; i < parts; i++) {
        size_t n = src_read(src, inbuf, CHUNK);
        snprintf(aad, sizeof aad, "%s/%d", id, i);
        snprintf(part, sizeof part, "%s/%d", tmp, i);
        spit(part, outbuf, seal(inbuf, n, aad, outbuf));
    }
    rmtree(dir);
    if (rename(tmp, dir)) die("rename failed: %s", dir);
    printf("  packed %d part%s\n", parts, parts > 1 ? "s" : "");
    return parts;
}

static int store_file(const char *path, const char *rel, char id[25]) {
    unsigned char sha[32];
    struct stat st;
    sha_file(path, rel, sha);
    stat(path, &st);
    Src s = {fopen(path, "rb"), NULL, 0};
    if (!s.f) die("can't read %s", path);
    int parts = store(sha, &s, st.st_size, id);
    fclose(s.f);
    return parts;
}

static int store_mem(const unsigned char *p, size_t n, char id[25]) {
    unsigned char sha[32];
    EVP_Digest(p, n, sha, NULL, EVP_sha256(), NULL);
    Src s = {NULL, p, n};
    return store(sha, &s, (long long)n, id);
}

static uint32_t crc32_of(const unsigned char *p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void le16(Buf *b, unsigned v) { char x[2] = {(char)v, (char)(v >> 8)}; b_add(b, x, 2); }
static void le32(Buf *b, uint32_t v) { char x[4] = {(char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24)}; b_add(b, x, 4); }

static int visible(const struct dirent *e) { return e->d_name[0] != '.'; }

// stored zip with a fixed timestamp, so an unchanged bios zips to the same bytes
static Buf bios_zip(const char *dir) {
    Buf z = {0}, cd = {0};
    struct dirent **list;
    int n = scandir(dir, &list, visible, alphasort), count = 0;
    for (int i = 0; i < n; i++) {
        char p[PATHMAX];
        snprintf(p, sizeof p, "%s/%s", dir, list[i]->d_name);
        struct stat st;
        if (stat(p, &st) || !S_ISREG(st.st_mode)) { free(list[i]); continue; }
        size_t len;
        unsigned char *body = (unsigned char *)slurp(p, &len);
        uint32_t crc = crc32_of(body, len), off = (uint32_t)z.n;
        size_t nl = strlen(list[i]->d_name);
        le32(&z, 0x04034b50); le16(&z, 20); le16(&z, 0); le16(&z, 0); le16(&z, 0); le16(&z, 0x21);
        le32(&z, crc); le32(&z, (uint32_t)len); le32(&z, (uint32_t)len); le16(&z, (unsigned)nl); le16(&z, 0);
        b_add(&z, list[i]->d_name, nl);
        b_add(&z, (char *)body, len);
        le32(&cd, 0x02014b50); le16(&cd, 20); le16(&cd, 20); le16(&cd, 0); le16(&cd, 0); le16(&cd, 0); le16(&cd, 0x21);
        le32(&cd, crc); le32(&cd, (uint32_t)len); le32(&cd, (uint32_t)len); le16(&cd, (unsigned)nl);
        le16(&cd, 0); le16(&cd, 0); le16(&cd, 0); le16(&cd, 0); le32(&cd, 0); le32(&cd, off);
        b_add(&cd, list[i]->d_name, nl);
        free(body);
        free(list[i]);
        count++;
    }
    free(list);
    uint32_t cdoff = (uint32_t)z.n;
    b_add(&z, cd.s ? cd.s : "", cd.n);
    le32(&z, 0x06054b50); le16(&z, 0); le16(&z, 0); le16(&z, (unsigned)count); le16(&z, (unsigned)count);
    le32(&z, (uint32_t)cd.n); le32(&z, cdoff); le16(&z, 0);
    free(cd.s);
    return z;
}

static int lone_zip(const char *dir, char *name) {
    struct dirent **list;
    int n = scandir(dir, &list, visible, alphasort), ok = 0;
    if (n == 1) {
        const char *dot = strrchr(list[0]->d_name, '.');
        if (dot && !strcasecmp(dot, ".zip")) {
            snprintf(name, PATHMAX, "%s", list[0]->d_name);
            ok = 1;
        }
    }
    for (int i = 0; i < n; i++) free(list[i]);
    if (n >= 0) free(list);
    return ok;
}

static int loadable(const char *sys, const char *name) {
    if (strcmp(sys, "neogeo") && strcmp(sys, "arcade")) return 1;
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".zip") || !strcasecmp(dot, ".7z"));
}

static int is_system(const char *s) {
    for (int i = 0; SYSTEMS[i]; i++)
        if (!strcmp(SYSTEMS[i], s)) return 1;
    return 0;
}

static const char *cover_type(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return NULL;
    for (int i = 0; COVERS[i][0]; i++)
        if (!strcasecmp(dot, COVERS[i][0])) return COVERS[i][1];
    return NULL;
}

static size_t stem_len(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && dot != name ? (size_t)(dot - name) : strlen(name);
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv) {
    (void)argc;
    char *self = realpath(argv[0], NULL);
    if (!self) die("can't resolve %s", argv[0]);
    snprintf(root, sizeof root, "%s", self);
    *strrchr(root, '/') = 0;
    free(self);
    snprintf(roms, sizeof roms, "%s/roms", root);
    snprintf(lib, sizeof lib, "%s/lib", root);
    snprintf(data, sizeof data, "%s/d", lib);
    if (!is_dir(roms)) die("no %s folder", roms);

    inbuf = xmalloc(CHUNK);
    outbuf = xmalloc(CHUNK + 28);
    load_key();
    cache_load();

    Buf man = {0};
    int ngames = 0, nbios = 0;
    b_str(&man, "{\"v\":1,\"games\":[");

    struct dirent **sys;
    int ns = scandir(roms, &sys, visible, alphasort);
    for (int i = 0; i < ns; i++) {
        char sd[PATHMAX];
        snprintf(sd, sizeof sd, "%s/%s", roms, sys[i]->d_name);
        if (!is_dir(sd) || !strcmp(sys[i]->d_name, "bios")) continue;
        if (!is_system(sys[i]->d_name)) {
            printf("skipping %s/: not a system name\n", sys[i]->d_name);
            continue;
        }
        struct dirent **fl;
        int nf = scandir(sd, &fl, visible, alphasort);
        for (int j = 0; j < nf; j++) {
            const char *fn = fl[j]->d_name;
            char fp[PATHMAX * 2], rel[PATHMAX * 2], id[25];
            snprintf(fp, sizeof fp, "%s/%s", sd, fn);
            if (is_dir(fp) || cover_type(fn)) continue;
            if (!loadable(sys[i]->d_name, fn)) {
                printf("skipping %s/%s: fbneo only loads .zip or .7z romsets\n", sys[i]->d_name, fn);
                continue;
            }
            snprintf(rel, sizeof rel, "%s/%s", sys[i]->d_name, fn);
            printf("%s\n", rel);
            int parts = store_file(fp, rel, id);
            size_t sl = stem_len(fn);
            char stem[PATHMAX];
            snprintf(stem, sizeof stem, "%.*s", (int)sl, fn);

            if (ngames++) b_str(&man, ",");
            b_str(&man, "{\"name\":");
            b_json(&man, stem);
            b_str(&man, ",\"core\":");
            b_json(&man, sys[i]->d_name);
            b_fmt(&man, ",\"file\":{\"id\":\"%s\",\"parts\":%d,\"name\":", id, parts);
            b_json(&man, fn);
            b_str(&man, "}");

            for (int k = 0; k < nf; k++) {
                const char *cn = fl[k]->d_name, *type = cover_type(cn);
                if (!type || stem_len(cn) != sl || strncmp(cn, fn, sl)) continue;
                char cp[PATHMAX * 2], crel[PATHMAX * 2], cid[25];
                snprintf(cp, sizeof cp, "%s/%s", sd, cn);
                snprintf(crel, sizeof crel, "%s/%s", sys[i]->d_name, cn);
                int cparts = store_file(cp, crel, cid);
                b_fmt(&man, ",\"cover\":{\"id\":\"%s\",\"parts\":%d,\"type\":\"%s\"}", cid, cparts, type);
                break;
            }
            b_str(&man, "}");
        }
        for (int j = 0; j < nf; j++) free(fl[j]);
        free(fl);
    }
    b_str(&man, "],\"bios\":{");

    char bd[PATHMAX + 8];
    snprintf(bd, sizeof bd, "%s/bios", roms);
    if (is_dir(bd)) {
        struct dirent **bl;
        int nb = scandir(bd, &bl, visible, alphasort);
        for (int j = 0; j < nb; j++) {
            char p[PATHMAX * 2], id[25];
            snprintf(p, sizeof p, "%s/%s", bd, bl[j]->d_name);
            if (is_dir(p)) {
                printf("bios/%s\n", bl[j]->d_name);
                char name[PATHMAX] = "bios.zip", zp[PATHMAX * 3], zrel[PATHMAX * 2];
                int parts;
                // a lone zip (neogeo.zip) is kept as-is so it arrives under its own name
                if (lone_zip(p, name)) {
                    snprintf(zp, sizeof zp, "%s/%s", p, name);
                    snprintf(zrel, sizeof zrel, "bios/%s/%s", bl[j]->d_name, name);
                    parts = store_file(zp, zrel, id);
                } else {
                    Buf z = bios_zip(p);
                    parts = store_mem((unsigned char *)z.s, z.n, id);
                    free(z.s);
                }
                if (nbios++) b_str(&man, ",");
                b_json(&man, bl[j]->d_name);
                b_fmt(&man, ":{\"id\":\"%s\",\"parts\":%d,\"name\":", id, parts);
                b_json(&man, name);
                b_str(&man, "}");
            }
            free(bl[j]);
        }
        free(bl);
    }
    b_str(&man, "}}");

    char mp[PATHMAX + 8];
    snprintf(mp, sizeof mp, "%s/m", lib);
    unsigned char *sealed = xmalloc(man.n + 28);
    spit(mp, sealed, seal((unsigned char *)man.s, man.n, "manifest", sealed));

    long long total = 0;
    struct dirent **dl;
    int nd = scandir(data, &dl, visible, alphasort);
    for (int i = 0; i < nd; i++) {
        char p[PATHMAX * 2];
        int keep = 0;
        snprintf(p, sizeof p, "%s/%s", data, dl[i]->d_name);
        for (size_t k = 0; k < nused && !keep; k++) keep = !strcmp(used[k], dl[i]->d_name);
        if (!keep) {
            rmtree(p);
            printf("removed %s\n", dl[i]->d_name);
        } else {
            struct dirent **pl;
            int np = scandir(p, &pl, visible, alphasort);
            for (int k = 0; k < np; k++) {
                char pp[PATHMAX * 3];
                struct stat st;
                snprintf(pp, sizeof pp, "%s/%s", p, pl[k]->d_name);
                if (!stat(pp, &st)) total += st.st_size;
                free(pl[k]);
            }
            if (np > 0) free(pl);
        }
        free(dl[i]);
    }
    if (nd > 0) free(dl);

    cache_save();
    for (int i = 0; i < ns; i++) free(sys[i]);
    free(sys);
    printf("\n%d games, %d bios, %.1f MB in lib/\n", ngames, nbios, total / 1e6);
    return 0;
}
