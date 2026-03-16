# KB12→KB26 コンバータ 現状分析レポート

## 概要

弥生会計12のバックアップファイル (.KB12) を弥生26形式 (.KB26) に変換するコンバータを開発中。
ZIPコンテナレベルの変換は成功しているが、内部MDBデータベースの互換性問題により
**やよいの青色申告26がクラッシュ（ダイアログなしで強制終了）** する状態。

## 現在の変換フロー

```
KB12ファイル読み込み
  ↓
1. YZ署名確認 (弥生変形ZIP)
2. EOCD検索・セントラルディレクトリ解析
3. エントリ名 .KD12 → .KD26 書き換え (LFH, CDE両方)
4. EOCD後の弥生独自メタデータ内 .KD12 → .KD26 パッチ
5. KDエントリ (Jet 4.0 MDB) を展開・パッチ・再圧縮
   a. raw deflate で展開
   b. SystemInfo テーブルの DataVersion を 2100→3600 に更新
   c. SystemInfo に不足行 (Id=5,6,9,10,11) を追加
   d. TDEF行カウント・データページ空き領域フィールドを更新
   e. CRC-32 再計算
   f. raw deflate で再圧縮
   g. LFH/CDE/EOCD の圧縮サイズ・CRC-32・CDオフセットを更新
6. 出力ファイル書き出し
```

## 成功していること

### ZIPコンテナ変換
- YZ署名の弥生変形ZIP構造を正しくパース
- int64サイズフィールド (LFH=38bytes固定, CDE=58bytes固定) に対応
- エントリ名・後付けメタデータ内のバージョン文字列書き換え
- EOCD + 736バイトの弥生独自トレーリングメタデータの構造保持
- 再圧縮後のCRC-32・圧縮サイズ・CDオフセットの整合性

### MDB内部パッチ
- SystemInfo TDEF ページの特定 (UTF-16LE "DataVersion" 検索)
- Id=1 の DataVersion フィールドを 2100→3600 に正常更新
- 不足行 (Id=5,6,9,10,11) の Jet 4.0 データページへの挿入
- TDEF ヘッダ行カウント (offset 16-19) の更新
- データページ空き領域 (offset 2-3) の更新

### 検証済み
- Python スクリプトでの展開・行読み取り検証で全7行が正常
- CRC-32 一致確認済み
- 弥生26の「バックアップファイルの復元」で KB26 ファイルとして認識される

## 失敗していること

### クラッシュ症状
- やよいの青色申告26で復元 → **ダイアログなしで強制終了**
- 復元されたKDファイルを直接開いても同様にクラッシュ
- Windows イベントログ:
  - 例外コード: `0xc0000409` (STATUS_STACK_BUFFER_OVERRUN / `__fastfail`)
  - モジュール: `ucrtbase.dll` オフセット `0x0002da51` (全クラッシュ共通)
  - アプリ: `YKaikei26.exe` (v32.0.4.112)
- `0xc0000409` は Yayoi のコードが意図的に `abort()` を呼んでいることを示す

### DataVersion変遷
| バージョン | 結果 |
|-----------|------|
| 2100 (元のKB12値) | ダイアログ「現在の弥生会計のバージョンでは使用できないデータです」 |
| 3600 (パッチ後) | ダイアログなしでクラッシュ |

## 判明している構造差異

### 実KD26作業ファイルとの比較

実際にやよい26で正常に使用されている KD26 ファイル
(`C:\Users\rinak\OneDrive\ドキュメント\Yayoi\弥生会計26データフォルダ\宮本　宏美　Cafe nest(令和06年度～令和08年度).KD26`)
との比較結果:

#### SystemInfo テーブル (修正済み)

| フィールド | 実KD26 | 変換後 | 状態 |
|-----------|--------|--------|------|
| Id=1 DataVersion | 3600 | 3600 | OK |
| Id=2 DataVersion | 320004112 | 210101307 | **差異あり** |
| Id=5 DataVersion | 3200 | 3200 | OK (追加済み) |
| Id=6 DataVersion | 13 | 13 | OK (追加済み) |
| Id=9 DataVersion | 3400 | 3400 | OK (追加済み) |
| Id=10 DataVersion | 3200 | 3200 | OK (追加済み) |
| Id=11 DataVersion | 3004 | 3004 | OK (追加済み) |
| TDEF行カウント | 7 | 7 | OK (修正済み) |
| データページ空き領域 | 3879 | 3879 | OK (修正済み) |

