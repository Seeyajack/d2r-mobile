// 鸿蒙侧最小 nn::fs 实现。设计取舍见 dm_fs_ohos.h 顶部注释。
// Minimal nn::fs implementation for HarmonyOS. See the header for the rationale.
//
// 两条读取后端：
//   ① 内嵌归档（dm_romfs.S，段 dmromfs）：启动必需的 romfs 子集直接编进 .so，从内存读；
//      鸿蒙应用的沙箱看不到 /data/local/tmp（真机实证 access→ENOENT），hdc 又写不进应用沙箱，
//      所以"放进 HAP"是唯一稳定的投递方式。
//   ② 宿主目录回退（DM_ROMFS，默认 /data/local/tmp/romfs）：归档里没有的路径再试文件系统，
//      留给以后"推到别处 / 大文件流式"的扩展。
// Two read backends: (1) the embedded archive (section dmromfs) for the boot-essential subset --
// the HarmonyOS sandbox cannot see /data/local/tmp (access -> ENOENT on device) and hdc cannot
// write into the app sandbox, so "inside the HAP" is the only reliable delivery; (2) a host
// directory fallback (DM_ROMFS) for paths not in the archive, kept for future streaming.
#include "dm_fs_ohos.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// 返回码按公开 Horizon ABI（与上游 hle_fs.c 同值）。
#define RES_OK              0u
#define RES_PATH_NOT_FOUND  0x202u
#define RES_PATH_EXISTS     0x402u
#define RES_FAIL            0x2A2u

enum { MODE_READ = 1, MODE_WRITE = 2, MODE_APPEND = 4 };
enum { ENT_DIR = 0, ENT_FILE = 1 };
enum { DIRMODE_DIR = 1, DIRMODE_FILE = 2 };

// DirectoryEntry = {char name[0x301]; pad[3]; s8 type; pad[3]; s64 size}（0x310 字节）
typedef struct {
    char name[0x301];
    uint8_t pad0[3];
    int8_t type;
    uint8_t pad1[3];
    int64_t size;
} dm_dir_entry;
_Static_assert(sizeof(dm_dir_entry) == 0x310, "DirectoryEntry 布局应为 0x310");

static void (*g_log)(const char *, ...);
static uint64_t g_fs_base;   // np80：映像基址（探针注入，用于把 LR 换算成偏移）
void dm_fs_set_log(void (*fn)(const char *, ...)) { g_log = fn; }
void dm_fs_set_base(void *base) { g_fs_base = (uint64_t)(uintptr_t)base; }
#define FSLOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

// ---------- 内嵌归档 ----------
// 归档格式见 dm/ohos/tools/mk_romfs_pack.py：magic "DMRF" + version + count + 0x110/条索引。
extern const unsigned char __start_dmromfs[], __stop_dmromfs[];
#define ARC_ENT   0x110
#define ARC_HANDLE 0x8000000000000000ull      // 归档句柄标志（与 OS fd 区分）

static const unsigned char *g_arc;
static uint32_t g_arc_n;
static int g_arc_state;                        // 0 未探测 / 1 有 / -1 无
static size_t g_arc_len;

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const unsigned char *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static void arc_init(void) {
    if (g_arc_state) return;
    g_arc = __start_dmromfs;
    g_arc_len = (size_t)(__stop_dmromfs - __start_dmromfs);
    if (g_arc_len > 0x10 && !memcmp(g_arc, "DMRF", 4)) {
        uint32_t ver = rd32(g_arc + 4), n = rd32(g_arc + 8);
        if (ver == 1 && n && 0x10 + (uint64_t)n * ARC_ENT <= g_arc_len) {
            g_arc_n = n;
            g_arc_state = 1;
            FSLOG("[fs] 内嵌归档：%u 个文件，%.1f MB\n", n, g_arc_len / 1048576.0);
            return;
        }
    }
    g_arc_state = -1;
    FSLOG("[fs] 内嵌归档：无（仅宿主目录回退）\n");
}

// 客体路径 → 归档相对键："content:/System/x" / "/System/x" / "System/x" → "System/x"
static int arc_key(const char *g, char *out, size_t n) {
    if (!g) return 0;
    const char *p = g;
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    if (colon && (!slash || colon < slash)) p = colon + 1;    // 去掉 "<挂载名>:"
    while (*p == '/') p++;
    if (!*p || strstr(p, "..")) return 0;
    if (strlen(p) >= n) return 0;
    snprintf(out, n, "%s", p);
    return 1;
}

