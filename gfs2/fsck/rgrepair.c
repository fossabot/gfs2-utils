#include "clusterautoconfig.h"

#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "osi_list.h"
#include "fsck.h"

int rindex_modified = FALSE;
struct special_blocks false_rgrps;

#define BAD_RG_PERCENT_TOLERANCE 11
#define AWAY_FROM_BITMAPS 0x1000

#define ri_equal(ondisk, expected, field) (ondisk.field == expected.field)

#define ri_compare(rg, ondisk, expected, field, fmt, type)	\
	if (ondisk.field != expected.field) { \
		log_warn( _("rindex #%d " #field " discrepancy: index 0x%" \
			    fmt	" != expected: 0x%" fmt "\n"),		\
			  rg + 1, (type)ondisk.field, (type)expected.field);	\
		ondisk.field = expected.field;				\
		rindex_modified = TRUE;					\
	}

/*
 * find_journal_entry_rgs - find all RG blocks within all journals
 *
 * Since Resource Groups (RGs) are journaled, it is not uncommon for them
 * to appear inside a journal.  But if there is severe damage to the rindex
 * file or some of the RGs, we may need to hunt and peck for RGs and in that
 * case, we don't want to mistake these blocks that look just a real RG
 * for a real RG block.  These are "fake" RGs that need to be ignored for
 * the purposes of finding where things are.
 */
static void find_journaled_rgs(struct gfs2_sbd *sdp)
{
	int j, new = 0;
	unsigned int jblocks;
	uint64_t b, dblock;
	uint32_t extlen;
	struct gfs2_inode *ip;
	struct gfs2_buffer_head *bh;

	osi_list_init(&false_rgrps.list);
	for (j = 0; j < sdp->md.journals; j++) {
		log_debug( _("Checking for rgrps in journal%d.\n"), j);
		ip = sdp->md.journal[j];
		jblocks = ip->i_di.di_size / sdp->sd_sb.sb_bsize;
		for (b = 0; b < jblocks; b++) {
			block_map(ip, b, &new, &dblock, &extlen, 0);
			if (!dblock)
				break;
			bh = bread(sdp, dblock);
			if (!gfs2_check_meta(bh, GFS2_METATYPE_RG)) {
				log_debug( _("False rgrp found at block 0x%llx\n"),
					  (unsigned long long)dblock);
				gfs2_special_set(&false_rgrps, dblock);
			}
			brelse(bh);
		}
	}
}

static int is_false_rg(uint64_t block)
{
	if (blockfind(&false_rgrps, block))
		return 1;
	return 0;
}

/*
 * find_shortest_rgdist - hunt and peck for the shortest distance between RGs.
 *
 * Sample several of them because an RG that's been blasted may
 * look like twice the distance.  If we can find 6 of them, that
 * should be enough to figure out the correct layout.
 * This also figures out first_rg_dist since that's always different.
 */
static uint64_t find_shortest_rgdist(struct gfs2_sbd *sdp,
				     uint64_t *initial_first_rg_dist,
				     uint64_t *first_rg_dist)
{
	uint64_t blk, block_of_last_rg, shortest_dist_btwn_rgs;
	struct gfs2_buffer_head *bh;
	int number_of_rgs = 0;
	struct gfs2_rindex buf, tmpndx;

	/* Figure out if there are any RG-looking blocks in the journal we
	   need to ignore. */
	find_journaled_rgs(sdp);

	*initial_first_rg_dist = *first_rg_dist = sdp->sb_addr + 1;
	block_of_last_rg = sdp->sb_addr + 1;
	shortest_dist_btwn_rgs = sdp->device.length;

	for (blk = sdp->sb_addr + 1;
	     blk < sdp->device.length && number_of_rgs < 6; blk++) {
		bh = bread(sdp, blk);
		if (((blk == sdp->sb_addr + 1) ||
		    (!gfs2_check_meta(bh, GFS2_METATYPE_RG))) &&
		    !is_false_rg(blk)) {
			log_debug( _("rgrp found at block 0x%llx\n"),
				(unsigned long long)blk);
			if (blk > sdp->sb_addr + 1) {
				uint64_t rgdist;
				
				rgdist = blk - block_of_last_rg;
				log_debug("dist 0x%llx = 0x%llx - 0x%llx",
					  (unsigned long long)rgdist,
					  (unsigned long long)blk,
					  (unsigned long long)block_of_last_rg);
				/* ----------------------------------------- */
				/* We found an RG.  Check to see if we need  */
				/* to set the first_rg_dist based on whether */
				/* it's still at its initial value (i.e. the */
				/* fs.)  The first rg distance is different  */
				/* from the rest because of the superblock   */
				/* and 64K dead space.                       */
				/* ----------------------------------------- */
				if (*first_rg_dist == *initial_first_rg_dist)
					*first_rg_dist = rgdist;
				if (rgdist < shortest_dist_btwn_rgs) {
					shortest_dist_btwn_rgs = rgdist;
					log_debug( _("(shortest so far)\n"));
				}
				else
					log_debug("\n");
			}
			block_of_last_rg = blk;
			number_of_rgs++;
			blk += 250; /* skip ahead for performance */
		}
		brelse(bh);
	}
	/* -------------------------------------------------------------- */
	/* Sanity-check our first_rg_dist. If RG #2 got nuked, the        */
	/* first_rg_dist would measure from #1 to #3, which would be bad. */
	/* We need to take remedial measures to fix it (from the index).  */
	/* -------------------------------------------------------------- */
	log_debug( _("First rgrp distance: 0x%llx\n"), (unsigned long long)*first_rg_dist);
	log_debug( _("Distance between rgrps: 0x%llx\n"),
		  (unsigned long long)shortest_dist_btwn_rgs);
	if (*first_rg_dist >= shortest_dist_btwn_rgs +
	    (shortest_dist_btwn_rgs / 4)) {
		/* read in the second RG index entry for this subd. */
		gfs2_readi(sdp->md.riinode, (char *)&buf,
			   sizeof(struct gfs2_rindex),
			   sizeof(struct gfs2_rindex));
		gfs2_rindex_in(&tmpndx, (char *)&buf);
		if (tmpndx.ri_addr > sdp->sb_addr + 1) { /* sanity check */
			log_warn( _("rgrp 2 is damaged: getting dist from index: "));
			*first_rg_dist = tmpndx.ri_addr - (sdp->sb_addr + 1);
			log_warn("0x%llx\n", (unsigned long long)*first_rg_dist);
		}
		else {
			log_warn( _("rgrp index 2 is damaged: extrapolating dist: "));
			*first_rg_dist = sdp->device.length -
				(sdp->rgrps - 1) *
				(sdp->device.length / sdp->rgrps);
			log_warn("0x%llx\n", (unsigned long long)*first_rg_dist);
		}
		log_debug( _("Adjusted first rgrp distance: 0x%llx\n"),
			   (unsigned long long)*first_rg_dist);
	} /* if first RG distance is within tolerance */

	gfs2_special_free(&false_rgrps);
	return shortest_dist_btwn_rgs;
}

