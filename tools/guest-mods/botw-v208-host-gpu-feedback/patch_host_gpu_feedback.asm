[BotW_JP_v208_HostGpuFeedback]
moduleMatches = 0x6267BFD0

.origin = codecave
RegisterGuestFeedbackPolicy:
stwu r1, -0x20(r1)
mflr r0
stw r0, 0x24(r1)
lis r3, 0x031F
ori r3, r3, 0xAA14
li r4, 1
li r5, 1
bla import.gx2.hook_RegisterGuestFeedbackPolicy
lwz r0, 0x24(r1)
mtlr r0
addi r1, r1, 0x20
blr

.callback entry RegisterGuestFeedbackPolicy
