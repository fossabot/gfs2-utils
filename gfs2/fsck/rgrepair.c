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

#include <logging.h>
#include "libgfs2.h"
#include "osi_list.h"
#include "fsck.h"
#include "fs_recovery.h"

static int rindex_modified = 0;
static struct special_blocks false_rgrps;
static struct osi_root rgcalc;

#define BAD_RG_PERCENT_TOLERANCE 11
#define AWAY_FROM_BITMAPS 0x1000
#define MAX_RGSEGMENTS 20

#define ri_compare(rg, ondisk, expected, field, fmt)	\
	if (ondisk->field != expected->field) { \
		log_warn( _("rindex #%d " #field " discrepancy: index 0x%" \
			    fmt	" != expected: 0x%" fmt "\n"),		\
			  rg + 1, ondisk->field, expected->field);	\
		ondisk->field = expected->field;				\
		rindex_modified = 1;					\
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
 *
 * NOTE: This function assumes that the jindex and journals have been read in,
 *       which isn't often the case. Normally the rindex needs to be read in
 *       first. If the rindex is damaged, that's not an option.
 */
static void find_journaled_rgs(struct gfs2_sbd *sdp)
{
	int j, new = 0;
	unsigned int jblocks;
	uint64_t b, dblock;
	struct gfs2_inode *ip;
	struct gfs2_buffer_head *bh;
	int false_count;

	osi_list_init(&false_rgrps.list);
	for (j = 0; j < sdp->md.journals; j++) {
		ip = sdp->md.journal[j];
		log_debug(_("Checking for rgrps in journal%d which starts at block 0x%"PRIx64".\n"),
		          j, ip->i_num.in_addr);
		jblocks = ip->i_size / sdp->sd_bsize;
		false_count = 0;
		for (b = 0; b < jblocks; b++) {
			block_map(ip, b, &new, &dblock, NULL, 0);
			if (!dblock)
				break;
			bh = bread(sdp, dblock);
			if (!gfs2_check_meta(bh->b_data, GFS2_METATYPE_RG)) {
				/* False rgrp found at block dblock */
				false_count++;
				gfs2_special_set(&false_rgrps, dblock);
			}
			brelse(bh);
		}
		log_debug("\n%d false positives identified.\n", false_count);
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
 *
 * This function was revised to return the number of segments, usually 2.
 * The shortest distance is now returned in the highest entry in rg_dist
 */
static int find_shortest_rgdist(struct gfs2_sbd *sdp, uint64_t *dist_array,
				int *dist_cnt)
{
	uint64_t blk, block_last_rg, shortest_dist_btwn_rgs;
	struct gfs2_buffer_head *bh;
	int rgs_sampled = 0;
	uint64_t initial_first_rg_dist;
	int gsegment = 0;
	int is_rgrp;

	/* Figure out if there are any RG-looking blocks in the journal we
	   need to ignore. */
	find_journaled_rgs(sdp);

	initial_first_rg_dist = dist_array[0] = block_last_rg =
		LGFS2_SB_ADDR(sdp) + 1;
	shortest_dist_btwn_rgs = sdp->device.length;

	for (blk = LGFS2_SB_ADDR(sdp) + 1; blk < sdp->device.length; blk++) {
		uint64_t dist;

		if (blk == LGFS2_SB_ADDR(sdp) + 1)
			is_rgrp = 1;
		else if (is_false_rg(blk))
			is_rgrp = 0;
		else {
			bh = bread(sdp, blk);
			is_rgrp = (gfs2_check_meta(bh->b_data, GFS2_METATYPE_RG) == 0);
			brelse(bh);
		}
		if (!is_rgrp) {
			if (rgs_sampled >= 6) {
				uint64_t nblk;

				log_info(_("rgrp not found at block 0x%llx. "
					   "Last found rgrp was 0x%llx. "
					   "Checking the next one.\n"),
					 (unsigned long long)blk,
					 (unsigned long long)block_last_rg);
				/* check for just a damaged rgrp */
				nblk = blk + dist_array[gsegment];
				if (is_false_rg(nblk)) {
					is_rgrp = 0;
				} else {
					bh = bread(sdp, nblk);
					is_rgrp = (((gfs2_check_meta(bh->b_data,
						GFS2_METATYPE_RG) == 0)));
					brelse(bh);
				}
				if (is_rgrp) {
					log_info(_("Next rgrp is intact, so "
						   "this one is damaged.\n"));
					blk = nblk - 1;
					dist_cnt[gsegment]++;
					continue;
				}
				log_info(_("Looking for new segment.\n"));
				blk -= 16;
				rgs_sampled = 0;
				shortest_dist_btwn_rgs = sdp->device.length;
				/* That last one didn't pan out, so: */
				dist_cnt[gsegment]--;
				gsegment++;
				if (gsegment >= MAX_RGSEGMENTS)
					break;
			}
			if ((blk - block_last_rg) > (524288 * 2)) {
				log_info(_("No rgrps were found within 4GB "
					   "of the last rgrp. Must be the "
					   "end of the file system.\n"));

				break;
			}
			continue;
		}

		dist_cnt[gsegment]++;
		if (rgs_sampled >= 6) {
			block_last_rg = blk;
			blk += dist_array[gsegment] - 1; /* prev value in
							    array minus 1. */
			continue;
		}
		log_info(_("segment %d: rgrp found at block 0x%llx\n"),
			 gsegment + 1, (unsigned long long)blk);
		dist = blk - block_last_rg;
		if (blk > LGFS2_SB_ADDR(sdp) + 1) { /* not the very first rgrp */

			log_info("dist 0x%llx = 0x%llx - 0x%llx ",
				 (unsigned long long)dist,
				 (unsigned long long)blk,
				 (unsigned long long)block_last_rg);
			/**
			 * We found an RG.  Check to see if we need to set the
			 * first_rg_dist based on whether it is still at its
			 * initial value (i.e. the fs.)  The first rg distance
			 * is different from the rest because of the
			 * superblock and 64K dead space.
			 **/
			if (dist_array[0] == initial_first_rg_dist) {
				dist_array[0] = dist;
				dist_cnt[0] = 1;
				rgs_sampled = 0;
			}
			if (dist < shortest_dist_btwn_rgs) {
				shortest_dist_btwn_rgs = dist;
				log_info( _("(shortest so far)"));
			}
			log_info("\n");
			if (++rgs_sampled == 6) {
				dist_array[gsegment] = shortest_dist_btwn_rgs;
				log_info(_("Settled on distance 0x%llx for "
					   "segment %d\n"),
					 (unsigned long long)
					 dist_array[gsegment], gsegment + 1);
			}
		} else {
			gsegment++;
			if (gsegment >= MAX_RGSEGMENTS)
				break;
		}
		block_last_rg = blk;
		if (rgs_sampled < 6)
			blk += 250; /* skip ahead for performance */
		else
			blk += shortest_dist_btwn_rgs - 1;
	}
	if (gsegment >= MAX_RGSEGMENTS) {
		log_err(_("Maximum number of rgrp grow segments reached.\n"));
		log_err(_("This file system has more than %d resource "
			  "group segments.\n"), MAX_RGSEGMENTS);
	}
	/* -------------------------------------------------------------- */
	/* Sanity-check our first_rg_dist. If RG #2 got nuked, the        */
	/* first_rg_dist would measure from #1 to #3, which would be bad. */
	/* We need to take remedial measures to fix it (from the index).  */
	/* -------------------------------------------------------------- */
	if (*dist_array >= shortest_dist_btwn_rgs +
	    (shortest_dist_btwn_rgs / 4)) {
		struct gfs2_rindex ri;

		/* read in the second RG index entry for this subd. */
		gfs2_readi(sdp->md.riinode, &ri, sizeof(ri), sizeof(ri));

		if (be64_to_cpu(ri.ri_addr) > LGFS2_SB_ADDR(sdp) + 1) { /* sanity check */
			log_warn( _("rgrp 2 is damaged: getting dist from index: "));
			*dist_array = be64_to_cpu(ri.ri_addr) - (LGFS2_SB_ADDR(sdp) + 1);
			log_warn("0x%llx\n", (unsigned long long)*dist_array);
		} else {
			log_warn( _("rgrp index 2 is damaged: extrapolating dist: "));
			*dist_array = sdp->device.length - (sdp->rgrps - 1) *
				(sdp->device.length / sdp->rgrps);
			log_warn("0x%llx\n", (unsigned long long)*dist_array);
		}
		log_debug( _("Adjusted first rgrp distance: 0x%llx\n"),
			   (unsigned long long)*dist_array);
	} /* if first RG distance is within tolerance */

	gfs2_special_free(&false_rgrps);
	return gsegment;
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
	if (first) {
		if (sdp->gfs1)
			off = sizeof(struct gfs_rgrp);
		else
			off = sizeof(struct gfs2_rgrp);
	} else
		off = sizeof(struct gfs2_meta_header);
	bytes_to_check = sdp->sd_bsize - off;
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
				    struct rgrp_tree *prevrgd)
{
	struct osi_node *n, *next = NULL;
	uint64_t rgrp_dist = 0, used_blocks, block, next_block, twogigs;
	struct rgrp_tree *rgd = NULL, *next_rgd;
	struct gfs2_buffer_head *bh;
	int first, length, b, found;
	uint64_t mega_in_blocks;
	uint32_t free_blocks;

	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rgd = (struct rgrp_tree *)n;
		if (rgd->rt_addr == blk)
			break;
	}
	if (rgd && n && osi_next(n) && rgd->rt_addr == blk) {
		n = osi_next(n);
		next_rgd = (struct rgrp_tree *)n;
		rgrp_dist = next_rgd->rt_addr - rgd->rt_addr;
		return rgrp_dist;
	}
	mega_in_blocks = (1024 * 1024)  / sdp->sd_bsize;
	twogigs = (uint64_t)mega_in_blocks * 2048;
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
	block = prevrgd->rt_addr;
	first = 1;
	found = 0;
	while (1) {
		struct gfs2_meta_header *mh;

		if (block >= sdp->device.length)
			break;
		if (block >= prevrgd->rt_addr + twogigs)
			break;
		bh = bread(sdp, block);
		mh = (struct gfs2_meta_header *)bh->b_data;
		if ((be32_to_cpu(mh->mh_magic) != GFS2_MAGIC) ||
		    (first && be32_to_cpu(mh->mh_type) != GFS2_METATYPE_RG) ||
		    (!first && be32_to_cpu(mh->mh_type) != GFS2_METATYPE_RB)) {
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
		next_block = prevrgd->rt_addr + rgrp_dist;
		/* Now we account for block rounding done by mkfs.gfs2 */
		for (b = 0; b <= length + GFS2_NBBY; b++) {
			if (next_block + b >= sdp->device.length)
				break;
			bh = bread(sdp, next_block + b);
			mh = (struct gfs2_meta_header *)bh->b_data;
			if (be32_to_cpu(mh->mh_magic) == GFS2_MAGIC) {
				if (be32_to_cpu(mh->mh_type) == GFS2_METATYPE_RG)
					found = 1;
				/* if the first thing we find is a bitmap,
				   there must be a damaged rgrp on the
				   previous block. */
				if (be32_to_cpu(mh->mh_type) == GFS2_METATYPE_RB) {
					found = 1;
					rgrp_dist--;
				}
			}
			brelse(bh);
			if (found)
				break;
			rgrp_dist++;
		}
		if (found) {
			log_info(_("rgrp found at 0x%"PRIx64", length=%d, used=%"PRIu64", free=%d\n"),
			         prevrgd->rt_addr, length, used_blocks, free_blocks);
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
			      struct rgrp_tree *prevrgd, uint64_t last_bump)
{
	uint64_t rgrp_dist = 0, block, twogigs, last_block, last_meg;
	struct gfs2_buffer_head *bh;
	struct gfs2_meta_header *mh;
	int b, mega_in_blocks;

	/* Skip ahead the previous amount: we might get lucky.
	   If we're close to the end of the device, take the rest. */
	if (gfs2_check_range(sdp, blk + last_bump))
		return sdp->fssize - blk;

	bh = bread(sdp, blk + last_bump);
	mh = (struct gfs2_meta_header *)bh->b_data;
	if (be32_to_cpu(mh->mh_magic) == GFS2_MAGIC &&
	    be32_to_cpu(mh->mh_type) == GFS2_METATYPE_RG) {
		log_info( _("rgrp found at 0x%llx, length=%lld\n"),
			  (unsigned long long)blk + last_bump,
			  (unsigned long long)last_bump);
		brelse(bh);
		return last_bump;
	}
	brelse(bh);

	rgrp_dist = AWAY_FROM_BITMAPS; /* Get away from any bitmaps
					  associated with the previous rgrp */
	block = prevrgd->rt_addr + rgrp_dist;
	/* Now we account for block rounding done by mkfs.gfs2.  A rgrp can
	   be at most 2GB in size, so that's where we call it. We do somewhat
	   obscure math here to avoid integer overflows. */
	mega_in_blocks = (1024 * 1024)  / sdp->sd_bsize;
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
		uint32_t magic, type;

		bh = bread(sdp, block + b);
		mh = (struct gfs2_meta_header *)bh->b_data;
		magic = be32_to_cpu(mh->mh_magic);
		type = be32_to_cpu(mh->mh_type);
		brelse(bh);
		if (magic == GFS2_MAGIC) {
			if (type == GFS2_METATYPE_RG)
				break;
			/* if the first thing we find is a bitmap, there must
			   be a damaged rgrp on the previous block. */
			if (type == GFS2_METATYPE_RB) {
				rgrp_dist--;
				break;
			}
		}
		rgrp_dist++;
	}
	return rgrp_dist + last_meg;
}

/*
 * rindex_rebuild - rebuild a corrupt Resource Group (RG) index manually
 *                  where trust_lvl == DISTRUST
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
static int rindex_rebuild(struct gfs2_sbd *sdp, int *num_rgs, int gfs_grow)
{
	struct osi_node *n, *next = NULL;
	struct gfs2_buffer_head *bh;
	uint64_t rg_dist[MAX_RGSEGMENTS] = {0, };
	int rg_dcnt[MAX_RGSEGMENTS] = {0, };
	uint64_t blk;
	uint64_t fwd_block, block_bump;
	struct rgrp_tree *calc_rgd, *prev_rgd;
	int number_of_rgs, rgi, segment_rgs;
	int rg_was_fnd = 0, corrupt_rgs = 0;
	int error = -1, j, i;
	int grow_segments, segment = 0;

	/*
	 * In order to continue, we need to initialize the jindex. We need
	 * the journals in order to correctly eliminate false positives during
	 * rgrp repair. IOW, we need to properly ignore rgrps that appear in
	 * the journals, and we can only do that if we have the journals.
	 * To make matters worse, journals may span several (small) rgrps,
	 * so we can't go by the rgrps.
	 */
	if (init_jindex(sdp, 0) != 0) {
		log_crit(_("Error: Can't read jindex required for rindex "
			   "repairs.\n"));
		return -1;
	}

	rgcalc.osi_node = NULL;
	grow_segments = find_shortest_rgdist(sdp, &rg_dist[0], &rg_dcnt[0]);
	for (i = 0; i < grow_segments; i++)
		log_info(_("Segment %d: rgrp distance: 0x%llx, count: %d\n"),
			  i + 1, (unsigned long long)rg_dist[i], rg_dcnt[i]);
	number_of_rgs = segment_rgs = 0;
	/* -------------------------------------------------------------- */
	/* Now go through the RGs and verify their integrity, fixing as   */
	/* needed when corruption is encountered.                         */
	/* -------------------------------------------------------------- */
	prev_rgd = NULL;
	block_bump = rg_dist[0];
	blk = LGFS2_SB_ADDR(sdp) + 1;
	while (blk <= sdp->device.length) {
		log_debug( _("Block 0x%llx\n"), (unsigned long long)blk);
		bh = bread(sdp, blk);
		rg_was_fnd = (!gfs2_check_meta(bh->b_data, GFS2_METATYPE_RG));
		brelse(bh);
		/* Allocate a new RG and index. */
		calc_rgd = rgrp_insert(&rgcalc, blk);
		if (!calc_rgd) {
			log_crit( _("Can't allocate memory for rgrp repair.\n"));
			goto out;
		}
		calc_rgd->rt_length = 1;
		if (!rg_was_fnd) { /* if not an RG */
			/* ------------------------------------------------- */
			/* This SHOULD be an RG but isn't.                   */
			/* ------------------------------------------------- */
			corrupt_rgs++;
			if (corrupt_rgs < 5)
				log_debug(_("Missing or damaged rgrp at block "
					    "%llu (0x%llx)\n"),
					  (unsigned long long)blk,
					  (unsigned long long)blk);
			else {
				log_crit( _("Error: too many missing or "
					    "damaged rgrps using this method. "
					    "Time to try another method.\n"));
				goto out;
			}
		}
		/* ------------------------------------------------ */
		/* Now go through and count the bitmaps for this RG */
		/* ------------------------------------------------ */
		for (fwd_block = blk + 1; fwd_block < sdp->device.length; fwd_block++) {
			int bitmap_was_fnd;
			bh = bread(sdp, fwd_block);
			bitmap_was_fnd = !gfs2_check_meta(bh->b_data, GFS2_METATYPE_RB);
			brelse(bh);
			if (bitmap_was_fnd) /* if a bitmap */
				calc_rgd->rt_length++;
			else
				break; /* end of bitmap, so call it quits. */
		} /* for subsequent bitmaps */

		calc_rgd->rt_data0 = calc_rgd->rt_addr +
			calc_rgd->rt_length;
		if (prev_rgd) {
			uint32_t rgblocks;

			prev_rgd->rt_length = rgblocks2bitblocks(sdp->sd_bsize, block_bump, &rgblocks);
			prev_rgd->rt_data = rgblocks;
			prev_rgd->rt_data0 = prev_rgd->rt_addr +
				prev_rgd->rt_length;
			prev_rgd->rt_data -= prev_rgd->rt_data %
				GFS2_NBBY;
			prev_rgd->rt_bitbytes = prev_rgd->rt_data /
				GFS2_NBBY;
			log_debug(_("Prev ri_data set to: 0x%"PRIx32"\n"), prev_rgd->rt_data);
		}
		number_of_rgs++;
		segment_rgs++;
		if (rg_was_fnd)
			log_info( _("  rgrp %d at block 0x%llx intact\n"),
				  number_of_rgs, (unsigned long long)blk);
		else
			log_warn( _("* rgrp %d at block 0x%llx *** DAMAGED ***\n"),
				  number_of_rgs, (unsigned long long)blk);
		prev_rgd = calc_rgd;
		/*
		 * Figure out where our next rgrp should be.
		 */
		if ((blk == LGFS2_SB_ADDR(sdp) + 1) || (!gfs_grow)) {
			block_bump = rg_dist[segment];
			if (segment_rgs >= rg_dcnt[segment]) {
				log_debug(_("End of segment %d\n"), ++segment);
				segment_rgs = 0;
				if (segment >= grow_segments) {
					log_debug(_("Last segment.\n"));
					break;
				}
			}
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
	if (prev_rgd && !prev_rgd->rt_data) {
		uint32_t rgblocks;

		prev_rgd->rt_length = rgblocks2bitblocks(sdp->sd_bsize, block_bump, &rgblocks);
		prev_rgd->rt_data0 = prev_rgd->rt_addr + prev_rgd->rt_length;
		prev_rgd->rt_data = rgblocks;
		prev_rgd->rt_data -= prev_rgd->rt_data % GFS2_NBBY;
		prev_rgd->rt_bitbytes = prev_rgd->rt_data / GFS2_NBBY;
		log_debug(_("Prev ri_data set to: 0x%"PRIx32"\n"), prev_rgd->rt_data);
		prev_rgd = NULL; /* make sure we don't use it later */
	}
        /* ---------------------------------------------- */
        /* Now dump out the information (if verbose mode) */      
        /* ---------------------------------------------- */
        log_debug( _("rindex rebuilt as follows:\n"));
	for (n = osi_first(&rgcalc), rgi = 0; n; n = next, rgi++) {
		next = osi_next(n);
		calc_rgd = (struct rgrp_tree *)n;
                log_debug("%d: 0x%"PRIx64"/%"PRIx32"/0x%"PRIx64"/0x%"PRIx32"/0x%"PRIx32"\n",
		          rgi + 1, calc_rgd->rt_addr, calc_rgd->rt_length,
			  calc_rgd->rt_data0, calc_rgd->rt_data,
			  calc_rgd->rt_bitbytes);
        }
	*num_rgs = number_of_rgs;
	error = 0;
out:
	for (j = 0; j < sdp->md.journals; j++)
		inode_put(&sdp->md.journal[j]);
	inode_put(&sdp->md.jiinode);
	free(sdp->md.journal);
	return error;
}

#define DIV_RU(x, y) (((x) + (y) - 1) / (y))

/**
 * how_many_rgrps - figure out how many RG to put in a subdevice
 * @w: the command line
 * @dev: the device
 *
 * Returns: the number of RGs
 */
static uint64_t how_many_rgrps(struct gfs2_sbd *sdp, struct device *dev)
{
	uint64_t nrgrp;
	uint32_t rgblocks1, rgblocksn, bitblocks1, bitblocksn;

	while (1) {
		nrgrp = DIV_RU(dev->length, (sdp->rgsize << 20) / sdp->sd_bsize);

		/* check to see if the rg length overflows max # bitblks */
		bitblocksn = rgblocks2bitblocks(sdp->sd_bsize, dev->length / nrgrp, &rgblocksn);
		/* calculate size of the first rgrp */
		bitblocks1 = rgblocks2bitblocks(sdp->sd_bsize, dev->length - (nrgrp - 1) * (dev->length / nrgrp),
		                                &rgblocks1);
		if (bitblocks1 <= 2149 && bitblocksn <= 2149)
			break;

		sdp->rgsize -= GFS2_DEFAULT_RGSIZE; /* smaller rgs */

		if (sdp->rgsize < GFS2_DEFAULT_RGSIZE) {
			log_err(_("Cannot use the entire device with block size %u bytes.\n"),
			        sdp->sd_bsize);
			return 0;
		}
	}

	log_debug("  rg sz = %"PRIu32"\n  nrgrp = %"PRIu64"\n", sdp->rgsize,
		  nrgrp);

	return nrgrp;
}

/**
 * compute_rgrp_layout - figure out where the RG in a FS are
 */
static struct osi_root compute_rgrp_layout(struct gfs2_sbd *sdp)
{
	struct device *dev;
	struct rgrp_tree *rl, *rlast = NULL;
	unsigned int rgrp = 0, nrgrp, rglength;
	struct osi_root rgtree = {NULL};
	uint64_t rgaddr;

	dev = &sdp->device;

	dev->length -= LGFS2_SB_ADDR(sdp) + 1;
	nrgrp = how_many_rgrps(sdp, dev);
	if (nrgrp == 0)
		return (struct osi_root){NULL};
	rglength = dev->length / nrgrp;

	for (; rgrp < nrgrp; rgrp++) {
		if (rgrp) {
			rgaddr = rlast->rt_addr + rlast->rt_skip;
			rl = rgrp_insert(&rgtree, rgaddr);
			rl->rt_skip = rglength;
		} else {
			rgaddr = LGFS2_SB_ADDR(sdp) + 1;
			rl = rgrp_insert(&rgtree, rgaddr);
			rl->rt_skip = dev->length -
				(nrgrp - 1) * (dev->length / nrgrp);
		}
		rlast = rl;
	}

	sdp->rgrps = nrgrp;
	return rgtree;
}

static int calc_rgrps(struct gfs2_sbd *sdp)
{
	struct osi_node *n, *next = NULL;
	struct rgrp_tree *rl;
	uint32_t rgblocks, bitblocks;

	for (n = osi_first(&rgcalc); n; n = next) {
		next = osi_next(n);
		rl = (struct rgrp_tree *)n;

		bitblocks = rgblocks2bitblocks(sdp->sd_bsize, rl->rt_skip, &rgblocks);

		rl->rt_length = bitblocks;
		rl->rt_data0 = rl->rt_addr + bitblocks;
		rl->rt_data = rgblocks;
		rl->rt_bitbytes = rgblocks / GFS2_NBBY;
		rl->rt_free = rgblocks;

		if (gfs2_compute_bitstructs(sdp->sd_bsize, rl))
			return -1;

		sdp->blks_total += rgblocks;
		sdp->fssize = rl->rt_data0 + rl->rt_data;
	}
	return 0;
}

/*
 * gfs2_rindex_calculate - calculate what the rindex should look like
 *                          in a perfect world (trust_lvl == OPEN_MINDED)
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
static int gfs2_rindex_calculate(struct gfs2_sbd *sdp, int *num_rgs)
{
	uint64_t num_rgrps = 0;

	/* ----------------------------------------------------------------- */
	/* Calculate how many RGs there are supposed to be based on the      */
	/* rindex filesize.  Remember that our trust level is open-minded    */
	/* here.  If the filesize of the rindex file is not a multiple of    */
	/* our rindex structures, then something's wrong and we can't trust  */
	/* the index.                                                        */
	/* ----------------------------------------------------------------- */
	*num_rgs = sdp->md.riinode->i_size / sizeof(struct gfs2_rindex);

	rgcalc.osi_node = NULL;
	fix_device_geometry(sdp);

	/* Try all possible rgrp sizes: 2048, 1024, 512, 256, 128, 64, 32 */
	for (sdp->rgsize = GFS2_DEFAULT_RGSIZE; sdp->rgsize >= 32;
	     sdp->rgsize /= 2) {
		num_rgrps = how_many_rgrps(sdp, &sdp->device);
		if (num_rgrps == *num_rgs) {
			log_info(_("rgsize must be: %lld (0x%llx)\n"),
				 (unsigned long long)sdp->rgsize,
				 (unsigned long long)sdp->rgsize);
			break;
		}
	}
	/* Compute the default resource group layout as mkfs would have done */
	rgcalc = compute_rgrp_layout(sdp);
	if (calc_rgrps(sdp)) { /* Calculate but don't write to disk. */
		fprintf(stderr, _("Failed to build resource groups\n"));
		exit(-1);
	}
	log_debug( _("fs_total_size = 0x%llx blocks.\n"),
		  (unsigned long long)sdp->device.length);
	log_warn( _("L3: number of rgs in the index = %d.\n"), *num_rgs);
	return 0;
}

/*
 * rewrite_rg_block - rewrite ("fix") a buffer with rg or bitmap data
 * returns: 0 if the rg was repaired, otherwise 1
 */
static int rewrite_rg_block(struct gfs2_sbd *sdp, struct rgrp_tree *rg,
			    uint64_t errblock)
{
	int x = errblock - rg->rt_addr;
	const char *typedesc = x ? "GFS2_METATYPE_RB" : "GFS2_METATYPE_RG";
	ssize_t ret;
	char *buf;

	log_err(_("Block #%"PRIu64" (0x%"PRIx64") (%d of %"PRIu32") is not %s.\n"),
	        rg->rt_addr + x, rg->rt_addr + x, x+1, rg->rt_length, typedesc);
	if (!query( _("Fix the resource group? (y/n)")))
		return 1;

	log_err(_("Attempting to repair the resource group.\n"));

	buf = calloc(1, sdp->sd_bsize);
	if (buf == NULL) {
		log_err(_("Failed to allocate resource group block: %s"), strerror(errno));
		return 1;
	}
	ret = pread(sdp->device_fd, buf, sdp->sd_bsize, errblock * sdp->sd_bsize);
	if (ret != sdp->sd_bsize) {
		log_err(_("Failed to read resource group block %"PRIu64": %s\n"),
		        errblock, strerror(errno));
		free(buf);
		return 1;
	}
	if (x) {
		struct gfs2_meta_header mh = {
			.mh_magic = cpu_to_be32(GFS2_MAGIC),
			.mh_type = cpu_to_be32(GFS2_METATYPE_RB),
			.mh_format = cpu_to_be32(GFS2_FORMAT_RB)
		};
		memcpy(buf, &mh, sizeof(mh));
	} else {
		rg->rt_free = rg->rt_data;
		if (sdp->gfs1)
			lgfs2_gfs_rgrp_out(rg, buf);
		else
			lgfs2_rgrp_out(rg, buf);
	}
	ret = pwrite(sdp->device_fd, buf, sdp->sd_bsize, errblock * sdp->sd_bsize);
	if (ret != sdp->sd_bsize) {
		log_err(_("Failed to write resource group block %"PRIu64": %s\n"),
		        errblock, strerror(errno));
		free(buf);
		return 1;
	}
	free(buf);
	return 0;
}

/*
 * expect_rindex_sanity - the rindex file seems trustworthy, so use those
 *                        values as our expected values and assume the
 *                        damage is only to the rgrps themselves.
 */
static int expect_rindex_sanity(struct gfs2_sbd *sdp, int *num_rgs)
{
	struct osi_node *n, *next = NULL;
	struct rgrp_tree *rgd, *exp;

	*num_rgs = sdp->md.riinode->i_size / sizeof(struct gfs2_rindex) ;
	for (n = osi_first(&sdp->rgtree); n; n = next) {
		next = osi_next(n);
		rgd = (struct rgrp_tree *)n;
		exp = rgrp_insert(&rgcalc, rgd->rt_addr);
		if (exp == NULL) {
			fprintf(stderr, "Out of memory in %s\n", __FUNCTION__);
			exit(-1);
		}
		exp->rt_data0 = rgd->rt_data0;
		exp->rt_data = rgd->rt_data;
		exp->rt_length = rgd->rt_length;
		exp->rt_bitbytes = rgd->rt_bitbytes;
		exp->rt_flags = rgd->rt_flags;
		exp->rt_free = rgd->rt_free;
		exp->rt_igeneration = rgd->rt_igeneration;
		exp->rt_dinodes = rgd->rt_dinodes;
		exp->rt_skip = rgd->rt_skip;
		exp->bits = NULL;
		gfs2_compute_bitstructs(sdp->sd_bsize, exp);
	}
	sdp->rgrps = *num_rgs;
	return 0;
}

/*
 * rindex_repair - try to repair a damaged rg index (rindex)
 * trust_lvl - This is how much we trust the rindex file.
 *             BLIND_FAITH means we take the rindex at face value.
 *             OPEN_MINDED means it might be okay, but we should verify it.
 *             DISTRUST means it's not to be trusted, so we should go to
 *             greater lengths to build it from scratch.
 *             INDIGNATION means we have corruption, but the file system
 *             was converted from GFS via gfs2_convert, and its rgrps are
 *             not on nice boundaries thanks to previous gfs_grow ops. Lovely.
 */
int rindex_repair(struct gfs2_sbd *sdp, int trust_lvl, int *ok)
{
	struct osi_node *n, *next = NULL, *e, *enext;
	int error, discrepancies, percent;
	int calc_rg_count = 0, rg;
	struct gfs2_rindex buf;

	if (trust_lvl == BLIND_FAITH)
		return 0;
	if (trust_lvl == YE_OF_LITTLE_FAITH) { /* if rindex seems sane */
		/* Don't free previous incarnations in memory, if any.
		 * We need them to copy in the next function:
		 * gfs2_rgrp_free(&sdp->rglist); */
		if (!(*ok)) {
			log_err(_("The rindex file does not meet our "
				  "expectations.\n"));
			return -1;
		}
		error = expect_rindex_sanity(sdp, &calc_rg_count);
		if (error) {
			gfs2_rgrp_free(sdp, &rgcalc);
			return error;
		}
	} else if (trust_lvl == OPEN_MINDED) { /* If we can't trust RG index */
		/* Free previous incarnations in memory, if any. */
		gfs2_rgrp_free(sdp, &sdp->rgtree);

		/* Calculate our own RG index for comparison */
		error = gfs2_rindex_calculate(sdp, &calc_rg_count);
		if (error) { /* If calculated RGs don't match the fs */
			gfs2_rgrp_free(sdp, &rgcalc);
			return -1;
		}
	} else if (trust_lvl == DISTRUST) { /* If we can't trust RG index */
		/* Free previous incarnations in memory, if any. */
		gfs2_rgrp_free(sdp, &sdp->rgtree);

		error = rindex_rebuild(sdp, &calc_rg_count, 0);
		if (error) {
			log_crit( _("Error rebuilding rgrp list.\n"));
			gfs2_rgrp_free(sdp, &rgcalc);
			return -1;
		}
	} else if (trust_lvl == INDIGNATION) { /* If we can't trust anything */
		/* Free previous incarnations in memory, if any. */
		gfs2_rgrp_free(sdp, &sdp->rgtree);

		error = rindex_rebuild(sdp, &calc_rg_count, 1);
		if (error) {
			log_crit( _("Error rebuilding rgrp list.\n"));
			gfs2_rgrp_free(sdp, &rgcalc);
			return -1;
		}
	}
	/* Read in the rindex */
	sdp->rgtree.osi_node = NULL; /* Just to be safe */
	rindex_read(sdp, &sdp->rgrps, ok);
	if (sdp->md.riinode->i_size % sizeof(struct gfs2_rindex)) {
		log_warn( _("WARNING: rindex file has an invalid size.\n"));
		if (!query( _("Truncate the rindex size? (y/n)"))) {
			log_err(_("The rindex was not repaired.\n"));
			gfs2_rgrp_free(sdp, &rgcalc);
			gfs2_rgrp_free(sdp, &sdp->rgtree);
			return -1;
		}
		sdp->md.riinode->i_size /= sizeof(struct gfs2_rindex);
		sdp->md.riinode->i_size *= sizeof(struct gfs2_rindex);
		bmodified(sdp->md.riinode->i_bh);
		log_err(_("Changing rindex size to %"PRIu64".\n"), sdp->md.riinode->i_size);
	}
	log_warn( _("L%d: number of rgs expected     = %lld.\n"), trust_lvl + 1,
		 (unsigned long long)sdp->rgrps);
	if (calc_rg_count != sdp->rgrps) {
		int most_that_fit;

		log_warn( _("L%d: They don't match; either (1) the fs was "
			    "extended, (2) an odd\n"), trust_lvl + 1);
		log_warn( _("L%d: rgrp size was used, or (3) we have a corrupt "
			    "rg index.\n"), trust_lvl + 1);
		/* If the trust level is OPEN_MINDED, we would have calculated
		   the rindex based on the device size. If it's not the same
		   number, don't trust it. Complain about the discrepancy,
		   then try again with a little more DISTRUST. */
		if ((trust_lvl < DISTRUST) ||
		    !query( _("Attempt to use what rgrps we can? (y/n)"))) {
			gfs2_rgrp_free(sdp, &rgcalc);
			gfs2_rgrp_free(sdp, &sdp->rgtree);
			log_err(_("The rindex was not repaired.\n"));
			return -1;
		}
		/* We cannot grow rindex at this point. Since pass1 has not
		   yet run, we can't allocate blocks. Therefore we must use
		   whatever will fix in the space given. */
		most_that_fit = sdp->md.riinode->i_size / sizeof(struct gfs2_rindex);
		log_debug(_("The most we can fit is %d rgrps\n"),
			  most_that_fit);
		if (most_that_fit < calc_rg_count)
			calc_rg_count = most_that_fit;
		log_err(_("Attempting to fix rindex with %d rgrps.\n"),
			calc_rg_count);
	}
	/* ------------------------------------------------------------- */
	/* Now compare the rindex to what we think it should be.         */
	/* See how far off our expected values are.  If too much, abort. */
	/* The theory is: if we calculated the index to have 32 RGs and  */
	/* we have a large number that are completely wrong, we should   */
	/* abandon this method of recovery and try a better one.         */
	/* ------------------------------------------------------------- */
	discrepancies = 0;
	for (rg = 0, n = osi_first(&sdp->rgtree), e = osi_first(&rgcalc);
	     n && e && !fsck_abort && rg < calc_rg_count; rg++) {
		struct rgrp_tree *expected, *actual;

		next = osi_next(n);
		enext = osi_next(e);

		expected = (struct rgrp_tree *)e;
		actual = (struct rgrp_tree *)n;
		if (actual->rt_addr < expected->rt_addr) {
			n = next;
			discrepancies++;
			log_info(_("%d addr: 0x%"PRIx64" < 0x%"PRIx64" * mismatch\n"),
				 rg + 1, actual->rt_addr, expected->rt_addr);
			continue;
		} else if (expected->rt_addr < actual->rt_addr) {
			e = enext;
			discrepancies++;
			log_info(_("%d addr: 0x%"PRIx64" > 0x%"PRIx64" * mismatch\n"),
				 rg + 1, actual->rt_addr, expected->rt_addr);
			continue;
		}
		if (actual->rt_length != expected->rt_length ||
		    actual->rt_data0 != expected->rt_data0 ||
		    actual->rt_data != expected->rt_data ||
		    actual->rt_bitbytes != expected->rt_bitbytes) {
			discrepancies++;
			log_info(_("%d addr: 0x%"PRIx64" 0x%"PRIx64" * has mismatch\n"),
				 rg + 1, actual->rt_addr, expected->rt_addr);
		}
		n = next;
		e = enext;
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
			gfs2_rgrp_free(sdp, &rgcalc);
			gfs2_rgrp_free(sdp, &sdp->rgtree);
			return -1;
		}
	}
	log_debug("Calculated %d rgrps: Total: %d Match: %d Mismatch: %d\n",
		  calc_rg_count, rg, rg - discrepancies, discrepancies);
	/* ------------------------------------------------------------- */
	/* Now compare the rindex to what we think it should be.         */
	/* Our rindex should be pretty predictable unless we've grown    */
	/* so look for index problems first before looking at the rgs.   */
	/* ------------------------------------------------------------- */
	for (rg = 0, n = osi_first(&sdp->rgtree), e = osi_first(&rgcalc);
	     e && !fsck_abort && rg < calc_rg_count; rg++) {
		struct rgrp_tree *expected, *actual;

		if (n)
			next = osi_next(n);
		enext = osi_next(e);
		expected = (struct rgrp_tree *)e;
		actual = (struct rgrp_tree *)n;

		/* If the next "actual" rgrp in memory is too far away,
		   fill in a new one with the expected value. -or-
		   If we ran out of actual rindex entries due to rindex
		   damage, fill in a new one with the expected values. */
		if (!n || /* end of actual rindex */
		    expected->rt_addr < actual->rt_addr) {
			log_err(_("Entry missing from rindex: 0x%"PRIx64"\n"),
			        expected->rt_addr);
			actual = rgrp_insert(&sdp->rgtree, expected->rt_addr);
			if (!actual) {
				log_err(_("Out of memory!\n"));
				break;
			}
			rindex_modified = 1;
			next = n; /* Ensure that the old actual gets checked
				     against a new expected, since we added */
		} else {
			ri_compare(rg, actual, expected, rt_addr, PRIx64);
			ri_compare(rg, actual, expected, rt_length, PRIx32);
			ri_compare(rg, actual, expected, rt_data0, PRIx64);
			ri_compare(rg, actual, expected, rt_data, PRIx32);
			ri_compare(rg, actual, expected, rt_bitbytes, PRIx32);
		}
		/* If we modified the index, write it back to disk. */
		if (rindex_modified) {
			if (query( _("Fix the index? (y/n)"))) {
				lgfs2_rindex_out(expected, (char *)&buf);
				gfs2_writei(sdp->md.riinode, (char *)&buf,
					    rg * sizeof(struct gfs2_rindex),
					    sizeof(struct gfs2_rindex));
				actual->rt_addr = expected->rt_addr;
				actual->rt_length = expected->rt_length;
				actual->rt_data0 = expected->rt_data0;
				actual->rt_data = expected->rt_data;
				actual->rt_bitbytes = expected->rt_bitbytes;
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
			gfs2_compute_bitstructs(sdp->sd_bsize, actual);
			rindex_modified = 0;
		}
		e = enext;
		if (n)
			n = next;
	}
	/* ------------------------------------------------------------- */
	/* Read the real RGs and check their integrity.                  */
	/* Now we can somewhat trust the rindex and the RG addresses,    */
	/* so let's read them in, check them and optionally fix them.    */
	/* ------------------------------------------------------------- */
	for (rg = 0, n = osi_first(&sdp->rgtree); n && !fsck_abort &&
		     rg < calc_rg_count; n = next, rg++) {
		struct rgrp_tree *rgd;
		uint64_t prev_err = 0, errblock;
		int i;

		next = osi_next(n);
		/* Now we try repeatedly to read in the rg.  For every block */
		/* we encounter that has errors, repair it and try again.    */
		i = 0;
		do {
			rgd = (struct rgrp_tree *)n;
			errblock = gfs2_rgrp_read(sdp, rgd);
			if (errblock) {
				if (errblock == prev_err)
					break;
				prev_err = errblock;
				rewrite_rg_block(sdp, rgd, errblock);
			} else {
				gfs2_rgrp_relse(sdp, rgd);
				break;
			}
			i++;
		} while (i < rgd->rt_length);
	}
	gfs2_rgrp_free(sdp, &rgcalc);
	gfs2_rgrp_free(sdp, &sdp->rgtree);
	/* We shouldn't need to worry about getting the user's permission to
	   make changes here. If b_modified is true, they already gave their
	   permission. */
	if (sdp->md.riinode->i_bh->b_modified) {
		log_debug("Syncing rindex inode changes to disk.\n");
		lgfs2_dinode_out(sdp->md.riinode, sdp->md.riinode->i_bh->b_data);
		bwrite(sdp->md.riinode->i_bh);
	}
	return 0;
}
