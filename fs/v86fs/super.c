// SPDX-License-Identifier: GPL-2.0
/*
 * v86fs - v86 host filesystem via custom virtio device
 */

#include <linux/backing-dev.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/writeback.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>

#define V86FS_VIRTIO_ID 63
#define V86FS_MAGIC     0x56383646

#define V86FS_VQ_HIPRIQ   0
#define V86FS_VQ_REQUESTQ 1
#define V86FS_VQ_NOTIFYQ  2
#define V86FS_VQ_MAX      3

#define V86FS_MSG_MOUNT     0x00
#define V86FS_MSG_LOOKUP    0x01
#define V86FS_MSG_GETATTR   0x02
#define V86FS_MSG_READDIR   0x03
#define V86FS_MSG_OPEN      0x04
#define V86FS_MSG_CLOSE     0x05
#define V86FS_MSG_READ      0x06
#define V86FS_MSG_CREATE    0x07
#define V86FS_MSG_WRITE     0x08
#define V86FS_MSG_MKDIR     0x09
#define V86FS_MSG_SETATTR   0x0A
#define V86FS_MSG_FSYNC     0x0B
#define V86FS_MSG_UNLINK    0x0C
#define V86FS_MSG_RENAME    0x0D
#define V86FS_MSG_SYMLINK   0x0E
#define V86FS_MSG_READLINK  0x0F
#define V86FS_MSG_STATFS    0x10

#define V86FS_MSG_MOUNT_R   0x80
#define V86FS_MSG_LOOKUP_R  0x81
#define V86FS_MSG_GETATTR_R 0x82
#define V86FS_MSG_READDIR_R 0x83
#define V86FS_MSG_OPEN_R    0x84
#define V86FS_MSG_CLOSE_R   0x85
#define V86FS_MSG_READ_R    0x86
#define V86FS_MSG_CREATE_R  0x87
#define V86FS_MSG_WRITE_R   0x88
#define V86FS_MSG_MKDIR_R   0x89
#define V86FS_MSG_SETATTR_R 0x8A
#define V86FS_MSG_FSYNC_R   0x8B
#define V86FS_MSG_UNLINK_R  0x8C
#define V86FS_MSG_RENAME_R  0x8D
#define V86FS_MSG_SYMLINK_R 0x8E
#define V86FS_MSG_READLINK_R 0x8F
#define V86FS_MSG_STATFS_R  0x90

#define V86FS_STATUS_OK     0
#define V86FS_STATUS_ENOENT 2
#define V86FS_HDR_SIZE      7

struct v86fs_device {
	struct virtio_device *vdev;
	struct virtqueue *vqs[V86FS_VQ_MAX];
	struct mutex req_lock;
	struct completion req_done;
};

struct v86fs_mount_opts { char *root_name; };
struct v86fs_sb_info { struct v86fs_device *v86dev; u64 root_inode_id; };
struct v86fs_file_info { u64 handle_id; };

static struct v86fs_device *v86fs_dev;

static void v86fs_pack_header(u8 *buf, u32 length, u8 type, u16 tag)
{
	buf[0] = length & 0xFF; buf[1] = (length >> 8) & 0xFF;
	buf[2] = (length >> 16) & 0xFF; buf[3] = (length >> 24) & 0xFF;
	buf[4] = type; buf[5] = tag & 0xFF; buf[6] = (tag >> 8) & 0xFF;
}

static void v86fs_pack_u16(u8 *buf, u16 val)
{ buf[0] = val & 0xFF; buf[1] = (val >> 8) & 0xFF; }

static void v86fs_pack_u32(u8 *buf, u32 val)
{ buf[0] = val & 0xFF; buf[1] = (val >> 8) & 0xFF;
  buf[2] = (val >> 16) & 0xFF; buf[3] = (val >> 24) & 0xFF; }

static void v86fs_pack_u64(u8 *buf, u64 val)
{
	int i;
	for (i = 0; i < 8; i++)
		buf[i] = (val >> (i * 8)) & 0xFF;
}

static u16 v86fs_read_u16(const u8 *buf)
{ return buf[0] | (buf[1] << 8); }

static u32 v86fs_read_u32(const u8 *buf)
{ return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24); }

