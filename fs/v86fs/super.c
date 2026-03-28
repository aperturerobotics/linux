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
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>

#define V86FS_VIRTIO_ID 63

#define V86FS_VQ_HIPRIQ   0
#define V86FS_VQ_REQUESTQ 1
#define V86FS_VQ_NOTIFYQ  2
#define V86FS_VQ_MAX      3

struct v86fs_device {
	struct virtio_device *vdev;
	struct virtqueue *vqs[V86FS_VQ_MAX];
};

static void v86fs_hipriq_cb(struct virtqueue *vq)
{
	pr_debug("v86fs: hipriq callback\n");
}

static void v86fs_requestq_cb(struct virtqueue *vq)
{
	pr_debug("v86fs: requestq callback\n");
}

static void v86fs_notifyq_cb(struct virtqueue *vq)
{
	pr_debug("v86fs: notifyq callback\n");
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

	pr_info("v86fs: probed, %d virtqueues ready\n", V86FS_VQ_MAX);
	return 0;

err_free:
	kfree(v86dev);
	return err;
}

static void v86fs_remove(struct virtio_device *vdev)
{
	struct v86fs_device *v86dev = vdev->priv;

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
	return register_virtio_driver(&v86fs_driver);
}

static void __exit v86fs_exit(void)
{
	unregister_virtio_driver(&v86fs_driver);
}

module_init(v86fs_init);
module_exit(v86fs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("v86 host filesystem via custom virtio device");
