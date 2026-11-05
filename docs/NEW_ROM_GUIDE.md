# 換一套 Neo Geo 遊戲：反組譯 → matching → 命名 的流程

本文件說明如何把這個專案的工具用在**另一套 Neo Geo（MVS）遊戲**上：產生可重組譯成原版位元組的組合語言、
讓它在程式碼位移後仍正確（指標符號化）、再加上名稱與註解。
KOF97 的實際操作紀錄在 `AGENT.md`（第 3 節 REA、第 4 節重編譯器、第 7.5 節 matching）；
本文件著重在「哪些是通用的、哪些要為新遊戲改」。

> ROM 與所有由 ROM 產生的檔案（`build/` 底下）只供本機使用，不可提交或散佈。

---

## 0. 適用範圍與前提

| 條件 | 說明 |
|---|---|
| 未加密的卡帶 | KOF98 以前的大多數遊戲。KOF99 之後的 CMC（C/S ROM）、SMA（P ROM）、PCM2（V ROM）加密需先解密，本工具不處理。 |
| P1 ≤ 1 MB 映射在 0x000000 | 遊戲碼在 P1；P2（若有）是 0x200000 的 bank 資料區（`0x2ffff0` 切 bank）。P1 是 2 MB 且前後半段交換的遊戲，要在 `romprep.py` 改排列方式。 |
| MVS BIOS | 目前用 `sp-s2.sp1`（`neo-geo.rom`）。換 BIOS 版本後，`symbols/curated.txt` 裡的 BIOS 內部位址（例如 `RANDOM_TABLE`）要重新確認。 |
| 工具鏈 | 與 KOF97 相同：REA + Ghidra 12、JDK 21、gcc、SDL2、`binutils-m68k-linux-gnu`（見 `AGENT.md` 第 2 節）。 |

---

## 1. 流程總覽

```
1. ROM 準備        tools/romprep.py                 → build/roms/{p1.be,p2.bin,bios.be,...}
2. 執行期能開機    make && ./build/kof97 --headless  → 截圖確認（覆蓋腳本與 exec-log 都需要它）
3. REA 分析        tools/rea/mkseeds.py + make rea  → build/rea/decomp_{p1,bios}/
4. 重編譯/程式碼地圖 make（m68k_recomp.py）          → 指令探索、m68kinfo、m68k_table.txt
5. 第 1 階段        tools/disasm/mkasm.py            → P1 byte-identical
6. 覆蓋腳本        games/kof97/scripts/*.txt       → 讓遊戲跑過盡量多的程式（遊戲專屬）
7. 第 2 階段        make matching / make shifttest   → 指標符號化 + 位移測試
8. 第 3 階段        games/kof97/symbols/curated.txt（+ charmap 類的觀測式命名）
```

每一步的指令、通用的部分、要為新遊戲修改的部分如下。第 3 節是所有遊戲專屬常數的總表。

---

## 2. 各步驟

### 步驟 1：ROM 準備 — `tools/romprep.py`

做的事（通用）：驗證 CRC、P ROM 做 16-bit byte swap（MAME 的 `ROM_LOAD16_WORD_SWAP`）、C ROM 兩兩交錯
（`ROM_LOAD16_BYTE`）、V ROM 串接、輸出正規化檔名（`p1.bin p2.bin bios.bin s1.bin sfix.bin m1.bin sm1.bin v.bin c.bin lo.bin`，
以及給 REA/反組譯用的 `p1.be`、`bios.be`）。

要改的：新增 `games/<遊戲>/game.json`（複製 `games/kof97/game.json`），以 `GAME=<遊戲>` 執行（`make GAME=xxx`，
或 `export GAME=xxx`；所有工具經 `tools/game.py` 讀取，預設 `kof97`）。
- `rom_zip`、`bios_zip`：MAME 的 zip 路徑（`Makefile` 的 `$(ROMS)` 規則也依賴它們）。
- `roms`：照 MAME driver（`neogeo.cpp` 的 `ROM_START( xxx )`）填 `[檔名, CRC32]`；`p1`/`p2`/`s1`/`m1` 各一個、
  `v` 依序串接、`c` 是 `[[偶, 奇], ...]` 配對依序排列。P ROM 若不是「p1 在 0、p2 為 bank 資料」的標準排列要改 `romprep.py`。

