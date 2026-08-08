[BotW_JP_v208_HostDisplayDependency]
moduleMatches = 0x6267BFD0

0x1046D420 = BotWVsyncOrdinal:

.origin = codecave
RegisterHostDisplayDependency:
stwu r1, -0x20(r1)
mflr r0
stw r0, 0x24(r1)
lis r3, BotWVsyncOrdinal@ha
addi r3, r3, BotWVsyncOrdinal@l
bla import.gx2.hook_RegisterDisplayOrdinalCounter
lwz r0, 0x24(r1)
mtlr r0
addi r1, r1, 0x20
blr

.callback entry RegisterHostDisplayDependency
