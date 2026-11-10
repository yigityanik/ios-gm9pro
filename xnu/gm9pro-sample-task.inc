#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/spl.h>
#include <vm/vm_shared_region.h>
#include <arm/thread.h>

/* Capture only blocked user threads. Never fault in a user stack or print
 * while holding a task or scheduler lock. A later stack walk is observational:
 * the task may have resumed between the register snapshot and that read.
 */
void gm9_sample_task(const char *name, int pid, task_t task);
void
gm9_sample_task(const char *name, int pid, task_t task)
{
    struct sample { uint64_t pc, lr, sp, fp, event; unsigned state; } rows[24];
    unsigned count = 0, total = 0;
    thread_t thread;
    uint64_t slide = 0;
    vm_shared_region_t sr = vm_shared_region_get(task);
    if (sr != NULL) {
        slide = sr->sr_slide;
        vm_shared_region_deallocate(sr);
    }
    vm_map_t map = get_task_map(task);
    vm_map_reference(map);
    task_lock(task);
    uint64_t mh = task->mach_header_vm_address;
    queue_iterate(&task->threads, thread, thread_t, task_threads) {
        total++;
        spl_t level = splsched();
        thread_lock(thread);
        if (count < 24 && (thread->state & TH_WAIT) && !(thread->state & TH_RUN)) {
            arm_saved_state_t *ss = find_user_regs(thread);
            if (ss != NULL && is_saved_state64(ss)) {
                rows[count++] = (struct sample) {
                    get_saved_state_pc(ss), get_saved_state_lr(ss),
                    get_saved_state_sp(ss), get_saved_state_fp(ss),
                    thread->wait_event, thread->state
                };
            }
        }
        thread_unlock(thread);
        splx(level);
    }
    task_unlock(task);
    printf("GM9 WAIT %s[%d] threads=%u blocked_shown=%u slide=0x%llx mh=0x%llx\n",
        name, pid, total, count, slide, mh);
    for (unsigned i = 0; i < count; i++) {
        struct sample *r = &rows[i];
        char line[256];
        int n = snprintf(line, sizeof(line),
            "GM9 WAIT %s[%d] t%u state=%x event=%llx pc=%llx lr=%llx sp=%llx bt",
            name, pid, i, r->state, r->event, r->pc, r->lr, r->sp);
        uint64_t fp = r->fp;
        for (unsigned frame_index = 0; frame_index < 6 && fp && !(fp & 7) &&
            n < (int)sizeof(line) - 20; frame_index++) {
            uint64_t frame[2];
            if (vm_map_read_user(map, fp, frame, sizeof(frame)) != KERN_SUCCESS) break;
            n += snprintf(line + n, sizeof(line) - n, " %llx", frame[1]);
            if (frame[0] <= fp) break;
            fp = frame[0];
        }
        printf("%s\n", line);
    }
    vm_map_deallocate(map);
}
#endif