static u64 v86fs_read_u64(const u8 *buf)
{ return (u64)v86fs_read_u32(buf) | ((u64)v86fs_read_u32(buf + 4) << 32); }

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
	sgs[0] = &sg_out; sgs[1] = &sg_in;
	err = virtqueue_add_sgs(vq, sgs, 1, 1, v86dev, GFP_KERNEL);
	if (err) { mutex_unlock(&v86dev->req_lock); return err; }
	virtqueue_kick(vq);
	wait_for_completion(&v86dev->req_done);
	mutex_unlock(&v86dev->req_lock);
	return 0;
}

static int v86fs_mount_request(struct v86fs_device *v86dev,
			       const char *root_name,
			       u64 *root_id, u32 *root_mode)
{
	u8 req[V86FS_HDR_SIZE + 2 + NAME_MAX], resp[64];
	u16 name_len = root_name ? strlen(root_name) : 0;
	u32 total_len = V86FS_HDR_SIZE + 2 + name_len;
	int err;

	v86fs_pack_header(req, total_len, V86FS_MSG_MOUNT, 0);
	v86fs_pack_u16(&req[7], name_len);
	if (name_len) memcpy(&req[9], root_name, name_len);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(v86dev, V86FS_VQ_REQUESTQ, req, total_len, resp, sizeof(resp));
	if (err) return err;
	if (resp[4] != V86FS_MSG_MOUNT_R) return -EIO;
	if (v86fs_read_u32(&resp[7]) != 0) return -EIO;
	*root_id = v86fs_read_u64(&resp[11]);
	*root_mode = v86fs_read_u32(&resp[19]);
	return 0;
}

static int v86fs_write_request(struct v86fs_device *v86dev, u64 inode_id,
			       loff_t offset, const void *data, size_t len)
{
	u32 total_len = V86FS_HDR_SIZE + 8 + 8 + 4 + len;
	u8 *req, resp[32];
	int err;

	req = kmalloc(total_len, GFP_KERNEL);
	if (!req) return -ENOMEM;
	v86fs_pack_header(req, total_len, V86FS_MSG_WRITE, 0);
	v86fs_pack_u64(&req[7], inode_id);
	v86fs_pack_u64(&req[15], offset);
	v86fs_pack_u32(&req[23], len);
	memcpy(&req[27], data, len);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(v86dev, V86FS_VQ_REQUESTQ, req, total_len, resp, sizeof(resp));
	kfree(req);
	if (err) return err;
	if (resp[4] != V86FS_MSG_WRITE_R) return -EIO;
	return v86fs_read_u32(&resp[7]) == V86FS_STATUS_OK ? 0 : -EIO;
}

/* VFS ops - forward declarations */
static struct dentry *v86fs_lookup(struct inode *, struct dentry *, unsigned int);
static int v86fs_readdir(struct file *, struct dir_context *);
static int v86fs_getattr(struct mnt_idmap *, const struct path *, struct kstat *, u32, unsigned int);
static int v86fs_setattr(struct mnt_idmap *, struct dentry *, struct iattr *);
static int v86fs_create(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, bool);
static struct dentry *v86fs_mkdir(struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
static int v86fs_unlink(struct inode *, struct dentry *);
static int v86fs_rmdir(struct inode *, struct dentry *);
static int v86fs_rename(struct mnt_idmap *, struct inode *, struct dentry *,
			struct inode *, struct dentry *, unsigned int);
static int v86fs_symlink(struct mnt_idmap *, struct inode *, struct dentry *, const char *);
static const char *v86fs_get_link(struct dentry *, struct inode *, struct delayed_call *);
static int v86fs_open(struct inode *, struct file *);
static int v86fs_release(struct inode *, struct file *);
static int v86fs_read_folio(struct file *, struct folio *);
static int v86fs_writepages(struct address_space *, struct writeback_control *);
static int v86fs_write_end(const struct kiocb *, struct address_space *, loff_t, unsigned, unsigned, struct folio *, void *);
static int v86fs_fsync(struct file *, loff_t, loff_t, int);
static int v86fs_statfs(struct dentry *, struct kstatfs *);

static const struct super_operations v86fs_super_ops = { .statfs = v86fs_statfs };

static const struct inode_operations v86fs_dir_inode_ops = {
	.lookup = v86fs_lookup, .getattr = v86fs_getattr,
	.setattr = v86fs_setattr, .create = v86fs_create, .mkdir = v86fs_mkdir,
	.unlink = v86fs_unlink, .rmdir = v86fs_rmdir,
	.rename = v86fs_rename, .symlink = v86fs_symlink,
};
static const struct inode_operations v86fs_file_inode_ops = {
	.getattr = v86fs_getattr, .setattr = v86fs_setattr,
};
static const struct inode_operations v86fs_symlink_inode_ops = {
	.get_link = v86fs_get_link, .getattr = v86fs_getattr,
};
static const struct file_operations v86fs_dir_fops = {
	.read = generic_read_dir, .iterate_shared = v86fs_readdir,
	.llseek = generic_file_llseek,
};
static const struct file_operations v86fs_file_fops = {
	.open = v86fs_open, .release = v86fs_release,
	.read_iter = generic_file_read_iter, .write_iter = generic_file_write_iter,
	.fsync = v86fs_fsync, .llseek = generic_file_llseek,
};
static const struct address_space_operations v86fs_aops = {
	.read_folio = v86fs_read_folio, .writepages = v86fs_writepages,
	.write_begin = simple_write_begin, .write_end = v86fs_write_end,
	.dirty_folio = filemap_dirty_folio,
};

static struct inode *v86fs_make_inode(struct super_block *sb, u64 ino, umode_t mode)
{
	struct inode *inode = new_inode(sb);
	if (!inode) return NULL;
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
		inode->i_fop = &v86fs_file_fops;
		inode->i_data.a_ops = &v86fs_aops;
		break;
	case S_IFLNK:
		inode->i_op = &v86fs_symlink_inode_ops;
		break;
	}
	return inode;
}