/*
 * count_usedspace - count the used bits in a rgrp bitmap buffer
 */
static uint64_t count_usedspace(struct gfs2_sbd *sdp, int first,
				struct gfs2_buffer_head *bh)
{
	int off, x, y, bytes_to_check;
	uint32_t rg_used = 0;
	unsigned int state;

	/* Count up the free blocks in the bitmap */
	off = (first) ? sizeof(struct gfs2_rgrp) :
		sizeof(struct gfs2_meta_header);
	bytes_to_check = sdp->bsize - off;
	for (x = 0; x < bytes_to_check; x++) {
		unsigned char *byte;

		byte = (unsigned char *)&bh->b_data[off + x];
		if (*byte == 0x55) {
			rg_used += GFS2_NBBY;
			continue;
		}
		if (*byte == 0x00)
			continue;
		for (y = 0; y < GFS2_NBBY; y++) {
			state = (*byte >> (GFS2_BIT_SIZE * y)) & GFS2_BIT_MASK;
			if (state == GFS2_BLKST_FREE ||
			    state == GFS2_BLKST_UNLINKED)
				continue;
			rg_used++;
		}
	}
	return rg_used;
}

/*
 * find_next_rgrp_dist - find the distance to the next rgrp
 *
 * This function is only called if the rgrps are determined to be on uneven
 * boundaries.  In a normal gfs2 file system, after mkfs.gfs2, all the
 * rgrps but the first and second one will be the same distance from the
 * previous rgrp.  (The first rgrp will predictably be after the superblock
 * and the second one will be adjusted based on the number 64KB skipped
 * at the start of the file system.)  The only way we can deviate from that
 * pattern is if the user did gfs_grow on a gfs1 file system, then converted
 * it to gfs2 using gfs2_convert.
 *
 * This function finds the distance to the next rgrp for these cases.
 */
static uint64_t find_next_rgrp_dist(struct gfs2_sbd *sdp, uint64_t blk,
				    struct rgrp_list *prevrgd)
{
	uint64_t rgrp_dist = 0, used_blocks, block, next_block, twogigs;
	osi_list_t *tmp;
	struct rgrp_list *rgd = NULL, *next_rgd;
	struct gfs2_buffer_head *bh;
	struct gfs2_meta_header mh;
	int first, length, b, found, mega_in_blocks;
	uint32_t free_blocks;

