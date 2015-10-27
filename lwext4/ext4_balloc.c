/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 *
 *
 * HelenOS:
 * Copyright (c) 2012 Martin Sucha
 * Copyright (c) 2012 Frantisek Princ
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4_balloc.c
 * @brief Physical block allocator.
 */

#include "ext4_config.h"
#include "ext4_balloc.h"
#include "ext4_super.h"
#include "ext4_crc32c.h"
#include "ext4_block_group.h"
#include "ext4_fs.h"
#include "ext4_bitmap.h"
#include "ext4_inode.h"

/**@brief Compute number of block group from block address.
 * @param sb superblock pointer.
 * @param baddr Absolute address of block.
 * @return Block group index
 */
uint32_t ext4_balloc_get_bgid_of_block(struct ext4_sblock *s,
				       uint64_t baddr)
{
	if (ext4_get32(s, first_data_block) && baddr)
		baddr--;

	return baddr / ext4_get32(s, blocks_per_group);
}

/**@brief Compute the starting block address of a block group
 * @param sb   superblock pointer.
 * @param bgid block group index
 * @return Block address
 */
uint64_t ext4_balloc_get_block_of_bgid(struct ext4_sblock *s,
				       uint32_t bgid)
{
	uint64_t baddr = 0;
	if (ext4_get32(s, first_data_block))
		baddr++;

	baddr += bgid * ext4_get32(s, blocks_per_group);
	return baddr;
}

static uint32_t ext4_balloc_bitmap_csum(struct ext4_sblock *sb,
					void *bitmap)
{
	uint32_t checksum = 0;
	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM)) {
		uint32_t blocks_per_group =
			ext4_get32(sb, blocks_per_group);

		/* First calculate crc32 checksum against fs uuid */
		checksum = ext4_crc32c(~0, sb->uuid, sizeof(sb->uuid));
		/* Then calculate crc32 checksum against block_group_desc */
		checksum = ext4_crc32c(checksum, bitmap,
				     blocks_per_group / 8);
	}
	return checksum;
}

/*
 * BIG FAT NOTES:
 *       Currently we do not verify the checksum of bitmaps.
 */

void ext4_balloc_set_bitmap_csum(struct ext4_sblock *sb,
				 struct ext4_bgroup *bg,
				 void *bitmap)
{
	int desc_size = ext4_sb_get_desc_size(sb);
	uint32_t checksum = ext4_balloc_bitmap_csum(sb, bitmap);
	uint16_t lo_checksum = to_le16(checksum & 0xFFFF),
		 hi_checksum = to_le16(checksum >> 16);
	
	/* See if we need to assign a 32bit checksum */
	bg->block_bitmap_csum_lo = lo_checksum;
	if (desc_size == EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE)
		bg->block_bitmap_csum_hi = hi_checksum;

}

