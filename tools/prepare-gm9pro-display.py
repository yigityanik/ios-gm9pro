#!/usr/bin/env python3
"""Add a guarded GM9 Pro MDP5 page-flip path to the SWEET framebuffer."""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1])

display = r"""#include <kern/clock.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <stdint.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define GM9_MDP_PP0_DONE (1U << 8)
#define GM9_MDP_WAIT_MS 50

static LCK_GRP_DECLARE(gm9_mdp_group, "gm9_mdp");
static LCK_MTX_DECLARE(gm9_mdp_lock, &gm9_mdp_group);
static volatile uint32_t *gm9_mdp_intr_status;
static volatile uint32_t *gm9_mdp_intr_clear;
static volatile uint32_t *gm9_mdp_ctl_start;
static bool gm9_mdp_ready;
static bool gm9_mdp_pending = true; /* ABL may have left a frame in flight. */
static bool gm9_mdp_timeout_logged;

void
gm9pro_display_init(uint64_t fb, uint32_t width, uint32_t height, uint32_t stride)
{
    if (fb != 0x9d400000ULL || width != 1080 || height != 2160 || stride != 4320) {
        printf("GM9 MDP: unexpected boot framebuffer %llx %ux%u stride %u; scanout disabled\n",
            (unsigned long long)fb, width, height, stride);
        return;
    }
    vm_offset_t intr = ml_io_map(0x0c901000, 0x1000);
    vm_offset_t ctl = ml_io_map(0x0c902000, 0x1000);
    vm_offset_t vig = ml_io_map(0x0c905000, 0x1000);
    if (intr == 0 || ctl == 0 || vig == 0) {
        printf("GM9 MDP: register mapping failed; scanout disabled\n");
        return;
    }
    volatile uint32_t *pipe = (volatile uint32_t *)vig;
    if (pipe[0] != 0x08700438U || pipe[0x14 / 4] != (uint32_t)fb ||
        pipe[0x24 / 4] != stride || pipe[0x30 / 4] != 0x000237ffU ||
        pipe[0x34 / 4] != 0x03020001U) {
        printf("GM9 MDP: ABL VIG0 handoff changed; scanout disabled\n");
        return;
    }
    gm9_mdp_intr_status = (volatile uint32_t *)(intr + 0x14);
    gm9_mdp_intr_clear = (volatile uint32_t *)(intr + 0x18);
    gm9_mdp_ctl_start = (volatile uint32_t *)(ctl + 0x1c);
    gm9_mdp_ready = true;
    printf("GM9 MDP: ABL VIG0 verified; command-mode page flips enabled\n");
}

/* Lock out concurrent swaps and wait before touching memory still scanned by MDP. */
bool
gm9pro_display_begin_frame(void)
{
    if (!gm9_mdp_ready) {
        return true; /* Preserve the software framebuffer path if handoff is absent. */
    }
    lck_mtx_lock(&gm9_mdp_lock);
    if (gm9_mdp_pending) {
        uint64_t deadline;
        clock_interval_to_deadline(GM9_MDP_WAIT_MS, NSEC_PER_MSEC, &deadline);
        while (!(*gm9_mdp_intr_status & GM9_MDP_PP0_DONE) &&
            mach_absolute_time() < deadline) {
            delay(100);
        }
        if (!(*gm9_mdp_intr_status & GM9_MDP_PP0_DONE)) {
            if (!gm9_mdp_timeout_logged) {
                gm9_mdp_timeout_logged = true;
                printf("GM9 MDP: previous frame timed out; dropping framebuffer swap\n");
            }
            lck_mtx_unlock(&gm9_mdp_lock);
            return false;
        }
        gm9_mdp_pending = false;
    }
    return true;
}

/* Called only after the framebuffer copy is complete. */
void
gm9pro_display_end_frame(void)
{
    if (gm9_mdp_ready) {
        *gm9_mdp_intr_clear = GM9_MDP_PP0_DONE;
        __asm__ volatile("dsb sy" ::: "memory");
        *gm9_mdp_ctl_start = 1;
        __asm__ volatile("dsb sy" ::: "memory");
        gm9_mdp_pending = true;
        lck_mtx_unlock(&gm9_mdp_lock);
    }
}
"""

