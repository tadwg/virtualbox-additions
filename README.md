# VirtualBox Guest Additions Mouse Driver for BTRON3 (超漢字V)

VirtualBox 上で動作する超漢字V 向けのマウスドライバです。  
VirtualBox の VMMDev PCI デバイス経由でホストのマウス絶対座標を取得し、  
BTRON3 の kbpd ドライバにポインタイベントとして渡します。

## 機能

- VMMDev PCI デバイス (Vendor: 0x80EE, Device: 0xCAFE) を使用
- `GetMouseStatusEx` (requestType=223) による絶対座標取得
- `NEW_PROTOCOL` フラグ (`VMMDEV_MOUSE_NEW_PROTOCOL`) 対応
- 左ボタン / 右ボタン / ホイール回転 / ホイールプッシュに対応
- ポーリングモード動作（10ms 間隔）
- 割り込みモードのコードも含む（現在はポーリングモード強制中）

## ファイル構成

```
src/
  vboxmousegpl.c    ドライバ本体
  vboxmousegpl.h    インクルードヘッダ
  Makefile          ビルド用 Makefile
bin/
  vboxmousegpl      リリースビルド（超漢字V に転送するバイナリ）
  vboxmousegpl.debug  デバッグビルド（DEBUG_PRINT 有効）
```

## ビルド方法

超漢字クロス開発環境 (brightv) が必要です。

```bash
# リリースビルド
cd pcat/
make BD=$BD GNUs=$GNUs GNU_BD=$GNU_BD GNUi386=$GNUi386

# デバッグビルド
cd pcat.debug/
make BD=$BD GNUs=$GNUs GNU_BD=$GNU_BD GNUi386=$GNUi386
```

## 導入方法

1. VirtualBox のマシン設定で「ポインティングデバイス」を **「USBタブレット」** に変更
2. `vboxmousegpl` を超漢字V の適切なディレクトリに配置
3. `STARTUP.CMD` に追加組み込みドライバとして登録

```
kerext  vboxmousegpl    !23
```

## エラーコード

起動失敗時は `ER_NOSPT (0xFFFD0000)` の下位16bit にエラー箇所が入ります。

| 返値 | 意味 |
|------|------|
| `0xFFFD0001` | kbpd オープン失敗 |
| `0xFFFD0002` | kbpd メールボックス ID 取得失敗 |
| `0xFFFD0003` | VMMDev PCI デバイスが見つからない |
| `0xFFFD0004` | BAR0 (I/O ポート) 取得失敗 |
| `0xFFFD0005` | GetHostVersion: request 失敗 |
| `0xFFFD0006` | GetHostVersion: VBox 側エラー |
| `0xFFFD0007` | SetMouseStatus: request 失敗 |
| `0xFFFD0008` | SetMouseStatus: VBox 側エラー |
| `0xFFFD0009` | defIntHdr 失敗 |
| `0xFFFD000A` | vcre_tsk 失敗 |
| `0xFFFD000B` | sta_tsk 失敗 |
| `0xFFFD000C` | ReportGuestInfo: request 失敗 |
| `0xFFFD000D` | ReportGuestInfo: VBox 側エラー |

## 参考資料

- [超漢字 PCI デバイス用デバイスドライバ説明書](http://www.chokanji.com/developer/info/pcidrv.html)
- [favo430 (Wacom FAVO USB タブレットドライバ for BTRON3, GPL)](https://yashiromann.sakura.ne.jp/prog/favo430/favo430.tar.gz)
- [VirtualBox OSE VMMDev.h](https://www.virtualbox.org/svn/vbox/trunk/include/VBox/VMMDev.h)

## ライセンス

GNU General Public License v2 or later

Copyright (C) 2024
