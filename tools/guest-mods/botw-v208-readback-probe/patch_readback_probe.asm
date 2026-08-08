[BotW_JP_v208_ReadbackPurposeProbe]
moduleMatches = 0x6267BFD0

; BetterVR identifies this leaf predicate as the AutoExposure enable gate.
; The original instruction is `clrlwi r3, r11, 24`; force a false return.
0x039D99A4 = li r3, 0
