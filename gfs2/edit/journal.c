#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <curses.h>
#include <term.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <dirent.h>

#include "copyright.cf"

#include "hexedit.h"
#include "libgfs2.h"
#include "extended.h"
#include "gfs2hex.h"
#include "journal.h"

/**
 * find_journal_block - figure out where a journal starts, given the name
 * Returns: journal block number, changes j_size to the journal size
 */
uint64_t find_journal_block(const char *journal, uint64_t *j_size)
{
	int journal_num;
	uint64_t jindex_block, jblock = 0;
	int amtread;
	struct gfs2_buffer_head *jindex_bh, *j_bh;

	journal_num = atoi(journal + 7);
	if (journal_num < 0)
		return 0;

	/* Figure out the block of the jindex file */
	if (sbd.gfs1)
		jindex_block = sbd.sd_jindex_di.in_addr;
	else
		jindex_block = masterblock("jindex");
	/* read in the block */
	jindex_bh = bread(&sbd, jindex_block);
	di = (struct gfs2_dinode *)jindex_bh->b_data;

	if (!sbd.gfs1)
		do_dinode_extended(jindex_bh->b_data); /* parse dir. */

	if (sbd.gfs1) {
		struct gfs2_inode *jiinode;
		struct gfs_jindex jidx;

		jiinode = lgfs2_inode_get(&sbd, jindex_bh);
		if (jiinode == NULL)
			return 0;
		amtread = gfs2_readi(jiinode, &jidx,
				   journal_num * sizeof(struct gfs_jindex),
				   sizeof(struct gfs_jindex));
		if (amtread) {
			jblock = be64_to_cpu(jidx.ji_addr);
			*j_size = (uint64_t)be32_to_cpu(jidx.ji_nsegment) * 0x10;
		}
		inode_put(&jiinode);
	} else {
		struct gfs2_dinode *jdi;

		if (journal_num > indirect->ii[0].dirents - 2)
			return 0;
		jblock = indirect->ii[0].dirent[journal_num + 2].inum.in_addr;
		j_bh = bread(&sbd, jblock);
		jdi = (struct gfs2_dinode *)j_bh->b_data;
		*j_size = be64_to_cpu(jdi->di_size);
		brelse(j_bh);
	}
	brelse(jindex_bh);
	return jblock;
}

static void check_journal_wrap(uint64_t seq, uint64_t *highest_seq)
{
	if (seq < *highest_seq) {
		print_gfs2("------------------------------------------------"
			   "------------------------------------------------");
		eol(0);
		print_gfs2("Journal wrapped here.");
		eol(0);
		print_gfs2("------------------------------------------------"
			   "------------------------------------------------");
		eol(0);
	}
	*highest_seq = seq;
}

/**
 * fsck_readi - same as libgfs2's gfs2_readi, but sets absolute block #
 *              of the first bit of data read.
 */
static int fsck_readi(struct gfs2_inode *ip, void *rbuf, uint64_t roffset,
	       unsigned int size, uint64_t *abs_block)
{
	struct gfs2_sbd *sdp;
	struct gfs2_buffer_head *lbh;
	uint64_t lblock, dblock;
	unsigned int o;
	uint32_t extlen = 0;
	unsigned int amount;
	int not_new = 0;
	int isdir;
	int copied = 0;

	if (ip == NULL)
		return 0;
	sdp = ip->i_sbd;
	isdir = !!(S_ISDIR(ip->i_mode));
	*abs_block = 0;
	if (roffset >= ip->i_size)
		return 0;
	if ((roffset + size) > ip->i_size)
		size = ip->i_size - roffset;
	if (!size)
		return 0;
	if (isdir) {
		o = roffset % sdp->sd_jbsize;
		lblock = roffset / sdp->sd_jbsize;
	} else {
		lblock = roffset >> sdp->sd_bsize_shift;
		o = roffset & (sdp->sd_bsize - 1);
	}

	if (!ip->i_height) /* inode_is_stuffed */
		o += sizeof(struct gfs2_dinode);
	else if (isdir)
		o += sizeof(struct gfs2_meta_header);

	while (copied < size) {
		amount = size - copied;
		if (amount > sdp->sd_bsize - o)
			amount = sdp->sd_bsize - o;
		if (!extlen)
			block_map(ip, lblock, &not_new, &dblock, &extlen,
				  FALSE);
		if (dblock) {
			lbh = bread(sdp, dblock);
			if (*abs_block == 0)
				*abs_block = lbh->b_blocknr;
			dblock++;
			extlen--;
		} else
			lbh = NULL;
		if (lbh) {
			memcpy(rbuf, lbh->b_data + o, amount);
			brelse(lbh);
		} else {
			memset(rbuf, 0, amount);
		}
		copied += amount;
		lblock++;
		o = (isdir) ? sizeof(struct gfs2_meta_header) : 0;
	}
	return copied;
}

