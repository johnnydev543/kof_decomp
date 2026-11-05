# AGENT.md — KOF97 (Neo Geo MVS) matching 反組譯專案

本專案的目標是 KOF97 主程式（P1 ROM，Motorola 68000）的 **組合語言反組譯**：
1. **matching**：反組譯出的 GNU as 原始碼（`build/disasm/p1.s`）重新組譯後與原 ROM 逐位元組相同；
2. **shiftable**：所有指標、跳躍表、handler 位址都是符號，任意插入或刪除程式碼後遊戲行為不變（位移測試驗證）；
3. **命名**：函式、資料、RAM、物件欄位都有名稱與註解（`games/kof97/symbols/`）。

不轉成 C：遊戲本身就是以 68000 組合語言寫成，反組譯成可重組的組語即為最終產物。
`src/` 的 Neo Geo 模擬器（SDL2、Musashi 直譯、YM2610、Z80）是驗證與觀測工具：位移測試、執行位址紀錄（`--exec-log`）、
`--break` / `--watch` 等執行期證據都靠它。`tools/recomp/m68k_recomp.py` 的 68000 解碼與程式碼探索是反組譯器
`mkasm.py` 的核心；它另外能把程式產生成 C 給模擬器加速執行（下文第 4 節與 `make modecheck`），那部分只是早期的附帶功能，
對反組譯的目標不需要。

本文件也記錄如何用 **REA (Reverse Engineer Anything, morluto/rea) MCP 工具** 做輔助分析。

> 換另一套 Neo Geo 遊戲（反組譯 → matching → 命名）的步驟與要修改的常數：見 `docs/NEW_ROM_GUIDE.md`。

> ROM 與所有由 ROM 產生的輸出（`build/` 底下：解出的 ROM、REA 反編譯結果、`build/recomp/*.c`）皆屬於使用者自有
> ROM 的衍生物，只供本機使用，**不可散佈或提交到公開位置**。

---

## 1. 總覽

```
roms/kof97.zip + roms/neogeo.zip
        │  tools/romprep.py         (CRC 驗證、68000 byte-swap、C ROM 交錯、V ROM 串接)
        ▼
build/roms/*.bin, p1.be, bios.be
        │  REA (Ghidra provider)    decompile_all / list_procedures / xrefs …  ← 種子: games/kof97/rea/seeds_p1.tsv, tools/rea/seeds_bios.tsv
        ▼
build/rea/decomp_{p1,bios}/         (函式清單 + Ghidra body ranges + 偽代碼，供分析與做為重編譯種子)
        │  tools/recomp/m68k_recomp.py   (遞迴下降反組譯 + 指標表掃描 + 執行期 miss 回饋 → C)
        ▼
build/recomp/rc_NN.c, rc_table.c, rc_tests.c
        │  Makefile  (+ Musashi 後備直譯器, ymfm YM2610, Z80 核心, SDL2)
        ▼
build/kof97                          (執行檔)
```

| 元件 | 位置 | 說明 |
|---|---|---|
| ROM 前處理 | `tools/romprep.py` | 驗證 MAME `kof97` 母版 CRC，輸出執行期與 REA 用的檔案 |
| REA 種子/包裝 | `tools/rea/mkseeds.py` → `seeds_p1.tsv`, `seeds_bios.tsv`；`rea.sh` | Ghidra 分析前注入的進入點（由檔頭跳躍與向量表產生）；`rea.sh` 設好 PATH/GHIDRA/JAVA |
| Ghidra 腳本 | `tools/ghidra/NeoGeoSetup.java` | （選用）建立完整 Neo Geo 記憶體映射的 Ghidra 專案，供 GUI 手動分析 |
| 68000 解碼器 / 重編譯器 | `tools/recomp/m68k_recomp.py` | 指令解碼與程式碼探索（反組譯器使用）；附帶的 68000 → C 產生器（選用） |
| Musashi 小工具 | `tools/recomp/m68kinfo.c`, `disasm.py` | opcode 合法性/週期表、反組譯（重編譯器交叉驗證用） |
| 執行期 | `src/` | `cpu.c` 調度器、`neogeo.c` 硬體、`video.c` LSPC、`audio.cpp` Z80+YM2610、`main.c` SDL2 前端 |
| 測試 | `tests/rc_difftest.c`, `tests/audio_test.cpp` | 每條翻譯指令與 Musashi 差分比對；音效子系統測試 |
| 第三方 | `third_party/` | Musashi (MIT)、ymfm (BSD-3)、superzazu/z80 (MIT，修補版在 `src/z80/`)、stb_image / stb_vorbis (public domain) |

---

## 2. 工具鏈與環境

