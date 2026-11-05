# 重製圖片與聲音（ComfyUI 等工具）

原生版可以用你重製的圖片和聲音取代原版素材，不需要修改遊戲程式。所有素材都由你自己的 ROM 產生，
只放在本機（`build/`、`mods/`），**不可提交或散佈**（兩者都在 `.gitignore`）。

## 1. 匯出原版素材

```sh
make assets          # = 下面兩個
python3 tools/assets/export_sprites.py --composites --sheet   # 圖片 -> build/assets/kof97/sprites/
python3 tools/assets/export_audio.py                          # 聲音 -> build/assets/kof97/audio/
```

- **圖片**：`build/assets/kof97/sprites/<編號>_<名稱>/fNNN_<ID>.png`，每個檔是遊戲的一個動畫格（角色的一個
  動作格、特效、HUD…），透明背景、原尺寸（16 的倍數）、角色預設顏色。`cNNNN.png` 是由多格拼成的整體動作
  （只供參考），`sheet.png` 是一覽表，`index.json` 列出所有 ID。
- **聲音**：`build/assets/kof97/audio/<指令>.wav`，16-bit 立體聲 55555 Hz；音樂含前奏與循環段，部分音樂分成
  幾個分軌（`<指令>_s0.wav`、`_s1.wav`…，各自循環）。`index.json` 列出每個聲音 ID 的類別與循環點。

## 2. 重製

- **圖片**：把 `fNNN_<ID>.png` 丟進 ComfyUI 重繪（可以放大），存成 **`mods/kof97/sprites/<ID>.png`**
  （檔名只要 ID，6 位十六進位）。任何尺寸都可以，繪製時會縮放到原動畫格的框；背景要透明（alpha < 128 視為
  透明）。翻轉、位置、前後順序由遊戲決定，不用處理。
- **聲音**：重製後存成同名檔 **`mods/kof97/audio/<指令>.wav`**（16-bit、55555 Hz、立體聲或單聲道；也可以是
  同名的 `.ogg`）。
  音樂的循環點沿用原版的樣本位置，所以重製音樂時請保持長度與循環點不變。

## 3. 執行

```sh
build/kof97 --gpu --native-audio --mods mods/kof97     # GPU 繪圖（視窗解析度）+ 原生聲音 + 你的素材
build/kof97 --hd 4 --native-audio --mods mods/kof97    # 不用 GPU：CPU 以 4 倍解析度繪圖
make audio-ogg && make web && make serve               # Web 版：打包 OGG 聲音與 mods/kof97，預設 WebGL 繪圖
```

- `--native-audio [DIR]`：用預錄的聲音檔（及 `mods/` 的替換檔）播放，不再模擬 Z80 / YM2610。找不到 `.wav`
  時改讀同名的 `.ogg`，替換聲音也可以存成 OGG（55555 Hz）。
- `make audio-ogg`：把聲音壓成 Ogg Vorbis 到 `build/assets/kof97/audio_ogg/`（約 55 MB，WAV 約 820 MB），
  Web 版會打包這份並預設使用原生聲音；網址加 `?audio=emu` 改回晶片模擬。沒有先跑這一步的話，Web 版使用
  晶片模擬的聲音。
- `--gpu`：用 OpenGL 以視窗的解析度繪圖（可任意縮放、全螢幕），替換圖以雙線性取樣、邊緣半透明混合。
  原版圖塊照像素放大，結果和 `--hd` 相同。Web 版預設使用（WebGL），網址加 `?gpu=0` 改用 CPU 繪圖；無法使用
  OpenGL / WebGL 時自動改用 CPU 繪圖。
- `make web MODS=目錄`：Web 版改打包別的替換素材目錄（預設 `mods/kof97`）。
- `--hd S`：以 S 倍解析度輸出（例如 4 = 1280×896）。原版圖塊照像素放大，替換圖以完整解析度取樣，所以
  重繪時放大的細節會保留。
- `--mods DIR`：替換素材的目錄（預設 `mods/kof97`）；沒有替換檔的動畫格 / 聲音照原版。
- 繪圖時機：每條掃描線在硬體畫它的時候才畫（遊戲在畫面途中的寫入，例如閃爍文字、調色盤淡入淡出，和真機一樣
  可見）；`--frame-render` 改成在 VBlank 開始時一次畫整個畫面。KOF97 沒有用計時器中斷做光柵效果。
- `make rendertest`：把匯出的原圖當替換圖，兩種繪圖時機下的 1x、`--hd 2` 都必須和原版相同（改了繪圖程式就跑）。
- 縮放（縮小）的物件也會用替換圖，取樣位置跟著硬體的縮放。調色盤只有單一顏色的物件（例如角色腳下的影子：
  角色圖垂直壓扁、用深色調色盤畫）只取替換圖的輪廓，填上那個顏色。
- 調色盤效果：替換圖會跟著遊戲的調色盤變化（受擊閃光、2P / 其他按鍵的配色、特效的調色盤動畫）。每個畫面
  像素比較原版圖塊該處「現在的顏色」和「匯出時的顏色」，把差值加到替換圖上；沒有變化時替換圖原樣顯示。
  所以重繪時請以匯出的配色為準（角色預設配色；非角色物件是灰階）。驗證：拿匯出的原圖當替換圖，畫面與原版
  逐像素相同。
- 和原圖同尺寸（或更小）的替換圖以最近鄰取樣（保持像素感），較大的重繪圖在 `--gpu` 下以雙線性取樣。