display_path = source / "bsd/dev/gm9pro_display.c"
display_path.parent.mkdir(parents=True, exist_ok=True)
display_path.write_text(display)
files = source / "bsd/conf/files"
files_text = files.read_text()
entry = "bsd/dev/gm9pro_display.c\t\tstandard"
if entry not in files_text:
    files.write_text(files_text.rstrip() + "\n" + entry + "\n")

framebuffer = source / "iokit/Kernel/arm/SweetFramebuffer.cpp"
s = framebuffer.read_text()
field_marker = "\tuint32_t      fWidth, fHeight, fRowBytes;\n"
if s.count(field_marker) != 1:
    raise RuntimeError("AppleCLCD geometry fields changed")
s = s.replace(field_marker, field_marker +
              "\tuint32_t      fLogicalW, fLogicalH;\n"
              "\tint32_t       fOffX, fOffY;\n")
class_marker = "OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);"
if s.count(class_marker) != 1:
    raise RuntimeError("AppleCLCD definition changed")
s = s.replace(class_marker, class_marker + "\nextern \"C\" void gm9pro_display_init(uint64_t, uint32_t, uint32_t, uint32_t);"
              "\nextern \"C\" bool gm9pro_display_begin_frame(void);"
              "\nextern \"C\" void gm9pro_display_end_frame(void);")

init_marker = "\tfRowBytes = (uint32_t)video.v_rowBytes;\n"
if s.count(init_marker) != 1:
    raise RuntimeError("AppleCLCD boot framebuffer setup changed")
s = s.replace(init_marker, init_marker +
    "\tgm9pro_display_init(fPhys, fWidth, fHeight, fRowBytes);\n"
    "\tfLogicalW = fWidth; fLogicalH = fHeight;\n"
    "\tif (fWidth == 1080 && fHeight == 2160) {\n"
    "\t\tfLogicalW = 1125; fLogicalH = 2436;\n"
    "\t}\n"
    "\tfOffX = (int32_t)(fLogicalW - fWidth) / 2;\n"
    "\tfOffY = (int32_t)(fLogicalH - fHeight) / 2;\n")

size_marker = "\tcase kSweetFBGetDisplaySize:\n\t\tout[0] = fFB->fWidth;\n\t\tout[1] = fFB->fHeight;"
if s.count(size_marker) != 1:
    raise RuntimeError("AppleCLCD display size selector changed")
s = s.replace(size_marker, "\tcase kSweetFBGetDisplaySize:\n\t\tout[0] = fFB->fLogicalW;\n\t\tout[1] = fFB->fLogicalH;")

method = "AppleCLCD::swap(const uint8_t *rec)"
match = re.search(re.escape(method) + r"\s*\{", s)
if not match:
    raise RuntimeError("AppleCLCD::swap missing")
body_start = match.end()
depth = 1
pos = body_start
while depth:
    depth += (s[pos] == "{") - (s[pos] == "}")
    pos += 1
body_end = pos - 1
body = s[body_start:body_end]
mask_line = "\tuint32_t mask = get32(rec, 0x104);\n"
if body.count(mask_line) != 1:
    raise RuntimeError("AppleCLCD::swap layer mask changed")
body = body.replace(mask_line, mask_line +
    "\tbool mdp_frame = false;\n"
    "\tif (mask) {\n"
    "\t\tmdp_frame = gm9pro_display_begin_frame();\n"
    "\t\tif (!mdp_frame) {\n"
    "\t\t\treturn;\n"
    "\t\t}\n"
    "\t}\n")
crop_marker = "\t\tif (w > (int32_t)s->width - sx) {\n"
if body.count(crop_marker) != 1:
    raise RuntimeError("AppleCLCD swap bounds changed")
body = body.replace(crop_marker,
    "\t\t/* Center the logical iPhone X surface on the physical panel. */\n"
    "\t\tdx -= fOffX; dy -= fOffY;\n"
    "\t\tif (dx < 0) { sx -= dx; w += dx; dx = 0; }\n"
    "\t\tif (dy < 0) { sy -= dy; h += dy; dy = 0; }\n" + crop_marker)
if not body.endswith("\t}\n"):
    raise RuntimeError("AppleCLCD::swap end changed")
body += "\tif (mdp_frame) {\n\t\tgm9pro_display_end_frame();\n\t}\n"
s = s[:body_start] + body + s[body_end:]
framebuffer.write_text(s)
print("GM9 Pro: guarded MDP page flip follows completed framebuffer copy")