| 項目 | 位置 / 版本 |
|---|---|
| REA | `rea-agents` 6.8.0，`~/.local/opt/node-v26.13.0-linux-x64/bin/rea`（Node 不在預設 PATH） |
| REA MCP 註冊 | `~/.claude.json` 的 `mcpServers.rea`（`rea mcp`，env 已含 `GHIDRA_INSTALL_DIR`、`JAVA_HOME`） |
| REA skill | `~/.claude/skills/reverse-engineer-anything/`（使用 REA 前先載入此 skill） |
| Ghidra | 12.1.4，`~/.local/opt/ghidra_12.1.4_PUBLIC` |
| JDK | OpenJDK 21，`/usr/lib/jvm/java-21-openjdk-amd64` |
| 其他 | gcc/g++ 13、`libsdl2-dev` 2.30、python3（PIL 只用於 `tools/contact_sheet.py`） |

CLI 一律透過包裝腳本執行：`tools/rea/rea.sh <command> ...`（自動設定 PATH、GHIDRA_INSTALL_DIR、JAVA_HOME、
`REA_GHIDRA_STARTUP_TIMEOUT_MS`）。檢查環境：`tools/rea/rea.sh doctor`。

MCP 工具在 Claude Code 啟動時載入；若 session 中看不到 `mcp__rea__*` 工具，重啟 Claude Code。
MCP 不可用時，用 CLI（指令名稱是 MCP 工具名的 kebab-case，例如 `decompile_all` → `decompile-all`）。

---

## 3. 本專案需要的 REA 工具

### 3.1 目標與參數（重要）

Neo Geo ROM 是無檔頭的 raw binary，REA 不會自動辨識。一定要指定：

| 目標 | `open_binary` 參數 | 對應 CLI 旗標 |
|---|---|---|
| 遊戲 P1（68000 主程式，映射於 0x000000） | `{"path": "<abs>/build/roms/p1.be", "format": "raw-binary", "architecture": "m68k", "entry_point": "0x0"}` | `--target-format raw-binary --architecture m68k --entry-point 0x0 --provider ghidra` |
| MVS BIOS（映射於 0xC00000） | `{"path": "<abs>/build/roms/bios.be", "format": "raw-binary", "architecture": "m68k", "entry_point": "0xc00000"}` | `... --entry-point 0xc00000 ...` |

- 必須用 **byte-swap 過的** `.be` 檔（`romprep.py` 產生）；原始 `232-p1.bin` 是 16-bit 位元組交換格式。
- REA 的 raw-binary 模式把檔案 offset 0 映射到 `entry_point`，只有單一區段：**沒有** Neo Geo I/O、RAM、
  P2 bank、BIOS 交叉參照。P1 內呼叫 BIOS（`jsr $c004xx`）在 P1 session 中會顯示成未映射位址；跨映像的問題要分別開兩個 session。
- **分析種子**：設定環境變數 `REA_GHIDRA_SEED_FILE=<abs>/games/kof97/rea/seeds_p1.tsv`（或 `seeds_bios.tsv`），格式
  `0x<addr>\tfunction\t<name>`。內容是向量表、卡匣檔頭進入點（USER 0x122、PLAYER_START 0x128、DEMO_END 0x12E、
  COIN_SOUND 0x134）、BIOS 跳躍表。CLI 每次呼叫前設定；MCP 則需在 server 啟動環境中設定。
  建議只為本專案註冊：
  `claude mcp add --scope local rea-kof97 -e REA_GHIDRA_SEED_FILE=$PWD/games/kof97/rea/seeds_p1.tsv -e GHIDRA_INSTALL_DIR=$HOME/.local/opt/ghidra_12.1.4_PUBLIC -e JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 -- $HOME/.local/opt/node-v26.13.0-linux-x64/bin/rea mcp`
  （不要把種子加到全域 `rea` 註冊，否則會污染其他目標的分析。）
- 每個 REA session 只有一個 Program；第一次查詢會觸發 Ghidra 匯入與自動分析（P1 約 40 秒）。

### 3.2 工具清單與用途

| MCP 工具 | CLI | 在本專案的用途 |
|---|---|---|
| `open_binary` | （CLI 每次傳 path） | 綁定 P1 或 BIOS（參數見上表） |
| `binary_session` | — | 檢查目前目標、provider 與可用工具 |
| `binary_overview` | `analyze` | 區段、函式數、字串數總覽 |
| `list_procedures` | `function` / `analyze` | 取得全部函式位址（重編譯器種子的來源之一） |
| `decompile_all` | `decompile-all <path> --output DIR` | **主要工具**：一次輸出每個函式的 Ghidra 偽代碼與 `decompile-index.json`（含 body ranges）。`make rea` 會執行它 |
| `procedure_pseudo_code` | `decompile <path> <addr>` | 單一函式偽代碼（閱讀遊戲邏輯） |
| `batch_decompile` | — | 一次反編譯多個指定位址 |
| `analyze_function` | `function` | 函式 dossier：body 範圍、呼叫者/被呼叫者、參照 |
| `read_function_instructions` | — | 函式的原始組合語言（偽代碼不可靠時用） |
| `inspect_native_instruction` | `inspect-native-instruction` | 單一位址的解碼、流程與參照 |
| `xrefs` | `xrefs` | 誰參照某位址（例如找誰寫 `0xD00100`、誰讀 `0x320001`） |
| `procedure_callers` / `procedure_callees` | — | 呼叫圖 |
| `resolve_native_call_targets` | `resolve-native-call-targets` | 解析單一呼叫點（間接呼叫會標示 unknown） |
| `search_strings` / `list_strings` | `search` | 找訊息字串（例如 `CALENDAR ERROR`、`WARNING`） |
| `search_bytes` / `read_bytes` | `search-bytes` / `read-bytes` | 找 opcode 樣式、讀資料表（指標表、跳躍表） |
| `annotate_native_function` / `set_address_name` | `annotate-native-function` | 為函式命名、加註解（session 內有效） |
| `update_analysis_settings` | — | 調整 `ghidra.startup_timeout_ms` / `ghidra.decompile_timeout_seconds` |
| `close_binary` | — | 結束 session（可選擇寫出 analysis snapshot） |

