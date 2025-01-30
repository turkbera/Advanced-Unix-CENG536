#include <linux/mm.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/mount.h>
#include <linux/printk.h>
#include <linux/atomic.h>
#include <linux/mm_types.h>

struct vma_info {
    long start;
    long end;
    struct {
        unsigned int readable : 1;
        unsigned int writeable : 1;
        unsigned int executable : 1;
        unsigned int shared : 1;
        unsigned int filemapped : 1;
        unsigned int heap : 1;
        unsigned int stack : 1;
    } flags;
    struct {
        unsigned long inode;
        int major, minor;
        char path[256];
        unsigned long long offset;
    } file;
    struct {
        unsigned long vmpages;
        unsigned long vmdata;
        unsigned long vmexec;
        unsigned long vmstack;
        unsigned long vmresanon;
        unsigned long vmresfile;
        int vmref;
    } mm;
};

SYSCALL_DEFINE2(check_addr, unsigned long, addr, struct vma_info __user *, info) {
    struct vm_area_struct *vma;
    struct vma_info kinfo = {0};
    struct mm_struct *mm = current->mm;

    // Acquire read lock on memory map
    mmap_read_lock(mm);

    vma = find_vma(mm, addr);
    if (!vma || addr < vma->vm_start) {
        mmap_read_unlock(mm);
        return -EINVAL;
    }

    kinfo.start = vma->vm_start;
    kinfo.end = vma->vm_end;

    // Populate flags
    kinfo.flags.readable = (vma->vm_flags & VM_READ) ? 1 : 0;
    kinfo.flags.writeable = (vma->vm_flags & VM_WRITE) ? 1 : 0;
    kinfo.flags.executable = (vma->vm_flags & VM_EXEC) ? 1 : 0;
    kinfo.flags.shared = (vma->vm_flags & VM_SHARED) ? 1 : 0;
    kinfo.flags.filemapped = vma->vm_file ? 1 : 0;

    if (vma->vm_start == mm->start_stack) {
        kinfo.flags.stack = 1;
    }
    if (vma->vm_start == mm->start_brk && vma->vm_end == mm->brk) {
        kinfo.flags.heap = 1;
    }

    // Populate file details if the VMA is file-mapped
    if (vma->vm_file) {
        struct file *file = vma->vm_file;
        struct inode *inode = file->f_inode;

        kinfo.file.inode = inode->i_ino;
        kinfo.file.major = MAJOR(inode->i_rdev);
        kinfo.file.minor = MINOR(inode->i_rdev);
        kinfo.file.offset = vma->vm_pgoff << PAGE_SHIFT;

        // Resolve the full file path
        char *path_buf = (char *)__get_free_page(GFP_KERNEL);
        if (path_buf) {
            char *resolved_path = d_path(&file->f_path, path_buf, PAGE_SIZE);
            if (!IS_ERR(resolved_path)) {
                strncpy(kinfo.file.path, resolved_path, sizeof(kinfo.file.path) - 1);
                kinfo.file.path[sizeof(kinfo.file.path) - 1] = '\0';
            }
            free_page((unsigned long)path_buf);
        }
    }

   // Populate memory statistics
kinfo.mm.vmpages = mm->total_vm;
kinfo.mm.vmdata = mm->data_vm;
kinfo.mm.vmexec = mm->exec_vm;
kinfo.mm.vmstack = mm->stack_vm;

kinfo.mm.vmresanon = percpu_counter_sum(&mm->rss_stat[MM_ANONPAGES]);
kinfo.mm.vmresfile = percpu_counter_sum(&mm->rss_stat[MM_FILEPAGES]);

kinfo.mm.vmref = mm->map_count;


    // Release read lock
    mmap_read_unlock(mm);

    if (copy_to_user(info, &kinfo, sizeof(kinfo))) {
        return -EFAULT;
    }

    return 0;
}