	for (tmp = sdp->rglist.next; tmp != &sdp->rglist; tmp = tmp->next) {
		rgd = osi_list_entry(tmp, struct rgrp_list, list);
		if (rgd->ri.ri_addr == blk)
			break;
	}
	if (rgd && tmp && tmp != &sdp->rglist && tmp->next &&
	    rgd->ri.ri_addr == blk) {
		tmp = tmp->next;
		next_rgd = osi_list_entry(tmp, struct rgrp_list, list);
		rgrp_dist = next_rgd->ri.ri_addr - rgd->ri.ri_addr;
		return rgrp_dist;
	}
	mega_in_blocks = (1024 * 1024)  / sdp->bsize;
	twogigs = 2048 * mega_in_blocks;
	/* Unfortunately, if we fall through to here we can't trust the
	   rindex.  So we have to analyze the current rgrp to figure out
	   the bare minimum block number where it ends. If we don't have
	   rindex, all we know about this rgrp is what's on disk: its
	   rg_free.  If we analyze the rgrp's bitmap and the bitmaps that
	   follow, we can figure out how many bits are used.  If we add
	   rg_free, we get the total number of blocks this rgrp
	   represents.  After that should be the next rgrp, but it may
	   skip a few blocks (hopefully no more than 4).  */
	used_blocks = 0;
	length = 0;
	block = prevrgd->ri.ri_addr;
	first = 1;
	found = 0;
	while (1) {
		if (block >= sdp->device.length)
			break;
		if (block >= prevrgd->ri.ri_addr + twogigs)
			break;
		bh = bread(sdp, block);
		gfs2_meta_header_in(&mh, bh);
		if ((mh.mh_magic != GFS2_MAGIC) ||
		    (first && mh.mh_type != GFS2_METATYPE_RG) ||
		    (!first && mh.mh_type != GFS2_METATYPE_RB)) {
			brelse(bh);
			break;
		}
		if (first) {
			struct gfs2_rgrp *rg;

			rg = (struct gfs2_rgrp *)bh->b_data;
			free_blocks = be32_to_cpu(rg->rg_free);
		}
		used_blocks += count_usedspace(sdp, first, bh);
		first = 0;
		block++;
		length++;
		brelse(bh);
		/* Check if this distance points to an rgrp:
		   We have to look for blocks that resemble rgrps and bitmaps.
		   If they do, we need to count blocks used and free and see
		   if adding that number of free blocks accounts for the
		   next rgrp we find. Otherwise, you could have a length of
		   6 with additional user blocks that just happen to look like
		   bitmap blocks.  Count them all as bitmaps and you'll be
		   hopelessly lost. */
		rgrp_dist = used_blocks + free_blocks + length;
		next_block = prevrgd->ri.ri_addr + rgrp_dist;
		/* Now we account for block rounding done by mkfs.gfs2 */
		for (b = 0; b <= length + GFS2_NBBY; b++) {
			if (next_block >= sdp->device.length)
				break;
			bh = bread(sdp, next_block + b);
			gfs2_meta_header_in(&mh, bh);
			brelse(bh);
			if (mh.mh_magic == GFS2_MAGIC) {
				if (mh.mh_type == GFS2_METATYPE_RG) {
					found = 1;
					break;
				}
				/* if the first thing we find is a bitmap,
				   there must be a damaged rgrp on the
				   previous block. */
				if (mh.mh_type == GFS2_METATYPE_RB) {
					found = 1;
					rgrp_dist--;
					break;
				}
			}
			rgrp_dist++;
		}
		if (found) {
			block = next_block;
			log_info( _("rgrp found at 0x%llx, length=%d, "
				    "used=%llu, free=%d\n"),
				  prevrgd->ri.ri_addr, length,
				  (unsigned long long)used_blocks,
				  free_blocks);
			break;
		}
	}
	return rgrp_dist;
}

/*
 * hunt_and_peck - find the distance to the next rgrp
 *
 * This function is only called if the rgrps are determined to be on uneven
 * boundaries, and also corrupt.  So we have to go out searching for one.
 */
static uint64_t hunt_and_peck(struct gfs2_sbd *sdp, uint64_t blk,
			      struct rgrp_list *prevrgd, uint64_t last_bump)
{
	uint64_t rgrp_dist = 0, block, twogigs, last_block, last_meg;
	struct gfs2_buffer_head *bh;
	struct gfs2_meta_header mh;
	int b, mega_in_blocks;

	/* Skip ahead the previous amount: we might get lucky.
	   If we're close to the end of the device, take the rest. */
	if (gfs2_check_range(sdp, blk + last_bump))
		return sdp->fssize - blk;

	bh = bread(sdp, blk + last_bump);
	gfs2_meta_header_in(&mh, bh);
	brelse(bh);
	if (mh.mh_magic == GFS2_MAGIC && mh.mh_type == GFS2_METATYPE_RG) {
		log_info( _("rgrp found at 0x%llx, length=%lld\n"),
			  (unsigned long long)blk + last_bump,
			  (unsigned long long)last_bump);
		return last_bump;
	}

	rgrp_dist = AWAY_FROM_BITMAPS; /* Get away from any bitmaps
					  associated with the previous rgrp */
	block = prevrgd->ri.ri_addr + rgrp_dist;
	/* Now we account for block rounding done by mkfs.gfs2.  A rgrp can
	   be at most 2GB in size, so that's where we call it. We do somewhat
	   obscure math here to avoid integer overflows. */
	mega_in_blocks = (1024 * 1024)  / sdp->bsize;
	twogigs = 2048 * mega_in_blocks;
	if (block + twogigs <= sdp->fssize) {
		last_block = twogigs;
		last_meg = 0;
	} else {
		/* There won't be a rgrp in the last megabyte. */
		last_block = sdp->fssize - block - mega_in_blocks;
		last_meg = mega_in_blocks;
	}
	for (b = AWAY_FROM_BITMAPS; b < last_block; b++) {
		bh = bread(sdp, block + b);
		gfs2_meta_header_in(&mh, bh);
		brelse(bh);
		if (mh.mh_magic == GFS2_MAGIC) {
			if (mh.mh_type == GFS2_METATYPE_RG)
				break;
			/* if the first thing we find is a bitmap, there must
			   be a damaged rgrp on the previous block. */
			if (mh.mh_type == GFS2_METATYPE_RB) {
				rgrp_dist--;
				break;
			}
		}
		rgrp_dist++;
	}
	return rgrp_dist + last_meg;
}