### 3.3 REA 輸出如何被使用

1. `build/rea/decomp_p1/decompile-index.json`、`build/rea/decomp_bios/decompile-index.json`：
   每個函式的位址、名稱、body ranges → `m68k_recomp.py` 的種子與 C 函式分組（`rc_p1_<addr>`）。
2. `build/rea/decomp_*/functions/0x*.c`：Ghidra 偽代碼，**不可直接編譯**（含 `WARNING: bad instruction data`、
   被移除的區塊、未映射的 BIOS 位址），用來閱讀與追查遊戲/BIOS 邏輯。
3. 實際可執行的 C 由靜態重編譯器產生（下一節）。

---

## 4. 68000 解碼器與（選用的）C 重編譯 `tools/recomp/m68k_recomp.py`

> 反組譯只用到本節的解碼與程式碼探索（`decode()` / `discover()` / 指標表掃描）；產生 C 只用於加速模擬器，不是專案目標。


- **輸入**：`p1.be`、`bios.be`、REA index、向量表/檔頭/BIOS 跳躍表、`tools/recomp/extra_seeds.txt`（手動）、
  `build/recomp/runtime_misses.txt`（執行期回饋）、`build/m68k_table.txt` 與 `build/m68kinfo`（Musashi 提供的
  opcode 合法性、週期表、反組譯）。
- **探索**：從種子遞迴下降解碼；另以「指標表掃描」把 P1 中指向合法指令序列的 longword 當作額外進入點
  （`--no-scan` 關閉）。誤判只會多出不可達的 C，不影響正確性（每個位址的解碼是唯一的）。
- **驗證**：每條指令的長度必須與 Musashi 反組譯一致，否則中止。帶 68020 擴充字（bit 8–10）的序列視為非程式碼。
- **輸出**：每個 chunk 一個 C 函式，每條 68000 指令一個 label（`L_<addr>`），入口 `switch (REG_PC)` 允許從任何指令進入。
  - 區塊內分支 = `goto`（附週期預算檢查，讓中斷/時間片得以切換）；其他控制流設定 `REG_PC` 後回到調度器。
  - 常見指令（MOVE/ALU/分支/位元/移位/MUL…，約 99%）翻成原生 C，使用 Musashi 的旗標巨集與 CPU 狀態 `m68ki_cpu`。
  - 少數複雜指令（MOVEM、DIV、ADDX/SUBX、BCD、SR/CCR、TRAP、RTE、LINK/UNLK、ROXd、記憶體移位…）呼叫對應
    Musashi opcode handler（"helper"）。
  - 週期：Musashi 68000 週期表 + 動態部分（Bcc 未跳、DBcc、Scc、移位次數、MULU/MULS 位元數）。
  - **週期精確**：每條會往下執行的指令後都檢查預算（`if (m68ki_remaining_cycles <= 0) { REG_PC = next; return; }`），
    和 Musashi 停在同一條指令，所以中斷在同一條指令被接受；`make modecheck` 驗證整場遊戲與純直譯逐幀相同。
- **執行期調度** (`src/cpu.c`)：查表 `pc → chunk`；查不到（RAM 程式碼、靜態分析漏掉的目標）就交給 Musashi 逐條直譯直到回到已重編譯碼，
  並把該位址寫入 `build/recomp/runtime_misses.txt`。下次 `make` 會自動把它們納入重編譯。

覆蓋率迴圈（新角色/關卡被走到後執行）：
```
./build/kof97 --headless --autoplay --frames 30000 --nvram build/test.nvram   # 收集 misses
make                                                                          # 重新產生 C 並編譯
./build/kof97 --headless --autoplay --frames 30000 --nvram build/test.nvram   # 結尾的 "interpreter fallbacks" 應趨近 0
```
目前：108k 條指令被重編譯（99.1% 原生 C）；15000 幀 autoplay（開機、標題、選角、數場對戰）0 次直譯後備。

---

