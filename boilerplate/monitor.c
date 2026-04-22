#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/pid.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");

#define DEVICE_NAME "container_monitor"
#define MAX_TARGETS 16
static int major;

struct target_info {
    pid_t pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
    int in_use;
};

static struct target_info targets[MAX_TARGETS];
static struct timer_list monitor_timer;
static DEFINE_SPINLOCK(targets_lock);

static void monitor_check(struct timer_list *t) {
    struct task_struct *task;
    struct pid *pid_struct;
    unsigned long rss = 0;
    int i;

    spin_lock(&targets_lock);
    for (i = 0; i < MAX_TARGETS; i++) {
        if (!targets[i].in_use || targets[i].pid <= 0) continue;

        pid_struct = find_get_pid(targets[i].pid);
        if (pid_struct) {
            task = get_pid_task(pid_struct, PIDTYPE_PID);
            if (task) {
                if (task->mm) {
                    rss = get_mm_rss(task->mm) * (PAGE_SIZE / 1024);
                    //pr_info("[%s] Watching PID %d | Current RSS: %lu KB\n", DEVICE_NAME, targets[i].pid, rss);

                    if (rss > targets[i].hard_limit) { 
                        pr_alert("[%s] CRITICAL: PID %d RSS %lu KB > Hard Limit. Killing.\n", DEVICE_NAME, targets[i].pid, rss);
                        send_sig(SIGKILL, task, 1);
                    } else if (rss > targets[i].soft_limit) { 
                        pr_warn("[%s] WARNING: PID %d RSS %lu KB > Soft Limit.\n", DEVICE_NAME, targets[i].pid, rss);
                    }
                }
                put_task_struct(task);
            }
            put_pid(pid_struct);
        }
    }
    spin_unlock(&targets_lock);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}

static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct monitor_request req;
    int i, slot = -1;

    if (copy_from_user(&req, (struct monitor_request *)arg, sizeof(req)))
        return -EFAULT;

    spin_lock(&targets_lock);
    switch (cmd) {
        case MONITOR_REGISTER:
            for (i = 0; i < MAX_TARGETS; i++) {
                if (!targets[i].in_use) { slot = i; break; }
            }
            if (slot >= 0) {
                targets[slot].pid = req.pid;
                targets[slot].soft_limit = req.soft_limit_bytes / 1024;
                targets[slot].hard_limit = req.hard_limit_bytes / 1024;
                targets[slot].in_use = 1;
                pr_info("[%s] Registered PID: %d, Soft: %lu KB, Hard: %lu KB\n", 
                        DEVICE_NAME, req.pid, targets[slot].soft_limit, targets[slot].hard_limit);
            }
            break;
        case MONITOR_UNREGISTER:
            for (i = 0; i < MAX_TARGETS; i++) {
                if (targets[i].in_use && targets[i].pid == req.pid) {
                    targets[i].in_use = 0;
                    pr_info("[%s] Unregistered PID: %d\n", DEVICE_NAME, req.pid);
                    break;
                }
            }
            break;
        default:
            spin_unlock(&targets_lock);
            return -EINVAL;
    }
    spin_unlock(&targets_lock);
    return 0;
}

static struct file_operations fops = {
    .unlocked_ioctl = dev_ioctl,
};

static struct class *monitor_class;
static struct device *monitor_device;

static int __init monitor_init(void) {
    int i;
    for (i = 0; i < MAX_TARGETS; i++) targets[i].in_use = 0;

    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) return major;

    // Automatically create the /dev/container_monitor file
    monitor_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(monitor_class)) {
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(monitor_class);
    }

    monitor_device = device_create(monitor_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(monitor_device)) {
        class_destroy(monitor_class);
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(monitor_device);
    }

    timer_setup(&monitor_timer, monitor_check, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
    pr_info("[%s] Loaded successfully with major number %d.\n", DEVICE_NAME, major);
    return 0;
}

static void __exit monitor_exit(void) {
    del_timer(&monitor_timer);
    
    // Clean up the device file and class
    device_destroy(monitor_class, MKDEV(major, 0));
    class_destroy(monitor_class);
    
    unregister_chrdev(major, DEVICE_NAME);
}

module_init(monitor_init);
module_exit(monitor_exit);
