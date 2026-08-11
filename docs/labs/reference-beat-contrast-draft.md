# 対照分析の機械ドラフト

生成: 2026-08-11T14:31:08.383094+00:00

これは機械の中間出力。耳確認を通るまで「境界」とは書かない。

## view別の分離度

7つ検定しているのでBenjamini–Hochberg補正後の判定を採る。

| view | n(+/-) | AUC | p | BH | 分離 |
|---|---|---:|---:|---|---|
| topline_harmony | 22/10 | 0.565 | 0.2393 | 落ち | 分離しない |
| drum_placement | 22/10 | 0.588 | 0.1774 | 落ち | 分離しない |
| drum_audio | 23/10 | 0.624 | 0.0233 | 落ち | 分離しない |
| bass_harmony | 22/9 | 0.706 | 0.0012 | 通過 | 分離 |
| arrangement | 22/10 | 0.565 | 0.2893 | 落ち | 分離しない |
| vocal_space | 22/10 | 0.496 | 0.9349 | 落ち | 分離しない |
| overall | 22/9 | 0.652 | 0.0205 | 落ち | 分離しない |

## 近接性（否定例が正例からどれだけ離れているか）

正例内の最近傍距離: median 0.187 / 遠い判定閾値 0.249

| 曲 | 最近傍の正例 | 距離 | 遠い候補 |
|---|---|---:|---|
| noizy - "TWISTED" (21 Savage x JID / Chill Rap type beat) | TORAUMA × 水9％ - salt water taffy | 0.235 |  |
| Omamurin - "loved" (emo x sad x chill x guitar type beat) | RIP SLYME - One | 0.221 |  |
| Prod. Santu - "Chillhood Love" (R&B type beat) | 空音 × kojikoji - Hug | 0.218 |  |
| R&B Beats Daily - "Love" (R&B type beat) | STUTS × SIKK-O × 鈴木真海子 - 0℃の日曜 | 0.218 |  |
| King MasterMind - "Missing You" (Mellow Chill type beat) | 空音 × kojikoji - Hug | 0.216 |  |
| SHAYZEX BEATS - "Breath" (Chill Freestyle type beat) | 15MUS - 本当のこと | 0.202 |  |
| ocean - "REHAB" (Larry June / Chill Rap type beat) | 15MUS - 本当のこと | 0.201 |  |
| noizy - "OCEAN VIEWS" (Isaiah Rashad x Aaron May type beat) | Mom - タクシードライバー | 0.192 |  |
| SAINTHERAPY - "Chemistry" (Sainte x Jazz / Chill UK Rap type beat) | TORAUMA × 水9％ - salt water taffy | 0.185 |  |

## 採用された軸（feature群・距離で判定）

採用ゼロ。この粒度では境界が立たなかった。

## 採用された軸（個別feature・目標レンジ）

| 軸 | 正例 p10–p90 | 否定例 p10–p90 | AUC | 重なり | confound |
|---|---|---|---:|---:|---|
| arrangement.occupancy.drums | 0.721–0.997 | 0.540–0.889 | 0.864 | 0.427 | - |
| bass_harmony.density.onset_rate | 1.640–4.355 | 1.110–2.378 | 0.854 | 0.449 | - |
| drum_audio.production.hpss_ratio | 0.115–1.037 | 0.015–0.258 | 0.826 | 0.565 | selection_biased |

## 分離しなかった軸（＝好みの境界ではない／自由に振ってよい）

arrangement.occupancy.bass, arrangement.occupancy.guitar, arrangement.shape.reentry_count, vocal_space.time_space.gap_ratio, bass_harmony.pitch_relation.root_ratio, drum_placement.drum_shape.hat_per_second, vocal_space.band_occupancy.band_flatness, drum_audio.production.centroid_hz, drum_placement.drum_shape.snare_backbeat, topline_harmony.harmony.confidence, bass_harmony.motion.repetition, drum_placement.timing.swing_ratio, bass_harmony.density.rms_db, arrangement.shape.mean_length_bars, drum_audio.production.rolloff95_hz

## confoundで採用しなかった軸

| 軸 | AUC | 理由 |
|---|---:|---|
| drum_placement.drum_shape.snare_spread | 0.759 | bleed_sensitive |
| drum_audio.hit_character | 0.745 | bleed_sensitive |
| drum_placement.drum_shape.snare_per_second | 0.686 | bleed_sensitive |
| drum_placement.timing.onset_rate | 0.636 | bleed_sensitive |
| bass_harmony.placement | 0.631 | bleed_sensitive |
| drum_placement.timing | 0.603 | bleed_sensitive |
| vocal_space.time_space | 0.580 | bleed_sensitive |
| vocal_space.time_space.gap_ratio | 0.573 | bleed_sensitive |
| drum_placement.profiles | 0.570 | bleed_sensitive |
| bass_harmony.density | 0.556 | loudness_sensitive |
| topline_harmony.texture | 0.478 | bleed_sensitive |
| bass_harmony.density.rms_db | 0.480 | loudness_sensitive |
| drum_audio.production | 0.502 | bleed_sensitive |
| drum_audio.production.rolloff95_hz | 0.500 | bleed_sensitive |

## 注記として持ち回るconfound

selection_biased

採用は止めないが、レポートでは必ず併記する。

