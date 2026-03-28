// SPDX-License-Identifier: GPL-2.0
/*
 * v86fs - v86 host filesystem via custom virtio device
 *
 * Uses virtio device type 63 (PCI device ID 0x107F) with three virtqueues:
 *   Queue 0 (hipriq):   high-priority metadata requests
 *   Queue 1 (requestq): data requests (READ, WRITE, READDIR)
 *   Queue 2 (notifyq):  host-to-guest push invalidation
 */

#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/module.h>
#include <linux/mutex.h>
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

/* Protocol message types */
#define V86FS_MSG_MOUNT     0x00
#define V86FS_MSG_LOOKUP    0x01
#define V86FS_MSG_GETATTR   0x02

/* Response types (type | 0x80) */
#define V86FS_MSG_MOUNT_R   0x80
#define V86FS_MSG_LOOKUP_R  0x81
#define V86FS_MSG_GETATTR_R 0x82
#define V86FS_MSG_ERROR_R   0xFF

/* Message header: 4B length + 1B type + 2B tag = 7 bytes */
#define V86FS_HDR_SIZE 7

/* Max message buffer size */
#define V86FS_MSG_MAX 256

struct v86fs_device {
	struct virtio_device *vdev;
	struct virtqueue *vqs[V86FS_VQ_MAX];
	struct mutex req_lock; /* serialized requests for now */
	struct completion req_done;
};

/* Mount options */
struct v86fs_mount_opts {
	char *root_name;
};

/* Superblock private data */
struct v86fs_sb_info {
	struct v86fs_device *v86dev;
	u64 root_inode_id;
};

/* Global device pointer (single device supported for now) */
static struct v86fs_device *v86fs_dev;

/*
 * Protocol helpers
 */

static void v86fs_pack_header(u8 *buf, u32 length, u8 type, u16 tag)
{
	buf[0] = length & 0xFF;
	buf[1] = (length >> 8) & 0xFF;
	buf[2] = (length >> 16) & 0xFF;
	buf[3] = (length >> 24) & 0xFF;
	buf[4] = type;
	buf[5] = tag & 0xFF;
	buf[6] = (tag >> 8) & 0xFF;
}

static u32 v86fs_read_u32(const u8 *buf)
{
	return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

static u64 v86fs_read_u64(const u8 *buf)
{
	return (u64)v86fs_read_u32(buf) | ((u64)v86fs_read_u32(buf + 4) << 32);
}

/*
 * Send a request on a virtqueue and wait for the response.
 * req_buf/req_len: outgoing data (device-readable)
 * resp_buf/resp_len: incoming data (device-writable)
 * Returns 0 on success, negative errno on failure.
 */
static int v86fs_request(struct v86fs_device *v86dev, int queue_id,
			 void *req_buf, u32 req_len,
			 void *resp_buf, u32 resp_len)
{
	struct virtqueue *vq = v86dev->vqs[queue_id];
	struct scatterlist sg_out, sg_in;
	struct scatterlist *sgs[2];
	int err;

	mutex_lock(&v86dev->req_lock);
	reinit_completion(&v86dev->req_done);

	sg_init_one(&sg_out, req_buf, req_len);
	sg_init_one(&sg_in, resp_buf, resp_len);
	sgs[0] = &sg_out;
	sgs[1] = &sg_in;

	err = virtqueue_add_sgs(vq, sgs, 1, 1, v86dev, GFP_KERNEL);
	if (err) {
		mutex_unlock(&v86dev->req_lock);
		return err;
	}

	virtqueue_kick(vq);

	/* Wait for host to process and return the buffer */
	wait_for_completion(&v86dev->req_done);
	mutex_unlock(&v86dev->req_lock);

	/* Check response status (first 4 bytes after header) */
	return 0;
}

/*
 * Send MOUNT message and get root inode info.
 */
static int v86fs_mount_request(struct v86fs_device *v86dev,
			       const char *root_name,
			       u64 *root_id, u32 *root_mode)
{
	u8 req[V86FS_MSG_MAX];
	u8 resp[V86FS_MSG_MAX];
	u16 name_len;
	u32 total_len;
	u32 status;
	int err;

	name_len = root_name ? strlen(root_name) : 0;
	total_len = V86FS_HDR_SIZE + 2 + name_len;

	v86fs_pack_header(req, total_len, V86FS_MSG_MOUNT, 0);
	req[7] = name_len & 0xFF;
	req[8] = (name_len >> 8) & 0xFF;
	if (name_len)
		memcpy(&req[9], root_name, name_len);

	memset(resp, 0, sizeof(resp));
	err = v86fs_request(v86dev, V86FS_VQ_REQUESTQ,
			    req, total_len, resp, V86FS_MSG_MAX);
	if (err)
		return err;

	/* Parse MOUNT_R: [7B hdr] [4B status] [8B root_id] [4B mode] */
	if (resp[4] != V86FS_MSG_MOUNT_R) {
		pr_err("v86fs: unexpected response type 0x%02x\n", resp[4]);
		return -EIO;
	}

	status = v86fs_read_u32(&resp[7]);
	if (status != 0)
		return -EIO;

	*root_id = v86fs_read_u64(&resp[11]);
	*root_mode = v86fs_read_u32(&resp[19]);
	return 0;
}

/*
 * VFS operations
 */

static const struct super_operations v86fs_super_ops = {
	.statfs		= simple_statfs,
};

static const struct inode_operations v86fs_dir_inode_ops = {
	.lookup		= simple_lookup,
};

static struct inode *v86fs_make_inode(struct super_block *sb, u64 ino,
				      umode_t mode)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	inode->i_ino = ino;
	inode->i_mode = mode;
	simple_inode_init_ts(inode);

	switch (mode & S_IFMT) {
	case S_IFDIR:
		inode->i_op = &v86fs_dir_inode_ops;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
		break;
	case S_IFREG:
		break;
	}

	return inode;
}