/**
 * ld_is_pertinent - determine if a log descriptor is pertinent
 *
 * This function checks a log descriptor buffer to see if it contains
 * references to a given traced block, or its rgrp bitmap block.
 */
static int ld_is_pertinent(const __be64 *b, const char *end, uint64_t tblk,
			   struct rgrp_tree *rgd, uint64_t bitblk)
{
	const __be64 *blk = b;

	if (!tblk)
		return 1;

	while (*blk && (char *)blk < end) {
		if (be64_to_cpu(*blk) == tblk || be64_to_cpu(*blk) == bitblk)
			return 1;
		blk++;
		if (sbd.gfs1)
			blk++;
	}
	return 0;
}

/**
 * print_ld_blks - print all blocks given in a log descriptor
 * returns: the number of block numbers it printed
 */
static int print_ld_blks(const __be64 *b, const char *end, int start_line,
			 uint64_t tblk, uint64_t *tblk_off, uint64_t bitblk,
			 struct rgrp_tree *rgd, uint64_t abs_block, int prnt,
			 uint64_t *bblk_off, int is_meta_ld)
{
	int bcount = 0, found_tblk = 0, found_bblk = 0;
	static char str[256];
	struct gfs2_buffer_head *j_bmap_bh;

	if (tblk_off)
		*tblk_off = 0;
	if (bblk_off)
		*bblk_off = 0;
	while (*b && (char *)b < end) {
		if (!termlines ||
		    (print_entry_ndx >= start_row[dmode] &&
		     ((print_entry_ndx - start_row[dmode])+1) *
		     lines_per_row[dmode] <= termlines - start_line - 2)) {
			if (prnt && bcount && bcount % 4 == 0) {
				eol(0);
				print_gfs2("                    ");
			}
			bcount++;
			if (prnt) {
				if (is_meta_ld) {
					j_bmap_bh = bread(&sbd, abs_block +
							  bcount);
					sprintf(str, "0x%"PRIx64" %2s",
						be64_to_cpu(*b),
						mtypes[lgfs2_get_block_type(j_bmap_bh->b_data)]);
					brelse(j_bmap_bh);
				} else {
					sprintf(str, "0x%"PRIx64, be64_to_cpu(*b));
				}
				print_gfs2("%-18.18s ", str);
			}
			if (!found_tblk && tblk_off)
				(*tblk_off)++;
			if (!found_bblk && bblk_off)
				(*bblk_off)++;
			if (tblk && (be64_to_cpu(*b) == tblk)) {
				found_tblk = 1;
				print_gfs2("<-------------------------0x%llx ",
					   (unsigned long long)tblk);
				eol(18 * (bcount % 4) + 1);
				print_gfs2("                    ");
			}
			if (tblk && rgd && (be64_to_cpu(*b) == bitblk)) {
				int type, bmap = 0;
				uint64_t o;
				char *save_ptr;

				found_bblk = 1;
				print_gfs2("<-------------------------");
				if (is_meta_ld) {
					o = tblk - rgd->rt_data0;
					if (o >= (rgd->bits->bi_start +
						  rgd->bits->bi_len) *
					    GFS2_NBBY)
						o += (sizeof(struct gfs2_rgrp) -
						      sizeof(struct gfs2_meta_header))
							* GFS2_NBBY;
					bmap = o / sbd.sd_blocks_per_bitmap;
					save_ptr = rgd->bits[bmap].bi_data;
					j_bmap_bh = bread(&sbd, abs_block +
							  bcount);
					rgd->bits[bmap].bi_data = j_bmap_bh->b_data;
					type = lgfs2_get_bitmap(&sbd, tblk, rgd);
					brelse(j_bmap_bh);
					if (type < 0) {
						perror("Error printing log descriptor blocks");
						exit(1);
					}
					rgd->bits[bmap].bi_data = save_ptr;
					print_gfs2("bit for blk 0x%llx is %d "
						   "(%s)",
						   (unsigned long long)tblk,
						   type,
						   allocdesc[sbd.gfs1][type]);
				} else {
					print_gfs2("bitmap for blk 0x%llx "
						   "was revoked",
						   (unsigned long long)tblk);
				}
				eol(18 * (bcount % 4) + 1);
				print_gfs2("                    ");
			}
		}
		b++;
		if (sbd.gfs1)
			b++;
	}
	if (prnt)
		eol(0);
	if (tblk_off && (!found_tblk || !is_meta_ld))
		*tblk_off = 0;
	if (bblk_off && (!found_bblk || !is_meta_ld))
		*bblk_off = 0;
	return bcount;
}

