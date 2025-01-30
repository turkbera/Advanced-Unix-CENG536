#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>    /* printk() */
#include <linux/slab.h>      /* kmalloc() */
#include <linux/fs.h>        /* file_operations, register_chrdev_region... */
#include <linux/errno.h>     /* error codes like ENOMEM */
#include <linux/types.h>     /* size_t, etc. */
#include <linux/fcntl.h>     /* O_ACCMODE, O_WRONLY, O_RDONLY */
#include <linux/cdev.h>      /* cdev utilities */
#include <linux/uaccess.h>   /* copy_to_user, copy_from_user */
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/string.h>

#include "cipher.h"          /* local definitions */

MODULE_AUTHOR("Advanced Unix");
MODULE_LICENSE("Dual BSD/GPL");

/* ------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------ */
#define DEFAULTCIPHER  "CENG536 IS THE BEST!!!!!"
#define BUCKET_SIZE    256       /* Each 'bucket' is 256 bytes */
#define MAX_WRITE_LEN  8192      /* Writers can write at most 8192 bytes */

/* ------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------ */
static int cipher_major;           /* dynamic major allocated at load time */
static int cipher_nr_devs = CIPHER_NR_DEVS;  /* number of devices => 8 */

module_param_named(nr_devs, cipher_nr_devs, int, S_IRUGO);
/* ^ You can remove if you truly have no module parameters. 
   Shown here just for completeness/paralleling scull's approach. */

/* ------------------------------------------------------------------
 * Data Structures
 * ------------------------------------------------------------------ */

/* Each 256-byte chunk is stored in a bucket. Linked by list_head. */
struct bucket {
    struct list_head bucketlist; /* for linking into cipher_dev->buckets */
    int end;                     /* how many bytes are currently used */
    int refcount;                /* how many open contexts "hold" it */
    struct mutex mut;            /* not always necessary, but for safety */
    char buffer[BUCKET_SIZE];
};

/* Per-open-file context */
struct fcontext {
    struct file *fp;            /* back pointer to the file struct */
    struct bucket *next;        /* next bucket to read/write */
    int off;                    /* offset within that bucket */
    int keyoff;                 /* offset in the key (for XOR) */
    int mode;                   /* 0 => read, 1 => write */
    struct list_head contexes;  /* linked into cipher_dev->contexes */
    struct cipher_dev *dev;     /* the device we belong to */
    ssize_t totalwritten;       /* how many bytes this writer has written */
};

/* Main device structure */
struct cipher_dev {
    struct list_head buckets;   /* list of buckets storing all data */
    int keyfirst;               /* key offset at the start (for first bucket) */
    int keylast;                /* key offset at the end (for last bucket) */
    struct list_head contexes;  /* list of all open contexts on this device */
    char cipher[4096];          /* encryption key string */
    int cipherlen;              /* length of that key string */
    struct mutex mut;           /* device-level mutex */
    struct cdev cdev;           /* char device struct from <linux/cdev.h> */
};

/* We'll allocate an array of these, one per minor device */
static struct cipher_dev *cipher_devices;

/* ------------------------------------------------------------------
 * Helper: XOR a buffer with dev->cipher, updating key offset
 * ------------------------------------------------------------------ */
static void cipher_xor_buffer(struct cipher_dev *dev, char *data,
                              size_t len, int *keyoff)
{
    int i;
    for (i = 0; i < len; i++) {
        data[i] ^= dev->cipher[(*keyoff) % dev->cipherlen];
        (*keyoff)++;
    }
}

/* ------------------------------------------------------------------
 * Helper: free all buckets in a device (like scull_trim)
 * ------------------------------------------------------------------ */
static void cipher_free_buckets(struct cipher_dev *dev)
{
    struct list_head *pos, *n;

    list_for_each_safe(pos, n, &dev->buckets) {
        struct bucket *b = list_entry(pos, struct bucket, bucketlist);
        list_del(pos);
        kfree(b);
    }
}

/* ------------------------------------------------------------------
 * OPEN
 * ------------------------------------------------------------------ */