enum v86fs_param {
	Opt_root,
};

static const struct fs_parameter_spec v86fs_param_spec[] = {
	fsparam_string("root", Opt_root),
	{}
};

static int v86fs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct v86fs_mount_opts *opts = fc->fs_private;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, v86fs_param_spec, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_root:
		kfree(opts->root_name);
		opts->root_name = param->string;
		param->string = NULL;
		break;
	}
	return 0;
}

static int v86fs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct v86fs_mount_opts *opts = fc->fs_private;
	struct v86fs_sb_info *sbi;
	struct inode *root_inode;
	u64 root_id = 1;
	u32 root_mode = S_IFDIR | 0755;
	int err;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	if (!v86fs_dev) {
		kfree(sbi);
		pr_err("v86fs: no virtio device\n");
		return -ENODEV;
	}

	sbi->v86dev = v86fs_dev;

	sb->s_maxbytes	= MAX_LFS_FILESIZE;
	sb->s_blocksize	= PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic	= V86FS_MAGIC;
	sb->s_op	= &v86fs_super_ops;
	sb->s_time_gran	= 1;
	sb->s_fs_info	= sbi;

	/* Send MOUNT message to host */
	err = v86fs_mount_request(sbi->v86dev, opts->root_name,
				  &root_id, &root_mode);
	if (err) {
		pr_err("v86fs: mount request failed: %d\n", err);
		kfree(sbi);
		return err;
	}

	sbi->root_inode_id = root_id;

	root_inode = v86fs_make_inode(sb, root_id, root_mode);
	if (!root_inode) {
		kfree(sbi);
		return -ENOMEM;
	}

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		kfree(sbi);
		return -ENOMEM;
	}

	return 0;
}

static int v86fs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, v86fs_fill_super);
}

static void v86fs_free_fc(struct fs_context *fc)
{
	struct v86fs_mount_opts *opts = fc->fs_private;

	if (opts) {
		kfree(opts->root_name);
		kfree(opts);
	}
}

static const struct fs_context_operations v86fs_context_ops = {
	.parse_param	= v86fs_parse_param,
	.get_tree	= v86fs_get_tree,
	.free		= v86fs_free_fc,
};

static int v86fs_init_fs_context(struct fs_context *fc)
{
	struct v86fs_mount_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	fc->fs_private = opts;
	fc->ops = &v86fs_context_ops;
	return 0;
}

static void v86fs_kill_sb(struct super_block *sb)
{
	struct v86fs_sb_info *sbi = sb->s_fs_info;

	kill_anon_super(sb);
	kfree(sbi);
}

static struct file_system_type v86fs_type = {
	.name		= "v86fs",
	.init_fs_context = v86fs_init_fs_context,
	.kill_sb	= v86fs_kill_sb,
	.parameters	= v86fs_param_spec,
	.owner		= THIS_MODULE,
};

/*
 * Virtio device
 */

static void v86fs_vq_done(struct virtqueue *vq)
{
	struct v86fs_device *v86dev = vq->vdev->priv;
	void *buf;
	unsigned int len;

	while ((buf = virtqueue_get_buf(vq, &len)) != NULL)
		;
	complete(&v86dev->req_done);
}

static int v86fs_init_vqs(struct v86fs_device *v86dev)
{
	struct virtqueue *vqs[V86FS_VQ_MAX];
	struct virtqueue_info vqs_info[V86FS_VQ_MAX] = {};
	int err;

	vqs_info[V86FS_VQ_HIPRIQ].callback = v86fs_vq_done;
	vqs_info[V86FS_VQ_HIPRIQ].name = "hipriq";

	vqs_info[V86FS_VQ_REQUESTQ].callback = v86fs_vq_done;
	vqs_info[V86FS_VQ_REQUESTQ].name = "requestq";

	vqs_info[V86FS_VQ_NOTIFYQ].callback = v86fs_vq_done;
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
	mutex_init(&v86dev->req_lock);
	init_completion(&v86dev->req_done);

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