int ext4_balloc_free_block(struct ext4_inode_ref *inode_ref, ext4_fsblk_t baddr)
{
	struct ext4_fs *fs = inode_ref->fs;
	struct ext4_sblock *sb = &fs->sb;

	uint32_t block_group = ext4_balloc_get_bgid_of_block(sb, baddr);
	uint32_t index_in_group = ext4_fs_baddr2_index_in_group(sb, baddr);

	/* Load block group reference */
	struct ext4_block_group_ref bg_ref;
	int rc = ext4_fs_get_block_group_ref(fs, block_group, &bg_ref);
	if (rc != EOK)
		return rc;

	/* Load block with bitmap */
	ext4_fsblk_t bitmap_block_addr =
	    ext4_bg_get_block_bitmap(bg_ref.block_group, sb);

	struct ext4_block bitmap_block;

	rc = ext4_block_get(fs->bdev, &bitmap_block, bitmap_block_addr);
	if (rc != EOK) {
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	/* Modify bitmap */
	ext4_bmap_bit_clr(bitmap_block.data, index_in_group);
	ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group,
				    bitmap_block.data);
	bitmap_block.dirty = true;

	/* Release block with bitmap */
	rc = ext4_block_set(fs->bdev, &bitmap_block);
	if (rc != EOK) {
		/* Error in saving bitmap */
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	uint32_t block_size = ext4_sb_get_block_size(sb);

	/* Update superblock free blocks count */
	uint64_t sb_free_blocks = ext4_sb_get_free_blocks_cnt(sb);
	sb_free_blocks++;
	ext4_sb_set_free_blocks_cnt(sb, sb_free_blocks);

	/* Update inode blocks count */
	uint64_t ino_blocks = ext4_inode_get_blocks_count(sb, inode_ref->inode);
	ino_blocks -= block_size / EXT4_INODE_BLOCK_SIZE;
	ext4_inode_set_blocks_count(sb, inode_ref->inode, ino_blocks);
	inode_ref->dirty = true;

	/* Update block group free blocks count */
	uint32_t free_blocks =
	    ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
	free_blocks++;
	ext4_bg_set_free_blocks_count(bg_ref.block_group, sb, free_blocks);

	bg_ref.dirty = true;

	/* Release block group reference */
	return ext4_fs_put_block_group_ref(&bg_ref);
}

int ext4_balloc_free_blocks(struct ext4_inode_ref *inode_ref, ext4_fsblk_t first,
			    uint32_t count)
{
	int rc = EOK;
	struct ext4_fs *fs = inode_ref->fs;
	struct ext4_sblock *sb = &fs->sb;

	/* Compute indexes */
	uint32_t block_group_first = ext4_balloc_get_bgid_of_block(sb, first);

	/* Compute indexes */
	uint32_t block_group_last =
	    ext4_balloc_get_bgid_of_block(sb, first + count - 1);

	if (!ext4_sb_feature_incom(sb, EXT4_FINCOM_FLEX_BG)) {
		/*It is not possible without flex_bg that blocks are continuous
		 * and and last block belongs to other bg.*/
		ext4_assert(block_group_first == ext4_balloc_get_bgid_of_block(
						     sb, first + count - 1));
	}

	/* Load block group reference */
	struct ext4_block_group_ref bg_ref;
	while (block_group_first <= block_group_last) {

		rc =
		    ext4_fs_get_block_group_ref(fs, block_group_first, &bg_ref);
		if (rc != EOK)
			return rc;

		uint32_t index_in_group_first =
		    ext4_fs_baddr2_index_in_group(sb, first);

		/* Load block with bitmap */
		ext4_fsblk_t bitmap_block_addr =
		    ext4_bg_get_block_bitmap(bg_ref.block_group, sb);

		struct ext4_block bitmap_block;

		rc = ext4_block_get(fs->bdev, &bitmap_block, bitmap_block_addr);
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}

		uint32_t free_cnt =
		    ext4_sb_get_block_size(sb) * 8 - index_in_group_first;

		/*If last block, free only count blocks*/
		free_cnt = count > free_cnt ? free_cnt : count;

		/* Modify bitmap */
		ext4_bmap_bits_free(bitmap_block.data, index_in_group_first,
				    free_cnt);
		ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group,
					    bitmap_block.data);
		bitmap_block.dirty = true;

		count -= free_cnt;
		first += free_cnt;

		/* Release block with bitmap */
		rc = ext4_block_set(fs->bdev, &bitmap_block);
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}

		uint32_t block_size = ext4_sb_get_block_size(sb);

		/* Update superblock free blocks count */
		uint64_t sb_free_blocks = ext4_sb_get_free_blocks_cnt(sb);
		sb_free_blocks += free_cnt;
		ext4_sb_set_free_blocks_cnt(sb, sb_free_blocks);

		/* Update inode blocks count */
		uint64_t ino_blocks =
		    ext4_inode_get_blocks_count(sb, inode_ref->inode);
		ino_blocks -= free_cnt * (block_size / EXT4_INODE_BLOCK_SIZE);
		ext4_inode_set_blocks_count(sb, inode_ref->inode, ino_blocks);
		inode_ref->dirty = true;

		/* Update block group free blocks count */
		uint32_t free_blocks =
		    ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
		free_blocks += free_cnt;
		ext4_bg_set_free_blocks_count(bg_ref.block_group, sb,
					      free_blocks);
		bg_ref.dirty = true;

		/* Release block group reference */
		rc = ext4_fs_put_block_group_ref(&bg_ref);
		if (rc != EOK)
			break;

		block_group_first++;
	}

	/*All blocks should be released*/
	ext4_assert(count == 0);
	return rc;
}

