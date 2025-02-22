#include "clusterautoconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#define _(String) gettext(String)

#include <logging.h>
#include "libgfs2.h"
#include "fsck.h"
#include "inode_hash.h"
#include "link.h"
#include "util.h"

struct gfs2_bmap nlink1map = { 0 }; /* map of dinodes with nlink == 1 */
struct gfs2_bmap clink1map = { 0 }; /* map of dinodes w/counted links == 1 */

int link1_set(struct gfs2_bmap *bmap, uint64_t bblock, int mark)
{
	static unsigned char *byte;
	static uint64_t b;

	if (!bmap)
		return 0;
	if (bblock > bmap->size)
		return -1;

	byte = bmap->map + BLOCKMAP_SIZE1(bblock);
	b = BLOCKMAP_BYTE_OFFSET1(bblock);
	*byte &= ~(BLOCKMAP_MASK1 << b);
	*byte |= (mark & BLOCKMAP_MASK1) << b;
	return 0;
}

int set_di_nlink(struct gfs2_inode *ip)
{
	struct inode_info *ii;
	struct dir_info *di;

	if (is_dir(ip, ip->i_sbd->gfs1)) {
		di = dirtree_find(ip->i_num.in_addr);
		if (di == NULL) {
			log_err(_("Error: directory %"PRIu64" (0x%"PRIx64") is not "
				  "in the dir_tree (set).\n"),
			        ip->i_num.in_addr, ip->i_num.in_addr);
			return -1;
		}
		di->di_nlink = ip->i_nlink;
		return 0;
	}
	if (ip->i_nlink == 1) {
		link1_set(&nlink1map, ip->i_num.in_addr, 1);
		return 0;
	}
	/*log_debug( _("Setting link count to %u for %" PRIu64
	  " (0x%" PRIx64 ")\n"), count, inode_no, inode_no);*/
	/* If the list has entries, look for one that matches inode_no */
	ii = inodetree_find(ip->i_num.in_addr);
	if (!ii) {
		struct lgfs2_inum no = ip->i_num;
		ii = inodetree_insert(no);
	}
	if (ii)
		ii->di_nlink = ip->i_nlink;
	else
		return -1;
	return 0;
}

/* I'm making whyincr a macro rather than function so that the debug output
 * matches older versions. */
#define whyincr(no_addr, why, referenced_from, counted_links)		\
	log_debug(_("Dir (0x%llx) incremented counted links to %u "	\
		    "for (0x%llx) via %s\n"),				\
		  (unsigned long long)referenced_from, counted_links,	\
		  (unsigned long long)no_addr, why);

int incr_link_count(struct lgfs2_inum no, struct gfs2_inode *ip, const char *why)
{
	struct inode_info *ii = NULL;
	uint64_t referenced_from = ip ? ip->i_num.in_addr : 0;
	struct dir_info *di;
	struct gfs2_inode *link_ip;

	di = dirtree_find(no.in_addr);
	if (di) {
		if (di->dinode.in_formal_ino != no.in_formal_ino)
			return INCR_LINK_INO_MISMATCH;

		di->counted_links++;
		whyincr(no.in_addr, why, referenced_from, di->counted_links);
		return INCR_LINK_GOOD;
	}
	ii = inodetree_find(no.in_addr);
	/* If the list has entries, look for one that matches inode_no */
	if (ii) {
		if (ii->num.in_formal_ino != no.in_formal_ino)
			return INCR_LINK_INO_MISMATCH;

		ii->counted_links++;
		whyincr(no.in_addr, why, referenced_from, ii->counted_links);
		return INCR_LINK_GOOD;
	}
	if (link1_type(&clink1map, no.in_addr) != 1) {
		link1_set(&clink1map, no.in_addr, 1);
		whyincr(no.in_addr, why, referenced_from, 1);
		return INCR_LINK_GOOD;
	}

	link_ip = fsck_load_inode(ip->i_sbd, no.in_addr);
	/* Check formal ino against dinode before adding to inode tree. */
	if (no.in_formal_ino != link_ip->i_num.in_formal_ino) {
		fsck_inode_put(&link_ip);
		return INCR_LINK_INO_MISMATCH; /* inode mismatch */
	}
	/* Move it from the link1 maps to a real inode tree entry */
	link1_set(&nlink1map, no.in_addr, 0);
	link1_set(&clink1map, no.in_addr, 0);

	/* If no match was found, it must be a hard link. In theory, it can't
	   be a duplicate because those were resolved in pass1b. Add a new
	   inodetree entry and set its counted links to 2 */
	ii = inodetree_insert(no);
	if (!ii) {
		log_debug( _("Ref: (0x%llx) Error incrementing link for "
			     "(0x%llx)!\n"),
			   (unsigned long long)referenced_from,
			   (unsigned long long)no.in_addr);
		fsck_inode_put(&link_ip);
		return INCR_LINK_BAD;
	}
	ii->num.in_addr = link_ip->i_num.in_addr;
	ii->num.in_formal_ino = link_ip->i_num.in_formal_ino;
	fsck_inode_put(&link_ip);
	ii->di_nlink = 1; /* Must be 1 or it wouldn't have gotten into the
			     nlink1map */
	ii->counted_links = 2;
	whyincr(no.in_addr, why, referenced_from, ii->counted_links);
	/* We transitioned a dentry link count from 1 to 2, and we know it's
	   not a directory. But the new reference has the correct formal
	   inode number, so the first reference is suspect: we need to
	   check it in case it's a bad reference, and not just a hard link. */
	return INCR_LINK_CHECK_ORIG;
}

#define whydecr(no_addr, why, referenced_from, counted_links)		\
	log_debug(_("Dir (0x%llx) decremented counted links to %u "	\
		    "for (0x%llx) via %s\n"),				\
		  (unsigned long long)referenced_from, counted_links,	\
		  (unsigned long long)no_addr, why);

int decr_link_count(uint64_t inode_no, uint64_t referenced_from, int gfs1,
		    const char *why)
{
	struct inode_info *ii = NULL;
	struct dir_info *di;

	di = dirtree_find(inode_no);
	if (di) {
		if (!di->counted_links) {
			log_debug( _("Dir (0x%llx)'s link to "
				     "(0x%llx) via %s is zero!\n"),
				   (unsigned long long)referenced_from,
				   (unsigned long long)inode_no, why);
			return 0;
		}
		di->counted_links--;
		whydecr(inode_no, why, referenced_from, di->counted_links);
		return 0;
	}

	ii = inodetree_find(inode_no);
	/* If the list has entries, look for one that matches
	 * inode_no */
	if (ii) {
		if (!ii->counted_links) {
			log_debug( _("Dir (0x%llx)'s link to "
			     "(0x%llx) via %s is zero!\n"),
			   (unsigned long long)referenced_from,
			   (unsigned long long)inode_no, why);
			return 0;
		}
		ii->counted_links--;
		whydecr(inode_no, why, referenced_from, ii->counted_links);
		return 0;
	}
	if (link1_type(&clink1map, inode_no) == 1) { /* 1 -> 0 */
		link1_set(&clink1map, inode_no, 0);
		whydecr(inode_no, why, referenced_from, 0);
		return 0;
	}

	log_debug( _("No match found when decrementing link for (0x%llx)!\n"),
		   (unsigned long long)inode_no);
	return -1;

}