static struct dentry *v86fs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct v86fs_sb_info *sbi = dir->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8 + 2 + NAME_MAX], resp[64];
	u16 name_len = dentry->d_name.len;
	u32 total_len = V86FS_HDR_SIZE + 8 + 2 + name_len;
	u32 status; u64 ino; struct inode *inode; int err;

	v86fs_pack_header(req, total_len, V86FS_MSG_LOOKUP, 0);
	v86fs_pack_u64(&req[7], dir->i_ino);
	v86fs_pack_u16(&req[15], name_len);
	memcpy(&req[17], dentry->d_name.name, name_len);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, total_len, resp, sizeof(resp));
	if (err) return ERR_PTR(err);
	if (resp[4] != V86FS_MSG_LOOKUP_R) return ERR_PTR(-EIO);
	status = v86fs_read_u32(&resp[7]);
	if (status == V86FS_STATUS_ENOENT) { d_add(dentry, NULL); return NULL; }
	if (status != V86FS_STATUS_OK) return ERR_PTR(-EIO);
	ino = v86fs_read_u64(&resp[11]);
	inode = v86fs_make_inode(dir->i_sb, ino, v86fs_read_u32(&resp[19]));
	if (!inode) return ERR_PTR(-ENOMEM);
	i_size_write(inode, v86fs_read_u64(&resp[23]));
	d_add(dentry, inode);
	return NULL;
}

static int v86fs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct v86fs_sb_info *sbi = dir->i_sb->s_fs_info;
	u32 buf_size = 8192;
	u8 *req, *resp;
	u32 total_len, count, i;
	int err, buf_off;
	loff_t entry_pos;

	if (!dir_emit_dots(file, ctx)) return 0;
	req = kmalloc(buf_size, GFP_KERNEL);
	resp = kmalloc(buf_size, GFP_KERNEL);
	if (!req || !resp) { kfree(req); kfree(resp); return -ENOMEM; }

	total_len = V86FS_HDR_SIZE + 8;
	v86fs_pack_header(req, total_len, V86FS_MSG_READDIR, 0);
	v86fs_pack_u64(&req[7], dir->i_ino);
	memset(resp, 0, buf_size);
	err = v86fs_request(sbi->v86dev, V86FS_VQ_REQUESTQ, req, total_len, resp, buf_size);
	if (err) goto out;
	if (resp[4] != V86FS_MSG_READDIR_R || v86fs_read_u32(&resp[7]) != 0) { err = -EIO; goto out; }

	count = v86fs_read_u32(&resp[11]);
	buf_off = 15; entry_pos = 2;
	for (i = 0; i < count; i++) {
		u64 ino; u8 type; u16 name_len;
		if (buf_off + 11 > (int)buf_size) break;
		ino = v86fs_read_u64(&resp[buf_off]);
		type = resp[buf_off + 8];
		name_len = v86fs_read_u16(&resp[buf_off + 9]);
		buf_off += 11;
		if (buf_off + name_len > (int)buf_size) break;
		if (ctx->pos <= entry_pos) {
			if (!dir_emit(ctx, (char *)&resp[buf_off], name_len, ino, type))
				goto out;
			ctx->pos = entry_pos + 1;
		}
		buf_off += name_len; entry_pos++;
	}
	err = 0;