static int arc_index(const char *key) {
    arc_init();
    for (uint32_t i = 0; i < g_arc_n; i++)
        if (!strcmp((const char *)(g_arc + 0x10 + (uint64_t)i * ARC_ENT), key)) return (int)i;
    return -1;
}

static const unsigned char *arc_ent_ptr(int idx) { return g_arc + 0x10 + (uint64_t)idx * ARC_ENT; }
static uint64_t arc_ent_off(int idx) { return rd64(arc_ent_ptr(idx) + 0x100); }
static uint64_t arc_ent_size(int idx) { return rd64(arc_ent_ptr(idx) + 0x108); }

// 目录判定：有任意条目以 "<key>/" 开头即算目录（归档里只存文件，目录是推导出来的）
static int arc_is_dir(const char *key) {
    arc_init();
    if (!*key) return 1;
    size_t kl = strlen(key);
    for (uint32_t i = 0; i < g_arc_n; i++) {
        const char *p = (const char *)arc_ent_ptr(i);
        if (!strncmp(p, key, kl) && p[kl] == '/') return 1;
    }
    return 0;
}

// ---------- 宿主目录回退 ----------
static const char *rom_root(void) {
    const char *e = getenv("DM_ROMFS");
    return (e && *e) ? e : "/data/local/tmp/romfs";
}
static const char *save_root(void) {
    const char *e = getenv("DM_SAVE");
    return (e && *e) ? e : "/data/local/tmp/dmsave";
}
static int to_host(const char *g, char *out, size_t n) {
    const char *root = rom_root();
    if (!g) return 0;
    if (!strncmp(g, "save:", 5)) { root = save_root(); g += 5; }
    const char *p = g;
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    if (colon && (!slash || colon < slash)) p = colon + 1;
    while (*p == '/') p++;
    if (!*p || strstr(p, "..")) { out[0] = 0; return 0; }
    int w = snprintf(out, n, "%s/%s", root, p);
    return w > 0 && (size_t)w < n;
}

// ---------- 文件 ----------
static uint32_t fs_query_rom_cache(size_t *out) { *out = 0x1000; return RES_OK; }

static uint32_t fs_mount_rom(const char *name, void *cache, size_t sz) {
    (void)cache; (void)sz;
    // np80：谁挂的载？x30 = 调用者的返回地址（宿主函数入口就能读，不需要额外 plumbing）。
    //   基址通过 dm_fs_set_base() 由探针注入；没有基址时打原始值。
    {
        uint64_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
        if (g_fs_base && lr >= g_fs_base && lr < g_fs_base + 0x2404a70ull)
            FSLOG("[fs] MountRom 调用者 = +%#llx（映像内）\n",
                  (unsigned long long)(lr - g_fs_base));
        else
            FSLOG("[fs] MountRom 调用者 = %#llx（映像外）\n", (unsigned long long)lr);
    }
    arc_init();
    FSLOG("[fs] MountRom(%s)：归档 %u 文件%s\n", name && *name ? name : "?", g_arc_n,
          g_arc_state == 1 ? "，宿主回退启用" : "（无归档，仅宿主回退）");
    return RES_OK;
}

static uint32_t fs_unmount(const char *name) { (void)name; return RES_OK; }