#### ページタイプ分布

| タイプ | 名称 | 実KD26 | 変換後 | 差分 |
|-------|------|--------|--------|------|
| 0x01 | Data | 1206 | 1095 | -111 |
| 0x02 | TDEF | 274 | 255 | **-19** |
| 0x03 | IntermIndex | 41 | 40 | -1 |
| 0x04 | LVAL | 724 | 693 | -31 |
| 0x05 | LeafIndex | 5 | 1 | -4 |

#### 不足テーブル (19 TDEF)

実KD26に存在するが変換後MDBに存在しない TDEF ページ:

| テーブル/インデックス名 | TDEF ページ | 分類 |
|----------------------|------------|------|
| InvoiceRateKBN | 1102 | インボイス制度テーブル |
| PK_HankanhiTekiyo_KessanshoItemId | 288 | 決算書関連インデックス |
| PK_InvoiceKBN_Id | 341 | インボイス区分PK |
| PK_InvoiceRateKBN_Id | 346 | インボイス税率区分PK |
| PK_InvoiceVarData_Id | 351 | インボイスVarDataPK |
| PK_JigyoshoDataExtInfo_Id | 355 | 事業所拡張情報PK |
| PK_KessanshoItemEtaxMap_NendoId | 528 | e-Tax連携PK |
| PK_KurikoshiTuuchi_Id | 835 | 繰越通知PK |
| PK_Torihikisaki2_Id | 1546 | 取引先2 PK |
| PK_TorihikisakiHojoSettei2_Id | 1552 | 取引先補助設定2 PK |
| PK_TorihikisakiHojoSettei_Id | 1549 | 取引先補助設定PK |
| PK_Torihikisaki_Id | 1543 | 取引先PK |
| PK_ZeiKBNData2Ex_Id | 2071 | 税区分データ2拡張PK |
| PK_ZeiKBNData2_Id | 2054 | 税区分データ2 PK |
| PK_ZeiKBNShisanhyoKamokuVarData_Id | 396 | 税区分資産表勘定PK |
| PK_ZeiKBNShisanhyoKamoku_KamokuZeiKBNId | 391 | 税区分資産表勘定PK |
| PX_ZeiJigyoKBNData_Id | 2031 | 税事業区分PX |
| PX_ZeiKBNData3_Id | 2092 | 税区分データ3 PX |
| PX_ZeiKBNRateMapping_Id | 2095 | 税区分率マッピングPX |
| PX_ZeiRateData_Id | 2117 | 税率データPX |

大半がインボイス制度 (2023年10月導入) 関連のテーブル・インデックス。
KB12時代 (弥生12) にはインボイス制度が存在しなかったため、これらのテーブルがない。

### Id=2 DataVersion の謎

SystemInfo Id=2 の DataVersion 値はエンコードされた値:
- 変換後 (KB12由来): `210101307` (hex: `0x0C85E43B`)
- 実KD26: `320004112` (hex: `0x1312E010`)

パターン分析:
- KB12: `2101` + `01307` → 前半が DataVersion 2100 に近い
- KD26: `3200` + `04112` → 前半が DataVersion 3200 に近い

この値はスキーマバージョンのチェックサムまたはエンコードされたメタデータの可能性。
現状では未修正。

## クラッシュ原因の仮説

### 仮説1: 不足テーブルへのアクセス (有力)

DataVersion=3600 により弥生26は「完全なKB26スキーマ」を前提にDBを開く。
インボイス関連テーブル (InvoiceKBN, InvoiceRateKBN, Torihikisaki等) にアクセスしようとして、
テーブルが存在しないため Jet エンジンがエラーを返し、弥生のコードが `abort()` を呼ぶ。

**根拠**: 19個のTDEFページが不足。すべてKB26で追加されたテーブル/インデックス。

### 仮説2: Id=2 DataVersion の不整合

Id=2 の DataVersion がスキーマの整合性チェックに使われている場合、
KB12時代の値 `210101307` が KB26 の期待値 `320004112` と一致しないためエラーになる。

