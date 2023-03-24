// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Ernesto A. Fernández <ernesto@corellium.com>
 */

#include <linux/mount.h>
#include <linux/slab.h>
#include "apfs.h"

/**
 * apfs_create_superblock_snapshot - Take a snapshot of the volume superblock
 * @sb:		superblock structure
 * @bno:	on return, the block number for the new superblock copy
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_create_superblock_snapshot(struct super_block *sb, u64 *bno)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct buffer_head *curr_bh = NULL;
	struct buffer_head *snap_bh = NULL;
	struct apfs_superblock *snap_raw = NULL;
	int err;

	err = apfs_spaceman_allocate_block(sb, bno, true /* backwards */);
	if (err)
		goto fail;

	snap_bh = apfs_getblk(sb, *bno);
	if (!snap_bh) {
		err = -EIO;
		goto fail;
	}
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	le64_add_cpu(&vsb_raw->apfs_fs_alloc_count, 1);

	curr_bh = sbi->s_vobject.o_bh;
	memcpy(snap_bh->b_data, curr_bh->b_data, sb->s_blocksize);
	curr_bh = NULL;

	err = apfs_transaction_join(sb, snap_bh);
	if (err)
		goto fail;
	set_buffer_csum(snap_bh);

	snap_raw = (struct apfs_superblock *)snap_bh->b_data;
	/* Volume superblocks in snapshots are physical objects */
	snap_raw->apfs_o.o_oid = cpu_to_le64p(bno);
	snap_raw->apfs_o.o_type = cpu_to_le32(APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_FS);
	/* The omap is shared with the current volume */
	snap_raw->apfs_omap_oid = 0;
	/* The extent reference tree is given by the snapshot metadata */
	snap_raw->apfs_extentref_tree_oid = 0;
	/* No snapshots inside snapshots */
	snap_raw->apfs_snap_meta_tree_oid = 0;

	err = 0;
fail:
	snap_raw = NULL;
	brelse(snap_bh);
	snap_bh = NULL;
	return err;
}

static int apfs_create_snap_metadata_rec(struct inode *mntpoint, struct apfs_node *snap_root, const char *name, int name_len, u64 sblock_oid)
{
	struct super_block *sb = mntpoint->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_query *query = NULL;
	struct apfs_key key = {0};
	struct apfs_snap_metadata_key raw_key;
	struct apfs_snap_metadata_val *raw_val = NULL;
	int val_len;
	struct timespec64 now;
	u64 xid = APFS_NXI(sb)->nx_xid;
	int err;

	apfs_init_snap_metadata_key(xid, &key);

	query = apfs_alloc_query(snap_root, NULL /* parent */);
	if (!query) {
		err = -ENOMEM;
		goto fail;
	}
	query->key = &key;
	query->flags |= APFS_QUERY_SNAP_META | APFS_QUERY_EXACT;

	err = apfs_btree_query(sb, &query);
	if (err == 0) {
		err = -EFSCORRUPTED;
		goto fail;
	}
	if (err != -ENODATA)
		goto fail;

	apfs_key_set_hdr(APFS_TYPE_SNAP_METADATA, xid, &raw_key);

	val_len = sizeof(*raw_val) + name_len + 1;
	raw_val = kzalloc(val_len, GFP_KERNEL);
	if (!raw_val) {
		err = -ENOMEM;
		goto fail;
	}
	raw_val->extentref_tree_oid = vsb_raw->apfs_extentref_tree_oid;
	raw_val->sblock_oid = cpu_to_le64(sblock_oid);
	now = current_time(mntpoint);
	raw_val->create_time = cpu_to_le64(timespec64_to_ns(&now));
	raw_val->change_time = raw_val->create_time;
	raw_val->inum = 0; /* TODO: what is this? */
	raw_val->extentref_tree_type = vsb_raw->apfs_extentref_tree_type;
	raw_val->flags = 0;
	raw_val->name_len = cpu_to_le16(name_len + 1); /* Count the null byte */
	strcpy(raw_val->name, name);

	err = apfs_btree_insert(query, &raw_key, sizeof(raw_key), raw_val, val_len);
fail:
	kfree(raw_val);
	raw_val = NULL;
	apfs_free_query(query);
	query = NULL;
	return err;
}