out:
	kfree(req); kfree(resp);
	return err;
}

static int v86fs_getattr(struct mnt_idmap *idmap, const struct path *path,
			 struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct v86fs_sb_info *sbi = inode->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8], resp[64];
	int err;

	v86fs_pack_header(req, V86FS_HDR_SIZE + 8, V86FS_MSG_GETATTR, 0);
	v86fs_pack_u64(&req[7], inode->i_ino);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, V86FS_HDR_SIZE + 8, resp, sizeof(resp));
	if (err) return err;
	if (resp[4] != V86FS_MSG_GETATTR_R || v86fs_read_u32(&resp[7]) != 0) return -EIO;

	inode->i_mode = v86fs_read_u32(&resp[11]);
	i_size_write(inode, v86fs_read_u64(&resp[15]));
	inode_set_mtime(inode, v86fs_read_u64(&resp[23]), v86fs_read_u32(&resp[31]));
	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

static int v86fs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct v86fs_sb_info *sbi = inode->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8 + 4 + 4 + 8], resp[32];
	int err;

	err = setattr_prepare(idmap, dentry, attr);
	if (err) return err;

	v86fs_pack_header(req, sizeof(req), V86FS_MSG_SETATTR, 0);
	v86fs_pack_u64(&req[7], inode->i_ino);
	v86fs_pack_u32(&req[15], attr->ia_valid);
	v86fs_pack_u32(&req[19], attr->ia_mode);
	v86fs_pack_u64(&req[23], attr->ia_size);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, sizeof(req), resp, sizeof(resp));
	if (err) return err;
	if (resp[4] != V86FS_MSG_SETATTR_R || v86fs_read_u32(&resp[7]) != 0) return -EIO;

	setattr_copy(idmap, inode, attr);
	if (attr->ia_valid & ATTR_SIZE)
		truncate_pagecache(inode, attr->ia_size);
	return 0;
}

static int v86fs_open(struct inode *inode, struct file *file)
{
	struct v86fs_sb_info *sbi = inode->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8 + 4], resp[32];
	struct v86fs_file_info *fi;
	int err;

	fi = kzalloc(sizeof(*fi), GFP_KERNEL);
	if (!fi) return -ENOMEM;
	v86fs_pack_header(req, sizeof(req), V86FS_MSG_OPEN, 0);
	v86fs_pack_u64(&req[7], inode->i_ino);
	v86fs_pack_u32(&req[15], file->f_flags);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, sizeof(req), resp, sizeof(resp));
	if (err || resp[4] != V86FS_MSG_OPEN_R || v86fs_read_u32(&resp[7]) != 0) {
		kfree(fi); return err ? err : -EIO;
	}
	fi->handle_id = v86fs_read_u64(&resp[11]);
	file->private_data = fi;
	return 0;
}

static int v86fs_release(struct inode *inode, struct file *file)
{
	struct v86fs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct v86fs_file_info *fi = file->private_data;
	u8 req[V86FS_HDR_SIZE + 8], resp[32];

	if (!fi) return 0;
	v86fs_pack_header(req, sizeof(req), V86FS_MSG_CLOSE, 0);
	v86fs_pack_u64(&req[7], fi->handle_id);
	memset(resp, 0, sizeof(resp));
	v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, sizeof(req), resp, sizeof(resp));
	kfree(fi); file->private_data = NULL;
	return 0;
}

