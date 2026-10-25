#include <pspkernel.h>
#include <pspintrman.h>
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "klog.h"

#define LOG_PATH "ms0:/seplugins/videobg.log"
#define BUF_SIZE (128 * 1024)

static char buf[BUF_SIZE];
static int head;
static volatile int enabled;

void klog_enable(int on) { enabled = on; }
int klog_enabled(void) { return enabled; }

void klog(const char *fmt, ...)
{
    if (!enabled)
        return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';

    int intr = sceKernelCpuSuspendIntr();
    if (head + n <= BUF_SIZE) {
        memcpy(buf + head, line, n);
        head += n;
    }
    sceKernelCpuResumeIntr(intr);
}

void klog_flush(void)
{
    int n = head;
    if (!enabled || n == 0)
        return;
    SceUID fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0)
        return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);

    /* lines logged during the write sit after n: shift them down atomically */
    int intr = sceKernelCpuSuspendIntr();
    if (head > n)
        memmove(buf, buf + n, head - n);
    head -= n;
    sceKernelCpuResumeIntr(intr);
}
