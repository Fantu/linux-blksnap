// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME "-snapimage: " fmt
#include <linux/slab.h>
#include <linux/cdrom.h>
#include <linux/blk-mq.h>
#include <uapi/linux/blksnap.h>
#include "snapimage.h"
#include "diff_area.h"
#include "chunk.h"
#include "cbt_map.h"

#define NR_SNAPIMAGE_DEVT	(1 << MINORBITS)

struct submit_event {
	struct list_head link;
	struct bio *bio;
};

static unsigned int _major;
static DEFINE_IDA(snapimage_devt_ida);

struct bio_set snapimage_bioset;

static void bio_process(struct snapimage *snapimage, struct bio *bio)
{
	struct diff_area_image_ctx io_ctx;
	struct bio_vec bvec;
	struct bvec_iter iter;
	sector_t pos = bio->bi_iter.bi_sector;

	diff_area_throttling_io(snapimage->diff_area);
	diff_area_image_ctx_init(&io_ctx, snapimage->diff_area,
				 op_is_write(bio_op(bio)));
	bio_for_each_segment(bvec, bio, iter) {
		blk_status_t st;

		st = diff_area_image_io(&io_ctx, &bvec, &pos);
		if (unlikely(st != BLK_STS_OK))
			break;
	}
	diff_area_image_ctx_done(&io_ctx);
	bio_endio(bio);
}

static inline bool submit_bio_have(struct snapimage *snapimage)
{
	bool ret;

	spin_lock(&snapimage->submit_list_lock);
	ret = !list_empty(&snapimage->submit_list);
	spin_unlock(&snapimage->submit_list_lock);

	return ret;
}

static inline struct bio *submit_bio_pop(struct snapimage *snapimage)
{
	struct bio *bio;
	struct submit_event *ev;

	spin_lock(&snapimage->submit_list_lock);
	ev = list_first_entry_or_null(&snapimage->submit_list,
				      struct submit_event, link);
	if (ev) {
		bio = ev->bio;
		list_del(&ev->link);
	} else
		bio = NULL;
	spin_unlock(&snapimage->submit_list_lock);

	kfree(ev);

	return bio;
}

static inline int submit_bio_push(struct snapimage *snapimage, struct bio *bio,
				  gfp_t gfp)
{
	struct submit_event *ev;

	ev = kmalloc(sizeof(struct submit_event), gfp);
	if (!ev)
		return -ENOMEM;

	INIT_LIST_HEAD(&ev->link);
	ev->bio = bio;

	spin_lock(&snapimage->submit_list_lock);
	list_add_tail(&ev->link, &snapimage->submit_list);
	spin_unlock(&snapimage->submit_list_lock);

	return 0;
}

static int snapimage_submit_worker_fn(void *data)
{
	struct snapimage *snapimage = data;
	struct bio *bio;

	pr_debug("Worker for the device [%d:%d] started",
		MAJOR(snapimage->image_dev_id), MINOR(snapimage->image_dev_id));

	set_user_nice(current, MIN_NICE);
	current->flags |= PF_LOCAL_THROTTLE | PF_MEMALLOC_NOIO;

	while (!kthread_should_stop()) {
		wait_event_interruptible_timeout(snapimage->submit_waitqueue,
			submit_bio_have(snapimage) || kthread_should_stop(),
			5 * HZ);

		while ((bio = submit_bio_pop(snapimage)))
			bio_process(snapimage, bio);

		schedule();
	};

	pr_debug("Delete device [%d:%d]",
		MAJOR(snapimage->image_dev_id), MINOR(snapimage->image_dev_id));

	del_gendisk(snapimage->disk);

	while ((bio = submit_bio_pop(snapimage)))
		bio_process(snapimage, bio);

	pr_debug("Worker for the device [%d:%d] stopped",
		MAJOR(snapimage->image_dev_id), MINOR(snapimage->image_dev_id));

	return 0;
}

static void snapimage_submit_bio(struct bio *bio)
{
	struct snapimage *snapimage = bio->bi_bdev->bd_disk->private_data;
	gfp_t gfp = GFP_NOIO;

	if (bio->bi_opf & REQ_NOWAIT)
		gfp |= GFP_NOWAIT;

	if (!snapimage->is_ready) {
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return;
	}

	if (!submit_bio_push(snapimage, bio, gfp)) {
		wake_up(&snapimage->submit_waitqueue);
		return;
	}

	if (bio->bi_opf & REQ_NOWAIT)
		bio->bi_status = BLK_STS_AGAIN;
	else
		bio->bi_status = BLK_STS_IOERR;
	bio_endio(bio);
}

const struct block_device_operations bd_ops = {
	.owner = THIS_MODULE,
	.submit_bio = snapimage_submit_bio
};

