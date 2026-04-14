#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/pid.h>

MODULE_LICENSE("GPL");

#define DEVICE_NAME "container_monitor"
static int major;
static int target_pid = 0;
static struct timer_list monitor_timer;

static void monitor_check(struct timer_list *t) {
    struct task_struct *task;
    struct pid *pid_struct;
    unsigned long rss = 0;

    if (target_pid > 0) {
        pid_struct = find_get_pid(target_pid);
        if (pid_struct) {
            task = get_pid_task(pid_struct, PIDTYPE_PID);
            if (task) {
                if (task->mm) {
                    // Calculate exact Physical RAM (RSS) in KB
                    rss = get_mm_rss(task->mm) * (PAGE_SIZE / 1024);
                    
                    // Diagnostic Heartbeat: Proves the kernel sees the memory
                    pr_info("[%s] Watching PID %d | Current RSS: %lu KB\n", DEVICE_NAME, target_pid, rss);

                    if (rss > 20480) { // 20MB Hard Limit
                        pr_alert("[%s] CRITICAL: PID %d RSS %lu KB > Hard Limit. Killing.\n", DEVICE_NAME, target_pid, rss);
                        send_sig(SIGKILL, task, 1);
                    } else if (rss > 10240) { // 10MB Soft Limit
                        pr_warn("[%s] WARNING: PID %d RSS %lu KB > Soft Limit.\n", DEVICE_NAME, target_pid, rss);
                    }
                }
                put_task_struct(task);
            }
            put_pid(pid_struct);
        }
    }
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000)); // Run every 1 second
}

static ssize_t dev_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[16];
    if (count > sizeof(kbuf) - 1) count = sizeof(kbuf) - 1;
    if (copy_from_user(kbuf, buf, count)) return -EFAULT;
    kbuf[count] = '\0';
    sscanf(kbuf, "%d", &target_pid);
    pr_info("[%s] Target PID locked to: %d\n", DEVICE_NAME, target_pid);
    return count;
}

static struct file_operations fops = { .write = dev_write };

static int __init monitor_init(void) {
    major = register_chrdev(0, DEVICE_NAME, &fops);
    timer_setup(&monitor_timer, monitor_check, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
    pr_info("[%s] Loaded successfully.\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void) {
    del_timer(&monitor_timer);
    unregister_chrdev(major, DEVICE_NAME);
}

module_init(monitor_init);
module_exit(monitor_exit);
