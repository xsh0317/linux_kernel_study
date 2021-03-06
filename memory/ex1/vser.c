#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/kfifo.h>

#include <linux/ioctl.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/aio.h>

#include <linux/interrupt.h>
#include <linux/random.h>

#include "vser.h"

#define VSER_MAJOR	256
#define VSER_MINOR	0
#define VSER_DEV_CNT	1
#define VSER_DEV_NAME	"vser"
#define VSER_FIFO_SIZE	32

struct vser_dev {
	struct kfifo fifo;
	wait_queue_head_t rwqh;
	struct fasync_struct *fapp;
	atomic_t available;
	unsigned int baud;
	struct option opt;
	struct cdev cdev;
};

static struct vser_dev *vsdev;

static void vser_work(struct work_struct *work);
DECLARE_WORK(vswork, vser_work);

static int vser_fasync(int fd, struct file *filp, int on);

static int vser_open(struct inode *inode, struct file *filp)
{
	if (atomic_dec_and_test(&vsdev->available))
		return 0;
	else {
		atomic_inc(&vsdev->available);
		return -EBUSY;
	}
}

static int vser_release(struct inode *inode, struct file *filp)
{
	vser_fasync(-1, filp, 0);
	atomic_inc(&vsdev->available);
	return 0;
}

static ssize_t vser_read(struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
	int ret;
	int len;
	char tbuf[VSER_FIFO_SIZE];

	len = count > sizeof(tbuf) ? sizeof(tbuf) : count;
	spin_lock(&vsdev->rwqh.lock);
	if (kfifo_is_empty(&vsdev->fifo)) {
		if (filp->f_flags & O_NONBLOCK) {
			spin_unlock(&vsdev->rwqh.lock);
			return -EAGAIN;
		}

		if (wait_event_interruptible_locked(vsdev->rwqh, !kfifo_is_empty(&vsdev->fifo))) {
			spin_unlock(&vsdev->rwqh.lock);
			return -ERESTARTSYS;
		}
	}

	len = kfifo_out(&vsdev->fifo, tbuf, len);
	spin_unlock(&vsdev->rwqh.lock);

	ret = copy_to_user(buf, tbuf, len);
	return len - ret;
}

static ssize_t vser_write(struct file *filp, const char __user *buf, size_t count, loff_t *pos)
{

	int ret;
	int len;
	char *tbuf[VSER_FIFO_SIZE];

	len = count > sizeof(tbuf) ? sizeof(tbuf) : count;
	ret = copy_from_user(tbuf, buf, len);

	return len - ret;
}

static long vser_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) != VS_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case VS_SET_BAUD:
		vsdev->baud = arg;
		break;
	case VS_GET_BAUD:
		arg = vsdev->baud;
		break;
	case VS_SET_FFMT:
		if (copy_from_user(&vsdev->opt, (struct option __user *)arg, sizeof(struct option)))
			return -EFAULT;
		break;
	case VS_GET_FFMT:
		if (copy_to_user((struct option __user *)arg, &vsdev->opt, sizeof(struct option)))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}

	return 0;
}

static unsigned int vser_poll(struct file *filp, struct poll_table_struct *p)
{
	int mask = POLLOUT | POLLWRNORM;

	poll_wait(filp, &vsdev->rwqh, p);

	spin_lock(&vsdev->rwqh.lock);
	if (!kfifo_is_empty(&vsdev->fifo))
		mask |= POLLIN | POLLRDNORM;
	spin_unlock(&vsdev->rwqh.lock);

	return mask;
}

static ssize_t vser_aio_read(struct kiocb *iocb, const struct iovec *iov, unsigned long nr_segs, loff_t pos)
{
	size_t read = 0;
	unsigned long i;
	ssize_t ret;

	for (i = 0; i < nr_segs; i++) {
		ret = vser_read(iocb->ki_filp, iov[i].iov_base, iov[i].iov_len, &pos);
		if (ret < 0)
			break;
		read += ret;
	}

	return read ? read : -EFAULT;
}

