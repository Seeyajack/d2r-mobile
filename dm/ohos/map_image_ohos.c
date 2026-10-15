// 见 map_image_ohos.h。依据：dm/ohos/装载方案-镜像嵌入与定位.md 第二、三节。
#include "map_image_ohos.h"

#include <errno.h>
#include <hilog/log.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_DOMAIN 0xD002
#define LOG_TAG "dmohos"
#define LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO,  LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

// ───────────────── 链接器为自定义段生成的符号 ─────────────────
// 段名由 gen_embed_ohos.py 生成：dm<模块名>x0 / dm<模块名>w / dm<模块名>x1。
// 段名必须是合法的 C 标识符，链接器才会生成 __start_<段名> / __stop_<段名>
// 这一对（真机实验 C 已验证）。
//
// 全部声明为【弱符号】：这样只嵌入了部分模块时也不会链接失败
// （段不存在 → 运行时为 NULL，由 find_img() 跳过）。实测 lld 行为：
//   段存在  → 链接器生成强符号，弱引用被正常解析
//   段不存在 → 弱引用保持未定义，值为 0，不报 undefined symbol
#define DM_IMG_SYMS(nm) \
    __attribute__((weak)) extern const uint8_t __start_dm##nm##x0[], __stop_dm##nm##x0[]; \
    __attribute__((weak)) extern const uint8_t __start_dm##nm##w[],  __stop_dm##nm##w[];  \
    __attribute__((weak)) extern const uint8_t __start_dm##nm##x1[], __stop_dm##nm##x1[];

DM_IMG_SYMS(main)
DM_IMG_SYMS(sdk)
DM_IMG_SYMS(subsdk0)
DM_IMG_SYMS(probe)      // 实验 D 的自造测试映像（test_image.S；与真实模块 main 分开命名）

// 强制接管表所在的段（上游 hle.h 的 HLE_FORCE 宏在鸿蒙侧改写到这个段名）
__attribute__((weak)) extern const uint8_t __start_dmswforce[], __stop_dmswforce[];

typedef struct {
    const char *name;
    const uint8_t *x0, *x0e;   // 原 text
    const uint8_t *w,  *we;    // 中间的 ro + data + bss
    const uint8_t *x1, *x1e;   // 补丁区
} img_sections;

static const img_sections g_imgs[] = {
#define IMG(nm) { #nm, \
    __start_dm##nm##x0, __stop_dm##nm##x0, \
    __start_dm##nm##w,  __stop_dm##nm##w,  \
    __start_dm##nm##x1, __stop_dm##nm##x1 }
    IMG(main), IMG(sdk), IMG(subsdk0), IMG(probe),
#undef IMG
};

const void *dm_ohos_force_table(size_t *nbytes) {
    if (nbytes) *nbytes = 0;
    // 弱符号：段不存在时两个符号都是 0
    if (!__start_dmswforce || !__stop_dmswforce) return NULL;
    size_t n = (size_t)(__stop_dmswforce - __start_dmswforce);
    if (nbytes) *nbytes = n;
    return n ? (const void *)__start_dmswforce : NULL;
}

static const img_sections *find_img(const char *name) {
    for (size_t i = 0; i < sizeof g_imgs / sizeof g_imgs[0]; i++) {
        // 弱符号：没嵌入的模块三个指针都是 0，跳过
        if (!g_imgs[i].x0 || !g_imgs[i].w || !g_imgs[i].x1) continue;
        if (!strcmp(g_imgs[i].name, name)) return &g_imgs[i];
    }
    return NULL;
}

// 与上游一致按 16 KB 对齐（嵌入脚本也按 16 KB 补零）。
// 真机实测系统页是 4 KB，16 KB 对齐对 4 KB 页同样成立。
#define PAGE_BITS 14
#define PAGE      (1u << PAGE_BITS)
#define ROUND_UP(v) (((v) + (PAGE - 1)) & ~(uint64_t)(PAGE - 1))

// ───────────────────── 第一步：分配 + 拷贝 ─────────────────────