static int cipher_open(struct inode *inode, struct file *filp)
{
    struct cipher_dev *dev;
    struct fcontext *cont;
    int accmode;

    /* Get our device object via container_of */
    dev = container_of(inode->i_cdev, struct cipher_dev, cdev);
    if (!dev) return -ENODEV;

    /* Allocate a new fcontext */
    cont = kmalloc(sizeof(*cont), GFP_KERNEL);
    if (!cont) return -ENOMEM;
    memset(cont, 0, sizeof(*cont));

    filp->private_data = cont;
    cont->fp   = filp;
    cont->dev  = dev;
    cont->off  = 0;
    cont->keyoff = 0;
    cont->totalwritten = 0;

    /* Decide mode (read or write) from open flags */
    accmode = filp->f_flags & O_ACCMODE;
    if (accmode == O_WRONLY) {
        cont->mode = 1; /* writer */
    } else if (accmode == O_RDONLY) {
        cont->mode = 0; /* reader */
    } else {
        /* Device only supports read-only or write-only. */
        kfree(cont);
        filp->private_data = NULL;
        return -EINVAL;
    }

    /* Now lock the device and do setup referencing existing buckets */
    mutex_lock(&dev->mut);

    /* 1) Increase refcount of all existing buckets to hold them alive
          for this new fcontext. */
    {
        struct list_head *pos;
        list_for_each(pos, &dev->buckets) {
            struct bucket *b = list_entry(pos, struct bucket, bucketlist);
            b->refcount++;
        }
    }

    /* 2) If read => start at HEAD with keyoff=keyfirst 
          If write => start at TAIL with keyoff=keylast */
    if (cont->mode == 0) {
        /* Read: HEAD */
        if (!list_empty(&dev->buckets)) {
            struct bucket *first = list_entry(dev->buckets.next,
                                              struct bucket, bucketlist);
            cont->next = first;
            cont->off  = 0;
            cont->keyoff = dev->keyfirst;
        } else {
            cont->next = NULL; /* empty device */
            cont->off  = 0;
            cont->keyoff = 0;
        }
    } else {
        /* Write: TAIL */
        if (!list_empty(&dev->buckets)) {
            struct bucket *last = list_entry(dev->buckets.prev,
                                             struct bucket, bucketlist);
            cont->next = last;
            cont->off  = last->end; /* write from end of last bucket */
            cont->keyoff = dev->keylast;
        } else {
            cont->next = NULL;
            cont->off  = 0;
            cont->keyoff = 0;
        }
    }

    /* 3) Insert this new context into dev->contexes list. */
    INIT_LIST_HEAD(&cont->contexes);
    list_add(&cont->contexes, &dev->contexes);

    mutex_unlock(&dev->mut);
    return 0;
}

/* ------------------------------------------------------------------
 * RELEASE (close)
 * ------------------------------------------------------------------ */
static int cipher_release(struct inode *inode, struct file *filp)
{
    struct fcontext *context = filp->private_data;
    struct cipher_dev *dev;
    struct list_head *pos, *n;

    if (!context) return 0; /* no context => nothing */

    dev = context->dev;
    if (!dev) {
        kfree(context);
        filp->private_data = NULL;
        return 0;
    }

    /* Decrement refcount on all buckets for this closer. */
    mutex_lock(&dev->mut);
    list_for_each_safe(pos, n, &dev->buckets) {
        struct bucket *b = list_entry(pos, struct bucket, bucketlist);
        if (b->refcount > 0) {
            b->refcount--;
            /* If it hits 0, free it if we are a reader or if truly no other references exist.
               We'll just free unconditionally if refcount=0. */
            if (b->refcount == 0) {
                list_del(&b->bucketlist);
                kfree(b);
            }
        }
    }

    /* Remove from dev->contexes */
    list_del(&context->contexes);

    mutex_unlock(&dev->mut);

    kfree(context);
    filp->private_data = NULL;
    return 0;
}

/* ------------------------------------------------------------------
 * READ
 * ------------------------------------------------------------------ */