static int apfs_create_snap_name_rec(struct apfs_node *snap_root, const char *name, int name_len)
{
	struct super_block *sb = snap_root->object.sb;
	struct apfs_query *query = NULL;
	struct apfs_key key = {0};
	struct apfs_snap_name_key *raw_key = NULL;
	struct apfs_snap_name_val raw_val;
	int key_len;
	int err;

	apfs_init_snap_name_key(name, &key);

	query = apfs_alloc_query(snap_root, NULL /* parent */);
	if (!query) {
		err = -ENOMEM;
		goto fail;
	}
	query->key = &key;
	query->flags |= APFS_QUERY_SNAP_META | APFS_QUERY_EXACT;

	err = apfs_btree_query(sb, &query);
	if (err == 0) {
		/* TODO: avoid transaction abort here */
		apfs_info(sb, "a snapshot with that name already exists");
		err = -EEXIST;
		goto fail;
	}
	if (err != -ENODATA)
		goto fail;

	key_len = sizeof(*raw_key) + name_len + 1;
	raw_key = kzalloc(key_len, GFP_KERNEL);
	if (!raw_key) {
		err = -ENOMEM;
		goto fail;
	}
	apfs_key_set_hdr(APFS_TYPE_SNAP_NAME, APFS_SNAP_NAME_OBJ_ID, raw_key);
	raw_key->name_len = cpu_to_le16(name_len + 1); /* Count the null byte */
	strcpy(raw_key->name, name);

	raw_val.snap_xid = cpu_to_le64(APFS_NXI(sb)->nx_xid);

	err = apfs_btree_insert(query, raw_key, key_len, &raw_val, sizeof(raw_val));
fail:
	kfree(raw_key);
	raw_key = NULL;
	apfs_free_query(query);
	query = NULL;
	return err;
}

static int apfs_create_snap_meta_records(struct inode *mntpoint, const char *name, u64 sblock_oid)
{
	struct super_block *sb = mntpoint->i_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_node *snap_root = NULL;
	size_t name_len;
	int err;

	snap_root = apfs_read_node(sb, le64_to_cpu(vsb_raw->apfs_snap_meta_tree_oid), APFS_OBJ_PHYSICAL, true /* write */);
	if (IS_ERR(snap_root))
		return PTR_ERR(snap_root);
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	vsb_raw->apfs_snap_meta_tree_oid = cpu_to_le64(snap_root->object.oid);

	name_len = strlen(name);
	if (name_len > APFS_SNAP_MAX_NAMELEN) {
		err = -EFSCORRUPTED;
		goto fail;
	}

	err = apfs_create_snap_metadata_rec(mntpoint, snap_root, name, name_len, sblock_oid);
	if (err)
		goto fail;
	err = apfs_create_snap_name_rec(snap_root, name, name_len);

fail:
	apfs_node_free(snap_root);
	return err;
}

static int apfs_create_new_extentref_tree(struct super_block *sb)
{
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	u64 oid;
	int err;

	err = apfs_make_empty_btree_root(sb, APFS_OBJECT_TYPE_BLOCKREFTREE, &oid);
	if (err)
		return err;

	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	vsb_raw->apfs_extentref_tree_oid = cpu_to_le64(oid);
	return 0;
}

/**
 * apfs_update_omap_snap_tree - Add the current xid to the omap's snapshot tree
 * @sb:		filesystem superblock
 * @oid_p:	pointer to the on-disk block number for the root node
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_update_omap_snap_tree(struct super_block *sb, __le64 *oid_p)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_node *root = NULL;
	u64 oid = le64_to_cpup(oid_p);
	struct apfs_key key = {0};
	struct apfs_query *query = NULL;
	__le64 raw_key;
	struct apfs_omap_snapshot raw_val = {0};
	int err;

	/* An empty snapshots tree may not even have a root yet */
	if (!oid) {
		err = apfs_make_empty_btree_root(sb, APFS_OBJECT_TYPE_OMAP_SNAPSHOT, &oid);
		if (err)
			return err;
	}

	root = apfs_read_node(sb, oid, APFS_OBJ_PHYSICAL, true /* write */);
	if (IS_ERR(root))
		return PTR_ERR(root);
	oid = 0;

	apfs_init_omap_snap_key(nxi->nx_xid, &key);

	query = apfs_alloc_query(root, NULL /* parent */);
	if (!query) {
		err = -ENOMEM;
		goto fail;
	}
	query->key = &key;
	query->flags = APFS_QUERY_OMAP_SNAP | APFS_QUERY_EXACT;

	err = apfs_btree_query(sb, &query);
	if (err == 0) {
		err = -EFSCORRUPTED;
		goto fail;
	}
	if (err != -ENODATA)
		goto fail;

	raw_key = cpu_to_le64(nxi->nx_xid);
	err = apfs_btree_insert(query, &raw_key, sizeof(raw_key), &raw_val, sizeof(raw_val));
	*oid_p = cpu_to_le64(root->object.block_nr);