驗證：`python3 tools/romprep.py` 不報 CRC 錯誤。

### 步驟 2：讓執行期能跑（建議，後面 3 步都要靠它）

`src/` 的硬體模型（記憶體映射、LSPC、YM2610/Z80、日曆晶片、watchdog、backup RAM）是 Neo Geo 通用的；
執行檔名稱叫 `kof97` 但不含 KOF97 專屬邏輯。

```
make -j$(nproc)
./build/kof97 --headless --frames 3000 --dump build/shots/boot --dump-every 150
python3 tools/contact_sheet.py 'build/shots/boot_*.ppm' build/shots/boot.png 5
```

注意：
- 重編譯器會在 `make` 時跑（步驟 4），第一次也可以先用 `--interp` 純直譯確認硬體模型。
- KOF97 踩過的硬體坑（日曆 TP 1 Hz、watchdog 防盜版）是硬體行為，已在模型裡；新遊戲若卡在錯誤畫面，照
  `AGENT.md` 第 8 節：`--trace-pc 60` 看 PC/SR，`--interp` 判斷是重編譯還是硬體模型問題。
- 舊的 `kof97.nvram`/測試用 `.nv` 檔不要跨遊戲共用（backup RAM 內容是遊戲專屬的）。

### 步驟 3：REA 分析 — `tools/rea/mkseeds.py` + `make rea`

```
python3 tools/rea/mkseeds.py        # 由卡帶檔頭跳躍與向量表產生 seeds_p1.tsv / seeds_bios.tsv（通用）
make rea                            # REA decompile-all（Ghidra，raw-binary m68k，P1 @0x0、BIOS @0xC00000）
```

全部通用；只要 `build/roms/p1.be` 換成新遊戲即可。BIOS 的分析結果可以沿用。
REA 的函式清單（`decompile-index.json`）是重編譯器與反組譯器的種子；偽代碼用來閱讀與找邏輯
（例如 `grep -l '00320000' build/rea/decomp_p1/functions/*.c` 找寫聲音指令的函式）。

### 步驟 4：重編譯 / 程式碼地圖 — `make`

`tools/recomp/m68k_recomp.py` 的探索（REA 種子 + 向量 + 檔頭 + 指標表掃描 + 執行期 miss）也是 `mkasm.py`
的程式碼地圖來源，所以即使只要反組譯也要先 `make`（同時產生 `build/m68kinfo`、`build/m68k_table.txt`）。

通用。`make test`（每條翻譯指令與 Musashi 差分比對）必須 0 failed。
覆蓋率不夠時：跑一段 `./build/kof97 --headless --autoplay --frames 30000` 收集 miss，再 `make`（`AGENT.md` 第 4 節）。

### 步驟 5：第 1 階段（byte-identical）— `tools/disasm/mkasm.py`

```
python3 tools/disasm/mkasm.py       # 預期: P1 image match: True
```

通用。輸出的 raw fallback（`.word`）多半是被誤判成程式碼的資料；`mkasm.py` 已有的判斷規則
（不可能的編碼、分支到非 label/奇數位址、line-A/F、奇數位址存取觸發 address error、執行期觀測）大多可直接適用。
剩下的看報告的原因分類逐一處理（`AGENT.md` 7.5「第 1 階段」）。

要改的：
- 若 P1 結尾不是 `0xffff` 填充，`--insert-nop`（位移測試）會失敗：`emit()` 結尾的 `assert p1[-2:] == b'\xff\xff'`，
  要改成從其他未使用區域刪 2 bytes。

### 步驟 6：覆蓋腳本 — `games/kof97/scripts/`（完全遊戲專屬）

第 2 階段的正確性只對「被執行過的程式」有保證，所以要讓遊戲跑遍各種情況。格式見 `./build/kof97 --help`：
`frame duration key[,key]`、`poke:ADDR=VAL`（只寫 work RAM）、`mash FROM TO SEED p1|p2|both`。

怎麼做：
1. 先用截圖摸清選單流程與時間點（`--dump PREFIX --dump-every 20` + `contact_sheet.py`）。
2. 找出推進遊戲需要的 RAM 位址（體力、選角、關卡）：
   - `--ram-log FILE --ram-log-from N` 存每幀 work RAM，用 numpy 找「隨畫面變化」的位元組（KOF97 找體力、隊員 ID 的作法見 `AGENT.md` 7.5 最後一段）。
   - `--watch A-B`（加 `--interp`）看是哪條指令寫入；`--break PC` 看暫存器。
   - 寫 poke 前先單獨驗證（截圖確認效果），注意顯示用的副本（KOF97 的 `+0x150`）和真正的值（`+0x138`）不同。