int dm_ohos_map_image_alloc(module_t *m) {
    const img_sections *s = find_img(m->name);
    if (!s) {
        LOGE("【红】鸿蒙装载：没有 %{public}s 的映像段（gen_embed_ohos.py 没生成？）", m->name);
        return -1;
    }

    size_t z0 = (size_t)(s->x0e - s->x0);
    size_t zw = (size_t)(s->we - s->w);
    size_t z1 = (size_t)(s->x1e - s->x1);

    // 布局自检与拷贝范围：**以 fixups 的精确范围为準，不信任 ELF 段大小**。
    // 为什么（真机实测 2026-10-16）：lld 会给每个嵌入段尾垫一个对齐页（.S 内容 0x1634000
    // → .so 段 0x1638000），按 ELF 段大小拷贝会把 w 段数据错位 16K、还会用零盖掉 ro 开头。
    // 精确范围由调用方经 module_t 的 x0_end / img_end 传入（来自 fixups 生成头）；
    // 未设置（=0，如实验 D 的自造映像）时退回旧的 ELF 段大小路径（那些段没有被垫页）。
    size_t c0 = z0, cw = zw, c1 = z1;
    if (m->x0_end || m->img_end) {
        if (z0 < m->x0_end || zw < m->patch_off - m->x0_end || z1 < m->img_end - m->patch_off) {
            LOGE("【红】鸿蒙装载 %{public}s：嵌入段比 fixups 范围小（x0 %zu / w %zu / x1 %zu）",
                 m->name, z0, zw, z1);
            return -1;
        }
        c0 = (size_t)m->x0_end;                       // x0 只拷到 x0/w 边界
        cw = (size_t)(m->patch_off - m->x0_end);      // w 拷到 patch_off
        c1 = (size_t)(m->img_end - m->patch_off);     // x1 拷到映像末尾（含 bss 预留）
        LOGI("鸿蒙装载 %{public}s：按 fixups 范围拷贝（x0 %zu / w %zu / x1 %zu，"
             "ELF 段含链接器尾垫 %zu / %zu / %zu）",
             m->name, c0, cw, c1, z0, zw, z1);
    }

    // 布局自检：三块必须能按原相对位置拼回原映像（与 iOS 版同样那几条）
    if (c0 + cw != m->patch_off) {
        LOGE("【红】鸿蒙装载 %{public}s：段布局不符（x0 %zu + w %zu "
             "≠ PATCH_OFF %{public}llu）",
             m->name, c0, cw, (unsigned long long)m->patch_off);
        return -1;
    }
    if (!c0 || !c1) {
        LOGE("【红】鸿蒙装载 %{public}s：x0 %zu / x1 %zu 为空",
             m->name, c0, c1);
        return -1;
    }

    m->size = m->patch_off + c1;
    size_t total = (size_t)ROUND_UP(m->size);

    // 先按「可写」拿匿名内存 —— 拷映像、装键页都要写。
    // 真机实验 A 已证明随后能 mprotect 成 R+X 并真正执行。
    uint8_t *base = (uint8_t *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        LOGE("【红】鸿蒙装载 %{public}s：mmap %{public}zu 字节失败（%{public}s）",
             m->name, total, strerror(errno));
        return -1;
    }

    // 三块按原相对位置摆回去：x0 在 0，w 在 x0_end，x1 在 patch_off
    memcpy(base, s->x0, c0);
    memcpy(base + c0, s->w, cw);
    memcpy(base + m->patch_off, s->x1, c1);

    m->base = base;
    LOGI("鸿蒙装载 %{public}s：base 0x%{public}llx size %{public}zu"
         "（x0 %{public}zu / w %{public}zu / x1 %{public}zu，页 %{public}ld）",
         m->name, (unsigned long long)(uintptr_t)base, m->size, z0, zw, z1,
         (long)sysconf(_SC_PAGESIZE));
    return 0;
}

// ───────────────────── 第二步：封代码区 ─────────────────────

int dm_ohos_map_image_seal(module_t *m) {
    // 与上游桌面版一致：[0, text_end) 与补丁区只读+可执行；
    // 中间那段（ro 与 data 同页只能一起）保持可写。
    uint64_t rx = ROUND_UP(m->text_end);
    if (mprotect(m->base, rx, PROT_READ | PROT_EXEC) != 0) {
        LOGE("【红】鸿蒙装载 %{public}s：mprotect text（%{public}llu 字节）失败（%{public}s）",
             m->name, (unsigned long long)rx, strerror(errno));
        return -1;
    }
    uint64_t po = m->patch_off & ~(uint64_t)(PAGE - 1);
    if (mprotect(m->base + po, m->size - po, PROT_READ | PROT_EXEC) != 0) {
        LOGE("【红】鸿蒙装载 %{public}s：mprotect patch（从 +0x%{public}llx）失败（%{public}s）",
             m->name, (unsigned long long)po, strerror(errno));
        return -1;
    }
    LOGI("鸿蒙装载 %{public}s：代码区已封为 R+X（text %{public}llu 字节，补丁区从 +0x%{public}llx）",
         m->name, (unsigned long long)rx, (unsigned long long)po);
    return 0;
}