static int is_wrap_pt(void *buf, uint64_t *highest_seq)
{
	const struct lgfs2_metadata *mtype = get_block_type(buf);

	if (mtype != NULL && mtype->mh_type == GFS2_METATYPE_LH) {
		uint64_t seq;

		if (sbd.gfs1) {
			struct gfs_log_header *lh = buf;
			seq = be64_to_cpu(lh->lh_sequence);
		} else {
			struct gfs2_log_header *lh = buf;
			seq = be64_to_cpu(lh->lh_sequence);
		}
		if (seq < *highest_seq)
			return 1;
		*highest_seq = seq;
	}
	return 0;
}

/**
 * find_wrap_pt - figure out where a journal wraps
 * Returns: The wrap point, in bytes
 */
static uint64_t find_wrap_pt(struct gfs2_inode *ji, char *jbuf, uint64_t jblock, uint64_t j_size)
{
	uint64_t jb = 0;
	uint64_t highest_seq = 0;

	for (jb = 0; jb < j_size; jb += (sbd.gfs1 ? 1 : sbd.sd_bsize)) {
		int found = 0;

		if (sbd.gfs1) {
			struct gfs2_buffer_head *j_bh;

			j_bh = bread(&sbd, jblock + jb);
			found = is_wrap_pt(j_bh->b_data, &highest_seq);
			brelse(j_bh);
		} else {
			int copied;
			uint64_t abs_block;

			copied = fsck_readi(ji, jbuf, jb, sbd.sd_bsize, &abs_block);
			if (!copied) /* end of file */
				break;
			found = is_wrap_pt(jbuf, &highest_seq);
		}
		if (found)
			return jb;
	}
	return 0;
}

/**
 * process_ld - process a log descriptor
 */
static int process_ld(uint64_t abs_block, uint64_t wrappt, uint64_t j_size,
		      uint64_t jb, char *buf, int tblk,
		      uint64_t *tblk_off, uint64_t bitblk,
		      struct rgrp_tree *rgd, int *prnt, uint64_t *bblk_off)
{
	__be64 *b;
	struct gfs2_log_descriptor *ld = (void *)buf;
	int ltndx, is_meta_ld = 0;
	int ld_blocks = 0;
	uint32_t ld_type = be32_to_cpu(ld->ld_type);
	uint32_t ld_length = be32_to_cpu(ld->ld_length);
	uint32_t ld_data1 = be32_to_cpu(ld->ld_data1);
	uint32_t logtypes[2][6] = {
		{GFS2_LOG_DESC_METADATA, GFS2_LOG_DESC_REVOKE,
		 GFS2_LOG_DESC_JDATA, 0, 0, 0},
		{GFS_LOG_DESC_METADATA, GFS_LOG_DESC_IUL, GFS_LOG_DESC_IDA,
		 GFS_LOG_DESC_Q, GFS_LOG_DESC_LAST, 0}};
	const char *logtypestr[2][6] = {
		{"Metadata", "Revoke", "Jdata",
		 "Unknown", "Unknown", "Unknown"},
		{"Metadata", "Unlinked inode", "Dealloc inode",
		 "Quota", "Final Entry", "Unknown"}};