## 5. 執行期硬體模型（`src/neogeo.c`, `src/video.c`, `src/audio.cpp`）

68000 記憶體映射（MVS）：P1 `0x000000`（`0x0–0x7F` 向量依 REG_SWPBIOS/SWPROM 切換）、Work RAM `0x100000`、
P2 bank 視窗 `0x200000`（寫 `0x2FFFF0` 切 bank）、I/O `0x300000–0x3DFFFF`、調色盤 `0x400000`（兩個 bank）、
BIOS `0xC00000`、Backup RAM `0xD00000`（`0x3A001D` 解鎖 / `0x3A000D` 上鎖，存於 `kof97.nvram`）。

中斷：Level 1 = VBlank（第 240 線）、Level 2 = LSPC timer（`0x3C0006/08/0A`）、Level 3 = 冷開機；`0x3C000C` ACK。
一幀 264 線 × 768 個 68000 週期；每線同時跑 256 個 Z80 週期與對應的 YM2610 取樣（55555 Hz）。

**踩過的關鍵坑（不要移除）**：
1. **uPD4990A 日曆 TP 腳**：BIOS 量測兩個 TP 上升緣之間的 VBlank 數，必須在 57–63（1 Hz，指令 8 = 1 秒間隔）。
   錯了會顯示 `CALENDAR ERROR`（見 BIOS `0xC11A84`/`0xC11D94`）。
2. **Watchdog 是 KOF97 的防盜版機制**：遊戲在 `0xADB0` 寫 backup RAM 登錄資料後故意空轉 20 萬次不踢 watchdog。
   真機會在約 0.135 秒（1,622,015 個 68000 週期）被 watchdog 重置，因此 `0xD00100` 不會被設成 1；
   若沒有 watchdog，`0xD00100 = 1`，之後 `0x9A4A`/`0x9AB8` 會跳到 `0xAE9A` 顯示
   `WARNING THIS ROM CARTRIDGE IS FOR USE WITH ORIGINAL SNK BOARDS ONLY` 無限迴圈。
   若曾在無 watchdog 版本跑過，刪掉舊的 `.nvram` 檔。
3. 音效驅動在收到 `0x07` 之前不播放 ≥0x20 的指令（遊戲本身會送）；細節見 `src/AUDIO_NOTES.md`。

視訊：每條可見線（16–239）開始時依當下 VRAM/調色盤繪製（精靈鏈結、縮放表 L0 ROM、自動動畫、FIX 層），320×224。

---

## 6. 建置與執行

```
python3 tools/romprep.py         # 第一次（make 也會自動執行）
make -j$(nproc)                  # 產生 build/kof97
make rea                         # （選用）重新跑 REA decompile-all（需 Ghidra，數分鐘）
make test                        # 重編譯指令 vs Musashi 差分測試
./build/kof97                    # 視窗 + 音效
./build/kof97 --scale 4 --fullscreen
```

按鍵：P1 方向鍵 + `Z X C V`（A B C D）；P2 `R F D G` + `A S Q W`；`1`/`2` 開始；`5`/`6` 投幣；`9` service；
`F2` 測試模式；`F3` 重置；`P` 暫停；`F12` 截圖；`Esc` 離開。支援 SDL GameController（Back = 投幣、Start = 開始）。

其他選項：`--interp`（純 Musashi 參考模式）、`--headless --frames N`、`--dump PREFIX --dump-every N`（PPM 截圖）、
`--wav FILE`、`--script FILE`（`frame duration key[,key]`）、`--autoplay`、`--trace-pc N`、`--misses FILE`、`--nvram FILE`。

---

## 7. 驗證方式

| 檢查 | 指令 | 預期 |
|---|---|---|
| 指令翻譯正確性 | `make test`（或 `./build/rc_difftest 256`） | `22520 instruction tests x 256 random states: 0 failed`（數量隨覆蓋率變動） |
| 重編譯 vs 直譯（整個程式） | `make modecheck`（`tools/recomp/modecheck.py --all`） | 每個覆蓋腳本逐幀相同：幀末 PC、週期餘額、work RAM 雜湊（`--frame-sig`）、截圖、音訊、backup RAM |
| 開機/攻略 | `./build/kof97 --headless --autoplay --frames 15000 --dump build/shots/ap --dump-every 1000` + `python3 tools/contact_sheet.py 'build/shots/ap_*.ppm' out.png 5` | NEO GEO logo → 標題 → 選角 → 對戰 |
| 音效 | `--wav build/x.wav` 後計算 RMS | 開機 jingle（約 8–12 s）、attract 音樂（約 28 s 起） |
| 視窗路徑 | `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build/kof97 --frames 300` | 正常結束 |

---

## 7.5 Matching 反組譯

`tools/disasm/mkasm.py`：用重編譯器的程式碼地圖 + Musashi 反組譯 + 執行期觀測（`--exec-log`），把 P1 輸出成
GNU as 原始碼並逐位元組比對。binutils 自動偵測 `m68k-elf-*`（Homebrew）或 `m68k-linux-gnu-*`
（Ubuntu：`sudo apt install binutils-m68k-linux-gnu`），可用 `M68K_PREFIX` 覆蓋。

