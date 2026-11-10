/* Optional, bounded boot-wait snapshots. Start the sample budget only after
 * SpringBoard exists, so signature preparation cannot consume it first.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern task_t proc_task(proc_t);
extern int hz;
/* This declaration is hidden from the BSD layer by MACH_KERNEL_PRIVATE. */
extern void thread_terminate_self(void);
extern void gm9_sample_task(const char *, int, task_t);
extern void gm9_framebuffer_sample(void);
void gm9_sample_start(void);

static int
gm9_sample_proc(proc_t p, void *arg)
{
    char name[64] = {0};
    proc_name(proc_pid(p), name, sizeof(name));
    bool springboard = !strcmp(name, "SpringBoard");
    if (!springboard && strcmp(name, "backboardd") && strcmp(name, "PreBoard") &&
        strcmp(name, "PosterBoard") && strcmp(name, "chronod") &&
        strncmp(name, "com.apple.datam", 15) && strncmp(name, "com.apple.migra", 15))
        return PROC_RETURNED;
    if (springboard) *(bool *)arg = true;
    task_t task = proc_task(p);
    if (task != TASK_NULL) {
        task_reference(task);
        gm9_sample_task(name, proc_pid(p), task);
        task_deallocate(task);
    }
    return PROC_RETURNED;
}

static void
gm9_sample_thread(__unused void *arg, __unused wait_result_t result)
{
    static int channel;
    unsigned samples = 0;
    /* One hour maximum including the initial shared-cache scan. */
    for (unsigned poll = 0; poll < 240 && samples < 16; poll++) {
        (void)tsleep(&channel, PZERO, "gm9wait", 15 * hz);
        bool found = false;
        printf("GM9 WAIT snapshot=%u poll=%u\n", samples + 1, poll);
        gm9_framebuffer_sample();
        proc_iterate(PROC_ALLPROCLIST, gm9_sample_proc, &found, NULL, NULL);
        if (found) {
            samples++;
            (void)tsleep(&channel, PZERO, "gm9wait", 75 * hz);
            poll += 5;
        }
    }
    printf("GM9 WAIT sampling complete\n");
    thread_terminate_self();
}

void
gm9_sample_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("gm9sample", &enabled, sizeof(enabled)) || enabled != 1) return;
    thread_t thread;
    if (kernel_thread_start(gm9_sample_thread, NULL, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}