	if (sbd.gfs1)
		b = (__be64 *)(buf + sizeof(struct gfs_log_descriptor));
	else
		b = (__be64 *)(buf + sizeof(struct gfs2_log_descriptor));
	*prnt = ld_is_pertinent(b, (buf + sbd.sd_bsize), tblk, rgd, bitblk);

	if (*prnt) {
		print_gfs2("0x%"PRIx64" (j+%4"PRIx64"): Log descriptor, ",
			   abs_block, ((jb + wrappt) % j_size) / sbd.sd_bsize);
		print_gfs2("type %"PRIu32" ", ld_type);

		for (ltndx = 0;; ltndx++) {
			if (ld_type == logtypes[sbd.gfs1][ltndx] ||
			    logtypes[sbd.gfs1][ltndx] == 0)
				break;
		}
		print_gfs2("(%s) ", logtypestr[sbd.gfs1][ltndx]);
		print_gfs2("len:%"PRIu32", data1: %"PRIu32, ld_length, ld_data1);
		eol(0);
		print_gfs2("                    ");
	}
	ld_blocks = ld_data1;
	if (ld_type == GFS2_LOG_DESC_METADATA ||
	    ld_type == GFS_LOG_DESC_METADATA)
		is_meta_ld = 1;
	ld_blocks -= print_ld_blks(b, (buf + sbd.sd_bsize), line, tblk, tblk_off,
	                           bitblk, rgd, abs_block, *prnt, bblk_off,
	                           is_meta_ld);

	return ld_blocks;
}

/**
 * meta_has_ref - check if a metadata block references a given block
 */
static int meta_has_ref(uint64_t abs_block, int tblk)
{
	const struct lgfs2_metadata *mtype;
	struct gfs2_buffer_head *mbh;
	int structlen = 0, has_ref = 0;
	__be64 *b;
	struct gfs2_dinode *dinode;

	mbh = bread(&sbd, abs_block);
	mtype = get_block_type(mbh->b_data);
	if (mtype != NULL) {
		structlen = mtype->size;
		if (mtype->mh_type == GFS2_METATYPE_DI) {
			dinode = (struct gfs2_dinode *)mbh->b_data;
			if (be64_to_cpu(dinode->di_eattr) == tblk)
				has_ref = 1;
		}
	}
	b = (__be64 *)(mbh->b_data + structlen);
	while (!has_ref && mtype && (char *)b < mbh->b_data + sbd.sd_bsize) {
		if (be64_to_cpu(*b) == tblk)
			has_ref = 1;
		b++;
	}
	brelse(mbh);
	return has_ref;
}


/**
 * get_ldref - get a log descriptor reference block, given a block number
 *
 * Note that we can't pass in abs_block here, because journal wrap may
 * mean that the block we're interested in, in the journal, is before the
 * log descriptor that holds the reference we need.
 */
static uint64_t get_ldref(uint64_t abs_ld, int offset_from_ld)
{
	struct gfs2_buffer_head *jbh;
	uint64_t refblk;
	__be64 *b;

	jbh = bread(&sbd, abs_ld);
	b = (__be64 *)(jbh->b_data + sizeof(struct gfs2_log_descriptor));
	b += offset_from_ld - 1;
	refblk = be64_to_cpu(*b);
	brelse(jbh);
	return refblk;
}