static ssize_t vser_aio_write(struct kiocb *iocb, const struct iovec *iov, unsigned long nr_segs, loff_t pos)
{
	size_t written = 0;
	unsigned long i;
	ssize_t ret;

	for (i = 0; i < nr_segs; i++) {
		ret = vser_write(iocb->ki_filp, iov[i].iov_base, iov[i].iov_len, &pos);
		if (ret < 0)
			break;
		written += ret;
	}

	return written ? written : -EFAULT;
}

static int vser_fasync(int fd, struct file *filp, int on)
{
	return fasync_helper(fd, filp, on, &vsdev->fapp);
}

static irqreturn_t vser_handler(int irq, void *dev_id)
{
	schedule_work(&vswork);

	return IRQ_HANDLED;
}

static void vser_work(struct work_struct *work)
{
	char data;

	get_random_bytes(&data, sizeof(data));
	data %= 26;
	data += 'A';

	spin_lock(&vsdev->rwqh.lock);
	if (!kfifo_is_full(&vsdev->fifo))
		if(!kfifo_in(&vsdev->fifo, &data, sizeof(data)))
			printk(KERN_ERR "vser: kfifo_in failure\n");

	if (!kfifo_is_empty(&vsdev->fifo)) {
		spin_unlock(&vsdev->rwqh.lock);
		wake_up_interruptible(&vsdev->rwqh);
		kill_fasync(&vsdev->fapp, SIGIO, POLL_IN);
	} else
		spin_unlock(&vsdev->rwqh.lock);
}

static struct file_operations vser_ops = {
	.owner = THIS_MODULE,
	.open = vser_open,
	.release = vser_release,
	.read = vser_read,
	.write = vser_write,
	.unlocked_ioctl = vser_ioctl,
	.poll = vser_poll,
	.aio_read = vser_aio_read,
	.aio_write = vser_aio_write,
	.fasync = vser_fasync,
};

static int __init vser_init(void)
{
	int ret;
	dev_t dev;

	dev = MKDEV(VSER_MAJOR, VSER_MINOR);
	ret = register_chrdev_region(dev, VSER_DEV_CNT, VSER_DEV_NAME);
	if (ret)
		goto reg_err;

	vsdev = kzalloc(sizeof(struct vser_dev), GFP_KERNEL);
	if (!vsdev) {
		ret = -ENOMEM;
		goto mem_err;
	}

	ret = kfifo_alloc(&vsdev->fifo, VSER_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto fifo_err;

	cdev_init(&vsdev->cdev, &vser_ops);
	vsdev->cdev.owner = THIS_MODULE;
	vsdev->baud = 115200;
	vsdev->opt.datab = 8;
	vsdev->opt.parity = 0;
	vsdev->opt.stopb = 1;

	ret = cdev_add(&vsdev->cdev, dev, VSER_DEV_CNT);
	if (ret)
		goto add_err;

	init_waitqueue_head(&vsdev->rwqh);

	ret = request_irq(167, vser_handler, IRQF_TRIGGER_HIGH | IRQF_SHARED, "vser", &vsdev);
	if (ret)
		goto irq_err;

	atomic_set(&vsdev->available, 1);

	return 0;

irq_err:
	cdev_del(&vsdev->cdev);
add_err:
	unregister_chrdev_region(dev, VSER_DEV_CNT);
reg_err:
	kfifo_free(&vsdev->fifo);
fifo_err:
	kfree(vsdev);
mem_err:
	return ret;
}

static void __exit vser_exit(void)
{
	dev_t dev;

	dev = MKDEV(VSER_MAJOR, VSER_MINOR);

	free_irq(167, &vsdev);
	cdev_del(&vsdev->cdev);
	unregister_chrdev_region(dev, VSER_DEV_CNT);
	kfifo_free(&vsdev->fifo);
	kfree(vsdev);
}

module_init(vser_init);
module_exit(vser_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kevin Jiang <jiangxg@farsight.com.cn>");
MODULE_DESCRIPTION("A simple character device driver");
MODULE_ALIAS("virtual-serial");