3. 仿照 `games/kof97/scripts/gen_scripts.py` 寫產生器（KOF97 的 `PREFIX` 時間點、`TEAMS`、RAM 位址都要換）。
4. 測試模式（BIOS 選單 → 遊戲的 soft DIP 頁）是通用的好覆蓋來源；KOF97 的按鍵順序可參考 `testmode()`。

### 步驟 7：第 2 階段（shiftable）— `make matching` / `make shifttest`

```
make matching       # 先自動跑覆蓋腳本收集 build/disasm/exec_p1.bin，再符號化 + 組回 P1/P2 + 驗 CRC
make shifttest      # 所有腳本 × 3 個插入點：截圖、音訊、每次 VBlank 中斷位置必須完全相同
```

通用的部分：`mkasm.py` 的指標規則、`--exec-log` 的 4 種觀測（執行過的位址、long/word 讀取、已證實的程式指標）、
`shifttest.py` 的流程、除錯工具 `pairrun.py`/`tracediff.py`/`regdiff.py`/`writediff.py`/`ramdiff.py`。

要改的（`games/<遊戲>/game.json` 的 `disasm`）：
- `vblank_break`（KOF97 `9eb0`）：遊戲 VBlank 處理常式中「確定由遊戲執行」的一條指令。找法：向量表第 25 個 longword
  （`0x64`，level 1 autovector）指向的常式；很多遊戲開頭會依 `BIOS_SYSRET_STATUS` 決定交給 BIOS 或自己處理，
  取分岔後遊戲那一側的第一條指令。中斷位置比對就是靠這個斷點的 `sp+2`（被打斷的 PC）。
- `shift_inserts`（KOF97 `800`、`1e62`、`60d36`）：選 P1 開頭附近的第一個程式 label、前段一個、中段一個
  （`grep -m5 '^L_000[2-9a-f]' build/disasm/p1.s`）。插入點必須是指令 label。

分歧時照 `AGENT.md` 7.5「位移除錯迴圈」：`shifttest` 找第一個分歧幀 → `pairrun --trace-frame` + `tracediff`
（必要時 `--pc-only` 二分找週期開始漂移的那一幀）→ 找寫入者 → 修 `mkasm.py` 的規則 → 重跑全部（含第 1 階段仍 match）。
典型兩類：間接跳到「沒位移」的位址（指標漏了）、數值差 2（常數被誤判成指標）。
新腳本跑過後 `exec_p1.bin` 會自動擴充，很多分歧不需要改規則。

`whowrote.py` 的預設值來自設定檔 `objects`（KOF97：`dispatch 9de4` 物件迴圈 `jsr (A0)`、A4 = 物件；`spawn_store 30cc` 產生物件）；
新遊戲要先找出它自己的物件/任務分派點，或改用通用的 `--watch` + `writediff.py`。

### 步驟 8：第 3 階段（命名）— `games/kof97/symbols/curated.txt`

格式：`ADDR NAME | 註解`、縮排的 `| ...` 續行、`@field OFFSET NAME`（`(d16,%a4)` 的欄位提示）。
P1 位址 → label 別名（保留 `L_`/`D_`）；其他位址 → `.equ` 並取代絕對位址運算元。只改文字，改完 `make matching` 必須 OK。

可直接沿用的段落：
- 卡帶檔頭（`HDR_*`、`USER`/`PLAYER_START`/`DEMO_END`/`COIN_SOUND`），
- I/O 暫存器（`REG_*`、`PALETTE_RAM`、`BACKUP_RAM`），
- BIOS 呼叫（`SYSTEM_IO` 等）與 BIOS work RAM（`BIOS_*`）。