```
make matching                     # 第 1+2 階段：p1.s（符號化）→ 重組譯 P1 + P2(.incbin) → 與 MAME CRC 比對
make shifttest                    # 第 2 階段驗證：所有腳本 × 3 個插入點（0x800 / 0x1e62 / 0x60d36）
python3 tools/disasm/mkasm.py     # 只做第 1 階段（不符號化資料）
python3 tools/disasm/shifttest.py --script games/kof97/scripts/X.txt --insert 1e62   # 單一腳本
```

### 第 1 階段（byte-identical）
P1 = 向量表（`vectors:` 具名 `.long`）+ 指令（全部符號化，raw fallback = 0）+ 資料。`make matching` 另外用
`tools/disasm/mkrom.py` 組譯 P1 與 P2（4 MB 資料，`.incbin`），轉回 MAME 的位元組順序並驗證 `232-p1.bin`/`232-p2.bin` CRC。
程式碼/資料判定：
- `0x000–0x0ff` 永遠是資料（向量表）。
- 未經驗證（只來自指標掃描）的指令串若含不可能的編碼、分支到非 label / 奇數位址、line-A/F → 整串為資料；反覆到不再變動。
- 對奇數絕對位址做 word/long 存取（`move.w $1.w,$1.w`）會觸發 address error，不會往下執行。
- 執行期觀測：**被執行過**的位址一定是指令；**從未執行但被當資料讀取**的指令串是資料（即使靜態分析到得了）。

### 第 2 階段（shiftable：符號化指標）
在任一 label 前插入 `nop`（2 bytes）後，所有程式碼/資料位移，行為必須完全不變。指標判斷規則（`emit()` / `Conv.operand_syms()`），每條都對應一個實際踩到的誤判：
- 靜態：資料 longword 指向靜態入口（call/jump 目標、REA 函式、向量、種子）；程式碼參照的資料 label 開頭的短表；連續 ≥3 個像指標的值；`move.l #imm` 指向附近程式碼或靜態入口。排除低位元組為 0 的「整數」值（定點速度 `#$a0000`、`0x8000`）、`0007 0008` 這類序列、小 signed word 對（`0001 fffe`）。
- 「緊鄰指令」：`move.l #next_step,(A4)` 這種指向同函式下一步（距離 < 0x200）的值，即使是整數也是指標。
- 帶旗標的程式指標：最高位元組 0x80 + 24-bit 位址（68000 只用 A0–A23；指令串 `c01e 8007 9fc6 …`），輸出 `L_xxxxxx+0x80000000`。
- **執行期觀測**（`kof97 --interp --exec-log FILE`，shifttest 會自動收集並合併成 `build/disasm/exec_p1.bin`，4 個 bitmap）：
  1. 執行過的指令位址（PC 取 24 bit）→ 程式碼種子；沒有被 fall-through 進入的 → 動態入口（間接跳躍目標）。
  2. 32-bit 讀取起點、3. 16/8-bit 讀取（扣掉指令擷取）→ 只被 word 讀取、從未被 long 讀取的 longword 不是指標；用動態入口判斷資料 longword 時必須有 long 讀取。
  4. **已證實的程式指標**：從 P1 以 long 讀出的值（含 `move.l #imm` 的立即值），後來被 `jsr/jmp (An)`、`(d,An)`、`(d,An,Xn)` 跳到 → 一定符號化（不管是否為整數）。
- **展開迴圈**（`unrolled_bases()`）：`jmp (d8,PC,Xn)` 跳進「基底 + (N − 次數) × 大小」的展開迴圈，基底到第一個執行過的指令之間
  只在次數超過測試用過的值時才會執行，因此沒有執行紀錄；若那段位元組與該指令完全重複，基底就是程式碼。踩到的例子：
  `vram_fill_words` 的 `0xa512`（`move.w D0,(2,A1)` = `3340 0002`）曾被當成 `.long L_023340`，位移後變成 `move.w D2,(2,A1)`，
  只在填超過 110 個 word 時出錯，位移測試抓不到。
