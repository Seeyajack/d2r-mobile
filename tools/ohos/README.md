# tools/ohos/ —— 我们自己的脚本

> 规则一句话：**上游的工具目录不动，我们自己的独立脚本放这里。**
>
> 例外只有一族留在 `dm/tools/`：`nso.py`（上游）`import lz4lite`，
> 而 `nso_calls.py` / `find_callers.py` / `nso_survey.py` 又 `import nso` ——
> 这一族拆开就断，所以 `lz4lite.py` / `nso_mods.py` / `nso_calls.py` / `nso_survey.py` 与上游 `nso.py` 放在一起。

## 现有脚本

| 脚本 | 干什么 | 什么时候用 |
|---|---|---|
| `import_slots.py` | 离线解「客体 GOT 槽 → 导入名」；`--stub <地址>` 反汇编桩自动算槽 | 崩溃回溯给出桩地址，要翻译成是哪个导入 |
| `import_audit.py` | 算「客体要、宿主没有」的导入清单 = **未来崩溃点地图** | 接一款新游戏的第一步 |
| `pack_romfs_subset.py` | 圈 romfs 首启最小集 | 准备上机素材 |
| `export_all.py` | 批量导出/翻译着色器 | 出图前的素材准备 |
| `elf_port_asm.py` | 汇编层移植辅助（`.S` 生成） | 引擎嫁接 |
| `spirv-val-shim.py` | 无 spirv-val 时的校验替身 | 着色器校验 |
| `astc_ref.py` | BC→ASTC 的 CPU 参考实现（对拍 GPU 结果） | 贴图转码验证 |
| `decode_nonrelative_rela.py` | 解码非 RELATIVE 重定位 | 排障 |
| `gen_imports_list.py` | 出整份导入清单（读 DT_JMPREL/SYMTAB/STRTAB） | 摸底 |
| `nvn_coverage.py` | NVN 接口覆盖统计（请求 / 已实现 / 缺） | 评估图形层缺口 |

## 约定

- 新脚本放这里，别塞进 `dm/tools/` 或 `dm/gfx/tools/`（那两个是上游的）。
- 脚本要能独立跑：`python tools/ohos/xxx.py --help`。
- 路径用相对仓库根（`ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))`），不要写死盘符。