int ext4_balloc_alloc_block(struct ext4_inode_ref *inode_ref,
			    ext4_fsblk_t goal,
			    ext4_fsblk_t *fblock)
{
	ext4_fsblk_t allocated_block = 0;
	ext4_fsblk_t bitmap_block_addr;
	uint32_t rel_block_idx = 0;
	uint64_t free_blocks;
	struct ext4_block bitmap_block;
	int rc;

	struct ext4_sblock *sb = &inode_ref->fs->sb;

	/* Load block group number for goal and relative index */
	uint32_t block_group = ext4_balloc_get_bgid_of_block(sb, goal);
	uint32_t index_in_group = ext4_fs_baddr2_index_in_group(sb, goal);

	/* Load block group reference */
	struct ext4_block_group_ref bg_ref;
	rc = ext4_fs_get_block_group_ref(inode_ref->fs, block_group, &bg_ref);
	if (rc != EOK)
		return rc;

	free_blocks = ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
	if (free_blocks == 0) {
		/* This group has no free blocks */
		goto goal_failed;
	}

	/* Compute indexes */
	ext4_fsblk_t first_in_group = ext4_balloc_get_block_of_bgid(sb, bg_ref.index);

	uint32_t first_in_group_index =
	    ext4_fs_baddr2_index_in_group(sb, first_in_group);

	if (index_in_group < first_in_group_index)
		index_in_group = first_in_group_index;

	/* Load block with bitmap */
	bitmap_block_addr = ext4_bg_get_block_bitmap(bg_ref.block_group, sb);

	rc = ext4_block_get(inode_ref->fs->bdev, &bitmap_block,
			    bitmap_block_addr);
	if (rc != EOK) {
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	/* Check if goal is free */
	if (ext4_bmap_is_bit_clr(bitmap_block.data, index_in_group)) {
		ext4_bmap_bit_set(bitmap_block.data, index_in_group);
		ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group,
					    bitmap_block.data);
		bitmap_block.dirty = true;
		rc = ext4_block_set(inode_ref->fs->bdev, &bitmap_block);
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}

		allocated_block = ext4_fs_index_in_group2_baddr(
		    sb, index_in_group, block_group);

		goto success;
	}

	uint32_t blocks_in_group = ext4_blocks_in_group_cnt(sb, block_group);

	uint32_t end_idx = (index_in_group + 63) & ~63;
	if (end_idx > blocks_in_group)
		end_idx = blocks_in_group;

	/* Try to find free block near to goal */
	uint32_t tmp_idx;
	for (tmp_idx = index_in_group + 1; tmp_idx < end_idx; ++tmp_idx) {
		if (ext4_bmap_is_bit_clr(bitmap_block.data, tmp_idx)) {
			ext4_bmap_bit_set(bitmap_block.data, tmp_idx);

			ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group,
						    bitmap_block.data);
			bitmap_block.dirty = true;
			rc = ext4_block_set(inode_ref->fs->bdev, &bitmap_block);
			if (rc != EOK)
				return rc;

			allocated_block = ext4_fs_index_in_group2_baddr(
			    sb, tmp_idx, block_group);

			goto success;
		}
	}

	/* Find free bit in bitmap */
	rc = ext4_bmap_bit_find_clr(bitmap_block.data, index_in_group,
				    blocks_in_group, &rel_block_idx);
	if (rc == EOK) {
		ext4_bmap_bit_set(bitmap_block.data, rel_block_idx);
		ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group,
					    bitmap_block.data);
		bitmap_block.dirty = true;
		rc = ext4_block_set(inode_ref->fs->bdev, &bitmap_block);
		if (rc != EOK)
			return rc;

		allocated_block = ext4_fs_index_in_group2_baddr(
		    sb, rel_block_idx, block_group);

		goto success;
	}

	/* No free block found yet */
	rc = ext4_block_set(inode_ref->fs->bdev, &bitmap_block);
	if (rc != EOK) {
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

goal_failed:

	rc = ext4_fs_put_block_group_ref(&bg_ref);
	if (rc != EOK)
		return rc;

	/* Try other block groups */
	uint32_t block_group_count = ext4_block_group_cnt(sb);

	uint32_t bgid = (block_group + 1) % block_group_count;
	uint32_t count = block_group_count;

	while (count > 0) {
		rc = ext4_fs_get_block_group_ref(inode_ref->fs, bgid, &bg_ref);
		if (rc != EOK)
			return rc;

		free_blocks =
		    ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
		if (free_blocks == 0) {
			/* This group has no free blocks */
			goto next_group;
		}

		/* Load block with bitmap */
		bitmap_block_addr =
		    ext4_bg_get_block_bitmap(bg_ref.block_group, sb);

		rc = ext4_block_get(inode_ref->fs->bdev, &bitmap_block,
				    bitmap_block_addr);

		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}

		/* Compute indexes */
		first_in_group = ext4_balloc_get_block_of_bgid(sb, bgid);
		index_in_group =
		    ext4_fs_baddr2_index_in_group(sb, first_in_group);
		blocks_in_group = ext4_blocks_in_group_cnt(sb, bgid);

		first_in_group_index =
		    ext4_fs_baddr2_index_in_group(sb, first_in_group);

		if (index_in_group < first_in_group_index)
			index_in_group = first_in_group_index;

		rc = ext4_bmap_bit_find_clr(bitmap_block.data, index_in_group,
					    blocks_in_group, &rel_block_idx);

		if (rc == EOK) {

			ext4_bmap_bit_set(bitmap_block.data, rel_block_idx);

			ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group,
						    bitmap_block.data);
			bitmap_block.dirty = true;
			rc = ext4_block_set(inode_ref->fs->bdev, &bitmap_block);
			if (rc != EOK) {
				ext4_fs_put_block_group_ref(&bg_ref);
				return rc;
			}

			allocated_block = ext4_fs_index_in_group2_baddr(
			    sb, rel_block_idx, bgid);

			goto success;
		}

		rc = ext4_block_set(inode_ref->fs->bdev, &bitmap_block);
		if (rc != EOK) {
			ext4_fs_put_block_group_ref(&bg_ref);
			return rc;
		}

	next_group:
		rc = ext4_fs_put_block_group_ref(&bg_ref);
		if (rc != EOK) {
			return rc;
		}

		/* Goto next group */
		bgid = (bgid + 1) % block_group_count;
		count--;
	}

	return ENOSPC;