- **一定是程式碼的位址**（`spawn_handler_seeds()`；這類錯誤位移測試抓不到，只能靠規則）：
  1. `move.l #imm` 寫進 handler 欄位（`HANDLER_FIELDS`：物件 `(A4)`、`SCENE_HANDLER` `(0x500,A5)`、`+0x198` next_state、
     `+0x19c`/`+0x1a0` callback、`+0x134`/`+0x8c`/`+0x36`/`+0x1e8`；在 p1.s 量過，存進去的 label 99% 是程式碼）。
  2. A0 = `lea abs/(d,PC)` 或 `movea.l #imm`，A0 未被改寫（24 個指令內）就傳給 spawn 函式（`obj_spawn`、`obj_spawn_child`、
     `obj_init_static`，以及自動找出的「把 A0 原封不動傳下去」的包裝函式），或直接 `jsr/jmp (A0)`。
  3. 跳躍表：`movea.l (table,PC,Xn),A0` 或 `lea table,A0` + `movea.l (A0,Xn),A0`，接著 `jsr/jmp (A0)` → 表內每一項；
     遇到奇數、範圍外、或目標開頭是 line-A/F / 0000 的值就停（表後的 `0x00000404` 曾被當成入口）。
  原本約 2,800 個指令（測試沒跑到的 handler、投技表、場景）被當成 `.byte`，裡面的 `move.l #next_state,(A4)`、
  `jsr abs.l`、`bra.w` 都不會跟著位移（例：`0x6ea68` 的 `move.l #$6eb12,(A4)`、`0x326ae` 的 `jsr $1e300`）。
- 規則找到的入口也加進 `ENTRIES`：`move.l #imm,(handler 欄位)` 的立即值只有在目標是已知入口時才會符號化，原本有 110 個
  （77 個 `SCENE_HANDLER`、31 個 next_state）留著原始數字，位移後 handler 會指回舊位址。低位元組為 0 的值（`#$20000`、
  `#$e000` 寫進 NEXT STAGE 地球的 +0x36/+0x8c）是定點參數，不當成程式碼；`codecheck.py` 把仍是數字的 handler 欄位
  立即值列為錯誤。
- `drop_data_runs()`：被遊戲當資料讀的段落往後吞掉 fall-through 指令時，遇到「已信任、本身沒被當資料讀」的指令就停
  （投技 handler `0x35dba` 緊接在它用的動作表 `0x35daa` 後面，曾被一起當成資料）。
- **word 位移跳躍表**（`word_tables()`）：`lea 表,An` 之後以 An 為基底讀 word，經 `adda`/`add`/`lea (An,Dm)`/
  `jmp (An,Dm)` 合成位址再 `jsr/jmp`（可跨過 `bra` 到共用的分派程式），表內每一項輸出成 `.word L_目標-D_表`，目標當成
  程式碼。原本這些表是原始位元組：插入點落在表和 handler 之間時位移量就錯（位移測試的插入點都在前面，抓不到）。
  目前 14 張表、576 項（CPU AI 的 256 個 opcode `CPU_AI_OP_TABLE`、受擊反應 `D_016a58`…）。
- **檢查工具** `tools/disasm/codecheck.py`（`make codecheck`）：列出仍被當成資料、卻被當程式碼使用的 `D_` label
  （handler 欄位、spawn 的 A0、分支/呼叫目標），應為 0；另列出內含 `jsr/jmp abs.l` 的資料段（可疑，需人工看：多半是資料表
  後面沒有任何參照的程式碼）。每次改 mkasm 規則後跑一次。

現況：見 `README.md` 與 `make namecov`。

### 位移除錯迴圈（全部 `--interp --rtc-fixed 883612800`、新的 backup RAM，兩邊同樣輸入）
```
python3 tools/disasm/shifttest.py --script S.txt --insert 1e62          # 找第一個分歧的 VBlank 幀 F
python3 tools/disasm/pairrun.py --insert 1e62 --script S.txt --frames F+1 -- --trace-frame F
python3 tools/disasm/tracediff.py build/shifttest/pair/orig.log build/shifttest/pair/shift.log 1e62 [--pc-only]
    # 第一個 PC/週期不同的指令；（無 --pc-only 時）先報第一個「不是位移指標」的暫存器差異
python3 tools/disasm/whowrote.py --script S.txt --frame F --value HANDLER     # 物件 handler 是誰寫的（含 0x30cc 產生物件的呼叫者）
python3 tools/disasm/pairrun.py ... -- --break '{pc:XXXX}'  +  tools/disasm/regdiff.py   # 某指令處的暫存器比較
python3 tools/disasm/pairrun.py ... -- --watch A-B  +  tools/disasm/writediff.py [--exact]  # 第一筆不同的 RAM 寫入
python3 tools/disasm/pairrun.py ... -- --ram-log '{out}/{side}.ram' --ram-log-from N  +  tools/disasm/ramdiff.py
```
- 「第一個分歧的 VBlank」可能晚於真正分歧：先用 `--pc-only` 二分找出週期開始漂移的那一幀，再看該幀第一個不同的指令。
- 典型結果：(a) 間接跳躍到「沒位移」的位址 → 有指標沒符號化（找寫入者）；(b) 運算結果差 2（速度、乘數、MULU 週期）→ 有常數被誤判成指標。
- 曾經的例外（已由腳本修正，不是符號化錯誤）：`moves_newfaces` / `moves_orochi_newfaces` 第 12250 幀有一格 fix 圖塊不同。
  血條程式（`0x29468`）以 `health + health_drain`（`+0x138` + `+0x13a`）為列號讀 `0xabeee` 的 14 位元組血條表（列 0–0x68）。
  受擊時 `health -= d`、`drain += d`，drain 隔幀減 1，所以正常遊戲裡兩者相加不超過 0x67；但 `moves_*` 腳本每 10 幀把
  P2 血量 poke 回 0x67 而沒清 drain，相加到 0x6a，讀到表後面的字串指標表 `0xac4ac`，把指標數值（`0x0ac4fb` 的低 16 位元）
  當圖塊畫出，位移後跟著改變。`gen_scripts.py` 現在同時 poke drain = 0。教訓：poke 要維持遊戲的不變量，否則遊戲會
  讀到表外，而表外的指標會讓位移測試出現假的不同。