遊戲專屬、要重做的：遊戲程式（`game code` 段）、遊戲 RAM、`@field`、soft DIP 表位址（從檔頭 0x116/0x11A/0x11E 讀）。
找名稱的方法：
- REA 偽代碼找「誰碰哪個暫存器」（聲音 `0x320000`、VRAM `0x3c0000`、調色盤 `0x400000`、輸入 `0x300000`）。
- 呼叫次數最多的函式通常是共用工具（亂數、定點乘法、動畫、產生物件），值得先命名。
- `mkasm.py` 的 `a5hint()` 用設定檔 `disasm.a5_base`（KOF97 `108000`，遊戲以 A5 定址全域變數）；新遊戲若不同就改，或刪掉這個鍵停用。

觀測式命名（`tools/disasm/charmap.py`，KOF97/格鬥遊戲專用）：讓每個角色單獨出場跑一次（雙方隊伍 poke 成同一角色），
只在該角色場次執行到的函式入口命名為 `chr_<角色>_<位址>`。同樣的想法可以換成「逐關卡」「逐模式」「逐道具」，
但角色 ID 表與 `PREFIX` 時間點是 KOF97 的；隊伍/模式/體力 RAM 位址在設定檔 `ram`。

---

## 3. 遊戲專屬常數總表（KOF97 的值在 `games/kof97/game.json`）

| 位置 | 內容 | 新遊戲要做的事 |
|---|---|---|
| `games/<遊戲>/game.json` `rom_zip`/`bios_zip`/`roms` | ROM 檔名與 CRC、P/C/V ROM 組合（`romprep.py`、`mkrom.py`、`Makefile` 讀取） | 照 MAME driver 填 |
| `games/<遊戲>/game.json` `disasm` | `vblank_break`、`shift_inserts`、`a5_base`（`shifttest.py`、`mkasm.py`） | 找新遊戲的值 |
| `games/<遊戲>/game.json` `objects` | 物件分派、產生物件（`whowrote.py` 預設值） | 找對應點或改用 `--watch` |
| `games/<遊戲>/game.json` `ram` | 隊伍、模式、Orochi 旗標、體力（`gen_scripts.py`、`charmap.py`、`actionmap.py`、`specmap.py`） | 用 `--ram-log`/`--watch` 找 |
| `tools/disasm/mkasm.py` `emit()` 結尾 | P1 結尾需 `0xffff` 填充（位移測試） | 確認或改刪除位置 |
| `games/kof97/scripts/gen_scripts.py` | 選單時間點、隊伍、招式指令 | 重寫 |
| `tools/disasm/charmap.py`、`actionmap.py`、`specmap.py` | 角色 ID、時間點、招式指令 | 重寫或改成其他維度 |
| `games/kof97/symbols/curated.txt` | 遊戲段落、`@field`、soft DIP 位址 | 保留通用段落，其餘重做 |
| `games/kof97/symbols/chars.txt` | 產生檔 | 刪掉，用新的觀測重產 |
| `build/disasm/exec_p1.bin`、`build/recomp/runtime_misses.txt` | 前一套遊戲的觀測資料 | **一定要刪掉**再開始（位址不通用） |
| `tools/recomp/extra_seeds.txt`（若有） | 手動種子 | 清空 |

通用、不用改的：`tools/rea/*`（`mkseeds.py`、`rea.sh`）、`tools/recomp/*`、`tools/disasm/{pairrun,tracediff,regdiff,writediff,ramdiff,disasm}.py`、
`src/`（硬體模型）、`tests/`。

---

## 4. 建議的開工順序（檢查清單）

1. 新開一個工作目錄（或分支），刪掉 `build/`、`games/kof97/symbols/chars.txt`、`games/kof97/scripts/*.txt`。
2. 寫 `games/<遊戲>/game.json`、`export GAME=<遊戲>` → `python3 tools/romprep.py` CRC 全過。
3. `make -j3`（不要用全部核心）、`make test`；`--headless` 截圖確認能開機到標題（必要時修硬體模型）。
4. `python3 tools/rea/mkseeds.py && make rea && make`（REA 種子 → 重編譯器拿到函式清單）。
5. `python3 tools/disasm/mkasm.py` → `P1 image match: True`。
6. 找 VBlank 斷點與插入點，改 `shifttest.py`；先用 `--autoplay` 跑 `shifttest.py --autoplay --insert <點>`。
7. 寫覆蓋腳本（第 6 步）→ `make matching` → `make shifttest`，分歧就進除錯迴圈。
8. `symbols/curated.txt`：先保留通用段落，再逐步加遊戲名稱；每次改完 `make matching`。