static int v86fs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct v86fs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct v86fs_file_info *fi = file ? file->private_data : NULL;
	u32 buf_size = PAGE_SIZE + 64;
	u8 *req, *resp;
	loff_t pos = folio_pos(folio);
	size_t len = folio_size(folio);
	u32 bytes_read;
	int err;

	if (pos >= i_size_read(inode)) {
		folio_zero_range(folio, 0, len);
		folio_mark_uptodate(folio); folio_unlock(folio); return 0;
	}
	if (pos + len > i_size_read(inode)) len = i_size_read(inode) - pos;

	req = kmalloc(V86FS_HDR_SIZE + 20, GFP_KERNEL);
	resp = kmalloc(buf_size, GFP_KERNEL);
	if (!req || !resp) { kfree(req); kfree(resp); folio_unlock(folio); return -ENOMEM; }

	v86fs_pack_header(req, V86FS_HDR_SIZE + 20, V86FS_MSG_READ, 0);
	v86fs_pack_u64(&req[7], fi ? fi->handle_id : inode->i_ino);
	v86fs_pack_u64(&req[15], pos);
	v86fs_pack_u32(&req[23], len);
	memset(resp, 0, buf_size);
	err = v86fs_request(sbi->v86dev, V86FS_VQ_REQUESTQ, req, V86FS_HDR_SIZE + 20, resp, buf_size);
	if (err || resp[4] != V86FS_MSG_READ_R || v86fs_read_u32(&resp[7]) != 0)
		goto fail;

	bytes_read = v86fs_read_u32(&resp[11]);
	if (bytes_read > len) bytes_read = len;
	memcpy_to_folio(folio, 0, &resp[15], bytes_read);
	if (bytes_read < folio_size(folio))
		folio_zero_range(folio, bytes_read, folio_size(folio) - bytes_read);
	folio_mark_uptodate(folio); folio_unlock(folio);
	kfree(req); kfree(resp); return 0;
fail:
	folio_unlock(folio); kfree(req); kfree(resp);
	return err ? err : -EIO;
}

static int v86fs_write_end(const struct kiocb *iocb, struct address_space *mapping,
			   loff_t pos, unsigned len, unsigned copied,
			   struct folio *folio, void *fsdata)
{
	struct inode *inode = mapping->host;
	struct v86fs_sb_info *sbi = inode->i_sb->s_fs_info;

	if (!folio_test_uptodate(folio)) {
		if (copied < len)
			folio_zero_range(folio, offset_in_folio(folio, pos) + copied,
					 len - copied);
		folio_mark_uptodate(folio);
	}
	if (pos + copied > inode->i_size)
		i_size_write(inode, pos + copied);

	/* Write-through: send data to host immediately */
	if (copied > 0) {
		void *data = kmap_local_folio(folio, offset_in_folio(folio, pos));
		v86fs_write_request(sbi->v86dev, inode->i_ino, pos, data, copied);
		kunmap_local(data);
	}

	folio_unlock(folio);
	folio_put(folio);
	return copied;
}

static int v86fs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	/* Write-through in write_end, nothing to flush here */
	return 0;
}

static int v86fs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	struct v86fs_sb_info *sbi = inode->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8], resp[32];
	int err;

	err = filemap_write_and_wait_range(file->f_mapping, start, end);
	if (err) return err;
	v86fs_pack_header(req, sizeof(req), V86FS_MSG_FSYNC, 0);
	v86fs_pack_u64(&req[7], inode->i_ino);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, sizeof(req), resp, sizeof(resp));
	if (err) return err;
	return (resp[4] == V86FS_MSG_FSYNC_R && v86fs_read_u32(&resp[7]) == 0) ? 0 : -EIO;
}

static int v86fs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	struct v86fs_sb_info *sbi = dir->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8 + 2 + NAME_MAX + 4], resp[32];
	u16 name_len = dentry->d_name.len;
	u32 total_len = V86FS_HDR_SIZE + 8 + 2 + name_len + 4;
	struct inode *inode;
	int err;

	v86fs_pack_header(req, total_len, V86FS_MSG_CREATE, 0);
	v86fs_pack_u64(&req[7], dir->i_ino);
	v86fs_pack_u16(&req[15], name_len);
	memcpy(&req[17], dentry->d_name.name, name_len);
	v86fs_pack_u32(&req[17 + name_len], mode);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, total_len, resp, sizeof(resp));
	if (err) return err;
	if (resp[4] != V86FS_MSG_CREATE_R || v86fs_read_u32(&resp[7]) != 0) return -EIO;

	inode = v86fs_make_inode(dir->i_sb, v86fs_read_u64(&resp[11]), v86fs_read_u32(&resp[19]));
	if (!inode) return -ENOMEM;
	d_instantiate(dentry, inode);
	return 0;
}