/*
 * gfs2_rindex_rebuild - rebuild a corrupt Resource Group (RG) index manually
 *                        where trust_lvl == distrust
 *
 * If this routine is called, it means we have RGs in odd/unexpected places,
 * and there is a corrupt RG or RG index entry.  It also means we can't trust
 * the RG index to be sane, and the RGs don't agree with how mkfs would have
 * built them by default.  So we have no choice but to go through and count
 * them by hand.  We've tried twice to recover the RGs and RG index, and
 * failed, so this is our last chance to remedy the situation.
 *
 * This routine tries to minimize performance impact by:
 * 1. Skipping through the filesystem at known increments when possible.
 * 2. Shuffle through every block when RGs are not found at the predicted
 *    locations.
 *
 * Note: A GFS2 filesystem differs from a GFS1 file system in that there will
 * only be ONE chunk (i.e. no artificial subdevices on either size of the
 * journals).  The journals and even the rindex are kept as part of the file
 * system, so we need to rebuild that information by hand.  Also, with GFS1,
 * the different chunks ("subdevices") could have different RG sizes, which
 * made for quite a mess when trying to recover RGs.  GFS2 always uses the
 * same RG size determined by the original mkfs, so recovery is easier.
 *
 * If "gfs_grow" is specified the file system was most likely converted
 * from gfs1 to gfs2 after a gfs_grow operation.  In that case, the rgrps
 * will not be on predictable boundaries.
 */
static int gfs2_rindex_rebuild(struct gfs2_sbd *sdp, osi_list_t *ret_list,
			       int *num_rgs, int gfs_grow)
{
	struct gfs2_buffer_head *bh;
	uint64_t shortest_dist_btwn_rgs;
	uint64_t blk;
	uint64_t fwd_block, block_bump;
	uint64_t first_rg_dist, initial_first_rg_dist;
	struct rgrp_list *calc_rgd, *prev_rgd;
	int number_of_rgs, rgi;
	int rg_was_fnd = FALSE, corrupt_rgs = 0, bitmap_was_fnd;
	osi_list_t *tmp;

	osi_list_init(ret_list);
	initial_first_rg_dist = first_rg_dist = sdp->sb_addr + 1;
	shortest_dist_btwn_rgs = find_shortest_rgdist(sdp,
						      &initial_first_rg_dist,
						      &first_rg_dist);
	number_of_rgs = 0;
	/* -------------------------------------------------------------- */
	/* Now go through the RGs and verify their integrity, fixing as   */
	/* needed when corruption is encountered.                         */
	/* -------------------------------------------------------------- */
	prev_rgd = NULL;
	block_bump = first_rg_dist;
	blk = sdp->sb_addr + 1;
	while (blk <= sdp->device.length) {
		log_debug( _("Block 0x%llx\n"), (unsigned long long)blk);
		bh = bread(sdp, blk);
		rg_was_fnd = (!gfs2_check_meta(bh, GFS2_METATYPE_RG));
		brelse(bh);
		/* Allocate a new RG and index. */
		calc_rgd = malloc(sizeof(struct rgrp_list));
		if (!calc_rgd) {
			log_crit( _("Can't allocate memory for rgrp repair.\n"));
			return -1;
		}
		memset(calc_rgd, 0, sizeof(struct rgrp_list));
		osi_list_add_prev(&calc_rgd->list, ret_list);
		calc_rgd->ri.ri_length = 1;
		calc_rgd->ri.ri_addr = blk;
		if (!rg_was_fnd) { /* if not an RG */
			/* ------------------------------------------------- */
			/* This SHOULD be an RG but isn't.                   */
			/* ------------------------------------------------- */
			corrupt_rgs++;
			if (corrupt_rgs < 5)
				log_debug( _("Missing or damaged rgrp at block "
					  "%llu (0x%llx)\n"),
					  (unsigned long long)blk,
					  (unsigned long long)blk);
			else {
				log_crit( _("Error: too many missing or "
					    "damaged rgrps using this method. "
					    "Time to try another method.\n"));
				return -1;
			}
		}
		/* ------------------------------------------------ */
		/* Now go through and count the bitmaps for this RG */
		/* ------------------------------------------------ */
		bitmap_was_fnd = FALSE;
		for (fwd_block = blk + 1;
		     fwd_block < sdp->device.length; 
		     fwd_block++) {
			bh = bread(sdp, fwd_block);
			bitmap_was_fnd =
				(!gfs2_check_meta(bh, GFS2_METATYPE_RB));
			brelse(bh);
			if (bitmap_was_fnd) /* if a bitmap */
				calc_rgd->ri.ri_length++;
			else
				break; /* end of bitmap, so call it quits. */
		} /* for subsequent bitmaps */
		
		gfs2_compute_bitstructs(sdp, calc_rgd);
		calc_rgd->ri.ri_data0 = calc_rgd->ri.ri_addr +
			calc_rgd->ri.ri_length;
		if (prev_rgd) {
			uint32_t rgblocks, bitblocks;

			rgblocks = block_bump;
			rgblocks2bitblocks(sdp->bsize, &rgblocks, &bitblocks);

			prev_rgd->ri.ri_length = bitblocks;
			prev_rgd->ri.ri_data = rgblocks;
			prev_rgd->ri.ri_data0 = prev_rgd->ri.ri_addr +
				prev_rgd->ri.ri_length;
			prev_rgd->ri.ri_data -= prev_rgd->ri.ri_data %
				GFS2_NBBY;
			prev_rgd->ri.ri_bitbytes = prev_rgd->ri.ri_data /
				GFS2_NBBY;
			log_debug( _("Prev ri_data set to: %lx.\n"),
				  (unsigned long)prev_rgd->ri.ri_data);
		}
		number_of_rgs++;
		if (rg_was_fnd)
			log_info( _("  rgrp %d at block 0x%llx intact"),
				  number_of_rgs, (unsigned long long)blk);
		else
			log_warn( _("* rgrp %d at block 0x%llx *** DAMAGED ***"),
				  number_of_rgs, (unsigned long long)blk);
		prev_rgd = calc_rgd;
		/*
		 * Figure out where our next rgrp should be.
		 */
		if (blk == sdp->sb_addr + 1)
			block_bump = first_rg_dist;
		else if (!gfs_grow) {
			block_bump = shortest_dist_btwn_rgs;
			/* if we have uniformly-spaced rgrps, there may be
			   some wasted space at the end of the device.
			   Since we don't want to create a short rgrp and
			   break our uniformity, just quit here. */
			if (blk + (2 * block_bump) > sdp->device.length)
				break;
		} else if (rg_was_fnd)
			block_bump = find_next_rgrp_dist(sdp, blk, prev_rgd);
		else
			block_bump = hunt_and_peck(sdp, blk, prev_rgd,
						   block_bump);
		if (block_bump != 1) {
			if (rg_was_fnd)
				log_info( _(" [length 0x%llx]\n"),
					  (unsigned long long)block_bump);
			else
				log_warn( _(" [length 0x%llx]\n"),
					  (unsigned long long)block_bump);
		} else {
			log_warn("\n");
		}
		blk += block_bump;
	} /* for each rg block */
	/* ----------------------------------------------------------------- */
	/* If we got to the end of the fs, we still need to fix the          */
	/* allocation information for the very last RG.                      */
	/* ----------------------------------------------------------------- */
	if (prev_rgd && !prev_rgd->ri.ri_data) {
		uint32_t rgblocks, bitblocks;

		rgblocks = block_bump;
		rgblocks2bitblocks(sdp->bsize, &rgblocks, &bitblocks);

		prev_rgd->ri.ri_length = bitblocks;
		prev_rgd->ri.ri_data0 = prev_rgd->ri.ri_addr +
			prev_rgd->ri.ri_length;
		prev_rgd->ri.ri_data = rgblocks;
		prev_rgd->ri.ri_data -= prev_rgd->ri.ri_data % GFS2_NBBY;
		prev_rgd->ri.ri_bitbytes = prev_rgd->ri.ri_data / GFS2_NBBY;
		log_debug( _("Prev ri_data set to: %lx.\n"),
			  (unsigned long)prev_rgd->ri.ri_data);
		prev_rgd = NULL; /* make sure we don't use it later */
	}
        /* ---------------------------------------------- */
        /* Now dump out the information (if verbose mode) */      
        /* ---------------------------------------------- */
        log_debug( _("rindex rebuilt as follows:\n"));
        for (tmp = ret_list->next, rgi = 0; tmp != ret_list;
	     tmp = tmp->next, rgi++) {
                calc_rgd = osi_list_entry(tmp, struct rgrp_list, list);
                log_debug("%d: 0x%llx / %x / 0x%llx"
			  " / 0x%x / 0x%x\n", rgi + 1,
			  (unsigned long long)calc_rgd->ri.ri_addr,
			  calc_rgd->ri.ri_length,
			  calc_rgd->ri.ri_data0, calc_rgd->ri.ri_data, 
			  calc_rgd->ri.ri_bitbytes);
        }
	*num_rgs = number_of_rgs;
	return 0;
}

