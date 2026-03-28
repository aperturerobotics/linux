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
#define V86FS_MSG_READDIR   0x03

/* Response types (type | 0x80) */
#define V86FS_MSG_MOUNT_R   0x80
#define V86FS_MSG_LOOKUP_R  0x81
#define V86FS_MSG_GETATTR_R 0x82
#define V86FS_MSG_READDIR_R 0x83
#define V86FS_MSG_ERROR_R   0xFF

/* Status codes */
#define V86FS_STATUS_OK     0
#define V86FS_STATUS_ENOENT 2

/* Message header: 4B length + 1B type + 2B tag = 7 bytes */
#define V86FS_HDR_SIZE 7

/* Max message buffer size */
#define V86FS_MSG_MAX 4096

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

static void v86fs_pack_u16(u8 *buf, u16 val)
{
	buf[0] = val & 0xFF;
	buf[1] = (val >> 8) & 0xFF;
}

static void v86fs_pack_u64(u8 *buf, u64 val)
{
	buf[0] = val & 0xFF;
	buf[1] = (val >> 8) & 0xFF;
	buf[2] = (val >> 16) & 0xFF;
	buf[3] = (val >> 24) & 0xFF;
	buf[4] = (val >> 32) & 0xFF;
	buf[5] = (val >> 40) & 0xFF;
	buf[6] = (val >> 48) & 0xFF;
	buf[7] = (val >> 56) & 0xFF;
}

static u32 v86fs_read_u32(const u8 *buf)
{
	return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

static u64 v86fs_read_u64(const u8 *buf)
{
	return (u64)v86fs_read_u32(buf) | ((u64)v86fs_read_u32(buf + 4) << 32);
}

static u16 v86fs_read_u16(const u8 *buf)
{
	return buf[0] | (buf[1] << 8);
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

	return 0;
}

/*
 * Send MOUNT message and get root inode info.
 */
static int v86fs_mount_request(struct v86fs_device *v86dev,
			       const char *root_name,
			       u64 *root_id, u32 *root_mode)
{
	u8 req[V86FS_HDR_SIZE + 2 + NAME_MAX];
	u8 resp[64];
	u16 name_len;
	u32 total_len;
	u32 status;
	int err;

	name_len = root_name ? strlen(root_name) : 0;
	total_len = V86FS_HDR_SIZE + 2 + name_len;

	v86fs_pack_header(req, total_len, V86FS_MSG_MOUNT, 0);
	v86fs_pack_u16(&req[7], name_len);
	if (name_len)
		memcpy(&req[9], root_name, name_len);

	memset(resp, 0, sizeof(resp));
	err = v86fs_request(v86dev, V86FS_VQ_REQUESTQ,
			    req, total_len, resp, sizeof(resp));
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

/* Forward declarations */
static struct dentry *v86fs_lookup(struct inode *, struct dentry *,
				   unsigned int);
static int v86fs_readdir(struct file *, struct dir_context *);
static int v86fs_getattr(struct mnt_idmap *, const struct path *,
			 struct kstat *, u32, unsigned int);

static const struct inode_operations v86fs_dir_inode_ops = {
	.lookup		= v86fs_lookup,
	.getattr	= v86fs_getattr,
};

static const struct inode_operations v86fs_file_inode_ops = {
	.getattr	= v86fs_getattr,
};

static const struct file_operations v86fs_dir_fops = {
	.read		= generic_read_dir,
	.iterate_shared	= v86fs_readdir,
	.llseek		= generic_file_llseek,
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
		inode->i_fop = &v86fs_dir_fops;
		inc_nlink(inode);
		break;
	case S_IFREG:
		inode->i_op = &v86fs_file_inode_ops;
		break;
	}

	return inode;
}

/*
 * LOOKUP: resolve a child name in a directory.
 * Sends LOOKUP on hipriq, creates inode from response.
 *
 * LOOKUP request:  [7B hdr] [8B parent_id] [2B name_len] [name...]
 * LOOKUP_R reply:  [7B hdr] [4B status] [8B inode_id] [4B mode] [8B size]
 */
static struct dentry *v86fs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct v86fs_sb_info *sbi = dir->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8 + 2 + NAME_MAX];
	u8 resp[64];
	const char *name = dentry->d_name.name;
	u16 name_len = dentry->d_name.len;
	u32 total_len;
	u32 status, mode;
	u64 ino, size;
	struct inode *inode;
	int err;

	total_len = V86FS_HDR_SIZE + 8 + 2 + name_len;

	v86fs_pack_header(req, total_len, V86FS_MSG_LOOKUP, 0);
	v86fs_pack_u64(&req[7], dir->i_ino);
	v86fs_pack_u16(&req[15], name_len);
	memcpy(&req[17], name, name_len);

	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ,
			    req, total_len, resp, sizeof(resp));
	if (err)
		return ERR_PTR(err);

	if (resp[4] != V86FS_MSG_LOOKUP_R)
		return ERR_PTR(-EIO);

	status = v86fs_read_u32(&resp[7]);
	if (status == V86FS_STATUS_ENOENT) {
		d_add(dentry, NULL);
		return NULL;
	}
	if (status != V86FS_STATUS_OK)
		return ERR_PTR(-EIO);

	ino = v86fs_read_u64(&resp[11]);
	mode = v86fs_read_u32(&resp[19]);
	size = v86fs_read_u64(&resp[23]);

	inode = v86fs_make_inode(dir->i_sb, ino, mode);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	i_size_write(inode, size);
	d_add(dentry, inode);
	return NULL;
}