static struct dentry *v86fs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	struct v86fs_sb_info *sbi = dir->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8 + 2 + NAME_MAX + 4], resp[32];
	u16 name_len = dentry->d_name.len;
	u32 total_len = V86FS_HDR_SIZE + 8 + 2 + name_len + 4;
	struct inode *inode;
	int err;

	v86fs_pack_header(req, total_len, V86FS_MSG_MKDIR, 0);
	v86fs_pack_u64(&req[7], dir->i_ino);
	v86fs_pack_u16(&req[15], name_len);
	memcpy(&req[17], dentry->d_name.name, name_len);
	v86fs_pack_u32(&req[17 + name_len], mode | S_IFDIR);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, total_len, resp, sizeof(resp));
	if (err) return ERR_PTR(err);
	if (resp[4] != V86FS_MSG_MKDIR_R || v86fs_read_u32(&resp[7]) != 0) return ERR_PTR(-EIO);

	inode = v86fs_make_inode(dir->i_sb, v86fs_read_u64(&resp[11]), v86fs_read_u32(&resp[19]));
	if (!inode) return ERR_PTR(-ENOMEM);
	inc_nlink(dir);
	d_instantiate(dentry, inode);
	return NULL;
}

static int v86fs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct v86fs_sb_info *sbi = dir->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8 + 2 + NAME_MAX], resp[32];
	u16 name_len = dentry->d_name.len;
	u32 total_len = V86FS_HDR_SIZE + 8 + 2 + name_len;
	int err;

	v86fs_pack_header(req, total_len, V86FS_MSG_UNLINK, 0);
	v86fs_pack_u64(&req[7], dir->i_ino);
	v86fs_pack_u16(&req[15], name_len);
	memcpy(&req[17], dentry->d_name.name, name_len);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, total_len, resp, sizeof(resp));
	if (err) return err;
	if (resp[4] != V86FS_MSG_UNLINK_R || v86fs_read_u32(&resp[7]) != 0) return -EIO;
	drop_nlink(d_inode(dentry));
	return 0;
}

static int v86fs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int err = v86fs_unlink(dir, dentry);
	if (!err) {
		drop_nlink(d_inode(dentry));
		drop_nlink(dir);
	}
	return err;
}

static int v86fs_rename(struct mnt_idmap *idmap,
			struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry,
			unsigned int flags)
{
	struct v86fs_sb_info *sbi = old_dir->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8 + 2 + NAME_MAX + 8 + 2 + NAME_MAX], resp[32];
	u16 old_name_len = old_dentry->d_name.len;
	u16 new_name_len = new_dentry->d_name.len;
	u32 total_len;
	int off, err;

	if (flags & ~RENAME_NOREPLACE) return -EINVAL;

	off = V86FS_HDR_SIZE;
	total_len = off + 8 + 2 + old_name_len + 8 + 2 + new_name_len;
	v86fs_pack_header(req, total_len, V86FS_MSG_RENAME, 0);
	v86fs_pack_u64(&req[off], old_dir->i_ino); off += 8;
	v86fs_pack_u16(&req[off], old_name_len); off += 2;
	memcpy(&req[off], old_dentry->d_name.name, old_name_len); off += old_name_len;
	v86fs_pack_u64(&req[off], new_dir->i_ino); off += 8;
	v86fs_pack_u16(&req[off], new_name_len); off += 2;
	memcpy(&req[off], new_dentry->d_name.name, new_name_len);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, total_len, resp, sizeof(resp));
	if (err) return err;
	if (resp[4] != V86FS_MSG_RENAME_R || v86fs_read_u32(&resp[7]) != 0) return -EIO;

	if (d_really_is_positive(new_dentry)) {
		if (d_is_dir(new_dentry)) {
			drop_nlink(d_inode(new_dentry));
			drop_nlink(old_dir);
		}
		drop_nlink(d_inode(new_dentry));
	}
	if (d_is_dir(old_dentry)) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}
	return 0;
}