/*
 * gfs2_rindex_calculate - calculate what the rindex should look like
 *                          in a perfect world (trust_lvl == open_minded)
 *
 * Calculate what the rindex should look like, 
 * so we can later check if all RG index entries are sane.
 * This is a lot easier for gfs2 because we can just call the same libgfs2 
 * functions used by mkfs.
 *
 * Returns: 0 on success, -1 on failure
 * Sets:    sdp->rglist to a linked list of fsck_rgrp structs representing
 *          what we think the rindex should really look like.
 */
static int gfs2_rindex_calculate(struct gfs2_sbd *sdp, osi_list_t *ret_list,
			   int *num_rgs)
{
	uint64_t num_rgrps = 0;

	/* ----------------------------------------------------------------- */
	/* Calculate how many RGs there are supposed to be based on the      */
	/* rindex filesize.  Remember that our trust level is open-minded    */
	/* here.  If the filesize of the rindex file is not a multiple of    */
	/* our rindex structures, then something's wrong and we can't trust  */
	/* the index.                                                        */
	/* ----------------------------------------------------------------- */
	*num_rgs = sdp->md.riinode->i_di.di_size / sizeof(struct gfs2_rindex);

	osi_list_init(ret_list);
	if (device_geometry(sdp)) {
		fprintf(stderr, _("Geometry error\n"));
		exit(-1);
	}
	if (fix_device_geometry(sdp)) {
		fprintf(stderr, _("Device is too small (%llu bytes)\n"),
			(unsigned long long)sdp->device.length << GFS2_BASIC_BLOCK_SHIFT);
		exit(-1);
	}

	/* Try all possible rgrp sizes: 2048, 1024, 512, 256, 128, 64, 32 */
	for (sdp->rgsize = GFS2_DEFAULT_RGSIZE; sdp->rgsize >= 32;
	     sdp->rgsize /= 2) {
		num_rgrps = how_many_rgrps(sdp, &sdp->device, TRUE);
		if (num_rgrps == *num_rgs) {
			log_info(_("rgsize must be: %lld (0x%llx)\n"),
				 (unsigned long long)sdp->rgsize,
				 (unsigned long long)sdp->rgsize);
			break;
		}
	}
	/* Compute the default resource group layout as mkfs would have done */
	compute_rgrp_layout(sdp, TRUE);
	build_rgrps(sdp, FALSE); /* FALSE = calc but don't write to disk. */
	log_debug( _("fs_total_size = 0x%llx blocks.\n"),
		  (unsigned long long)sdp->device.length);
	log_warn( _("L3: number of rgs in the index = %d.\n"), *num_rgs);
	/* Move the rg list to the return list */
	ret_list->next = sdp->rglist.next;
	ret_list->prev = sdp->rglist.prev;
	ret_list->next->prev = ret_list;
	ret_list->prev->next = ret_list;
	return 0;
}

