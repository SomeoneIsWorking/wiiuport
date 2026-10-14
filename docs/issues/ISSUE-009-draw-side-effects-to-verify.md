# ISSUE-009 — Draw-time side effects the in-between draw may repeat

**State:** open, none reproduced yet. **Affects:** ST-OBJECTS.

The skipped call's draw phase runs every draw method a second time per tick. ZeldaWWHDRecomp
(`docs/prior-art.md`) had to handle these in the same situation; each needs a reproduction here before
a change:

| Candidate | Where | Their symptom |
|---|---|---|
| One-shot `NODRAW` request | actor `+0x2e0` bit `0x01000000`, consumed by `fopAc_Draw 0x025d4654` | Link flickers in the talk camera (their #126) |
| Random numbers drawn in a draw | `cM_rnd 0x02019788`, `cM_rndF 0x020198d8`, `cM_rndFX 0x02019918` | flicker of random-target effects (raft light `0x02363374`) |
| Sound starts in a draw | `0x0201eba0`, `0x025e1988`..`0x025e1aa4` | doubled sounds |
| HD light-count clear | `0x0273841c` | point lights doubled |
| Draw methods with state | `d_a_bgn`, `d_a_ff`, `d_a_himo2` (rope settles per computation), `d_a_obj_ajav` (flash counted in `0x02313e28`, timer `+0x56`) | double-speed animation |
| Material animation outside an actor's draw | key animator `0x027df40c` | stepping at 30 |
| Cloth | KoRL sail upload `0x021625dc`, `dCloth 0x0251d864` | stepping at 30 |