/*
 * READDIR: list entries in a directory.
 * Sends READDIR on requestq, emits entries via dir_context.
 *
 * READDIR request:  [7B hdr] [8B dir_id]
 * READDIR_R reply:  [7B hdr] [4B status] [4B count]
 *   per entry:      [8B inode_id] [1B type] [2B name_len] [name...]
 */
static int v86fs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct v86fs_sb_info *sbi = dir->i_sb->s_fs_info;
	u8 *req, *resp;
	u32 total_len, status, count;
	int err, buf_off;
	loff_t entry_pos;
	u32 i;

	if (!dir_emit_dots(file, ctx))
		return 0;

	req = kmalloc(V86FS_MSG_MAX, GFP_KERNEL);
	resp = kmalloc(V86FS_MSG_MAX, GFP_KERNEL);
	if (!req || !resp) {
		kfree(req);
		kfree(resp);
		return -ENOMEM;
	}

	/* Build READDIR: [7B hdr] [8B dir_id] */
	total_len = V86FS_HDR_SIZE + 8;
	v86fs_pack_header(req, total_len, V86FS_MSG_READDIR, 0);
	v86fs_pack_u64(&req[7], dir->i_ino);

	memset(resp, 0, V86FS_MSG_MAX);
	err = v86fs_request(sbi->v86dev, V86FS_VQ_REQUESTQ,
			    req, total_len, resp, V86FS_MSG_MAX);
	if (err)
		goto out;

	err = 0;
	if (resp[4] != V86FS_MSG_READDIR_R) {
		err = -EIO;
		goto out;
	}

	status = v86fs_read_u32(&resp[7]);
	if (status != V86FS_STATUS_OK) {
		err = -EIO;
		goto out;
	}

	count = v86fs_read_u32(&resp[11]);
	buf_off = 15; /* after hdr(7) + status(4) + count(4) */
	entry_pos = 2; /* after . and .. */

	for (i = 0; i < count; i++) {
		u64 ino;
		u8 type;
		u16 name_len;

		if (buf_off + 11 > V86FS_MSG_MAX)
			break;

		ino = v86fs_read_u64(&resp[buf_off]);
		type = resp[buf_off + 8];
		name_len = v86fs_read_u16(&resp[buf_off + 9]);
		buf_off += 11;

		if (buf_off + name_len > V86FS_MSG_MAX)
			break;

		if (ctx->pos <= entry_pos) {
			if (!dir_emit(ctx, (char *)&resp[buf_off],
				      name_len, ino, type)) {
				goto out;
			}
			ctx->pos = entry_pos + 1;
		}

		buf_off += name_len;
		entry_pos++;
	}

out:
	kfree(req);
	kfree(resp);
	return err;
}

/*
 * GETATTR: get inode attributes from host.
 * Sends GETATTR on hipriq, updates inode stat.
 *
 * GETATTR request:  [7B hdr] [8B inode_id]
 * GETATTR_R reply:  [7B hdr] [4B status] [4B mode] [8B size]
 *                   [8B mtime_sec] [4B mtime_nsec]
 */
static int v86fs_getattr(struct mnt_idmap *idmap, const struct path *path,
			 struct kstat *stat, u32 request_mask,
			 unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct v86fs_sb_info *sbi = inode->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8];
	u8 resp[64];
	u32 total_len, status, mode;
	u64 size, mtime_sec;
	u32 mtime_nsec;
	int err;

	total_len = V86FS_HDR_SIZE + 8;
	v86fs_pack_header(req, total_len, V86FS_MSG_GETATTR, 0);
	v86fs_pack_u64(&req[7], inode->i_ino);

	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ,
			    req, total_len, resp, sizeof(resp));
	if (err)
		return err;

	if (resp[4] != V86FS_MSG_GETATTR_R)
		return -EIO;

	status = v86fs_read_u32(&resp[7]);
	if (status != V86FS_STATUS_OK)
		return -EIO;

	mode = v86fs_read_u32(&resp[11]);
	size = v86fs_read_u64(&resp[15]);
	mtime_sec = v86fs_read_u64(&resp[23]);
	mtime_nsec = v86fs_read_u32(&resp[31]);

	inode->i_mode = mode;
	i_size_write(inode, size);
	inode_set_mtime(inode, mtime_sec, mtime_nsec);

	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
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
