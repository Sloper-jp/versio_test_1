# versio_test_1 — Versio Reverse

Noise Engineering Versio 用カスタムファームウエア。クロックに同期してピッチ変化なしで逆再生し、逆再生されたトランジェントをビートに合わせます。

仕様: [docs/design.md](docs/design.md)

## 操作

| コントロール | 機能 |
|---|---|
| KNOB_0 ＋ CV | 50% 以上で REVERSE（Wet 100%）、未満で DRY（原音） |
| KNOB_1 ＋ CV | トランジェント位置のオフセット −350〜+350ms（中央 0） |
| FSU ゲート入力 | クロック（2PPQN、BPM 50 以上） |

| LED | 表示 |
|---|---|
| LED0 | REVERSE 時に赤 |
| LED1 | 区間開始で点滅（実クロック=白、内部クロック=黄） |
| LED2 | オフセット（負=青、正=緑、明るさ=量） |
| LED3 | 実クロック未受信の間、暗いオレンジ |

## ビルド

```bash
git clone --recursive https://github.com/Sloper-jp/versio_test_1.git
cd versio_test_1
make -C lib/libDaisy   # 初回のみ
make                   # build/versio_reverse.bin
make test              # PC 上の単体テスト
```

`arm-none-eabi-gcc` が必要です。GitHub Actions でも同じビルドを行い、`.bin` を Artifact として保存します。

## 書き込み

1. Versio 背面の Daisy Seed を USB で PC に接続
2. BOOT ボタンを押したまま RESET を押して DFU モードにする
3. [Daisy Web Programmer](https://electro-smith.github.io/Programmer/) で `versio_reverse.bin` を書き込む（または `make program-dfu`）

公式ファームウエアへは Noise Engineering のアップデーターで戻せます。

## 実機で確認が必要な点

- `KNOB_0` / `KNOB_1` のパネル上の位置
- ゲート入力の極性（クロックに合わせて LED1 が白く点滅しなければ `src/config.h` の `kGateInvert` を `true` に）