static ssize_t cipher_read(struct file *filp, char __user *buf, size_t count,
                           loff_t *f_pos)
{
    struct fcontext *context = filp->private_data;
    struct cipher_dev *dev = context->dev;
    struct bucket *bucket;
    size_t total_copied = 0;
    char tempbuf[BUCKET_SIZE];

    if (!dev) return -EINVAL;

    mutex_lock(&dev->mut);
    bucket = context->next;

    while (count > 0 && bucket) {
        int avail = bucket->end - context->off;
        if (avail <= 0) {
            /* move on to next bucket */
            /* first decrement refcount on the old bucket */
            bucket->refcount--;
            if (bucket->refcount == 0) {
                list_del(&bucket->bucketlist);
                kfree(bucket);
            }
            /* next bucket */
            if (list_empty(&dev->buckets)) {
                bucket = NULL;
            } else {
                /* we can find next in the device list from bucket->list.next,
                   but we need to be sure it's not the head again */
                struct list_head *nx = (bucket ?
                                bucket->bucketlist.next : dev->buckets.next);
                if (nx == &dev->buckets) {
                    /* end of list */
                    bucket = NULL;
                } else {
                    bucket = list_entry(nx, struct bucket, bucketlist);
                }
            }
            context->next = bucket;
            context->off  = 0;
            continue;
        }

        /* We have data in this bucket. Read min(avail, count). */
        {
            size_t to_copy = (avail < count) ? avail : count;
            memcpy(tempbuf, bucket->buffer + context->off, to_copy);

            /* XOR to decrypt. */
            cipher_xor_buffer(dev, tempbuf, to_copy, &context->keyoff);

            if (copy_to_user(buf, tempbuf, to_copy)) {
                mutex_unlock(&dev->mut);
                return -EFAULT;
            }

            context->off += to_copy;
            buf          += to_copy;
            count        -= to_copy;
            total_copied += to_copy;
        }
    }

    mutex_unlock(&dev->mut);
    return total_copied;
}

/* ------------------------------------------------------------------
 * WRITE
 * ------------------------------------------------------------------ */
static ssize_t cipher_write(struct file *filp, const char __user *buf,
                            size_t count, loff_t *f_pos)
{
    struct fcontext *context = filp->private_data;
    struct cipher_dev *dev = context->dev;
    struct bucket *bucket;
    ssize_t total_written = 0;
    char tempbuf[BUCKET_SIZE];

    if (!dev) return -EINVAL;

    mutex_lock(&dev->mut);

    /* Enforce 8192 limit per writer. */
    if (context->totalwritten >= MAX_WRITE_LEN) {
        mutex_unlock(&dev->mut);
        return 0; /* no more writes allowed */
    }
    if (context->totalwritten + count > MAX_WRITE_LEN) {
        count = MAX_WRITE_LEN - context->totalwritten;
    }

    bucket = context->next;

    while (count > 0) {
        /* If no bucket or current is full => allocate new. */
        if (!bucket || bucket->end >= BUCKET_SIZE) {
            /* If we had a bucket, writer can decrement refcount */
            if (bucket) {
                bucket->refcount--;
                /* We do not free if refcount=0 (instructions say writer
                   does not deallocate). But you *could* free if you want. */
            }
            /* create a fresh bucket */
            {
                int readers_count = 0;
                struct list_head *pos;
                list_for_each(pos, &dev->contexes) {
                    struct fcontext *fc = list_entry(pos, struct fcontext, contexes);
                    if (fc->mode == 0) readers_count++;
                }
                bucket = kmalloc(sizeof(struct bucket), GFP_KERNEL);
                if (!bucket) {
                    mutex_unlock(&dev->mut);
                    return -ENOMEM;
                }
                memset(bucket, 0, sizeof(*bucket));
                INIT_LIST_HEAD(&bucket->bucketlist);
                mutex_init(&bucket->mut);

                bucket->refcount = 1 + readers_count;
                bucket->end = 0;

                list_add_tail(&bucket->bucketlist, &dev->buckets);
            }
            context->next = bucket;
            context->off  = 0;
        }

        /* Write data into current bucket. */
        {
            int space = BUCKET_SIZE - bucket->end;
            size_t to_copy = (space < count) ? space : count;
            if (copy_from_user(tempbuf, buf, to_copy)) {
                mutex_unlock(&dev->mut);
                return -EFAULT;
            }
            /* XOR encrypt before storing. */
            cipher_xor_buffer(dev, tempbuf, to_copy, &context->keyoff);

            memcpy(bucket->buffer + bucket->end, tempbuf, to_copy);
            bucket->end += to_copy;

            dev->keylast = context->keyoff; /* track last key offset */

            count         -= to_copy;
            total_written += to_copy;
            buf           += to_copy;
            context->totalwritten += to_copy;
        }
    }

    mutex_unlock(&dev->mut);
    return total_written;
}

