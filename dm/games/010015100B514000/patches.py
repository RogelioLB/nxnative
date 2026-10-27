# Super Mario Bros. Wonder (010015100B514000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B


# Render scale (state +0x10, float written by the loader: DM_RENDER_SCALE, 1.0 by default) of 1.2.1. main+0x4f9a0.. sizes a sead::FrameBuffer each frame: its physical area
# (+0x10..+0x1c) is the parent's area x the view's scale (+0x2110 / +0x2114 of its 0x21b0-byte state, the aspect set up at main+0x782040) and, for a view whose entry has
# bit 0 of +0x1c set (the 3D scene's, which the game's dynamic resolution drives: 960x540 and 959x540 of 1280x720 in handheld on an Adreno 619), x the entry's dynamic
# factor (+0x28 of a 0x588-byte entry, read at main+0x4fa80), clamped to 16384 and floored; the 1 / size pair the shaders use (+0x1b0) comes from the same result. The stub
# multiplies that dynamic factor by the host's, so only the views the game itself scales shrink, and everything the game derives from the area follows. x13 is dead from
# main+0x4fa7c to its reload at main+0x4fad4, and s31 is unused in the function. 1.0 multiplies by one (the game's size); a factor of 0 (no state written) skips it
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fs_site(0x4fa80, '862940bd')
    start = here()
    emit(0xbd402986)   # ldr s6,[x12,#0x28]     the view's dynamic factor (original)
    emit(ADRP(13, here(), FS_STATE_OFF))
    for w in (0xb94011ad,   # ldr w13,[x13,#0x10]    render scale (float bits)
              0x3400006d,   # cbz w13,back (+3)      not written
              0x1e2701bf,   # fmov s31,w13
              0x1e3f08c6):  # fmul s6,s6,s31
        emit(w)
    emit(B(here(), 0x4fa84))
    fs_put(0x4fa80, [B(0x4fa80, start)], '3D view dynamic-resolution factor x the host render scale (1.0 = the game\'s)'); ctx.render_scale = 1


FRAME_STEP = {'ff773e90972d544eb79406eaa65396d5': frame_step}
