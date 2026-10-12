# Issue #40 X3バッテリー診断

`py -3 test/run_battery_diagnostics_test.py --compiler <C++ compiler> --zig`
（Zig以外では `--zig` を省略。）本番のHalPowerManager.cppをI2C/時計/ADCスタブで実行する。
C3 release、C3 debug、S3 releaseの3構成でpoll抑制、84→7、15ポイント境界、
電流の符号、SOC送信失敗/短い応答、診断各項目の失敗、最後の正常値保持、
起動時/0%/100%超の既存表示処理、X4/S3のADC分離を確認する。

## 実機ログ

通常のdefault/gh_releaseは、直前に成功した生SOCとの差が15ポイント以上のときだけ
追加の4コマンドを読み、INFOのBAT/SOC_JUMPを出す。起動時の最初の値は急変とみなさない。
debug (`LOG_LEVEL=2`) では成功した各pollでBAT/SAMPLEも出す。
`X3_BATTERY_DIAGNOSTICS=1` を明示しても有効（INFOログも有効にする）。
pollは既存の呼び出しに従い最短1500ms間隔。独立した常時サンプリングではない。

例（架空値）:

```text
[INF] BAT: X3 SOC_JUMP poll_ms=120000 old_soc=84 soc=7 voltage_mV=3450 current_mA=-120 remaining_mAh=21 full_mAh=300 valid=0xF read_ms=2
```

`old_soc` / `soc` は補正前の16bit値。UIの100%上限処理は変更していない。
`valid` はbit0=電圧、bit1=電流、bit2=残容量、bit3=満充電容量。
失敗した値は電流=-32769、他=-1。無効値を実測値として扱わない。
初回SAMPLEのold_soc=-1は過去の成功値なし。SOC失敗はdebugでSOC_READ_FAILEDを記録し、
表示と比較元を最後の正常値に保つ。追加測定だけ失敗した場合は正常なSOCを表示する。

SOC(0x2C)、Voltage(0x08)、Current(0x0C)、RemainingCapacity(0x10)、
FullChargeCapacity(0x12)を読む。Currentはsigned mA、容量はmAh。
出典: [TI BQ27220 TRM SLUUBD4A, chapter 2](https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf)。
同じpollで順次読むため、厳密な同時測定ではない。read_msはSOC取得からの所要時間。
ゲージ更新間隔より短い瞬間的な電圧低下は捉えられない。

X3で充電/USB接続状況、使用フォント、読書/ページ更新/復帰の操作とともに
急落前後のBATログを保存する。必要ならdebugで前後のSAMPLEも記録する。
EDV到達やセル設定不一致はまだ原因候補であり、このログだけで確定しない。
ゲージ設定変更、残量の平滑化・補正は行わない。
# Issue #40: read-only gauge-state diagnostics (2026-10-02)

The manual diagnostic report reads the X3's BQ27220 standard registers:
SOC 0x2C, voltage 0x08, current 0x0C, remaining capacity 0x10,
full-charge capacity 0x12, design capacity 0x3C, BatteryStatus 0x0A,
and OperationStatus 0x3A. Words are little endian; current is signed mA.
No Control subcommands, unseal keys, Data Memory writes, or resets are sent.
The UI battery cache is unchanged by manual collection.

`battery_gauge_valid` bits 0..7 follow the register order above. Failed fields
are printed as `unknown`, not zero. OperationStatus provides security bits 2:1
(3 sealed, 2 unsealed, 1 full access), CFGUPDATE bit 10, INITCOMP bit 5,
and EDV2 bit 3. Collection is sequential, not an atomic gauge snapshot.
The caller holds RenderLock for collection to avoid concurrent UI gauge reads.
X4 ADC and S3 paths report `battery_gauge_available=false` without X3 I2C reads.

Jump/debug sampling retains the old `SOC_JUMP`/`SAMPLE` fields and adds a separate
`GAUGE_STATE` line with the same `poll_ms`. Its `valid` mask is independent:
bit 0 design capacity, bit 1 BatteryStatus, bit 2 OperationStatus. A failed
status read leaves raw zero with the corresponding validity bit clear.
Separate lines avoid truncating the 256-byte log entries.

Host checks cover each register's failed transaction and short read, unchanged
gauge contents and cached SOC, and the X4/S3 non-I2C paths. The gauge stub only
accepts one-byte register selections, so parameter writes are not supported.
TI reference: https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf, sections 2.7,
2.21, 2.27, and 2.28. These are diagnostics, not a capacity correction.

## v0.8.3 candidate: X3 EDV2 observation

At each existing X3 SOC poll, one extra standard OperationStatus word is read.
`GAUGE_OBS` is emitted on the first observation, an observed EDV2 or other
watched-status change, a read-availability change, or a SOC jump. The regular
release build does not log every unchanged poll. `poll_ms` and `op_ms` mark the
sequential reads; `prev_ms` is the last valid OperationStatus observation.
`edv2=-1` means unavailable, not clear. `missed` counts intervening failed
status reads. `SOC_JUMP` retains the raw SOC and four measurement validity bits;
`GAUGE_STATE` reports unavailable status words as `unknown`.

The 2026-10-12 X3 field check saved a report during ordinary reading: gauge
valid `0xFF`, Design/FullChargeCapacity 650 mAh, SOC 98%, and EDV2 clear.
The recent log showed OperationStatus `0x00A6` to `0x00B6` (VDQ changed),
`edv2=0->0`, and SOC `98->98`. The user also confirmed normal reading,
sleep/wake, Web UI entry/exit, and diagnostic access on X3 and X4. This
confirms the instrumented path works; it does not reproduce the earlier
30%->7% event or establish why EDV2 was set then. The diagnostic report is a
manual snapshot and the polling cadence cannot identify the exact edge time
between samples. No forced discharge is required to collect a future event.

## v0.8.1 X3 capacity correction

`run_battery_diagnostics_test.py` also runs the isolated production loader against
an upstream-derived BQ27220 model. It covers 27 individual refused transactions,
100 interrupted/restarted loader timelines, sustained bus loss and recovery,
already-correct/aged capacities, unknown capacity preservation, and initially
unsealed gauges. The HAL tests assert no loader I2C on C3 X4 / S3.

The runtime loader is X3-only and targets 650 mAh. It corrects DesignCapacity
3000, or DesignCapacity 650 with FullChargeCapacity > 812. Learned FCC above
812 is reset; plausible aged values are retained. Capacity writes use the TI
MAC checksum sequence. Closure verifies CFGUPDATE off and SEC=3; failed closure
keeps retrying slowly and postpones normal sleep, without blocking UI. Once
closed, failed correction retries twice, then logs failure for diagnosis.
Report fields `battery_capacity_*` describe loader state: step 0 check, 1 keys,
2 entry wait, 3 closure, 4 done. `changed` means an update was attempted, while
`verified` requires final capacity readback; `had_error` remains true after a
recovered failure. These are host-model checks, not physical power-cut tests.

Device checklist: X3 boot, wait 15 seconds, save diagnostics; expect design/full
650, CFGUPDATE=false, security=3, pending=false, verified=true. Reboot and confirm
same values with changed=false. Test sleep/wake, reading, and Web transfer, and
repeat diagnostics. X4 must boot/read/sleep normally and report gauge unavailable.
Do not cut power deliberately during the first physical correction test.