static uint32_t fs_open_file(uint64_t *out, const char *path, int mode) {
    char key[1024];
    if (!(mode & (MODE_WRITE | MODE_APPEND)) && arc_key(path, key, sizeof key)) {
        int idx = arc_index(key);
        if (idx >= 0) { *out = ARC_HANDLE | (uint64_t)idx; FSLOG("[fs] open(归档) %s → #%d\n", key, idx); return RES_OK; }
    }
    char hp[1024];
    if (!to_host(path, hp, sizeof hp)) return RES_PATH_NOT_FOUND;
    int wr = mode & (MODE_WRITE | MODE_APPEND);
    int fl = wr ? ((mode & MODE_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    int fd = open(hp, fl);
    if (fd < 0) {
        FSLOG("[fs] open 失败 %s（%s）errno=%d\n", path, hp, errno);
        return (errno == ENOENT || errno == ENOTDIR) ? RES_PATH_NOT_FOUND : RES_FAIL;
    }
    *out = (uint64_t)fd;
    FSLOG("[fs] open %s → fd %d（%s）\n", path, fd, wr ? "写" : "读");
    return RES_OK;
}

static uint32_t fs_read_file_opt(size_t *out, uint64_t h, int64_t off, void *buf, size_t sz, const void *opt) {
    (void)opt;
    if (out) *out = 0;
    if (h & ARC_HANDLE) {
        int idx = (int)(h & ~ARC_HANDLE);
        if (idx < 0 || (uint32_t)idx >= g_arc_n) return RES_FAIL;
        uint64_t esz = arc_ent_size(idx);
        if (off < 0) off = 0;
        if ((uint64_t)off >= esz) return RES_OK;
        size_t n = (size_t)(esz - (uint64_t)off);
        if (n > sz) n = sz;
        memcpy(buf, g_arc + arc_ent_off(idx) + (uint64_t)off, n);
        if (out) *out = n;
        return RES_OK;
    }
    if (!h) return RES_FAIL;
    size_t got = 0;
    while (got < sz) {
        ssize_t r = pread((int)h, (uint8_t *)buf + got, sz - got, (off_t)off + (off_t)got);
        if (r < 0) { if (errno == EINTR) continue; FSLOG("[fs] pread 失败 fd=%d errno=%d\n", (int)h, errno); return RES_FAIL; }
        if (r == 0) break;
        got += (size_t)r;
    }
    if (out) *out = got;
    return RES_OK;
}
static uint32_t fs_read_file(size_t *out, uint64_t h, int64_t off, void *buf, size_t sz) {
    return fs_read_file_opt(out, h, off, buf, sz, NULL);
}

static uint32_t fs_write_file(uint64_t h, int64_t off, const void *buf, size_t sz, const void *opt) {
    (void)opt;
    if (!h || (h & ARC_HANDLE)) return RES_FAIL;      // 归档只读
    size_t put = 0;
    while (put < sz) {
        ssize_t r = pwrite((int)h, (const uint8_t *)buf + put, sz - put, (off_t)off + (off_t)put);
        if (r < 0) { if (errno == EINTR) continue; return RES_FAIL; }
        put += (size_t)r;
    }
    return RES_OK;
}

static uint32_t fs_get_file_size(int64_t *out, uint64_t h) {
    if (h & ARC_HANDLE) {
        int idx = (int)(h & ~ARC_HANDLE);
        if (idx < 0 || (uint32_t)idx >= g_arc_n) return RES_FAIL;
        *out = (int64_t)arc_ent_size(idx);
        return RES_OK;
    }
    struct stat st;
    if (!h || fstat((int)h, &st)) return RES_FAIL;
    *out = (int64_t)st.st_size;
    return RES_OK;
}

static uint32_t fs_set_file_size(uint64_t h, int64_t sz) {
    if (!h || (h & ARC_HANDLE)) return RES_FAIL;
    return ftruncate((int)h, (off_t)sz) ? RES_FAIL : RES_OK;
}
static uint32_t fs_close_file(uint64_t h) { if (h && !(h & ARC_HANDLE)) close((int)h); return RES_OK; }
static uint32_t fs_flush_file(uint64_t h) { if (h && !(h & ARC_HANDLE)) fsync((int)h); return RES_OK; }

static uint32_t fs_create_file(const char *path, int64_t size) {
    char hp[1024];
    if (!to_host(path, hp, sizeof hp)) return RES_PATH_NOT_FOUND;
    int fd = open(hp, O_CREAT | O_WRONLY, 0666);
    if (fd < 0) return RES_FAIL;
    if (size > 0) ftruncate(fd, (off_t)size);
    close(fd);
    return RES_OK;
}

static uint32_t fs_delete_file(const char *path) {
    char hp[1024];
    if (!to_host(path, hp, sizeof hp)) return RES_PATH_NOT_FOUND;
    return unlink(hp) ? RES_FAIL : RES_OK;
}

static uint32_t fs_get_entry_type(int32_t *out, const char *path) {
    char key[1024];
    if (arc_key(path, key, sizeof key)) {
        if (!*key || arc_is_dir(key)) { *out = ENT_DIR; return RES_OK; }
        if (arc_index(key) >= 0) { *out = ENT_FILE; return RES_OK; }
    }
    char hp[1024];
    struct stat st;
    if (!to_host(path, hp, sizeof hp) || stat(hp, &st)) return RES_PATH_NOT_FOUND;
    *out = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE;
    return RES_OK;
}

// ---------- 目录 ----------
typedef struct {
    DIR *d;                          // 宿主目录（归档目录为 NULL）
    char path[1024];
    int mode;
    dm_dir_entry *ents;              // 归档目录的快照
    int64_t n, pos;
} dm_dir_handle;

// 归档里 "<key>/" 的直接子项（去重）
static int64_t arc_children(const char *key, int mode, dm_dir_entry *out, int64_t max) {
    size_t kl = strlen(key);
    int64_t n = 0;
    for (uint32_t i = 0; i < g_arc_n && n < max; i++) {
        const char *p = (const char *)arc_ent_ptr(i);
        if (kl) { if (strncmp(p, key, kl) || p[kl] != '/') continue; }
        const char *rest = p + (kl ? kl + 1 : 0);
        const char *end = strchr(rest, '/');
        size_t nl = end ? (size_t)(end - rest) : strlen(rest);
        if (nl >= sizeof out[0].name) continue;
        int isdir = end != NULL;
        if (!(mode & (isdir ? DIRMODE_DIR : DIRMODE_FILE))) continue;
        // 去重
        int dup = 0;
        for (int64_t j = 0; j < n; j++)
            if (strlen(out[j].name) == nl && !strncmp(out[j].name, rest, nl)) { dup = 1; break; }
        if (dup) continue;
        dm_dir_entry *e = &out[n++];
        memset(e, 0, sizeof *e);
        memcpy(e->name, rest, nl);
        e->type = (int8_t)(isdir ? ENT_DIR : ENT_FILE);
        if (!isdir) {
            char full[1024];
            snprintf(full, sizeof full, "%s%s%.*s", key, kl ? "/" : "", (int)nl, rest);
            int idx = arc_index(full);
            e->size = idx >= 0 ? (int64_t)arc_ent_size(idx) : 0;
        }
    }
    return n;
}

static uint32_t fs_open_dir(uint64_t *out, const char *path, int mode) {
    char key[1024];
    arc_init();
    if (g_arc_state == 1 && arc_key(path, key, sizeof key) && (!*key || arc_is_dir(key))) {
        dm_dir_handle *h = calloc(1, sizeof *h);
        if (!h) return RES_FAIL;
        h->mode = mode;
        snprintf(h->path, sizeof h->path, "%s", key);
        h->ents = calloc(4096, sizeof *h->ents);
        if (!h->ents) { free(h); return RES_FAIL; }
        h->n = arc_children(key, mode, h->ents, 4096);
        h->pos = 0;
        *out = (uint64_t)(uintptr_t)h;
        FSLOG("[fs] opendir(归档) %s → %lld 项\n", key, (long long)h->n);
        return RES_OK;
    }
    char hp[1024];
    if (!to_host(path, hp, sizeof hp)) return RES_PATH_NOT_FOUND;
    DIR *d = opendir(hp);
    if (!d) { FSLOG("[fs] opendir 失败 %s errno=%d\n", hp, errno); return (errno == ENOENT) ? RES_PATH_NOT_FOUND : RES_FAIL; }
    dm_dir_handle *h = calloc(1, sizeof *h);
    if (!h) { closedir(d); return RES_FAIL; }
    h->d = d;
    h->mode = mode;
    snprintf(h->path, sizeof h->path, "%s", hp);
    *out = (uint64_t)(uintptr_t)h;
    FSLOG("[fs] opendir %s\n", path);
    return RES_OK;
}

static uint32_t fs_read_dir(int64_t *count, void *ents, uint64_t hh, int64_t max) {
    if (!hh) { *count = 0; return RES_FAIL; }
    dm_dir_handle *h = (dm_dir_handle *)(uintptr_t)hh;
    dm_dir_entry *out = (dm_dir_entry *)ents;
    if (h->ents) {                                    // 归档快照
        int64_t n = 0;
        while (n < max && h->pos < h->n) out[n++] = h->ents[h->pos++];
        *count = n;
        return RES_OK;
    }
    int64_t n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(h->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1280];
        snprintf(c, sizeof c, "%s/%s", h->path, e->d_name);
        struct stat st;
        if (stat(c, &st)) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (!(h->mode & (isdir ? DIRMODE_DIR : DIRMODE_FILE))) continue;
        dm_dir_entry *o = &out[n++];
        memset(o, 0, sizeof *o);
        strncpy(o->name, e->d_name, sizeof o->name - 1);
        o->type = (int8_t)(isdir ? ENT_DIR : ENT_FILE);
        o->size = isdir ? 0 : (int64_t)st.st_size;
    }
    *count = n;
    return RES_OK;
}

static void fs_close_dir(uint64_t hh) {
    if (!hh) return;
    dm_dir_handle *h = (dm_dir_handle *)(uintptr_t)hh;
    if (h->d) closedir(h->d);
    free(h->ents);
    free(h);
}

static uint32_t fs_create_dir(const char *path) {
    char hp[1024];
    if (!to_host(path, hp, sizeof hp)) return RES_PATH_NOT_FOUND;
    if (!mkdir(hp, 0777)) return RES_OK;
    if (errno == EEXIST) return RES_PATH_EXISTS;
    // 中间目录可能不存在：逐级建
    for (char *p = hp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(hp, 0777);
            *p = '/';
        }
    }
    return mkdir(hp, 0777) ? (errno == EEXIST ? RES_OK : RES_FAIL) : RES_OK;
}

