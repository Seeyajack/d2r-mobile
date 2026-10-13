# 线程槽方案 · musl（鸿蒙）的改法

> 第 4 站成果 ｜ 更新：2026-10-13
> 解决的问题：改写器的线程槽方案建立在**苹果 pthread 的 TSD 布局**上，鸿蒙的 musl 不是那样
> 状态：**已由真机测量确定公式，改写器改法已明确**

---

## 一、原来的方案（苹果）

改写器把游戏里对线程槽的访问改成一条固定等式：

```
槽地址 = tpidrro_el0 + 键号 × 8
```

`patch_nso.py` 第 245 行固定了几个键号（苹果 key 空间上限 512，取最高的几个）：

```python
KEYS = {'K18': 508, 'KTLS': 509, 'KTLR': 510, 'KGATE': 511,
       'KRES0': 506, 'KRES1': 507, 'KASPECT': 505, 'KASPECTUI': 504}
```

生成的代码（`emit_slot_addr`）是这五条：

```
mrs  T, tpidrro_el0          ; T = 基址
adrp S, key_page             ; S = 键页
ldr  S, [S, idx*8]           ; S = 键号 × 8
add  T, T, S                 ; T = 槽地址
ldr  S, [T]                  ; S = 槽值
```

**苹果上能成立的原因**：Apple 内核把 `tpidrro_el0` 设为 pthread 结构指针，且
`pthread_setspecific(k, v)` 就存在 `pthread + k×8`。两边恰好吻合。

各改写点：

| 游戏原本的动作 | 读/写哪个槽 |
|---|---|
| 读/写 `x18` | K18 |
| `mrs tpidr_el0` | KTLS |
| `msr tpidr_el0` | KTLS |
| `mrs tpidrro_el0` | KTLR |
| `svc` / `mrs cntpct_el0` | KGATE |

---

## 二、真机测量结果（HUAWEI Mate 70 Pro+，API 26）

### 实验 E：直接套用公式 → 不行

```
tpidr_el0   = 0x5ae3d9cca0        ← 是线程指针（可用）
tpidrro_el0 = 0x700000000000000   ← 既不是 0 也不是线程指针（不可用）
建了 4 个键（键号 24-27）
→ 4 个键的槽值在 tpidr_el0 的 -0x2000..+0x8000 范围内全都找不到
```

**两条结论**：
1. `tpidrro_el0` 在鸿蒙上取不到线程指针，**基址不能用它**
2. 槽值根本不在 `tpidr_el0` 附近 —— musl **不是把槽直接存在 pthread 结构里**

### 实验 E2：逐级测出 musl 的真实布局

在 `struct pthread` 里扫，找「偏移 o，使 `*(pthread+o)` 是个指针 p，且 `p[键号]` 等于写入的 marker」：

```
✓ 找到了：pthread + (-272) 是指针 p，且 p[28]==marker1、p[29]==marker2
→ 完整公式：槽地址 = *(void**)(tpidr_el0 + (-272)) + 键号 × 8
```

**musl 的做法是两级间接**：

```
tpidr_el0 ──→ struct pthread
                 pthread + (-272) 处有个 void **tsd 指针
                     └──→ 单独分配的槽数组（malloc 出来的）
                             └── [键号 × 8] 才是值
```

---

## 三、改写器的改法（`dm/tools/patch_nso.py`）

改两处，都是机械改动：

### 1. 新增常量与指令编码

```python
# —— 鸿蒙（musl）专用 ——
OHOS_TSD_OFF = 272          # struct pthread 里 tsd 指针相对 tpidr_el0 的偏移（实测，2026-10-13）
def MRS_TPIDR(t):  return 0xD53BD040 | t      # mrs Xt, TPIDR_EL0
def SUB(d, n, imm): return 0xD1000000 | ((imm & 0xFFF) << 10) | (n << 5) | d   # sub Xd, Xn, #imm
```

### 2. `emit_slot_addr` 插入一次「取 tsd 指针」

```python
def emit_slot_addr(base, key_reg, kind):
    if OHOS:
        emit(SUB(base, base, OHOS_TSD_OFF))   # base = tpidr_el0 - 272
        emit(LDRX(base, base))                # base = *(void**)base = tsd 数组
    emit_key_addr(key_reg, kind)              # key_reg = 键号 × 8
    emit(ADDR(base, base, key_reg))           # base = tsd + 键号×8
```

### 3. 各改写点把 `MRS_TPIDRRO(T)` 换成 `MRS_TPIDR(T)`

共 6 处（第 335、346、362、373、396、406、420、475、792 行附近）：

```diff
-    emit(STP(S, T, 31, save)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
+    emit(STP(S, T, 31, save)); emit(MRS_TPIDR(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
```

### 改动后的代码序列

```
mrs  T, tpidr_el0          ; T = pthread 指针
sub  T, T, #272            ; T = &pthread->tsd
ldr  T, [T]                ; T = tsd 数组（musl 另分配的）
adrp S, key_page
ldr  S, [S, idx*8]         ; S = 键号 × 8
add  T, T, S               ; T = 槽地址
ldr  S, [T]                ; S = 槽值
```

