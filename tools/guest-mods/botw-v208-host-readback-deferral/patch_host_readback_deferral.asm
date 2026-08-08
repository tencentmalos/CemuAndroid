[BotW_JP_v208_HostReadbackDeferral]
moduleMatches = 0x6267BFD0

.origin = codecave
RegisterDrawDoneVisibilityDeferral:
stwu r1, -0x20(r1)
mflr r0
stw r0, 0x24(r1)
lis r3, 0x031F
ori r3, r3, 0xAA14
bla import.gx2.hook_RegisterDrawDoneVisibilityDeferral
lwz r0, 0x24(r1)
mtlr r0
addi r1, r1, 0x20
blr

.callback entry RegisterDrawDoneVisibilityDeferral
