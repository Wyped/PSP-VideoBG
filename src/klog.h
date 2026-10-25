#ifndef KLOG_H
#define KLOG_H

#define BUILD_NUM 22
#define VERSION_STR "1.0.0"

/* Buffered log appended to ms0:/seplugins/videobg.log. Nothing is buffered or
   written until klog_enable(1): the plugin enables it only when the marker file
   ms0:/seplugins/videobg.debug exists, so a normal install never touches the
   card for logging. klog_flush() opens, appends and closes, so what was flushed
   survives a hard freeze. Safe to call from hooks. */
void klog_enable(int on);
int klog_enabled(void);
void klog(const char *fmt, ...);
void klog_flush(void);

#endif