static void display_log_header(void *buf, uint64_t *highest_seq, uint64_t abs_block, uint64_t jb, uint64_t j_size)
{
	const struct lgfs2_metafield *lh_flags_field;
	const struct lgfs2_metadata *mtype;
	struct gfs2_log_header *lh = buf;
	struct gfs_log_header *lh1 = buf;
	char flags_str[256];

	if (sbd.gfs1) {
		mtype = &lgfs2_metadata[LGFS2_MT_GFS_LOG_HEADER];
		lh_flags_field = &mtype->fields[7]; /* lh_flags is the 8th field in the struct */
		check_journal_wrap(be64_to_cpu(lh1->lh_sequence), highest_seq);
	} else {
		mtype = &lgfs2_metadata[LGFS2_MT_GFS2_LOG_HEADER];
		lh_flags_field = &mtype->fields[6]; /* lh_flags is the 7th field in the struct */
		check_journal_wrap(be64_to_cpu(lh->lh_sequence), highest_seq);
	}
	lgfs2_field_str(flags_str, sizeof(flags_str), buf, lh_flags_field, (dmode == HEX_MODE));
	if (sbd.gfs1) {
		print_gfs2("0x%"PRIx64" (j+%4"PRIx64"): Log header: Seq: 0x%"PRIx64", "
		           "1st: 0x%"PRIx64", tail: 0x%"PRIx64", last: 0x%"PRIx64" [%s]",
		           abs_block, jb, be64_to_cpu(lh1->lh_sequence),
			   be64_to_cpu(lh1->lh_first), be64_to_cpu(lh1->lh_tail),
			   be64_to_cpu(lh1->lh_last_dump), flags_str);
	} else {
		print_gfs2("0x%"PRIx64" (j+%4"PRIx64"): Log header: Seq: 0x%"PRIx64", "
		           "tail: 0x%"PRIx32", blk: 0x%"PRIx32" [%s]",
		            abs_block, (jb % j_size) / sbd.sd_bsize, be64_to_cpu(lh->lh_sequence),
		            be32_to_cpu(lh->lh_tail), be32_to_cpu(lh->lh_blkno), flags_str);
	}
}

/**
 * dump_journal - dump a journal file's contents.
 * @journal: name of the journal to dump
 * @tblk: block number to trace in the journals
 *
 * This function dumps the contents of a journal. If a trace block is specified
 * then only information printed is: (1) log descriptors that reference that
 * block, (2) metadata in the journal that references the block, or (3)
 * rgrp bitmaps that reference that block's allocation bit status.
 */
