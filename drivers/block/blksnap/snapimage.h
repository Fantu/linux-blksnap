/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __BLK_SNAP_SNAPIMAGE_H
#define __BLK_SNAP_SNAPIMAGE_H

#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/kthread.h>

struct diff_area;
struct cbt_map;

/**
 * struct snapimage - Snapshot image block device.
 *
 * @image_dev_id:
 *	ID of the snapshot image block device.
 * @capacity:
 *	The size of the snapshot image in sectors must be equal to the size
 *	of the original device at the time of taking the snapshot.
 * @is_ready:
 *	The flag means that the snapshot image is ready for processing
 *	I/O items.
 * @worker:
 *	The worker thread for processing I/O items.
 * @submit_task:
 *	A pointer to the &struct task of the I/O items processor thread.
 * @submit_list:
 *	A list of I/O items scheduled for processing.
 * @submit_list_lock:
 *	Spinlock ensures the integrity of the list during multithreaded access.
 * @submit_waitqueue:
 *	Provides scheduling of the I/O items processing task.
 * @disk:
 *	A pointer to the &struct gendisk for the image block device.
 * @diff_area:
 *	A pointer to the owned &struct diff_area.
 * @cbt_map:
 *	A pointer to the owned &struct cbt_map.
 *
 * The snapshot image is presented in the system as a block device. But
 * when reading or writing a snapshot image, the data is redirected to
 * the original block device or to the block device of the difference storage.
 *
 * The module does not prohibit reading and writing data to the snapshot
 * from different threads in parallel. To avoid the problem with simultaneous
 * access, it is enough to open the snapshot image block device with the
 * FMODE_EXCL parameter.
 */
struct snapimage {
	dev_t image_dev_id;
	sector_t capacity;
	bool is_ready;

	struct task_struct *submit_task;
	struct list_head submit_list;
	spinlock_t submit_list_lock;
	wait_queue_head_t submit_waitqueue;

	struct gendisk *disk;

	struct diff_area *diff_area;
	struct cbt_map *cbt_map;
};

int snapimage_init(void);
void snapimage_done(void);
int snapimage_major(void);

void snapimage_free(struct snapimage *snapimage);
struct snapimage *snapimage_create(struct diff_area *diff_area,
				   struct cbt_map *cbt_map);
#endif /* __BLK_SNAP_SNAPIMAGE_H */