fail:
	apfs_free_query(query);
	query = NULL;
	apfs_node_free(root);
	root = NULL;
	return err;
}

/**
 * apfs_update_omap_snapshots - Add the current xid to the omap's snapshots
 * @sb:	filesystem superblock
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_update_omap_snapshots(struct super_block *sb)
{
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct buffer_head *bh = NULL;
	struct apfs_omap_phys *omap = NULL;
	u64 omap_blk;
	u64 xid;
	int err;

	xid = APFS_NXI(sb)->nx_xid;

	omap_blk = le64_to_cpu(vsb_raw->apfs_omap_oid);
	bh = apfs_read_object_block(sb, omap_blk, true /* write */, false /* preserve */);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	omap = (struct apfs_omap_phys *)bh->b_data;

	apfs_assert_in_transaction(sb, &omap->om_o);
	le32_add_cpu(&omap->om_snap_count, 1);
	omap->om_most_recent_snap = cpu_to_le64(xid);
	err = apfs_update_omap_snap_tree(sb, &omap->om_snapshot_tree_oid);

	omap = NULL;
	brelse(bh);
	bh = NULL;
	return err;
}

/**
 * apfs_do_ioc_takesnapshot - Actual work for apfs_ioc_take_snapshot()
 * @mntpoint:	inode of the mount point to snapshot
 * @name:	label for the snapshot
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_do_ioc_take_snapshot(struct inode *mntpoint, const char *name)
{
	struct super_block *sb = mntpoint->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_superblock *vsb_raw = NULL;
	struct apfs_omap *omap = sbi->s_omap;
	/* TODO: remember to update the maxops in the future */
	struct apfs_max_ops maxops = {0};
	u64 sblock_oid;
	int err;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;

	/*
	 * Flush the extent caches to the extenref tree before it gets moved to
	 * the snapshot. It seems safer in general to avoid big unpredictable
	 * changes to the layout after the snapshot is set up.
	 */
	err = apfs_transaction_flush_all_inodes(sb);
	if (err)
		return err;

	err = apfs_create_superblock_snapshot(sb, &sblock_oid);
	if (err)
		return err;

	err = apfs_create_snap_meta_records(mntpoint, name, sblock_oid);
	if (err)
		goto fail;

	err = apfs_create_new_extentref_tree(sb);
	if (err)
		goto fail;

	err = apfs_update_omap_snapshots(sb);
	if (err)
		goto fail;

	/*
	 * The official reference allows old implementations to ignore extended
	 * snapshot metadata, so I don't see any reason why we can't do the
	 * same for now.
	 */

	vsb_raw = sbi->s_vsb_raw;
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	le64_add_cpu(&vsb_raw->apfs_num_snapshots, 1);

	omap->omap_latest_snap = APFS_NXI(sb)->nx_xid;

	sbi->s_nxi->nx_transaction.t_state |= APFS_NX_TRANS_FORCE_COMMIT;
	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

