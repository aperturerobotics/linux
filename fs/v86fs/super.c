// SPDX-License-Identifier: GPL-2.0
/*
 * v86fs - v86 host filesystem via custom virtio device
 *
 * Uses virtio device type 63 (PCI device ID 0x107F) with three virtqueues:
 *   Queue 0 (hipriq):   high-priority metadata requests
 *   Queue 1 (requestq): data requests (READ, WRITE, READDIR)
 *   Queue 2 (notifyq):  host-to-guest push invalidation
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>

#define V86FS_VIRTIO_ID 63
#define V86FS_MAGIC     0x56383646 /* "V86F" */

#define V86FS_VQ_HIPRIQ   0
#define V86FS_VQ_REQUESTQ 1
#define V86FS_VQ_NOTIFYQ  2
#define V86FS_VQ_MAX      3

struct v86fs_device {
	struct virtio_device *vdev;
	struct virtqueue *vqs[V86FS_VQ_MAX];
};

/* Global device pointer (single device supported for now) */
static struct v86fs_device *v86fs_dev;

/*
 * VFS operations
 */

static const struct super_operations v86fs_super_ops = {
	.statfs		= simple_statfs,

};

static const struct inode_operations v86fs_dir_inode_ops = {
	.lookup		= simple_lookup,
};


static struct inode *v86fs_make_inode(struct super_block *sb, umode_t mode)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	simple_inode_init_ts(inode);

	switch (mode & S_IFMT) {
	case S_IFDIR:
		inode->i_op = &v86fs_dir_inode_ops;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
		break;
	case S_IFREG:
		inode->i_fop = &simple_dir_operations;
		break;
	}

	return inode;
}

static int v86fs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *root_inode;

	sb->s_maxbytes	= MAX_LFS_FILESIZE;
	sb->s_blocksize	= PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic	= V86FS_MAGIC;
	sb->s_op	= &v86fs_super_ops;
	sb->s_time_gran	= 1;

	root_inode = v86fs_make_inode(sb, S_IFDIR | 0755);
	if (!root_inode)
		return -ENOMEM;

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

static int v86fs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, v86fs_fill_super);
}

static const struct fs_context_operations v86fs_context_ops = {
	.get_tree	= v86fs_get_tree,
};

static int v86fs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &v86fs_context_ops;
	return 0;
}

static void v86fs_kill_sb(struct super_block *sb)
{
	kill_anon_super(sb);
}

static struct file_system_type v86fs_type = {
	.name		= "v86fs",
	.init_fs_context = v86fs_init_fs_context,
	.kill_sb	= v86fs_kill_sb,
	.owner		= THIS_MODULE,
};

/*
 * Virtio device
 */

static void v86fs_hipriq_cb(struct virtqueue *vq)
{
}

static void v86fs_requestq_cb(struct virtqueue *vq)
{
}

static void v86fs_notifyq_cb(struct virtqueue *vq)
{
}

static int v86fs_init_vqs(struct v86fs_device *v86dev)
{
	struct virtqueue *vqs[V86FS_VQ_MAX];
	struct virtqueue_info vqs_info[V86FS_VQ_MAX] = {};
	int err;

	vqs_info[V86FS_VQ_HIPRIQ].callback = v86fs_hipriq_cb;
	vqs_info[V86FS_VQ_HIPRIQ].name = "hipriq";

	vqs_info[V86FS_VQ_REQUESTQ].callback = v86fs_requestq_cb;
	vqs_info[V86FS_VQ_REQUESTQ].name = "requestq";

	vqs_info[V86FS_VQ_NOTIFYQ].callback = v86fs_notifyq_cb;
	vqs_info[V86FS_VQ_NOTIFYQ].name = "notifyq";

	err = virtio_find_vqs(v86dev->vdev, V86FS_VQ_MAX, vqs,
			      vqs_info, NULL);
	if (err)
		return err;

	v86dev->vqs[V86FS_VQ_HIPRIQ] = vqs[V86FS_VQ_HIPRIQ];
	v86dev->vqs[V86FS_VQ_REQUESTQ] = vqs[V86FS_VQ_REQUESTQ];
	v86dev->vqs[V86FS_VQ_NOTIFYQ] = vqs[V86FS_VQ_NOTIFYQ];

	return 0;
}

static int v86fs_probe(struct virtio_device *vdev)
{
	struct v86fs_device *v86dev;
	int err;

	v86dev = kzalloc(sizeof(*v86dev), GFP_KERNEL);
	if (!v86dev)
		return -ENOMEM;

	v86dev->vdev = vdev;
	vdev->priv = v86dev;

	err = v86fs_init_vqs(v86dev);
	if (err) {
		pr_err("v86fs: failed to find virtqueues: %d\n", err);
		goto err_free;
	}

	virtio_device_ready(vdev);
	v86fs_dev = v86dev;

	pr_info("v86fs: probed, %d virtqueues ready\n", V86FS_VQ_MAX);
	return 0;

err_free:
	kfree(v86dev);
	return err;
}

static void v86fs_remove(struct virtio_device *vdev)
{
	struct v86fs_device *v86dev = vdev->priv;

	v86fs_dev = NULL;
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);
	kfree(v86dev);
	pr_info("v86fs: removed\n");
}

static const struct virtio_device_id v86fs_id_table[] = {
	{ V86FS_VIRTIO_ID, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver v86fs_driver = {
	.driver.name	= "v86fs",
	.id_table	= v86fs_id_table,
	.probe		= v86fs_probe,
	.remove		= v86fs_remove,
};

static int __init v86fs_init(void)
{
	int err;

	err = register_virtio_driver(&v86fs_driver);
	if (err)
		return err;

	err = register_filesystem(&v86fs_type);
	if (err) {
		unregister_virtio_driver(&v86fs_driver);
		return err;
	}

	pr_info("v86fs: registered\n");
	return 0;
}

static void __exit v86fs_exit(void)
{
	unregister_filesystem(&v86fs_type);
	unregister_virtio_driver(&v86fs_driver);
}

module_init(v86fs_init);
module_exit(v86fs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("v86 host filesystem via custom virtio device");