- `pairrun.py` 的 `{pc:XXXX}` 會換成該映像裡原始 label `L_XXXX` 的位址；`{out}`/`{side}` 是輸出目錄/`orig|shift`。
- 修改 `emit()`/`Conv` 規則後：`mkasm.py`（不加參數）仍須 match、`make matching` 須 OK、再跑 `make shifttest`。

### 第 3 階段（命名與註解）
- `games/kof97/symbols/curated.txt`（人工整理）：`ADDR NAME | 註解`（縮排的 `| ...` 是上一行註解的延續）。
  P1 位址 → 在原本的 `L_`/`D_` label 旁加上別名並在所有參照處使用（`L_`/`D_` 保留，位移工具靠它們換算位址）、
  函式前輸出註解；其他位址（I/O 暫存器、BIOS 呼叫、BIOS/遊戲 RAM）→ `.equ`，絕對位址運算元改用名稱。
  `(d16,A5)` 存取（A5 = `0x108000`）在註解加上 `[RAM 名稱]`；`@field OFFSET NAME` 定義物件欄位，`(d16,A4)` 註解加 `[obj.NAME]`。
- `games/kof97/symbols/chars.txt`（`make charmap` 產生，不要手改）：每個角色跑一次鏡像對戰（雙方隊伍都 poke 成同一角色），
  只在該角色的執行紀錄出現的函式入口命名為 `chr_<角色>_<位址>`（Orochi 版與本尊共用的歸給本尊）。依據是觀測，不是證明；
  只有 Iori/Shingo/Orochi 這種單人「隊伍」的關卡專用程式碼可能被誤歸。人工名稱優先。
- `games/kof97/symbols/actions.txt`（`make actionmap` 產生）：2P 對戰（P2 不動），P1 只做一個動作（走、跑、後跳、蹲、跳、
  輕/重拳腳、蹲/跳攻擊、翻滾、集氣、挑釁），和不動的基準場次比較；4 個角色都會因此執行的函式入口命名為
  `plr_<動作或群組>_<位址>`（群組：walk、move、crouch、jump、attack_stand、attack、ground_move、button、any_move）。
- `games/kof97/symbols/specials.txt`（`make specmap` 產生）：同樣的 2P 設定，P1 輸入一種指令（↓↘→ `qcf`、↓↙← `qcb`、
  →↓↘ `dp`、←↓↙ `rdp`、←↙↓↘→ `hcf`、→↘↓↙← `hcb`、蓄力 `chg_bf`/`chg_du`）＋拳（`p`）或腳（`k`）；只被一種指令觸發的
  角色專屬入口改名為 `chr_<角色>_<指令>_<p|k>_<位址>`（{qcf,hcf}、{qcb,hcb} 視為較短的那個）。`chg_du`（按住下再上）多半是
  角色各自的大跳，不一定是必殺技。
  載入順序：chars → actions → specials → derived → curated（後者優先），
  最後 moves 覆蓋 `chr_` 開頭或推導出的名稱（官方招式名）。
- `tools/disasm/derivemap.py`（`make derivemap`，產生 symbols/derived.txt）：未命名的入口若所有參照都來自同一個已命名
  函式 R，命名為 `R_state_位址`（被寫進 handler 欄位）、`R_obj_位址`（spawn 的物件）、`R_sub_位址`（被呼叫）；推導出的
  名稱再往下推導時仍用 R。改了 symbols 後的順序：make matching → derivemap → chrcheck --prune → make matching。
- 官方招式名：`games/kof97/moves.tsv`（MAME command.dat 的 KOF'97 出招表，數字鍵記號）與
  `games/kof97/commands.tsv`（從各角色的指令檢查程式解出：指令 → next_state / 投技索引）。`movenames.py`
  產生 symbols/moves.txt（唯一對應時 `chr_<角色>_<招式>`），並把 specmap 的指令標籤改成 `chr_<角色>_<招式>_<位址>`
  （歧義時只在註解列候選）。指令辨識：`motion_recognize`（0x12914）依角色的動作清單（`CHR_MOTION_LISTS`）設定輸入
  區塊 +4..+7 的動作位元；各角色三個檢查程序在玩家 +0x194（+0 投技、+4 空中投、+8 必殺/超必殺/指令技）。