static uint32_t fs_delete_dir_rec(const char *path) {
    char hp[1024];
    if (!to_host(path, hp, sizeof hp)) return RES_PATH_NOT_FOUND;
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", hp);   // 只用于存档/临时区；rom 是只读的
    return system(cmd) ? RES_FAIL : RES_OK;
}

// ---------- 存档（第一版：能挂、能写；不做原子提交） ----------
static uint32_t fs_mount_save(const char *name, const void *uid) {
    (void)uid;
    mkdir(save_root(), 0777);
    FSLOG("[fs] MountSaveData(%s) → %s（可写）\n", name && *name ? name : "?", save_root());
    return RES_OK;
}
static uint32_t fs_commit_save(const char *name) { (void)name; return RES_OK; }

// ---------- 杂项 ----------
static uint32_t fs_set_bool(int v) { (void)v; return RES_OK; }

// ---------- 符号表 ----------
typedef struct { const char *sym; void *fn; } dm_fs_entry;
static const dm_fs_entry g_fs_tab[] = {
    { "_ZN2nn2fs17SetLocalAccessLogEb", (void *)fs_set_bool },
    { "_ZN2nn2fs29SetResultHandledByApplicationEb", (void *)fs_set_bool },
    { "_ZN2nn2fs7UnmountEPKc", (void *)fs_unmount },
    { "_ZN2nn2fs22QueryMountRomCacheSizeEPm", (void *)fs_query_rom_cache },
    { "_ZN2nn2fs8MountRomEPKcPvm", (void *)fs_mount_rom },
    { "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", (void *)fs_get_entry_type },
    { "_ZN2nn2fs10CreateFileEPKcl", (void *)fs_create_file },
    { "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", (void *)fs_open_file },
    { "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", (void *)fs_set_file_size },
    { "_ZN2nn2fs9CloseFileENS0_10FileHandleE", (void *)fs_close_file },
    { "_ZN2nn2fs9FlushFileENS0_10FileHandleE", (void *)fs_flush_file },
    { "_ZN2nn2fs10DeleteFileEPKc", (void *)fs_delete_file },
    { "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", (void *)fs_read_file_opt },
    { "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", (void *)fs_read_file },
    { "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", (void *)fs_write_file },
    { "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", (void *)fs_get_file_size },
    { "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", (void *)fs_open_dir },
    { "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", (void *)fs_close_dir },
    { "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", (void *)fs_read_dir },
    { "_ZN2nn2fs15CreateDirectoryEPKc", (void *)fs_create_dir },
    { "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", (void *)fs_mount_save },
    { "_ZN2nn2fs14CommitSaveDataEPKc", (void *)fs_commit_save },
    { "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", (void *)fs_delete_dir_rec },
};