只比原来多两条指令（`sub` + `ldr`）。

---

## 四、宿主侧要改什么（意外地少）

| 位置 | 要改吗 | 说明 |
|---|---|---|
| `create_keys()` | ❌ 不动 | 还是 `pthread_key_create` 8 个键 |
| `install_key_page()` | ❌ **不动** | 键页里写的仍是 `键号 × 8`，格式完全不变 |
| `read_layout()` | ❌ 不动 | fixups.tsv 格式不变 |
| **`tsd_check()`** | ✅ **要改** | 断言用的公式得换成新的 |
| 其余 | ❌ 不动 | 宿主照旧用 `pthread_setspecific`/`pthread_getspecific` |

**关键**：键页格式不变 → **现有改写产物不用重生成**（除了把 `mrs` 换掉和插入两条指令）。

`tsd_check()` 换成：

```c
static void tsd_check(void) {
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uint64_t tsd = *(uint64_t *)(tp - 272);                 // 两级间接
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    if (((uint64_t *)tsd)[off / 8] != 0x1234) die("TSD 布局不符");
    pthread_setspecific(g_key18, 0);
}
```

---

## 五、这改变了什么（重要）

**这不是"打补丁"，而是推翻了底层假设。** 原来整套改写建立在一个**平台巧合**上：

> 苹果恰好把 TSD 放在 `tpidrro_el0 + 键号×8`。

换到 musl 后，正确的做法是走**两级间接**。好消息是：
- 键页格式不变（宿主几乎不用动）
- 每个改写点只多两条指令
- **方法的普适性反而增强了**——不再依赖某个 libc 的巧合布局，而是显式表达"槽在 musl 的 tsd 数组里"

---

## 六、真机验证（已完成）

### 实验 F：改写器生成的指令序列能否读到宿主写的槽 —— ✅ 通过

把 `emit_slot_addr` 在 OHOS 下生成的五条指令**原样执行**，值由 `pthread_setspecific` 写进去
（改写后的游戏代码就是这么读的）：

```
mrs x10, tpidr_el0
sub x10, x10, #272
ldr x10, [x10]
add x9,  x10, x9        /* x9 = 键页里存的值（键号×8） */
ldr S,   [x9]
```

结果：

```
键号 30：原生序列读回 0xa5a5dead00000001，宿主读回 0xa5a5dead00000001，写入 0xa5a5dead00000001  ✓
键号 31：原生序列读回 0xa5a5dead00000002，宿主读回 0xa5a5dead00000002，写入 0xa5a5dead00000002  ✓
结论：读得到，且与 pthread_getspecific 一致
```

### 指令编码已交叉验证

用 NDK 的 clang 汇编同样四条指令，对照 `patch_nso.py` 里的编码，逐条一致：

| 指令 | 真实字节 | patch_nso.py |
|---|---|---|
| `mrs x0, tpidr_el0` | `d53bd040` | `MRS_TPIDR(0) = 0xD53BD040` ✅ |
| `mrs x0, tpidrro_el0` | `d53bd060` | `MRS_TPIDRRO(0) = 0xD53BD060` ✅ |
| `sub x0, x0, #272` | `d1044000` | `SUB(0,0,272) = 0xD1044000` ✅ |
| `ldr x0, [x0]` | `f9400000` | `LDRX(0,0) = 0xF9400000` ✅ |

### 已落地的改动

| 文件 | 改动 |
|---|---|
| `dm/tools/patch_nso.py` | 加 `DM_PATCH_TARGET` 平台开关（缺省 apple，行为不变）；加 `MRS_TPIDR`/`SUB`/`mrs_tp`；`emit_slot_addr` 在 OHOS 下插入两条取 tsd 的指令；9 处调用点 `MRS_TPIDRRO` → `mrs_tp` |
| `dm/host/loader.c` | `tsd_check()` 加 `#if defined(__OHOS__)` 分支，断言改用两级间接 |

---

## 七、待办（还剩）

- [ ] 用真实 NSO 跑一次 `patch_nso.py`（`DM_PATCH_TARGET=ohos`），验证产物可执行
      —— 需要游戏文件，放在阶段 2
- [ ] 改写器其余部分的适配（剪口表 CUTS 等是 Mach-O 之外的通用逻辑，应无需改）

---

## 附：踩过的坑

| 坑 | 症状 | 原因 |
|---|---|---|
| `exp_e2` 里忘了装信号处理器 | 进程直接消失，日志停在扫描前 | `safe_read64` 靠 `siglongjmp` 回来，**必须先 `sigaction` 装好兜底**。第一次跑就死在这 |
| `tpidrro_el0` 不是 0 | 以为能拿它当基址 | 实测是 `0x700000000000000`（带标记的值），**既不能用也不能假设为 0** |
| 槽值不在 `tpidr_el0` 附近 | 实验 E 全数落空 | musl 的 tsd 是**单独分配的**，不在 pthread 结构里 —— 所以才要 E2 再测一层 |