/*
 * rewrite_rg_block - rewrite ("fix") a buffer with rg or bitmap data
 * returns: 0 if the rg was repaired, otherwise 1
 */
static int rewrite_rg_block(struct gfs2_sbd *sdp, struct rgrp_list *rg,
		     uint64_t errblock)
{
	int x = errblock - rg->ri.ri_addr;
	const char *typedesc = x ? "GFS2_METATYPE_RB" : "GFS2_METATYPE_RG";

	log_err( _("Block #%lld (0x%llx) (%d of %d) is not %s.\n"),
		 (unsigned long long)rg->ri.ri_addr + x,
		 (unsigned long long)rg->ri.ri_addr + x,
		 (int)x+1, (int)rg->ri.ri_length, typedesc);
	if (query( _("Fix the Resource Group? (y/n)"))) {
		log_err( _("Attempting to repair the rgrp.\n"));
		rg->bh[x] = bread(sdp, rg->ri.ri_addr + x);
		if (x) {
			struct gfs2_meta_header mh;

			mh.mh_magic = GFS2_MAGIC;
			mh.mh_type = GFS2_METATYPE_RB;
			mh.mh_format = GFS2_FORMAT_RB;
			gfs2_meta_header_out(&mh, rg->bh[x]);
		} else {
			memset(&rg->rg, 0, sizeof(struct gfs2_rgrp));
			rg->rg.rg_header.mh_magic = GFS2_MAGIC;
			rg->rg.rg_header.mh_type = GFS2_METATYPE_RG;
			rg->rg.rg_header.mh_format = GFS2_FORMAT_RG;
			rg->rg.rg_free = rg->ri.ri_data;
			gfs2_rgrp_out(&rg->rg, rg->bh[x]);
		}
		brelse(rg->bh[x]);
		rg->bh[x] = NULL;
		return 0;
	}
	return 1;
}

/*
 * expect_rindex_sanity - the rindex file seems trustworthy, so use those
 *                        values as our expected values and assume the
 *                        damage is only to the rgrps themselves.
 */
static int expect_rindex_sanity(struct gfs2_sbd *sdp, osi_list_t *ret_list,
				int *num_rgs)
{
	osi_list_t *tmp;
	struct rgrp_list *exp, *rgd; /* expected, actual */

	*num_rgs = sdp->md.riinode->i_di.di_size / sizeof(struct gfs2_rindex) ;
	osi_list_init(ret_list);
	for (tmp = sdp->rglist.next; tmp != &sdp->rglist; tmp = tmp->next) {
		rgd = osi_list_entry(tmp, struct rgrp_list, list);

		exp = calloc(1, sizeof(struct rgrp_list));
		if (exp == NULL) {
			fprintf(stderr, "Out of memory in %s\n", __FUNCTION__);
			exit(-1);
		}
		exp->start = rgd->start;
		exp->length = rgd->length;
		memcpy(&exp->ri, &rgd->ri, sizeof(exp->ri));
		memcpy(&exp->rg, &rgd->rg, sizeof(exp->rg));
		exp->bits = NULL;
		exp->bh = NULL;
		gfs2_compute_bitstructs(sdp, exp);
		osi_list_add_prev(&exp->list, ret_list);
	}
	sdp->rgrps = *num_rgs;
	return 0;
}

/*
 * sort_rgrp_list - sort the rgrp list
 *
 * A bit crude, perhaps, but we're talking about thousands, not millions of
 * entries to sort, and the list will be almost sorted anyway, so there
 * should be very few swaps.
 */
static void sort_rgrp_list(osi_list_t *head)
{
	struct rgrp_list *thisr, *nextr;
	osi_list_t *tmp, *x, *next;
	int swaps;

	while (1) {
		swaps = 0;
		osi_list_foreach_safe(tmp, head, x) {
			next = tmp->next;
			if (next == head) /* at the end */
				break;
			thisr = osi_list_entry(tmp, struct rgrp_list, list);
			nextr = osi_list_entry(next, struct rgrp_list, list);
			if (thisr->ri.ri_addr > nextr->ri.ri_addr) {
				osi_list_del(next);
				osi_list_add_prev(next, tmp);
				swaps++;
			}
		}
		if (!swaps)
			break;
	}
}

/*
 * rg_repair - try to repair a damaged rg index (rindex)
 * trust_lvl - This is how much we trust the rindex file.
 *             blind_faith means we take the rindex at face value.
 *             open_minded means it might be okay, but we should verify it.
 *             distrust means it's not to be trusted, so we should go to
 *             greater lengths to build it from scratch.
 *             indignation means we have corruption, but the file system
 *             was converted from GFS via gfs2_convert, and its rgrps are
 *             not on nice boundaries thanks to previous gfs_grow ops. Lovely.
 */