success:
    /* Empty command - because of syntax */
    ;

	uint32_t block_size = ext4_sb_get_block_size(sb);

	/* Update superblock free blocks count */
	uint64_t sb_free_blocks = ext4_sb_get_free_blocks_cnt(sb);
	sb_free_blocks--;
	ext4_sb_set_free_blocks_cnt(sb, sb_free_blocks);

	/* Update inode blocks (different block size!) count */
	uint64_t ino_blocks = ext4_inode_get_blocks_count(sb, inode_ref->inode);
	ino_blocks += block_size / EXT4_INODE_BLOCK_SIZE;
	ext4_inode_set_blocks_count(sb, inode_ref->inode, ino_blocks);
	inode_ref->dirty = true;

	/* Update block group free blocks count */
	uint64_t bg_free_blocks =
	    ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
	bg_free_blocks--;
	ext4_bg_set_free_blocks_count(bg_ref.block_group, sb, bg_free_blocks);

	bg_ref.dirty = true;

	rc = ext4_fs_put_block_group_ref(&bg_ref);

	*fblock = allocated_block;
	return rc;
}

int ext4_balloc_try_alloc_block(struct ext4_inode_ref *inode_ref,
				ext4_fsblk_t baddr, bool *free)
{
	int rc;

	struct ext4_fs *fs = inode_ref->fs;
	struct ext4_sblock *sb = &fs->sb;

	/* Compute indexes */
	uint32_t block_group = ext4_balloc_get_bgid_of_block(sb, baddr);
	uint32_t index_in_group = ext4_fs_baddr2_index_in_group(sb, baddr);

	/* Load block group reference */
	struct ext4_block_group_ref bg_ref;
	rc = ext4_fs_get_block_group_ref(fs, block_group, &bg_ref);
	if (rc != EOK)
		return rc;

	/* Load block with bitmap */
	ext4_fsblk_t bitmap_block_addr =
	    ext4_bg_get_block_bitmap(bg_ref.block_group, sb);

	struct ext4_block bitmap_block;

	rc = ext4_block_get(fs->bdev, &bitmap_block, bitmap_block_addr);
	if (rc != EOK) {
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	/* Check if block is free */
	*free = ext4_bmap_is_bit_clr(bitmap_block.data, index_in_group);

	/* Allocate block if possible */
	if (*free) {
		ext4_bmap_bit_set(bitmap_block.data, index_in_group);
		ext4_balloc_set_bitmap_csum(sb, bg_ref.block_group,
					    bitmap_block.data);
		bitmap_block.dirty = true;
	}

	/* Release block with bitmap */
	rc = ext4_block_set(fs->bdev, &bitmap_block);
	if (rc != EOK) {
		/* Error in saving bitmap */
		ext4_fs_put_block_group_ref(&bg_ref);
		return rc;
	}

	/* If block is not free, return */
	if (!(*free))
		goto terminate;

	uint32_t block_size = ext4_sb_get_block_size(sb);

	/* Update superblock free blocks count */
	uint64_t sb_free_blocks = ext4_sb_get_free_blocks_cnt(sb);
	sb_free_blocks--;
	ext4_sb_set_free_blocks_cnt(sb, sb_free_blocks);

	/* Update inode blocks count */
	uint64_t ino_blocks = ext4_inode_get_blocks_count(sb, inode_ref->inode);
	ino_blocks += block_size / EXT4_INODE_BLOCK_SIZE;
	ext4_inode_set_blocks_count(sb, inode_ref->inode, ino_blocks);
	inode_ref->dirty = true;

	/* Update block group free blocks count */
	uint32_t free_blocks =
	    ext4_bg_get_free_blocks_count(bg_ref.block_group, sb);
	free_blocks--;
	ext4_bg_set_free_blocks_count(bg_ref.block_group, sb, free_blocks);

	bg_ref.dirty = true;

terminate:
	return ext4_fs_put_block_group_ref(&bg_ref);
}

/**
 * @}
 */