void *dm_fs_impl(const char *sym_name) {
    if (!sym_name) return NULL;
    for (size_t i = 0; i < sizeof g_fs_tab / sizeof g_fs_tab[0]; i++)
        if (!strcmp(g_fs_tab[i].sym, sym_name)) return g_fs_tab[i].fn;
    return NULL;
}

void dm_fs_selftest(void) {
    arc_init();
    FSLOG("[fs] 自检：归档状态=%s（%u 文件，%.1f MB）；宿主回退根=%s\n",
          g_arc_state == 1 ? "有" : "无", g_arc_n, g_arc_len / 1048576.0, rom_root());
    static const char *keys[] = { "System/Version.txt", "Pack/Bootup.pack", "System/font/nvn_font/nvn_font_shader.bin" };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        int idx = arc_index(keys[i]);
        if (idx >= 0) {
            FSLOG("[fs] 自检：%s 在归档里，%llu 字节\n", keys[i], (unsigned long long)arc_ent_size(idx));
            if (!strcmp(keys[i], "System/Version.txt")) {
                int64_t sz = (int64_t)arc_ent_size(idx);
                char b[33] = {0};
                size_t n = (size_t)(sz < 32 ? sz : 32);
                memcpy(b, g_arc + arc_ent_off(idx), n);
                FSLOG("[fs] 自检：Version.txt 内容 = %s\n", b);
            }
        } else {
            FSLOG("[fs] 自检：%s 不在归档里（回退宿主目录）\n", keys[i]);
        }
    }
}
