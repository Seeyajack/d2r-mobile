# 鸿蒙通用库 —— 组成清单（route B）

> 为什么要有这份清单：仓库保持**上游镜像 + 我们的增量**（这样 `git merge upstream` 永远无痛），
> "通用库"不靠删目录实现，而是靠这份清单**生成**出来。
>
> 配套脚本：`python dist/make_dist.py`（输出到 `--out`，缺省 `dist/out/`）

## 一、路线说明（为什么不直接删）

「树干净」和「好合并上游」是互斥的：

| | 在 fork 里直接删/重排 | 本方案（保持镜像 + manifest 生成） |
|---|---|---|
| 仓库里的树 | 干净 | 仍是上游原样 |
| 合并上游 | 每次都是 delete/modify 冲突 | 无痛 |
| 交付物 | 就是仓库 | `dist/out/`（干净的通用库） |

上游仍在养 `dm/gfx/vulkan`（82 文件引擎），独立就要自己吸收它的演进——那比"树不够干净"贵得多。
所以：**仓库不删，交付物生成。**

## 二、分层与判定（沿用 `PORTABLE.md`）

判一个文件属于哪层，按顺序问三句：

1. 提到**某个具体游戏**的名字 / 地址 / 字段偏移 / 数据表？→ **L3**（不进通用库）
2. 提到**某类平台接口**（`nn::`、NVN、Mach）？→ **L2**（进通用库）
3. 都不是 → **L1**（进通用库）

## 三、清单语法

```
+ <相对仓库根的路径或 glob>    纳入
- <同上>                        排除（写在后面的覆盖前面的）
#                               注释
```

## 四、当前清单

```manifest
# ── L1/L2：宿主核心（框架骨架、svc、线程、loader、HLE 框架、系统服务）──
+ dm/host/**
# L3：暗黑2 玩法模块（47 个文件 15,677 行）——整文件不进通用库，代码一行不用改
# 判定依据：PORTABLE.md 三问里的第一条（提到具体游戏的名字/地址/数据表 ⇒ L3）
- dm/host/dm_actions.c
- dm/host/dm_autobuff.c
- dm/host/dm_beltauto.c
- dm/host/dm_bossrush.c
- dm/host/dm_bright.c
- dm/host/dm_cloudmerge.c
- dm/host/dm_cloudsync.c
- dm/host/dm_cursor.c
- dm/host/dm_cursor_item.c
- dm/host/dm_custom.c
- dm/host/dm_dropfilter.c
- dm/host/dm_drophaptic.c
- dm/host/dm_elysium.c
- dm/host/dm_freetp.c
- dm/host/dm_hud.c
- dm/host/dm_i18n.c
- dm/host/dm_kills.c
- dm/host/dm_lootfilter.c
- dm/host/dm_movie_decode.c
- dm/host/dm_namegen.c
- dm/host/dm_padmode.c
- dm/host/dm_pages.c
- dm/host/dm_potpick.c
- dm/host/dm_pouch.c
- dm/host/dm_prof.c
- dm/host/dm_rift.c
- dm/host/dm_rift_ui.c
- dm/host/dm_runeword.c
- dm/host/dm_rwlv_core.c
- dm/host/dm_shoprefresh.c
- dm/host/dm_textbox.c
- dm/host/dm_textbox_core.c
- dm/host/dm_textinput.c
- dm/host/dm_townwp.c
- dm/host/dm_turnsmooth.c
- dm/host/dm_uiscreen.c
- dm/host/dm_update.c
- dm/host/dm_vault.c
- dm/host/dm_vault_ui.c
- dm/host/dm_world.c
- dm/host/hle_charname.c
- dm/host/hle_movie.c
- dm/host/hle_private_aim.c
- dm/host/hle_tact.c
- dm/host/hle_target.c
- dm/host/hle_touch.c
- dm/host/touch_mac.c

# ── L2：图形（NVN→Vulkan 引擎 + NVN HLE + 贴图转码）──
+ dm/gfx/vulkan/**
+ dm/gfx/nvn/**
+ dm/gfx/tex/**
+ dm/gfx/hle_vi.c
+ dm/gfx/hle_nv.c
- dm/gfx/metal/**

# ── L1/L2：鸿蒙外壳（ArkTS + N-API）──
+ dm/harmony/**

# ── L1：改写器（上游） + 我们自己的分析脚本 ──
+ dm/tools/**
+ tools/ohos/**

# ── 构建必需的第三方与垫片 ──
+ dm/third_party/libdeflate/**
+ dm/android/compat/**

# ── 文档 ──
+ PORTABLE.md
+ NOTICE.md

# ── 明确不进通用库 ──
# dm/ohos/**           自研线（已降级为排障仪器，+30k 行）
# dm/games/**          按游戏分开的 L3 数据（随游戏走）
# dm/ios/** dm/tvos/** 苹果平台
```

## 五、每个游戏自己要带的东西（L3）

- `dm/games/<titleid>/`：游戏专属数据（`game.tsv` / PSO 清单 / 着色器链 …）
- 该游戏的路径 profile（硬编码地址外置后产生，见 `30-设计/` 里的迁移方案）

## 六、迁移进度

- [x] 清单与生成脚本就绪
- [ ] `dm/host` 里的 829 处具体游戏硬编码地址 → 外置成 profile（当前最大的一块 L3 污染）
- [ ] 清单随迁移进度调整