- CPU AI：`cpu_generate_input` 每格執行一個 4 個 opcode 的腳本（`+0x16a`，剩餘 `+0x16e`），opcode 經 `CPU_AI_OP_TABLE`
  分派（`cpu_op_<xx>_<意義>`）；腳本從 P2 ROM bank 0 的表選出（角色、距離帶 +0x172、對手動作 +0x160、積極度 +0x171）。
- 同一份名稱也用在重編譯器產生的 C 函式名稱（`rc_p1_009ea2_VBLANK_IRQ`）。
- 名稱只改文字、不改位元組：改完 `make matching` 必須 OK，`make shifttest` 必須全部相同。
- 命名進度：`make namecov`（`tools/disasm/namecov.py`）統計執行過的指令有多少落在具名 label 之後（到 return 為止），
  依名稱來源分類，並把未命名部分依「哪幾類腳本會執行」分組、列出最大的未命名函式（`--kind arcade` 等）。
  需要 `build/shifttest/runs/*_orig/exec.bin`，位移測試執行中不要跑。
- `tools/disasm/chrcheck.py`：charmap / specmap 的輸出都經過它過濾（`--prune` 可直接過濾現有檔案，不必重跑）。丟掉
  (a) 在沒有該角色的 `moves_<隊伍>` 測試裡也被執行的（P1 = 該隊、P2 = Benimaru×3）、(b) 函式中間的位置（沒被參照、
  前一個指令不是 return/jmp/bra；產生器的動態入口包含 `jsr` 返回點）、(c) 人工命名的非角色函式內部、(d) `NOT_CHARACTER`
  範圍（每類腳本都會執行、只是產生器裡剛好只有某招觸發的共用程式，例：fix 文字輸出、連段數 HUD）。第一次過濾
  去掉 4,485 個中的 2,195 個（`chr_king_072a3e` 其實是 NEXT STAGE 畫面、`chr_leona_0290fe` 是 HUD 更新的分支）。

### 覆蓋腳本（`games/kof97/scripts/`，由 `gen_scripts.py` 產生）
14 個街機腳本（9 支隊伍 + Iori/Shingo + Orochi Iori/Leona + Orochi 本人 + Orochi 版 New Faces；ADVANCED/EXTRA 交替；
以 RAM poke 指定隊伍、定期把 CPU 體力歸零以推進關卡、續關）、`versus`（雙人亂入）、`gameover`（不續關、輸入名字）、
`testmode`（BIOS 測試模式、KOF97 soft DIP 兩頁、記帳、日曆、改設定後實玩）。腳本格式見 `kof97 --help`
（`poke:ADDR=VAL` 只寫 work RAM、`mash FROM TO SEED p1|p2|both`）。
結局確認：`python3 tools/disasm/endcheck.py`（以 Japan 場次「打 Orochi → TOTAL SCORE」之間才執行、且 game over / 對戰腳本
不會執行的程式當作結局程式，檢查每個街機腳本的執行紀錄含有多少；目前 13 個街機腳本都在 87–100%，即都打到結局）。
RAM：`0x10a84b-d` P1 隊員 ID、`0x10a85c-e` P2 隊員 ID、`0x108238`/`0x108438` P1/P2 體力（word）、`0x10a849` P1 模式
（工具從 `games/kof97/game.json` 的 `ram` 讀這些位址；遊戲專屬常數都集中在 `games/<GAME>/game.json`，由 `tools/game.py` 載入，
環境變數 `GAME` 選遊戲，預設 `kof97`）。平行工作（`--jobs`）預設只用一半核心：這台機器還跑其他服務。
Orochi 版社/夏爾米/克里斯是 ID 21–23 加上 `0x10a857` 的旗標（bit n = 第 n 位隊員）；直接 poke 32–34 會在開場卡住。

## 8. 除錯流程

1. 畫面卡住：`./build/kof97 --headless --frames 2000 --trace-pc 60` 看 68000 PC / SR / Z80 PC。SR 中斷遮罩 = 7 且 PC 固定 → 無限迴圈或錯誤畫面。
2. 反組譯：`python3 tools/recomp/disasm.py <hexaddr> [count]`。
3. 用 REA 查邏輯：`xrefs`（誰讀寫某 I/O）、`search_strings`（錯誤訊息）、`procedure_pseudo_code`。CLI 範例：
   `REA_GHIDRA_SEED_FILE=$PWD/tools/rea/seeds_bios.tsv tools/rea/rea.sh decompile $PWD/build/roms/bios.be 0xc11a84 --target-format raw-binary --architecture m68k --entry-point 0xc00000 --provider ghidra`
   或直接 grep 已輸出的 `build/rea/decomp_*/functions/*.c`。
4. 判斷是重編譯錯還是硬體模型錯：用 `--interp` 重跑；兩者一致 → 硬體模型（`neogeo.c`/`video.c`/`audio.cpp`）問題。
5. 新增翻譯樣板後務必 `make test`；任何差分失敗都必須修正後才可提交。
