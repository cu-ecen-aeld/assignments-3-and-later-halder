/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "aesdchar.h"
#include "aesd_ioctl.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("halder");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    struct aesd_dev *dev;
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    size_t entry_offset = 0;
    size_t bytes_to_copy = 0;
    struct aesd_buffer_entry *entry;
    struct aesd_dev *dev = filp->private_data;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);
    if (!entry) {
        retval = 0;
        goto unlock_out;
    }

    bytes_to_copy = min(count, entry->size - entry_offset);
    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto unlock_out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

unlock_out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    char *write_buffer;
    char *newline;
    char *merged_buffer = NULL;
    size_t merged_size = 0;
    struct aesd_buffer_entry entry;
    struct aesd_dev *dev = filp->private_data;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    if (!count)
        return 0;

    write_buffer = kmalloc(count, GFP_KERNEL);
    if (!write_buffer)
        return -ENOMEM;

    if (copy_from_user(write_buffer, buf, count)) {
        kfree(write_buffer);
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(write_buffer);
        return -ERESTARTSYS;
    }

    merged_size = dev->pending_write.size + count;
    merged_buffer = kmalloc(merged_size, GFP_KERNEL);
    if (!merged_buffer)
        goto unlock_out;

    if (dev->pending_write.size) {
        memcpy(merged_buffer, dev->pending_write.buffptr, dev->pending_write.size);
        kfree(dev->pending_write.buffptr);
    }
    memcpy(merged_buffer + dev->pending_write.size, write_buffer, count);
    dev->pending_write.buffptr = NULL;
    dev->pending_write.size = 0;

    newline = memchr(merged_buffer, '\n', merged_size);
    if (!newline) {
        dev->pending_write.buffptr = merged_buffer;
        dev->pending_write.size = merged_size;
        retval = count;
        merged_buffer = NULL;
        goto unlock_out;
    }

    entry.buffptr = merged_buffer;
    entry.size = merged_size;

    if (dev->buffer.full && dev->buffer.entry[dev->buffer.in_offs].buffptr)
        kfree(dev->buffer.entry[dev->buffer.in_offs].buffptr);

    aesd_circular_buffer_add_entry(&dev->buffer, &entry);
    retval = count;
    *f_pos += retval;
    merged_buffer = NULL;

unlock_out:
    kfree(merged_buffer);
    kfree(write_buffer);
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence){
    struct aesd_dev *dev = filp->private_data;
    loff_t new_pos = 0;

    if (mutex_lock_interruptible(&dev->lock))
    {
        new_pos = -EFAULT;
        goto unlock_llseek;
    }
    
    new_pos = fixed_size_llseek(filp, offset, whence, 4096);
    if (new_pos >= 0)
        PDEBUG("llseek from %lld to %lld\n", filp->f_pos, new_pos);

unlock_llseek:
    mutex_unlock(&dev->lock);

    return new_pos;
}

long aesd_adjust_file_offset(struct file *filp, unsigned int write_cmd, unsigned int write_cmd_offset)
{
    long retval = 0;
    struct aesd_dev *dev = filp->private_data;
    int total_size = 0;
    long new_f_pos = 0;
    int i = 0;
    struct aesd_buffer_entry *entry;

    if (mutex_lock_interruptible(&dev->lock))
    {
        retval = -ERESTARTSYS;
        goto unlock_adjust;
    }

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, i)
    {
        if (entry->buffptr)
        {
            total_size += entry->size;
            if (i < write_cmd) // not command we're looking for, increment command
            {
                new_f_pos += entry->size;
            } else if (i == write_cmd)
            {
                if ((write_cmd_offset >= entry->size) || (new_f_pos > total_size)) // out of bounds; could not find command
                {
                    retval = -EINVAL;
                    goto unlock_adjust;
                }
                
                new_f_pos += write_cmd_offset;
            }
        }
    }
    
    filp->f_pos = new_f_pos;

unlock_adjust:
    mutex_unlock(&dev->lock);

adjust_out:
    return retval;
}

static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    long retval = 0;

    switch (cmd) {
        case AESDCHAR_IOCSEEKTO:
            struct aesd_seekto seekto; 
            if (copy_from_user(&seekto, (const void __user *) arg, sizeof(seekto)) != 0)
            {
                retval = -EFAULT;
            } else {
                retval = aesd_adjust_file_offset(filp, seekto.write_cmd, seekto.write_cmd_offset);
            }
            break;
        default:
            return -EINVAL;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner          = THIS_MODULE,
    .read           = aesd_read,
    .write          = aesd_write,
    .open           = aesd_open,
    .release        = aesd_release,
    .llseek         = aesd_llseek,
    .unlocked_ioctl = aesd_ioctl
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}


int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.buffer);
    aesd_device.pending_write.buffptr = NULL;
    aesd_device.pending_write.size = 0;
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr)
            kfree(entry->buffptr);
    }
    if (aesd_device.pending_write.buffptr)
        kfree(aesd_device.pending_write.buffptr);

    unregister_chrdev_region(devno, 1);
}


module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