**根拠**: DataStatus=4 が両ファイルで共通しており、特殊な管理行であることを示唆。

### 仮説3: MSysObjects カタログとの不整合

Jet 4.0 の MSysObjects システムテーブルが全テーブルの定義を管理している。
実KD26では MSysObjects のデータページが8つだが、変換後は7つ。
弥生26が MSysObjects を走査してテーブル定義を確認する際、
不足エントリがあるとクラッシュする可能性。

### 仮説4: 中間バージョンによるマイグレーション不足

正規の移行パス: **KB12 → KB19 → KB24 → KB26** (ユーザー証言)

直接 KB12→KB26 へジャンプすると、中間バージョンで行われるべき
スキーマ変更 (テーブル追加・列追加・インデックス作成等) がすべてスキップされる。
DataVersion=3600 はマイグレーション完了を意味するため、
弥生26はマイグレーションコードを実行せず直接DBを開こうとする。

## 技術仕様メモ

### 弥生バックアップ KB/KB26 形式
- 変形ZIPアーカイブ: 署名 "PK" → "YZ" に置換
- LFH: 38バイト固定 (comp_size/uncomp_size が uint64)
- CDE: 58バイト固定 (comp_size/uncomp_size/local_offset が uint64)
- EOCD: 22バイト標準 + 736バイト弥生独自メタデータ
- 内部エントリ: `.KD26` = Jet 4.0 MDB (raw deflate 圧縮)

### Jet 4.0 MDB 形式
- ページサイズ: 4096バイト
- ページタイプ: 0x00=空, 0x01=データ, 0x02=TDEF, 0x03=中間索引, 0x04=LVAL, 0x05=葉索引
- TDEF ヘッダ: [0]=type, [2-3]=free_space, [8-11]=next_page, [16-19]=row_count
- データページ: [0]=type, [2-3]=free_space, [4-7]=owner_tdef, [12-13]=row_count
  - 行オフセットテーブル: [14 + r*2] (uint16, ページ内オフセット)
  - 行データはページ末尾から下方向に成長

### SystemInfo テーブル行レイアウト (27バイト固定)
```
[0-1]   flags       (uint16) = 0x0005
[2-5]   Id          (int32 LE)
[6-9]   DataVersion (int32 LE)
[10-13] DataStatus  (int32 LE)
[14-17] ShoushinStatus (int32 LE)
[18-25] RecTimeStamp   (double LE)
[26]    null bitmap    (0x1F = 全5列非NULL)
```

## 次のステップ案

### A案: 中間DataVersionでマイグレーション誘発
DataVersion を 3600 ではなく中間値 (例: 3004, 3200, 3400) に設定し、
弥生26のマイグレーションコードが不足テーブルを自動生成するか検証。

### B案: 不足テーブルの手動作成
Jet 4.0 の TDEF ページ形式に従い、19個の不足テーブル/インデックスを
MDB 内に手動作成。複雑度が高い (TDEF構造・MSysObjects更新・空きページ管理等)。

### C案: DataVersionを未修正でリストア後に弥生のコンバート機能を利用
DataVersion=2100 のまま復元し、「使用できないデータです」エラーが出る状態で
弥生26側に中間バージョン (KB19/KB24) のコンバート機能がないか調査。

### D案: Id=2 DataVersion の修正
Id=2 の DataVersion を KB26 相当の値 `320004112` に変更し、
スキーマ整合性チェックをパスするか検証。

### E案: 弥生の中間バージョン (19/24) を経由する段階的変換
実際の正規パスに従い KB12→KB19→KB24→KB26 の各ステップを
別々のコンバータで処理。中間バージョンのバックアップ形式の調査が必要。

## ファイル構成

```
src/
  kb_converter.cpp  - メインコンバータ (ZIP解析・MDBパッチ・再圧縮)
  kb_converter.h    - パブリックAPI
  kb_format.h       - 弥生ZIP形式の定数定義
  main.cpp          - CLI エントリポイント
  inspect_kd.cpp    - KDファイル構造調査ツール
  find_version.cpp  - バージョン情報検索ツール
  parse_sysinfo.cpp - SystemInfoパースツール

*.py               - Python分析スクリプト (MDB構造調査用)
```
