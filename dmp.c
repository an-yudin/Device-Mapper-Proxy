#include <linux/device-mapper.h>

static DECLARE_RWSEM(_lock);

static char* stat_buf;
static struct kobject* kobj;

struct dmp_c {
        struct dm_dev *dev;
        sector_t start;
};

// can be overflowed, but there are no checks (for simplicity)
static unsigned long long read_reqs, read_avg_size;
static unsigned long long write_reqs, write_avg_size;

static ssize_t stat_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	down_read(&_lock);
	strncpy(buf, stat_buf, PAGE_SIZE - 1);
	up_read(&_lock);
        return PAGE_SIZE - 1;
}

static ssize_t stat_store(struct kobject *kobj, struct kobj_attribute *attr,const char *buf, size_t count)
{
        // doing nothing for now
        return 0;
}

// permission 0644 - allow write from root and read from anyone
static struct kobj_attribute stat_attr = __ATTR(volumes, 0644, stat_show, stat_store);

static size_t how_many_digits(unsigned long long number)
{
	size_t res = (number == 0);
	while (number != 0) {
		number /= 10;
		res++;
	}
	return res;
}

static void save_to_buf(void)
{
	size_t offset = 13; // 13 is the length of "read:\n reqs: " (line 178)
	sprintf(stat_buf + offset, "%llu\n avg size: ", read_reqs);
	
	offset += how_many_digits(read_reqs) + 12;
	sprintf(stat_buf + offset, "%llu\nwrite:\n reqs: ", read_avg_size);
	
	offset += how_many_digits(read_avg_size) + 15;
	sprintf(stat_buf + offset, "%llu\n avg size: ", write_reqs);
	
	offset += how_many_digits(write_reqs) + 12;
	sprintf(stat_buf + offset, "%llu\ntotal:\n reqs: ", write_avg_size);
	
	offset += how_many_digits(write_avg_size) + 15;
	sprintf(stat_buf + offset, "%llu\n avg size: ", read_reqs + write_reqs);
	
	offset += how_many_digits(read_reqs + write_reqs) + 12;
	if (read_reqs + write_reqs == 0) {
		sprintf(stat_buf + offset, "%i\n", 0);
	}
	else {
		sprintf(stat_buf + offset, "%llu\n", (read_avg_size * read_reqs + write_avg_size * write_reqs) / (read_reqs + write_reqs));
	}
}

static int dmp_get_args(struct dm_target *ti, unsigned int argc, char **argv)
{
	unsigned long long start = 0; // device's start sector
	struct dmp_c *dmpc;
	int ret;
	
	dmpc = (struct dmp_c *)kzalloc(sizeof(struct dmp_c), GFP_KERNEL);
	if (!dmpc) {
		ti->error = "Cannot allocate dmp context";
		return -ENOMEM;
	}
	
	dmpc->start = (sector_t)start;
	
	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &dmpc->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		kfree(dmpc);
		return ret;
	}

	ti->private = dmpc;
	
	return 0;
}

static int dmp_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	if (argc == 1) {
		int ret = dmp_get_args(ti, argc, argv);
		if (ret) {
			return ret;
		}
	}
	else {
		ti->error = "One argument required";
		return -EINVAL;
	}

	return 0;
}

static void dmp_dtr(struct dm_target *ti)
{
	struct dmp_c *dmpc = (struct dmp_c *)ti->private;
	
	if (dmpc) {
		dm_put_device(ti, dmpc->dev);
		kfree(dmpc);
	}
}

static int dmp_map(struct dm_target *ti, struct bio *bio)
{
	unsigned long long segs = 0;
	unsigned int len = 0;
	struct bio_vec bv;
	struct bvec_iter iter;
	struct dmp_c *dmpc = (struct dmp_c *)ti->private;

	bio_set_dev(bio, dmpc->dev->bdev);
	
	bio_for_each_segment(bv, bio, iter) {
			segs++;
			len += bv.bv_len;
	}
	
	switch (bio_op(bio)) {
	case REQ_OP_READ:
		if (bio->bi_opf & REQ_RAHEAD) {
			return DM_MAPIO_KILL;
		}
		read_avg_size = (read_avg_size * read_reqs + len) / (read_reqs + segs);
		read_reqs += segs;
		break;
	case REQ_OP_WRITE:
		write_avg_size = (write_avg_size * write_reqs + len) / (write_reqs + segs);
		write_reqs += segs;
		break;
	default:
		return DM_MAPIO_KILL;
	}
	
	save_to_buf();

	submit_bio(bio);

	return DM_MAPIO_SUBMITTED;
}

static struct target_type dmp_target = {
	.name = "dmp",
	.version = {1,0,0},
	.module = THIS_MODULE,
	.ctr = dmp_ctr,
	.dtr = dmp_dtr,
	.map = dmp_map,
};

static int sysfs_stat_init(void)
{
	struct kobject mod_kobj = (((struct module *)(THIS_MODULE))->mkobj).kobj;

	stat_buf = (char*)kzalloc(PAGE_SIZE - 1, GFP_KERNEL);
	if (!stat_buf) {
		printk(KERN_ERR "Cannot allocate memory for stat_buf\n");
		return -ENOMEM;
	}
	sprintf(stat_buf, "%s", "read:\n reqs: ");
	save_to_buf();
	
	kobj = kobject_create_and_add("stat", &mod_kobj);
	if (!kobj) {
		printk(KERN_ERR "kobject_create_and_add failed\n");
		kfree(stat_buf);
		return -ENOMSG;
	}
	
	if (sysfs_create_file(kobj, &stat_attr.attr)) {
		printk(KERN_ERR "sysfs_create_file failed\n");
		kobject_put(kobj);
		kfree(stat_buf);
		return -ENOMSG;
	}
	
	return 0;
}

static void sysfs_stat_exit(void)
{
	if (kobj) {
		kobject_put(kobj);
	}
	if (stat_buf) {
		kfree(stat_buf);
	}
}

static int __init dmp_init(void)
{
	int ret = sysfs_stat_init();
	// TODO : check ret
	return dm_register_target(&dmp_target);
}

static void __exit dmp_exit(void)
{
	sysfs_stat_exit();
	dm_unregister_target(&dmp_target);
}


module_init(dmp_init);
module_exit(dmp_exit);
MODULE_LICENSE("GPL");