int rg_repair(struct gfs2_sbd *sdp, int trust_lvl, int *rg_count, int *sane)
{
	int error, discrepancies, percent;
	osi_list_t expected_rglist;
	int calc_rg_count = 0, rgcount_from_index, rg;
	osi_list_t *exp, *act; /* expected, actual */
	struct gfs2_rindex buf;

	/* Free previous incarnations in memory, if any. */
	gfs2_rgrp_free(&sdp->rglist);

	if (trust_lvl == blind_faith)
		return 0;
	else if (trust_lvl == ye_of_little_faith) { /* if rindex seems sane */
		if (!(*sane)) {
			log_err(_("The rindex file does not meet our "
				  "expectations.\n"));
			return -1;
		}
		error = expect_rindex_sanity(sdp, &expected_rglist,
					     &calc_rg_count);
		if (error) {
			gfs2_rgrp_free(&expected_rglist);
			return error;
		}
	} else if (trust_lvl == open_minded) { /* If we can't trust RG index */
		/* Calculate our own RG index for comparison */
		error = gfs2_rindex_calculate(sdp, &expected_rglist,
					      &calc_rg_count);
		if (error) { /* If calculated RGs don't match the fs */
			gfs2_rgrp_free(&expected_rglist);
			return -1;
		}
	}
	else if (trust_lvl == distrust) { /* If we can't trust RG index */
		error = gfs2_rindex_rebuild(sdp, &expected_rglist,
					    &calc_rg_count, 0);
		if (error) {
			log_crit( _("Error rebuilding rgrp list.\n"));
			gfs2_rgrp_free(&expected_rglist);
			return -1;
		}
		sdp->rgrps = calc_rg_count;
	}
	else if (trust_lvl == indignation) { /* If we can't trust anything */
		error = gfs2_rindex_rebuild(sdp, &expected_rglist,
					    &calc_rg_count, 1);
		if (error) {
			log_crit( _("Error rebuilding rgrp list.\n"));
			gfs2_rgrp_free(&expected_rglist);
			return -1;
		}
		sdp->rgrps = calc_rg_count;
	}
	/* Read in the rindex */
	osi_list_init(&sdp->rglist); /* Just to be safe */
	rindex_read(sdp, 0, &rgcount_from_index, sane);
	if (sdp->md.riinode->i_di.di_size % sizeof(struct gfs2_rindex)) {
		log_warn( _("WARNING: rindex file is corrupt.\n"));
		gfs2_rgrp_free(&expected_rglist);
		gfs2_rgrp_free(&sdp->rglist);
		return -1;
	}
	log_warn( _("L%d: number of rgs expected     = %lld.\n"), trust_lvl + 1,
		 (unsigned long long)sdp->rgrps);
	if (calc_rg_count != sdp->rgrps) {
		log_warn( _("L%d: They don't match; either (1) the fs was "
			    "extended, (2) an odd\n"), trust_lvl + 1);
		log_warn( _("L%d: rgrp size was used, or (3) we have a corrupt "
			    "rg index.\n"), trust_lvl + 1);
		gfs2_rgrp_free(&expected_rglist);
		gfs2_rgrp_free(&sdp->rglist);
		return -1;
	}
	/* ------------------------------------------------------------- */
	/* Sort the rindex list.  Older versions of gfs_grow got the     */
	/* rindex out of sorted order.  But rebuilding the rindex from   */
	/* scratch will rebuild it in sorted order.                      */
	/* The gfs2_grow program should, in theory, drop new rgrps into  */
	/* the rindex in sorted order, so this should only matter for    */
	/* gfs1 converted file systems.                                  */
	/* ------------------------------------------------------------- */
	sort_rgrp_list(&sdp->rglist);
	/* ------------------------------------------------------------- */
	/* Now compare the rindex to what we think it should be.         */
	/* See how far off our expected values are.  If too much, abort. */
	/* The theory is: if we calculated the index to have 32 RGs and  */
	/* we have a large number that are completely wrong, we should   */
	/* abandon this method of recovery and try a better one.         */
	/* ------------------------------------------------------------- */
	discrepancies = 0;
	for (rg = 0, act = sdp->rglist.next, exp = expected_rglist.next;
	     act != &sdp->rglist && exp != &expected_rglist && !fsck_abort;
	     rg++) {
		struct rgrp_list *expected, *actual;

		expected = osi_list_entry(exp, struct rgrp_list, list);
		actual = osi_list_entry(act, struct rgrp_list, list);
		if (actual->ri.ri_addr < expected->ri.ri_addr) {
			act = act->next;
			discrepancies++;
			log_info(_("%d addr: 0x%llx < 0x%llx * mismatch\n"),
				 rg + 1, actual->ri.ri_addr,
				 expected->ri.ri_addr);
			continue;
		} else if (expected->ri.ri_addr < actual->ri.ri_addr) {
			exp = exp->next;
			discrepancies++;
			log_info(_("%d addr: 0x%llx > 0x%llx * mismatch\n"),
				 rg + 1, actual->ri.ri_addr,
				 expected->ri.ri_addr);
			continue;
		}
		if (!ri_equal(actual->ri, expected->ri, ri_length) ||
		    !ri_equal(actual->ri, expected->ri, ri_data0) ||
		    !ri_equal(actual->ri, expected->ri, ri_data) ||
		    !ri_equal(actual->ri, expected->ri, ri_bitbytes)) {
			discrepancies++;
			log_info(_("%d addr: 0x%llx 0x%llx * has mismatch\n"),
				 rg + 1, actual->ri.ri_addr,
				 expected->ri.ri_addr);
		}
		act = act->next;
		exp = exp->next;
	}
	if (rg) {
		/* Check to see if more than 2% of the rgrps are wrong.  */
		percent = (discrepancies * 100) / rg;
		if (percent > BAD_RG_PERCENT_TOLERANCE) {
			log_warn( _("Level %d didn't work.  Too many "
				    "discrepancies.\n"), trust_lvl + 1);
			log_warn( _("%d out of %d rgrps (%d percent) did not "
				    "match what was expected.\n"),
				  discrepancies, rg, percent);
			gfs2_rgrp_free(&expected_rglist);
			gfs2_rgrp_free(&sdp->rglist);
			return -1;
		}
	}
	/* ------------------------------------------------------------- */
	/* Now compare the rindex to what we think it should be.         */
	/* Our rindex should be pretty predictable unless we've grown    */
	/* so look for index problems first before looking at the rgs.   */
	/* ------------------------------------------------------------- */
	for (rg = 0, act = sdp->rglist.next, exp = expected_rglist.next;
	     exp != &expected_rglist && !fsck_abort; rg++) {
		struct rgrp_list *expected, *actual;

		expected = osi_list_entry(exp, struct rgrp_list, list);

		/* If we ran out of actual rindex entries due to rindex
		   damage, fill in a new one with the expected values. */
		if (act == &sdp->rglist) { /* end of actual rindex */
			log_err( _("Entry missing from rindex: 0x%llx\n"),
				 (unsigned long long)expected->ri.ri_addr);
			actual = (struct rgrp_list *)
				malloc(sizeof(struct rgrp_list));
			if (!actual) {
				log_err(_("Out of memory!\n"));
				break;
			}
			memset(actual, 0, sizeof(struct rgrp_list));
			osi_list_add_prev(&actual->list, &sdp->rglist);
			rindex_modified = 1;
		} else {
			actual = osi_list_entry(act, struct rgrp_list, list);
			ri_compare(rg, actual->ri, expected->ri, ri_addr,
				   "llx", unsigned long long);
			ri_compare(rg, actual->ri, expected->ri, ri_length,
				   "lx", unsigned long);
			ri_compare(rg, actual->ri, expected->ri, ri_data0,
				   "llx", unsigned long long);
			ri_compare(rg, actual->ri, expected->ri, ri_data,
				   "lx", unsigned long);
			ri_compare(rg, actual->ri, expected->ri, ri_bitbytes,
				   "lx", unsigned long);
		}
		/* If we modified the index, write it back to disk. */
		if (rindex_modified) {
			if (query( _("Fix the index? (y/n)"))) {
				gfs2_rindex_out(&expected->ri, (char *)&buf);
				gfs2_writei(sdp->md.riinode, (char *)&buf,
					    rg * sizeof(struct gfs2_rindex),
					    sizeof(struct gfs2_rindex));
				actual->ri.ri_addr = expected->ri.ri_addr;
				actual->ri.ri_length = expected->ri.ri_length;
				actual->ri.ri_data0 = expected->ri.ri_data0;
				actual->ri.ri_data = expected->ri.ri_data;
				actual->ri.ri_bitbytes =
					expected->ri.ri_bitbytes;
				/* If our rindex was hosed, ri_length is bad */
				/* Therefore, gfs2_compute_bitstructs might  */
				/* have malloced the wrong length for bitmap */
				/* buffers.  So we have to redo it.          */
				if (actual->bits) {
					free(actual->bits);
					actual->bits = NULL;
				}
			}
			else
				log_err( _("rindex not fixed.\n"));
			gfs2_compute_bitstructs(sdp, actual);
			rindex_modified = FALSE;
		}
		exp = exp->next;
		if (act != &sdp->rglist)
			act = act->next;
	}
	/* ------------------------------------------------------------- */
	/* Read the real RGs and check their integrity.                  */
	/* Now we can somewhat trust the rindex and the RG addresses,    */
	/* so let's read them in, check them and optionally fix them.    */
	/* ------------------------------------------------------------- */
	for (rg = 0, act = sdp->rglist.next; act != &sdp->rglist &&
		     !fsck_abort; act = act->next, rg++) {
		struct rgrp_list *rgd;
		uint64_t prev_err = 0, errblock;
		int i;

		/* Now we try repeatedly to read in the rg.  For every block */
		/* we encounter that has errors, repair it and try again.    */
		i = 0;
		do {
			rgd = osi_list_entry(act, struct rgrp_list, list);
			errblock = gfs2_rgrp_read(sdp, rgd);
			if (errblock) {
				if (errblock == prev_err)
					break;
				prev_err = errblock;
				rewrite_rg_block(sdp, rgd, errblock);
			} else {
				gfs2_rgrp_relse(rgd);
				break;
			}
			i++;
		} while (i < rgd->ri.ri_length);
	}
	*rg_count = rg;
	gfs2_rgrp_free(&expected_rglist);
	gfs2_rgrp_free(&sdp->rglist);
	return 0;
}