void dump_journal(const char *journal, int tblk)
{
	const struct lgfs2_metadata *mtype;
	struct gfs2_buffer_head *j_bh = NULL;
	uint64_t jblock, j_size, jb, abs_block, saveblk, wrappt = 0;
	int start_line, journal_num;
	struct gfs2_inode *j_inode = NULL;
	int ld_blocks = 0, offset_from_ld = 0;
	uint64_t tblk_off = 0, bblk_off = 0, bitblk = 0;
	uint64_t highest_seq = 0;
	char *jbuf = NULL;
	char *buf = NULL;
	struct rgrp_tree *rgd = NULL;
	uint64_t abs_ld = 0;

	mtype = lgfs2_find_mtype(GFS2_METATYPE_LH, sbd.gfs1 ? LGFS2_MD_GFS1 : LGFS2_MD_GFS2);

	start_line = line;
	lines_per_row[dmode] = 1;
	journal_num = atoi(journal + 7);
	print_gfs2("Dumping journal #%d.", journal_num);
	if (tblk) {
		dmode = HEX_MODE;
		print_gfs2(" Tracing block 0x%llx", (unsigned long long)tblk);
	}
	eol(0);
	jblock = find_journal_block(journal, &j_size);
	if (!jblock)
		return;

	if (!sbd.gfs1) {
		j_bh = bread(&sbd, jblock);
		j_inode = lgfs2_inode_get(&sbd, j_bh);
		if (j_inode == NULL) {
			fprintf(stderr, "Out of memory\n");
			exit(-1);
		}
		jbuf = malloc(sbd.sd_bsize);
		if (jbuf == NULL) {
			fprintf(stderr, "Out of memory\n");
			exit(-1);
		}
	}

	if (tblk) {
		uint64_t wp;

		rgd = gfs2_blk2rgrpd(&sbd, tblk);
		if (!rgd) {
			print_gfs2("Can't locate the rgrp for block 0x%x",
				   tblk);
			eol(0);
		} else {
			uint64_t o;
			int bmap = 0;

			print_gfs2("rgd: 0x%"PRIx64" for 0x%"PRIx32", ", rgd->rt_addr,
				   rgd->rt_length);
			o = tblk - rgd->rt_data0;
			if (o >= (rgd->bits->bi_start +
				  rgd->bits->bi_len) * (uint64_t)GFS2_NBBY)
				o += (sizeof(struct gfs2_rgrp) -
				      sizeof(struct gfs2_meta_header))
					* GFS2_NBBY;
			bmap = o / sbd.sd_blocks_per_bitmap;
			bitblk = rgd->rt_addr + bmap;
			print_gfs2("bitmap: %d, bitblk: 0x%"PRIx64, bmap, bitblk);
			eol(0);
		}

		wrappt = find_wrap_pt(j_inode, jbuf, jblock, j_size);
		wp = wrappt / (sbd.gfs1 ? 1 : sbd.sd_bsize);
		print_gfs2("Starting at journal wrap block: 0x%llx "
			   "(j + 0x%llx)",
			   (unsigned long long)jblock + wp,
			   (unsigned long long)wp);
		eol(0);
	}

	for (jb = 0; jb < j_size; jb += (sbd.gfs1 ? 1 : sbd.sd_bsize)) {
		int is_pertinent = 1;
		uint32_t block_type = 0;

		if (sbd.gfs1) {
			if (j_bh)
				brelse(j_bh);
			abs_block = jblock + ((jb + wrappt) % j_size);
			j_bh = bread(&sbd, abs_block);
			buf = j_bh->b_data;
		} else {
			int error = fsck_readi(j_inode, (void *)jbuf,
					   ((jb + wrappt) % j_size),
					   sbd.sd_bsize, &abs_block);
			if (!error) /* end of file */
				break;
			buf = jbuf;
		}
		offset_from_ld++;
		mtype = get_block_type(buf);
		if (mtype != NULL)
			block_type = mtype->mh_type;

		if (block_type == GFS2_METATYPE_LD) {
			ld_blocks = process_ld(abs_block, wrappt, j_size, jb,
					       buf, tblk, &tblk_off,
					       bitblk, rgd, &is_pertinent,
					       &bblk_off);
			offset_from_ld = 0;
			abs_ld = abs_block;
		} else if (!tblk && block_type == GFS2_METATYPE_LH) {
			display_log_header(buf, &highest_seq, abs_block, jb + wrappt, j_size);
			eol(0);
		} else if ((ld_blocks > 0) &&
			   (sbd.gfs1 || block_type == GFS2_METATYPE_LB)) {
			__be64 *b = (__be64 *)(buf + (sbd.gfs1 ? 0 : sizeof(struct gfs2_meta_header)));

			print_gfs2("0x%"PRIx64" (j+%4"PRIx64"): Log descriptor"
				   " continuation block", abs_block,
				   ((jb + wrappt) % j_size) / sbd.sd_bsize);
			eol(0);
			print_gfs2("                    ");
			ld_blocks -= print_ld_blks(b, (buf + sbd.sd_bsize), start_line,
			                     tblk, &tblk_off, 0, rgd, 0, 1, NULL, 0);
		} else if (block_type == 0) {
			continue;
		}
		/* Check if this metadata block references the block we're
		   trying to trace. */
		if (details || (tblk && ((is_pertinent &&
		     ((tblk_off && offset_from_ld == tblk_off) ||
		      (bblk_off && offset_from_ld == bblk_off))) ||
		     meta_has_ref(abs_block, tblk)))) {
			uint64_t ref_blk = 0;

			saveblk = block;
			block = abs_block;
			if (tblk && !details) {
				ref_blk = get_ldref(abs_ld, offset_from_ld);
				display(0, 1, tblk, ref_blk);
			} else {
				display(0, 0, 0, 0);
			}
			block = saveblk;
		}
	}
	if (j_inode != NULL)
		inode_put(&j_inode);
	brelse(j_bh);
	blockhist = -1; /* So we don't print anything else */
	free(jbuf);
	if (!termlines)
		fflush(stdout);
}