void snapimage_free(struct snapimage *snapimage)
{
	pr_info("Snapshot image disk [%u:%u] delete\n",
		MAJOR(snapimage->image_dev_id), MINOR(snapimage->image_dev_id));

	blk_mq_freeze_queue(snapimage->disk->queue);
	snapimage->is_ready = false;
	kthread_stop(snapimage->submit_task);
	blk_mq_unfreeze_queue(snapimage->disk->queue);

	put_disk(snapimage->disk);

	diff_area_put(snapimage->diff_area);
	cbt_map_put(snapimage->cbt_map);

	ida_free(&snapimage_devt_ida, MINOR(snapimage->image_dev_id));
	kfree(snapimage);
}

struct snapimage *snapimage_create(struct diff_area *diff_area,
				   struct cbt_map *cbt_map)
{
	int ret = 0;
	int minor;
	struct snapimage *snapimage = NULL;
	struct gendisk *disk;
	struct task_struct *task;

	snapimage = kzalloc(sizeof(struct snapimage), GFP_KERNEL);
	if (snapimage == NULL)
		return ERR_PTR(-ENOMEM);

	minor = ida_alloc_range(&snapimage_devt_ida, 0, NR_SNAPIMAGE_DEVT - 1,
				GFP_KERNEL);
	if (minor < 0) {
		ret = minor;
		pr_err("Failed to allocate minor for snapshot image device. errno=%d\n",
		       abs(ret));
		goto fail_free_image;
	}

	snapimage->is_ready = true;
	snapimage->capacity = cbt_map->device_capacity;
	snapimage->image_dev_id = MKDEV(_major, minor);
	pr_info("Create snapshot image device [%u:%u] for original device [%u:%u]\n",
		MAJOR(snapimage->image_dev_id),
		MINOR(snapimage->image_dev_id),
		MAJOR(diff_area->orig_bdev->bd_dev),
		MINOR(diff_area->orig_bdev->bd_dev));

	INIT_LIST_HEAD(&snapimage->submit_list);
	spin_lock_init(&snapimage->submit_list_lock);
	init_waitqueue_head(&snapimage->submit_waitqueue);

	task = kthread_create(snapimage_submit_worker_fn, snapimage,
			   BLK_SNAP_IMAGE_NAME "%d",
			   MINOR(snapimage->image_dev_id));
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		pr_err("Failed to create thread '%s%d'\n",
			BLK_SNAP_IMAGE_NAME, minor);
		goto fail_free_minor;
	}
	snapimage->submit_task = task;

	disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!disk) {
		pr_err("Failed to allocate disk\n");
		ret = -ENOMEM;
		goto fail_free_worker;
	}
	snapimage->disk = disk;

	blk_queue_max_hw_sectors(disk->queue, BLK_DEF_MAX_SECTORS);
	blk_queue_flag_set(QUEUE_FLAG_NOMERGES, disk->queue);

	if (snprintf(disk->disk_name, DISK_NAME_LEN, "%s%d",
		     BLK_SNAP_IMAGE_NAME, minor) < 0) {
		pr_err("Unable to set disk name for snapshot image device: invalid minor %u\n",
		       minor);
		ret = -EINVAL;
		goto fail_cleanup_disk;
	}
	pr_debug("Snapshot image disk name [%s]\n", disk->disk_name);

	disk->flags = 0;
#ifdef GENHD_FL_NO_PART_SCAN
	disk->flags |= GENHD_FL_NO_PART_SCAN;
#else
	disk->flags |= GENHD_FL_NO_PART;
#endif
	disk->major = _major;
	disk->first_minor = minor;
	disk->minors = 1; /* One disk has only one partition */

	disk->fops = &bd_ops;
	disk->private_data = snapimage;

	set_capacity(disk, snapimage->capacity);
	pr_debug("Snapshot image device capacity %lld bytes\n",
		 (u64)(snapimage->capacity << SECTOR_SHIFT));

	diff_area_get(diff_area);
	snapimage->diff_area = diff_area;
	cbt_map_get(cbt_map);
	snapimage->cbt_map = cbt_map;

	pr_debug("Add device [%d:%d]",
		MAJOR(snapimage->image_dev_id), MINOR(snapimage->image_dev_id));
	ret = add_disk(disk);
	if (ret) {
		pr_err("Failed to add disk [%s] for snapshot image device\n",
		       disk->disk_name);
		goto fail_cleanup_disk;
	}

	wake_up_process(snapimage->submit_task);

	return snapimage;

fail_cleanup_disk:
	put_disk(disk);
fail_free_worker:
	kthread_stop(snapimage->submit_task);
fail_free_minor:
	ida_free(&snapimage_devt_ida, minor);
fail_free_image:
	kfree(snapimage);

	return ERR_PTR(ret);
}

int snapimage_init(void)
{
	int mj;

	mj = register_blkdev(0, BLK_SNAP_IMAGE_NAME);
	if (mj < 0) {
		pr_err("Failed to register snapshot image block device\n");
		return mj;
	}

	_major = mj;
	pr_info("Snapshot image block device major %d was registered\n", mj);

	return 0;
}

void snapimage_done(void)
{
	unregister_blkdev(_major, BLK_SNAP_IMAGE_NAME);
	pr_info("Snapshot image block device [%d] was unregistered\n", _major);
}

int snapimage_major(void)
{
	return _major;
}