/**
 * apfs_ioc_take_snapshot - Ioctl handler for APFS_IOC_CREATE_SNAPSHOT
 * @file:	affected file
 * @arg:	ioctl argument
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_ioc_take_snapshot(struct file *file, void __user *user_arg)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct apfs_ioctl_snap_name *arg = NULL;
	int err;

	if (apfs_ino(inode) != APFS_ROOT_DIR_INO_NUM) {
		apfs_info(sb, "snapshot must be requested on mountpoint");
		return -ENOTTY;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	if (!inode_owner_or_capable(inode))
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	if (!inode_owner_or_capable(&init_user_ns, inode))
#else
	if (!inode_owner_or_capable(&nop_mnt_idmap, inode))
#endif
		return -EPERM;

	err = mnt_want_write_file(file);
	if (err)
		return err;

	arg = kzalloc(sizeof(*arg), GFP_KERNEL);
	if (!arg) {
		err = -ENOMEM;
		goto fail;
	}

	if (copy_from_user(arg, user_arg, sizeof(*arg))) {
		err = -EFAULT;
		goto fail;
	}

	err = apfs_do_ioc_take_snapshot(inode, arg->name);
fail:
	kfree(arg);
	arg = NULL;
	mnt_drop_write_file(file);
	return err;
}

static int apfs_snap_xid_from_query(struct apfs_query *query, u64 *xid)
{
	char *raw = query->node->object.data;
	__le64 *val = NULL;

	if (query->len != sizeof(*val))
		return -EFSCORRUPTED;
	val = (__le64 *)(raw + query->off);

	*xid = le64_to_cpup(val);
	return 0;
}

static int apfs_snapshot_name_to_xid(struct apfs_node *snap_root, const char *name, u64 *xid)
{
	struct super_block *sb = snap_root->object.sb;
	struct apfs_query *query = NULL;
	struct apfs_key key = {0};
	int err;

	apfs_init_snap_name_key(name, &key);

	query = apfs_alloc_query(snap_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_SNAP_META | APFS_QUERY_EXACT;

	err = apfs_btree_query(sb, &query);
	if (err)
		goto fail;

	err = apfs_snap_xid_from_query(query, xid);
fail:
	apfs_free_query(query);
	query = NULL;
	return err;
}

static int apfs_snap_sblock_from_query(struct apfs_query *query, u64 *sblock_oid)
{
	char *raw = query->node->object.data;
	struct apfs_snap_metadata_val *val = NULL;

	if (query->len < sizeof(*val))
		return -EFSCORRUPTED;
	val = (struct apfs_snap_metadata_val *)(raw + query->off);

	*sblock_oid = le64_to_cpu(val->sblock_oid);
	return 0;
}

static int apfs_snapshot_xid_to_sblock(struct apfs_node *snap_root, u64 xid, u64 *sblock_oid)
{
	struct super_block *sb = snap_root->object.sb;
	struct apfs_query *query = NULL;
	struct apfs_key key = {0};
	int err;

	apfs_init_snap_metadata_key(xid, &key);

	query = apfs_alloc_query(snap_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_SNAP_META | APFS_QUERY_EXACT;

	err = apfs_btree_query(sb, &query);
	if (err)
		goto fail;

	err = apfs_snap_sblock_from_query(query, sblock_oid);
fail:
	apfs_free_query(query);
	query = NULL;
	return err;
}

/**
 * apfs_switch_to_snapshot - Start working with the snapshot volume superblock
 * @sb: superblock structure
 *
 * Maps the volume superblock from the snapshot specified in the mount options.
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_switch_to_snapshot(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_superblock *vsb_raw = sbi->s_vsb_raw;
	struct apfs_node *snap_root = NULL;
	const char *name = NULL;
	u64 sblock_oid = 0;
	u64 xid = 0;
	int err;

	ASSERT(sb->s_flags & SB_RDONLY);

	name = sbi->s_snap_name;
	if (strlen(name) > APFS_SNAP_MAX_NAMELEN)
		return -EINVAL;

	snap_root = apfs_read_node(sb, le64_to_cpu(vsb_raw->apfs_snap_meta_tree_oid), APFS_OBJ_PHYSICAL, false /* write */);
	if (IS_ERR(snap_root))
		return PTR_ERR(snap_root);
	vsb_raw = NULL;

	err = apfs_snapshot_name_to_xid(snap_root, name, &xid);
	if (err)
		goto fail;
	sbi->s_snap_xid = xid;

	err = apfs_snapshot_xid_to_sblock(snap_root, xid, &sblock_oid);
	if (err)
		goto fail;

	apfs_unmap_volume_super(sb);
	err = apfs_map_volume_super_bno(sb, sblock_oid, nxi->nx_flags & APFS_CHECK_NODES);

fail:
	apfs_node_free(snap_root);
	return err;
}