static int v86fs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, const char *target)
{
	struct v86fs_sb_info *sbi = dir->i_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE + 8 + 2 + NAME_MAX + 2 + PATH_MAX], resp[32];
	u16 name_len = dentry->d_name.len;
	u16 target_len = strlen(target);
	u32 total_len;
	struct inode *inode;
	int off, err;

	off = V86FS_HDR_SIZE;
	total_len = off + 8 + 2 + name_len + 2 + target_len;
	v86fs_pack_header(req, total_len, V86FS_MSG_SYMLINK, 0);
	v86fs_pack_u64(&req[off], dir->i_ino); off += 8;
	v86fs_pack_u16(&req[off], name_len); off += 2;
	memcpy(&req[off], dentry->d_name.name, name_len); off += name_len;
	v86fs_pack_u16(&req[off], target_len); off += 2;
	memcpy(&req[off], target, target_len);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, total_len, resp, sizeof(resp));
	if (err) return err;
	if (resp[4] != V86FS_MSG_SYMLINK_R || v86fs_read_u32(&resp[7]) != 0) return -EIO;

	inode = v86fs_make_inode(dir->i_sb, v86fs_read_u64(&resp[11]), v86fs_read_u32(&resp[19]));
	if (!inode) return -ENOMEM;
	d_instantiate(dentry, inode);
	return 0;
}

static const char *v86fs_get_link(struct dentry *dentry, struct inode *inode,
				  struct delayed_call *callback)
{
	struct v86fs_sb_info *sbi;
	u8 req[V86FS_HDR_SIZE + 8], resp[V86FS_HDR_SIZE + 4 + 2 + PATH_MAX];
	u16 target_len;
	char *link;
	int err;

	if (!dentry) return ERR_PTR(-ECHILD);
	sbi = inode->i_sb->s_fs_info;
	v86fs_pack_header(req, sizeof(req), V86FS_MSG_READLINK, 0);
	v86fs_pack_u64(&req[7], inode->i_ino);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, sizeof(req), resp, sizeof(resp));
	if (err) return ERR_PTR(err);
	if (resp[4] != V86FS_MSG_READLINK_R || v86fs_read_u32(&resp[7]) != 0) return ERR_PTR(-EIO);

	target_len = v86fs_read_u16(&resp[11]);
	link = kmalloc(target_len + 1, GFP_KERNEL);
	if (!link) return ERR_PTR(-ENOMEM);
	memcpy(link, &resp[13], target_len);
	link[target_len] = '\0';
	set_delayed_call(callback, kfree_link, link);
	return link;
}

static int v86fs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct v86fs_sb_info *sbi = dentry->d_sb->s_fs_info;
	u8 req[V86FS_HDR_SIZE], resp[64];
	int err;

	v86fs_pack_header(req, V86FS_HDR_SIZE, V86FS_MSG_STATFS, 0);
	memset(resp, 0, sizeof(resp));
	err = v86fs_request(sbi->v86dev, V86FS_VQ_HIPRIQ, req, sizeof(req), resp, sizeof(resp));
	if (err) return err;
	if (resp[4] != V86FS_MSG_STATFS_R || v86fs_read_u32(&resp[7]) != 0) return -EIO;

	buf->f_type = V86FS_MAGIC;
	buf->f_blocks = v86fs_read_u64(&resp[11]);
	buf->f_bfree = v86fs_read_u64(&resp[19]);
	buf->f_bavail = v86fs_read_u64(&resp[27]);
	buf->f_files = v86fs_read_u64(&resp[35]);
	buf->f_ffree = v86fs_read_u64(&resp[43]);
	buf->f_bsize = v86fs_read_u32(&resp[51]);
	buf->f_namelen = NAME_MAX;
	return 0;
}

/* Mount infrastructure */
enum v86fs_param { Opt_root };
static const struct fs_parameter_spec v86fs_param_spec[] = {
	fsparam_string("root", Opt_root), {}
};

static int v86fs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct v86fs_mount_opts *opts = fc->fs_private;
	struct fs_parse_result result;
	int opt = fs_parse(fc, v86fs_param_spec, param, &result);
	if (opt < 0) return opt;
	if (opt == Opt_root) { kfree(opts->root_name); opts->root_name = param->string; param->string = NULL; }
	return 0;
}