/* ------------------------------------------------------------------
 * IOCTL
 * ------------------------------------------------------------------ */
static long cipher_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct fcontext *context = filp->private_data;
    struct cipher_dev *dev = context->dev;
    int err = 0, retval = 0;

    if (_IOC_TYPE(cmd) != CIPHER_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > CIPHER_IOC_MAXNR)    return -ENOTTY;

    if (_IOC_DIR(cmd) & _IOC_READ)
        err = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
    else if (_IOC_DIR(cmd) & _IOC_WRITE)
        err = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
    if (err) return -EFAULT;

    switch(cmd) {

    case CIPHER_IOCCLR: /* Reset device: free all buckets, restore default key. */
        mutex_lock(&dev->mut);
        cipher_free_buckets(dev);
        dev->keyfirst = 0;
        dev->keylast  = 0;
        memset(dev->cipher, 0, sizeof(dev->cipher));
        memcpy(dev->cipher, DEFAULTCIPHER, strlen(DEFAULTCIPHER));
        dev->cipherlen = strlen(dev->cipher);

        /* Also reset all open contexts to an empty state */
        {
            struct list_head *pos;
            list_for_each(pos, &dev->contexes) {
                struct fcontext *fc = list_entry(pos, struct fcontext, contexes);
                fc->next = NULL;
                fc->off  = 0;
                fc->keyoff = 0;
                fc->totalwritten = 0;
            }
        }
        mutex_unlock(&dev->mut);
        break;

    case CIPHER_IOCSKEY: /* Set a new key from user and also reset device. */
    {
        char kbuf[4096];
        memset(kbuf, 0, sizeof(kbuf));
        if (copy_from_user(kbuf, (char __user *)arg, 4096))
            return -EFAULT;

        mutex_lock(&dev->mut);
        cipher_free_buckets(dev);
        dev->keyfirst = 0;
        dev->keylast  = 0;
        memset(dev->cipher, 0, sizeof(dev->cipher));
        strncpy(dev->cipher, kbuf, 4095);
        dev->cipher[4095] = '\0';
        dev->cipherlen = strnlen(dev->cipher, 4096);

        /* Reset all contexts too. */
        {
            struct list_head *pos;
            list_for_each(pos, &dev->contexes) {
                struct fcontext *fc = list_entry(pos, struct fcontext, contexes);
                fc->next = NULL;
                fc->off  = 0;
                fc->keyoff = 0;
                fc->totalwritten = 0;
            }
        }
        mutex_unlock(&dev->mut);
    }
    break;

    case CIPHER_IOCQREM: /* Query how many bytes remain (to read or to write). */
    {
        int val = 0;
        /* If reading, count how many bytes remain from current position. 
           If writing, how many of the 8192 remain. */
        if (context->mode == 0) {
            /* Reader: sum up the remaining data from context->next onward. */
            struct bucket *b = context->next;
            int off = context->off;

            mutex_lock(&dev->mut);
            while (b) {
                int remain = b->end - off;
                if (remain > 0) val += remain;
                off = 0;
                /* move b to next in the device list */
                if (b->bucketlist.next == &dev->buckets) {
                    b = NULL;
                } else {
                    b = list_entry(b->bucketlist.next, struct bucket, bucketlist);
                }
            }
            mutex_unlock(&dev->mut);
        } else {
            /* Writer: how many left out of 8192? */
            if (context->totalwritten < MAX_WRITE_LEN) {
                val = MAX_WRITE_LEN - context->totalwritten;
            } else {
                val = 0;
            }
        }

        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            return -EFAULT;
    }
    break;

    default:
        retval = -ENOTTY;
        break;
    }

    return retval;
}

