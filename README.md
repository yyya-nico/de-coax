# CECH-ZD1 J / WinUSB 試作ドライバー

Sony CECH-ZD1 J（USB `054c:04b2`）をWindowsのWinUSBから操作するユーザー空間ドライバーです。`BonDriver_CECH.dll` とアプリ用の `WinSCard.dll` を含みます。地デジの受信・選局と内蔵B-CASスロットを同じホストが管理します。

現段階は実機評価用です。数日間の録画や抜き差しを含む、px4_drvと同等の運用実績はまだありません。実測値と未検証の条件は [検証記録](docs/validation.md) にまとめています。

## TVTest / EDCBへの配置

アプリのビット数に合わせて `dist/x64` または `dist/x86` を選びます。Windows自体が64bitでも、32bit版アプリにはx86版が必要です。

```text
TVTest.exe または EpgDataCap_Bon.exe
WinSCard.dll
BonDriver/
  BonDriver_CECH.dll
  cech-host.exe
```

`BonDriver_CECH.dll` と `cech-host.exe` をアプリと同じフォルダーへ置く構成も使えます。カード用DLLはアプリと同じフォルダーに置きます。`System32`への配置やシステムのWinSCardの置換は不要です。既存の `WinSCard.dll` がある環境では、まずアプリのコピーを別フォルダーに用意して評価できます。

1. coaxなど、この機器を使っているアプリを終了します。
2. 上の3ファイルを配置して、アプリを起動し直します。
3. `BonDriver_CECH.dll` を選び、「地上デジタル」でチャンネルスキャンします。
4. カードリーダーを指定するCAS設定では `Sony CECH-ZD1 J Smart Card Reader` を選びます。既存のTVTest用CASモジュールやEDCB用B25デコーダーをそのまま使う構成です。

BonDriverのチャンネル番号は `0 = 物理13ch`、`7 = 物理20ch` です。リモコン番号とは異なります。スキャン結果やEDCBのChSetはアプリ側が作成します。

WinSCard側は、同じフォルダー、またはその直下の `BonDriver` フォルダーにある **`BonDriver_CECH.dll` という名前**を探します。DLLを別名へ変更すると、カードとチューナーが別のUSB所有者を開こうとするため、この構成ではファイル名を維持してください。

## 実装と現時点の範囲

- MB86H47AのSRAMへcoax由来のFR30プログラムを読み込み、I2C操作とTS転送を実行します。外部ファームウェアファイルは不要です。
- DiB8096の初回選局は伝送パラメーターを自動探索します。同じホスト内で一度フルセグ受信を確認した周波数は、TMCCパラメーターを再利用します。再利用でロックを失った場合は、その値を破棄して自動探索を1回行います。
- FFT自動探索の開始前に90msの追加待機を設けています。22chで8K信号を2Kと誤判定して選局に失敗する症状への対策です。キャッシュを使う手動選局にはこの追加待機を入れません。比較結果は[22chの調査記録](docs/tune22-investigation.md)にまとめています。
- チューナー校正、AGC、受信回路の待機時間は維持しています。ワンセグだけのロックをフルセグ受信成功として扱うことは避け、TV用の複数セグメント階層のロックも確認します。
- 選局とPurgeは世代番号を更新し、ホスト、USB、DLLの古いTSを境界の向こうへ渡さない構成です。PurgeではRF選局を維持してブリッジとUSBの受信経路を作り直します。
- WinSCardはpx4_drv由来のAPI互換処理を使用します。内蔵カードの要求はBonDriverと同じUSBホストへ渡し、外付けリーダーの要求はWindows本来のWinSCardへ転送します。
- 内蔵カードはT=1、ATR、共有・排他接続、トランザクションに対応します。カードだけを開くことも、チューナーを閉じた後にカード接続を維持することも可能です。
- カード通信中も独立したTS受信処理がWinUSBの転送リングを読み続けます。チューナー制御とカード制御は直列化しています。

この版は**1プロセス・1台・1チューナー**の構成です。同じプロセスのBonDriverとWinSCardは共有できますが、別プロセス間のカード共有ホストにはまだ対応していません。TVTestとEDCBから同時に同じ機器を開く用途は今後の課題です。USB切断後の継続運転も未検証のため、再接続後はアプリを再起動して評価してください。

TSのスクランブル解除はアプリ側のCAS/B25処理が担当します。ドライバーのTS出力は放送TSそのものです。カードや受信電界に起因するエラーを含む安定性の評価には、受信と復号を組み合わせた長時間試験が必要です。

## 診断

```powershell
.\dist\x64\cechctl.exe --list
.\dist\x64\cechctl.exe --channels 20 --duration 30s
.\dist\x64\cechctl.exe --channels 18,20,23 --repeat 5 --duration 2s
.\dist\x64\cechctl.exe --channels 20 --duration 60s --out sample.ts
```

`--out` は既存ファイルを上書きしません。チャンネルは設置場所で受信できる物理チャンネルへ変更します。出力は1試行につき1行のJSONです。無信号、TS欠落、TEI、同期喪失、PATのCRC異常、TSIDの混在を検出すると終了コード1を返します。初期同期までに捨てた端数は `Framing.DiscardedBytes`、受信中の同期喪失は `Framing.SyncLosses` に分けています。

ホストの選局結果はDLLと同じフォルダーの `cech-host-*.log` に記録されます。カード番号、APDU、カードの応答データはログへ出力しない構成です。DLL側の接続エラーはWindowsのデバッグ出力に記録されます。

## ビルド・試験

Go 1.25以降、Visual StudioのC++ビルドツール、Windows SDKを使用します。

```powershell
.\build.ps1 -Architecture x64
.\build.ps1 -Architecture x86
go test ./...
go vet ./...
.\package.ps1
```

実機試験ツールは以下です。別の録画や視聴が機器を使用していない状態で実行します。

```powershell
.\dist\x64\bon_smoke.exe "$PWD\dist\x64\BonDriver_CECH.dll" 30 20
.\dist\x64\card_smoke.exe "$PWD\dist\x64" 30
```

`bon_smoke` は公開BonDriver API、TS、Purge、再オープンを確認します。追加の引数を指定すると、開発環境で停波中だった13chと受信可能だった18chを使う復帰試験も実行します。この追加試験は同じ受信条件がある環境に限って利用できます。

`card_smoke` は20chを受信しながら、内蔵B-CASへ読み取り用の初期設定条件コマンドを反復送信します。共有・排他接続、Purge後の通信、チューナーを閉じた後のカード通信も検査します。カード内の契約更新コマンドは試験に含めていません。

## 移植とライセンス

Goの受信コアは `receiver/`、チップ制御は `internal/coaxd/`、USBのOS依存部分は `pkg/usbio/` にあります。Windows固有のBonDriverとWinSCardは `bon/` と `winscard/` に限定しています。Linux向けの受信CLI・ホストはクロスビルドできます。Linux/macOSでの実機動作は未確認です。

coaxおよびLinuxのDiBcomドライバーを基にしたコードと、px4_drv由来のWinSCard処理を含むため、配布全体のライセンスは **GPL-2.0-only** です。出典と変更点は [設計・出典](docs/design.md)、ライセンス本文は [COPYING](COPYING)、coaxの原NOTICEは [NOTICE](NOTICE) にあります。