static int v86fs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct v86fs_mount_opts *opts = fc->fs_private;
	struct v86fs_sb_info *sbi;
	struct inode *root_inode;
	u64 root_id = 1; u32 root_mode = S_IFDIR | 0755;
	int err;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi) return -ENOMEM;
	if (!v86fs_dev) { kfree(sbi); return -ENODEV; }
	sbi->v86dev = v86fs_dev;
	sb->s_maxbytes = MAX_LFS_FILESIZE; sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT; sb->s_magic = V86FS_MAGIC;
	sb->s_op = &v86fs_super_ops; sb->s_time_gran = 1; sb->s_fs_info = sbi;

	err = v86fs_mount_request(sbi->v86dev, opts->root_name, &root_id, &root_mode);
	if (err) { kfree(sbi); return err; }
	sbi->root_inode_id = root_id;
	root_inode = v86fs_make_inode(sb, root_id, root_mode);
	if (!root_inode) { kfree(sbi); return -ENOMEM; }
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) { kfree(sbi); return -ENOMEM; }
	return 0;
}

static int v86fs_get_tree(struct fs_context *fc) { return get_tree_nodev(fc, v86fs_fill_super); }

static void v86fs_free_fc(struct fs_context *fc)
{
	struct v86fs_mount_opts *opts = fc->fs_private;
	if (opts) { kfree(opts->root_name); kfree(opts); }
}

static const struct fs_context_operations v86fs_context_ops = {
	.parse_param = v86fs_parse_param, .get_tree = v86fs_get_tree, .free = v86fs_free_fc,
};

static int v86fs_init_fs_context(struct fs_context *fc)
{
	struct v86fs_mount_opts *opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts) return -ENOMEM;
	fc->fs_private = opts; fc->ops = &v86fs_context_ops;
	return 0;
}

static void v86fs_kill_sb(struct super_block *sb)
{ kill_anon_super(sb); kfree(sb->s_fs_info); }

static struct file_system_type v86fs_type = {
	.name = "v86fs", .init_fs_context = v86fs_init_fs_context,
	.kill_sb = v86fs_kill_sb, .parameters = v86fs_param_spec, .owner = THIS_MODULE,
};

/* Virtio device */
static void v86fs_vq_done(struct virtqueue *vq)
{
	struct v86fs_device *v86dev = vq->vdev->priv;
	unsigned int len; while (virtqueue_get_buf(vq, &len)) ;
	complete(&v86dev->req_done);
}

static int v86fs_probe(struct virtio_device *vdev)
{
	struct v86fs_device *v86dev;
	struct virtqueue *vqs[V86FS_VQ_MAX];
	struct virtqueue_info vqs_info[V86FS_VQ_MAX] = {};
	int err;

	v86dev = kzalloc(sizeof(*v86dev), GFP_KERNEL);
	if (!v86dev) return -ENOMEM;
	v86dev->vdev = vdev; vdev->priv = v86dev;
	mutex_init(&v86dev->req_lock);
	init_completion(&v86dev->req_done);

	vqs_info[0].callback = v86fs_vq_done; vqs_info[0].name = "hipriq";
	vqs_info[1].callback = v86fs_vq_done; vqs_info[1].name = "requestq";
	vqs_info[2].callback = v86fs_vq_done; vqs_info[2].name = "notifyq";
	err = virtio_find_vqs(vdev, V86FS_VQ_MAX, vqs, vqs_info, NULL);
	if (err) { kfree(v86dev); return err; }
	v86dev->vqs[0] = vqs[0]; v86dev->vqs[1] = vqs[1]; v86dev->vqs[2] = vqs[2];

	virtio_device_ready(vdev);
	v86fs_dev = v86dev;
	pr_info("v86fs: probed, %d virtqueues ready\n", V86FS_VQ_MAX);
	return 0;
}

static void v86fs_remove(struct virtio_device *vdev)
{
	v86fs_dev = NULL; vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev); kfree(vdev->priv);
}

static const struct virtio_device_id v86fs_id_table[] = {
	{ V86FS_VIRTIO_ID, VIRTIO_DEV_ANY_ID }, { 0 },
};
static struct virtio_driver v86fs_driver = {
	.driver.name = "v86fs", .id_table = v86fs_id_table,
	.probe = v86fs_probe, .remove = v86fs_remove,
};

static int __init v86fs_init(void)
{
	int err = register_virtio_driver(&v86fs_driver);
	if (err) return err;
	err = register_filesystem(&v86fs_type);
	if (err) { unregister_virtio_driver(&v86fs_driver); return err; }
	pr_info("v86fs: registered\n");
	return 0;
}

static void __exit v86fs_exit(void)
{ unregister_filesystem(&v86fs_type); unregister_virtio_driver(&v86fs_driver); }

module_init(v86fs_init);
module_exit(v86fs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("v86 host filesystem via custom virtio device");