/* ------------------------------------------------------------------
 * LLseek (we do not truly support seeking, so we can just return -EINVAL
 * or mimic scull's approach if needed). We'll do a minimal approach.
 * ------------------------------------------------------------------ */
static loff_t cipher_llseek(struct file *filp, loff_t off, int whence)
{
    /* The device is "append-only" for write, read is sequential. 
       We can just disallow seeking. */
    return -EINVAL;
}

/* ------------------------------------------------------------------
 * File Operations
 * ------------------------------------------------------------------ */
static struct file_operations cipher_fops = {
    .owner            = THIS_MODULE,
    .llseek           = cipher_llseek,      /* minimal/no seeking */
    .read             = cipher_read,
    .write            = cipher_write,
    .unlocked_ioctl   = cipher_ioctl,
    .open             = cipher_open,
    .release          = cipher_release,
};

/* ------------------------------------------------------------------
 * Setup cdev (similar to scull_setup_cdev)
 * ------------------------------------------------------------------ */
static void cipher_setup_cdev(struct cipher_dev *dev, int index)
{
    int err;
    dev_t devno = MKDEV(cipher_major, index);

    cdev_init(&dev->cdev, &cipher_fops);
    dev->cdev.owner = THIS_MODULE;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
        printk(KERN_WARNING "cipher: Error %d adding cipher%d\n", err, index);
}

/* ------------------------------------------------------------------
 * Cleanup (like scull_cleanup_module)
 * ------------------------------------------------------------------ */
static void __exit cipher_cleanup_module(void)
{
    int i;
    dev_t devno = MKDEV(cipher_major, 0);

    if (cipher_devices) {
        for (i = 0; i < cipher_nr_devs; i++) {
            struct cipher_dev *cdevp = &cipher_devices[i];
            /* Remove cdev */
            cdev_del(&cdevp->cdev);

            /* Free buckets for this device */
            mutex_lock(&cdevp->mut);
            cipher_free_buckets(cdevp);
            mutex_unlock(&cdevp->mut);
        }
        kfree(cipher_devices);
    }

    unregister_chrdev_region(devno, cipher_nr_devs);
    printk(KERN_INFO "cipher: Module unloaded.\n");
}

/* ------------------------------------------------------------------
 * Init (like scull_init_module)
 * ------------------------------------------------------------------ */
static int __init cipher_init_module(void)
{
    int result, i;
    dev_t dev = 0;

    /* Acquire a range of device numbers */
    result = alloc_chrdev_region(&dev, 0, cipher_nr_devs, "cipher");
    if (result < 0) {
        printk(KERN_WARNING "cipher: can't get device number\n");
        return result;
    }
    cipher_major = MAJOR(dev);

    /* Allocate the array of devices */
    cipher_devices = kmalloc(cipher_nr_devs * sizeof(struct cipher_dev),
                             GFP_KERNEL);
    if (!cipher_devices) {
        unregister_chrdev_region(dev, cipher_nr_devs);
        return -ENOMEM;
    }
    memset(cipher_devices, 0, cipher_nr_devs * sizeof(struct cipher_dev));

    /* Initialize each device */
    for (i = 0; i < cipher_nr_devs; i++) {
        struct cipher_dev *cdevp = &cipher_devices[i];
        INIT_LIST_HEAD(&cdevp->buckets);
        INIT_LIST_HEAD(&cdevp->contexes);
        cdevp->keyfirst = 0;
        cdevp->keylast  = 0;
        mutex_init(&cdevp->mut);

        /* Set default cipher string */
        memset(cdevp->cipher, 0, sizeof(cdevp->cipher));
        memcpy(cdevp->cipher, DEFAULTCIPHER, strlen(DEFAULTCIPHER));
        cdevp->cipherlen = strlen(DEFAULTCIPHER);

        /* Setup cdev */
        cipher_setup_cdev(cdevp, i);
    }

    printk(KERN_INFO "cipher: Module loaded. Major=%d, nr_devs=%d\n",
           cipher_major, cipher_nr_devs);
    return 0;
}

module_init(cipher_init_module);
module_exit(cipher_cleanup_module);
