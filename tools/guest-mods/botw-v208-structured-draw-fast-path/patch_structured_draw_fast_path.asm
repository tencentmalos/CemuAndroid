[BotW_JP_v208_StructuredDrawFastPath]
moduleMatches = 0x6267BFD0

.origin = codecave
EnableStructuredDrawFastPath:
stwu r1, -0x20(r1)
mflr r0
stw r0, 0x24(r1)
li r3, 1
bla import.gx2.hook_EnableStructuredDrawFastPath
lwz r0, 0x24(r1)
mtlr r0
addi r1, r1, 0x20
blr

.callback entry EnableStructuredDrawFastPath
