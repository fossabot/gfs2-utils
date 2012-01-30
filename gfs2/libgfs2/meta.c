#include <stdint.h>
#include "libgfs2.h"

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
#define SYM(x) { x, #x },

const struct lgfs2_symbolic lgfs2_metatypes[] = {
SYM(GFS2_METATYPE_NONE)
SYM(GFS2_METATYPE_SB)
SYM(GFS2_METATYPE_RG)
SYM(GFS2_METATYPE_RB)
SYM(GFS2_METATYPE_DI)
SYM(GFS2_METATYPE_IN)
SYM(GFS2_METATYPE_LF)
SYM(GFS2_METATYPE_JD)
SYM(GFS2_METATYPE_LH)
SYM(GFS2_METATYPE_LD)
SYM(GFS2_METATYPE_LB)
SYM(GFS2_METATYPE_EA)
SYM(GFS2_METATYPE_ED)
SYM(GFS2_METATYPE_QC)
};

const unsigned lgfs2_metatype_size = ARRAY_SIZE(lgfs2_metatypes);

const struct lgfs2_symbolic lgfs2_metaformats[] = {
SYM(GFS2_FORMAT_NONE)
SYM(GFS2_FORMAT_SB)
SYM(GFS2_FORMAT_RG)
SYM(GFS2_FORMAT_RB)
SYM(GFS2_FORMAT_DI)
SYM(GFS2_FORMAT_IN)
SYM(GFS2_FORMAT_LF)
SYM(GFS2_FORMAT_JD)
SYM(GFS2_FORMAT_LH)
SYM(GFS2_FORMAT_LD)
SYM(GFS2_FORMAT_LB)
SYM(GFS2_FORMAT_EA)
SYM(GFS2_FORMAT_ED)
SYM(GFS2_FORMAT_QC)
SYM(GFS2_FORMAT_RI)
SYM(GFS2_FORMAT_DE)
SYM(GFS2_FORMAT_QU)
};

const unsigned lgfs2_metaformat_size = ARRAY_SIZE(lgfs2_metaformats);

const struct lgfs2_symbolic lgfs2_di_flags[] = {
SYM(GFS2_DIF_JDATA)
SYM(GFS2_DIF_EXHASH)
SYM(GFS2_DIF_UNUSED)
SYM(GFS2_DIF_EA_INDIRECT)
SYM(GFS2_DIF_DIRECTIO)
SYM(GFS2_DIF_IMMUTABLE)
SYM(GFS2_DIF_APPENDONLY)
SYM(GFS2_DIF_NOATIME)
SYM(GFS2_DIF_SYNC)
SYM(GFS2_DIF_SYSTEM)
SYM(GFS2_DIF_TRUNC_IN_PROG)
SYM(GFS2_DIF_INHERIT_DIRECTIO)
SYM(GFS2_DIF_INHERIT_JDATA)
};

const unsigned lgfs2_di_flag_size = ARRAY_SIZE(lgfs2_di_flags);

#undef SYM




#define F(f,...)  { .name = #f, \
		    .offset = offsetof(struct STRUCT, f), \
		    .length = sizeof(((struct STRUCT *)(0))->f), \
		    __VA_ARGS__ },
#define FP(f) F(f, .flags = LGFS2_MFF_POINTER )
#define RF(f) F(f, .flags = LGFS2_MFF_RESERVED)
#define RFP(f) F(f, .flags = LGFS2_MFF_POINTER|LGFS2_MFF_RESERVED)


#define MH(f) F(f.mh_magic) \
	      F(f.mh_type, .flags = LGFS2_MFF_ENUM, .symtab=lgfs2_metatypes, .nsyms=ARRAY_SIZE(lgfs2_metatypes)) \
	      RF(f.__pad0) \
	      F(f.mh_format, .flags = LGFS2_MFF_ENUM, .symtab=lgfs2_metaformats, .nsyms=ARRAY_SIZE(lgfs2_metaformats)) \
	      F(f.mh_jid)

#define IN(f) F(f.no_formal_ino) \
	      FP(f.no_addr)

#define INR(f) RF(f.no_formal_ino) \
	       RFP(f.no_addr)


#undef STRUCT
#define STRUCT gfs2_sb

static const struct lgfs2_metafield gfs2_sb_fields[] = {
MH(sb_header)
F(sb_fs_format)
F(sb_multihost_format)
RF(__pad0)
F(sb_bsize, .flags = LGFS2_MFF_BYTES)
F(sb_bsize_shift, .flags = LGFS2_MFF_BYTES|LGFS2_MFF_SHIFT)
RF(__pad1)
IN(sb_master_dir)
INR(__pad2)
IN(sb_root_dir)
F(sb_lockproto, .flags = LGFS2_MFF_STRING)
F(sb_locktable, .flags = LGFS2_MFF_STRING)
IN(__pad3)
IN(__pad4)
F(sb_uuid, .flags = LGFS2_MFF_UUID)
};

#undef STRUCT
#define STRUCT gfs_sb

static const struct lgfs2_metafield gfs_sb_fields[] = {
MH(sb_header)
F(sb_fs_format)
F(sb_multihost_format)
F(sb_flags)
F(sb_bsize, .flags = LGFS2_MFF_BYTES)
F(sb_bsize_shift, .flags = LGFS2_MFF_BYTES|LGFS2_MFF_SHIFT)
F(sb_seg_size, .flags = LGFS2_MFF_FSBLOCKS)
IN(sb_jindex_di)
IN(sb_rindex_di)
IN(sb_root_di)
F(sb_lockproto, .flags = LGFS2_MFF_STRING)
F(sb_locktable, .flags = LGFS2_MFF_STRING)
IN(sb_quota_di)
IN(sb_license_di)
RF(sb_reserved)
};

#undef STRUCT
#define STRUCT gfs2_rindex

static const struct lgfs2_metafield gfs2_rindex_fields[] = {
FP(ri_addr)
F(ri_length, .flags = LGFS2_MFF_FSBLOCKS)
RF(__pad)
FP(ri_data0)
F(ri_data, .flags = LGFS2_MFF_FSBLOCKS)
F(ri_bitbytes, .flags = LGFS2_MFF_BYTES)
F(ri_reserved)
};

#undef STRUCT
#define STRUCT gfs2_rgrp

static const struct lgfs2_metafield gfs2_rgrp_fields[] = {
MH(rg_header)
F(rg_flags)
F(rg_free, .flags = LGFS2_MFF_FSBLOCKS)
F(rg_dinodes, .flags = LGFS2_MFF_FSBLOCKS)
RF(__pad)
F(rg_igeneration)
RF(rg_reserved)
};

#undef STRUCT
#define STRUCT gfs_rgrp

static const struct lgfs2_metafield gfs_rgrp_fields[] = {
MH(rg_header)
F(rg_flags)
F(rg_free, .flags = LGFS2_MFF_FSBLOCKS)
F(rg_useddi, .flags = LGFS2_MFF_FSBLOCKS)
F(rg_freedi, .flags = LGFS2_MFF_FSBLOCKS)
IN(rg_freedi_list)
F(rg_usedmeta, .flags = LGFS2_MFF_FSBLOCKS)
F(rg_freemeta, .flags = LGFS2_MFF_FSBLOCKS)
RF(rg_reserved)
};

#undef STRUCT
struct gfs2_rgrp_bitmap { struct gfs2_meta_header rb_header; };
#define STRUCT gfs2_rgrp_bitmap

static const struct lgfs2_metafield gfs2_rgrp_bitmap_fields[] = {
MH(rb_header)
};

#undef STRUCT
#define STRUCT gfs2_dinode

static const struct lgfs2_metafield gfs2_dinode_fields[] = {
MH(di_header)
IN(di_num)
F(di_mode, .flags = LGFS2_MFF_MODE)
F(di_uid, .flags = LGFS2_MFF_UID)
F(di_gid, .flags = LGFS2_MFF_GID)
F(di_nlink)
F(di_size, .flags = LGFS2_MFF_BYTES)
F(di_blocks, .flags = LGFS2_MFF_FSBLOCKS)
F(di_atime, .flags = LGFS2_MFF_SECS)
F(di_mtime, .flags = LGFS2_MFF_SECS)
F(di_ctime, .flags = LGFS2_MFF_SECS)
F(di_major, .flags = LGFS2_MFF_MAJOR)
F(di_minor, .flags = LGFS2_MFF_MINOR)
FP(di_goal_meta)
FP(di_goal_data)
F(di_generation)
F(di_flags, .flags = LGFS2_MFF_MASK, .symtab=lgfs2_di_flags, .nsyms=ARRAY_SIZE(lgfs2_di_flags))
F(di_payload_format)
RF(__pad1)
F(di_height)
RF(__pad2)
RF(__pad3)
F(di_depth)
F(di_entries)
INR(__pad4)
FP(di_eattr)
F(di_atime_nsec, .flags = LGFS2_MFF_NSECS)
F(di_mtime_nsec, .flags = LGFS2_MFF_NSECS)
F(di_ctime_nsec, .flags = LGFS2_MFF_NSECS)
RF(di_reserved)
};

#undef STRUCT
#define STRUCT gfs_dinode

static const struct lgfs2_metafield gfs_dinode_fields[] = {
MH(di_header)
IN(di_num)
F(di_mode, .flags = LGFS2_MFF_MODE)
F(di_uid, .flags = LGFS2_MFF_UID)
F(di_gid, .flags = LGFS2_MFF_GID)
F(di_nlink)
F(di_size, .flags = LGFS2_MFF_BYTES)
F(di_blocks, .flags = LGFS2_MFF_FSBLOCKS)
F(di_atime, .flags = LGFS2_MFF_SECS)
F(di_mtime, .flags = LGFS2_MFF_SECS)
F(di_ctime, .flags = LGFS2_MFF_SECS)
F(di_major, .flags = LGFS2_MFF_MAJOR)
F(di_minor, .flags = LGFS2_MFF_MINOR)
FP(di_rgrp)
FP(di_goal_rgrp)
F(di_goal_dblk)
F(di_goal_mblk)
F(di_flags, .flags = LGFS2_MFF_MASK, .symtab=lgfs2_di_flags, .nsyms=ARRAY_SIZE(lgfs2_di_flags))
F(di_payload_format)
F(di_type)
F(di_height)
F(di_incarn)
F(di_pad)
F(di_depth)
F(di_entries)
INR(di_next_unused)
FP(di_eattr)
F(di_reserved)
};

#undef STRUCT
struct gfs2_indirect { struct gfs2_meta_header in_header; };
#define STRUCT gfs2_indirect

static const struct lgfs2_metafield gfs2_indirect_fields[] = {
MH(in_header)
};

#undef STRUCT
#define STRUCT gfs_indirect

static const struct lgfs2_metafield gfs_indirect_fields[] = {
MH(in_header)
RF(in_reserved)
};

#undef STRUCT
#define STRUCT gfs2_leaf

static const struct lgfs2_metafield gfs2_leaf_fields[] = {
MH(lf_header)
F(lf_depth)
F(lf_entries)
F(lf_dirent_format)
F(lf_next)
RF(lf_reserved)
};

#undef STRUCT
struct gfs2_jrnl_data { struct gfs2_meta_header jd_header; };
#define STRUCT gfs2_jrnl_data

static const struct lgfs2_metafield gfs2_jdata_fields[] = {
MH(jd_header)
};

#undef STRUCT
#define STRUCT gfs2_log_header

static const struct lgfs2_metafield gfs2_log_header_fields[] = {
MH(lh_header)
F(lh_sequence)
F(lh_flags)
F(lh_tail)
F(lh_blkno)
F(lh_hash, .flags = LGFS2_MFF_CHECK)
};

#undef STRUCT
#define STRUCT gfs_log_header

static const struct lgfs2_metafield gfs_log_header_fields[] = {
MH(lh_header)
F(lh_flags)
RF(lh_pad)
F(lh_first)
F(lh_sequence)
F(lh_tail)
F(lh_last_dump)
RF(lh_reserved)
};

#undef STRUCT
#define STRUCT gfs2_log_descriptor

static const struct lgfs2_metafield gfs2_log_desc_fields[] = {
MH(ld_header)
F(ld_type)
F(ld_length, .flags = LGFS2_MFF_FSBLOCKS)
F(ld_data1)
F(ld_data2)
RF(ld_reserved)
};

#undef STRUCT
#define STRUCT gfs_log_descriptor

static const struct lgfs2_metafield gfs_log_desc_fields[] = {
MH(ld_header)
F(ld_type)
F(ld_length, .flags = LGFS2_MFF_FSBLOCKS)
F(ld_data1)
F(ld_data2)
RF(ld_reserved)
};

#undef STRUCT
struct gfs2_log_block { struct gfs2_meta_header lb_header; };
#define STRUCT gfs2_log_block

static const struct lgfs2_metafield gfs2_log_block_fields[] = {
MH(lb_header)
};

#undef STRUCT
struct gfs2_ea_attr { struct gfs2_meta_header ea_header; };
#define STRUCT gfs2_ea_attr

static const struct lgfs2_metafield gfs2_ea_attr_fields[] = {
MH(ea_header)
};

#undef STRUCT
struct gfs2_ea_data { struct gfs2_meta_header ed_header; };
#define STRUCT gfs2_ea_data

static const struct lgfs2_metafield gfs2_ea_data_fields[] = {
MH(ed_header)
};

#undef STRUCT
#define STRUCT gfs2_quota_change

static const struct lgfs2_metafield gfs2_quota_change_fields[] = {
F(qc_change, .flags = LGFS2_MFF_FSBLOCKS)
F(qc_flags)
F(qc_id)
};

#undef STRUCT
#define STRUCT gfs2_dirent

static const struct lgfs2_metafield gfs2_dirent_fields[] = {
IN(de_inum)
F(de_hash, .flags = LGFS2_MFF_CHECK)
F(de_rec_len, .flags = LGFS2_MFF_BYTES)
F(de_name_len, .flags = LGFS2_MFF_BYTES)
F(de_type)
RF(__pad)
};

#undef STRUCT
#define STRUCT gfs2_ea_header

static const struct lgfs2_metafield gfs2_ea_header_fields[] = {
F(ea_rec_len, .flags = LGFS2_MFF_BYTES)
F(ea_data_len, .flags = LGFS2_MFF_BYTES)
F(ea_name_len, .flags = LGFS2_MFF_BYTES)
F(ea_type)
F(ea_flags)
F(ea_num_ptrs)
RF(__pad)
};

#undef STRUCT
#define STRUCT gfs2_inum_range

static const struct lgfs2_metafield gfs2_inum_range_fields[] = {
F(ir_start)
F(ir_length)
};

#undef STRUCT
#define STRUCT gfs2_statfs_change

static const struct lgfs2_metafield gfs2_statfs_change_fields[] = {
F(sc_total, .flags = LGFS2_MFF_FSBLOCKS)
F(sc_free, .flags = LGFS2_MFF_FSBLOCKS)
F(sc_dinodes, .flags = LGFS2_MFF_FSBLOCKS)
};

#undef STRUCT
#define STRUCT gfs_jindex

static const struct lgfs2_metafield gfs_jindex_fields[] = {
FP(ji_addr)
F(ji_nsegment)
RF(ji_pad)
RF(ji_reserved)
};

const struct lgfs2_metadata lgfs2_metadata[] = {
	[LGFS2_MT_GFS2_SB] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_SB,
		.mh_format = GFS2_FORMAT_SB,
		.name = "gfs2_sb",
		.fields = gfs2_sb_fields,
		.nfields = ARRAY_SIZE(gfs2_sb_fields),
		.size = sizeof(struct gfs2_sb),
	},
	[LGFS2_MT_GFS_SB] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_SB,
		.mh_format = GFS_FORMAT_SB,
		.name = "gfs_sb",
		.fields = gfs_sb_fields,
		.nfields = ARRAY_SIZE(gfs_sb_fields),
		.size = sizeof(struct gfs_sb),
	},
	[LGFS2_MT_RINDEX] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.name = "rindex",
		.fields = gfs2_rindex_fields,
		.nfields = ARRAY_SIZE(gfs2_rindex_fields),
		.size = sizeof(struct gfs2_rindex),
	},
	[LGFS2_MT_GFS2_RGRP] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_RG,
		.mh_format = GFS2_FORMAT_RG,
		.name = "gfs2_rgrp",
		.fields = gfs2_rgrp_fields,
		.nfields = ARRAY_SIZE(gfs2_rgrp_fields),
		.size = sizeof(struct gfs2_rgrp),
	},
	[LGFS2_MT_GFS_RGRP] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_RG,
		.mh_format = GFS2_FORMAT_RG,
		.name = "gfs_rgrp",
		.fields = gfs_rgrp_fields,
		.nfields = ARRAY_SIZE(gfs_rgrp_fields),
		.size = sizeof(struct gfs_rgrp),
	},
	[LGFS2_MT_RGRP_BITMAP] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_RB,
		.mh_format = GFS2_FORMAT_RB,
		.name = "gfs2_rgrp_bitmap",
		.fields = gfs2_rgrp_bitmap_fields,
		.nfields = ARRAY_SIZE(gfs2_rgrp_bitmap_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_GFS2_DINODE] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_DI,
		.mh_format = GFS2_FORMAT_DI,
		.name = "gfs2_dinode",
		.fields = gfs2_dinode_fields,
		.nfields = ARRAY_SIZE(gfs2_dinode_fields),
		.size = sizeof(struct gfs2_dinode),
	},
	[LGFS2_MT_GFS_DINODE] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_DI,
		.mh_format = GFS2_FORMAT_DI,
		.name = "gfs_dinode",
		.fields = gfs_dinode_fields,
		.nfields = ARRAY_SIZE(gfs_dinode_fields),
		.size = sizeof(struct gfs_dinode),
	},
	[LGFS2_MT_GFS2_INDIRECT] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_IN,
		.mh_format = GFS2_FORMAT_IN,
		.name = "gfs2_indirect",
		.fields = gfs2_indirect_fields,
		.nfields = ARRAY_SIZE(gfs2_indirect_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_GFS_INDIRECT] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_IN,
		.mh_format = GFS2_FORMAT_IN,
		.name = "gfs_indirect",
		.fields = gfs_indirect_fields,
		.nfields = ARRAY_SIZE(gfs_indirect_fields),
		.size = sizeof(struct gfs_indirect),
	},
	[LGFS2_MT_DIR_LEAF] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LF,
		.mh_format = GFS2_FORMAT_LF,
		.name = "gfs2_leaf",
		.fields = gfs2_leaf_fields,
		.nfields = ARRAY_SIZE(gfs2_leaf_fields),
		.size = sizeof(struct gfs2_leaf),
	},
	[LGFS2_MT_JRNL_DATA] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_JD,
		.mh_format = GFS2_FORMAT_JD,
		.name = "gfs2_jdata",
		.fields = gfs2_jdata_fields,
		.nfields = ARRAY_SIZE(gfs2_jdata_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_GFS2_LOG_HEADER] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LH,
		.mh_format = GFS2_FORMAT_LH,
		.name = "gfs2_log_header",
		.fields = gfs2_log_header_fields,
		.nfields = ARRAY_SIZE(gfs2_log_header_fields),
		.size = sizeof(struct gfs2_log_header),
	},
	[LGFS2_MT_GFS_LOG_HEADER] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LH,
		.mh_format = GFS2_FORMAT_LH,
		.name = "gfs_log_header",
		.fields = gfs_log_header_fields,
		.nfields = ARRAY_SIZE(gfs_log_header_fields),
		.size = sizeof(struct gfs_log_header),
	},
	[LGFS2_MT_GFS2_LOG_DESC] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LD,
		.mh_format = GFS2_FORMAT_LD,
		.name = "gfs2_log_desc",
		.fields = gfs2_log_desc_fields,
		.nfields = ARRAY_SIZE(gfs2_log_desc_fields),
		.size = sizeof(struct gfs2_log_descriptor),
	},
	[LGFS2_MT_GFS_LOG_DESC] = {
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LD,
		.mh_format = GFS2_FORMAT_LD,
		.name = "gfs_log_desc",
		.fields = gfs_log_desc_fields,
		.nfields = ARRAY_SIZE(gfs_log_desc_fields),
		.size = sizeof(struct gfs_log_descriptor),
	},
	[LGFS2_MT_GFS2_LOG_BLOCK] = {
		.gfs2 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_LB,
		.mh_format = GFS2_FORMAT_LB,
		.name = "gfs2_log_block",
		.fields = gfs2_log_block_fields,
		.nfields = ARRAY_SIZE(gfs2_log_block_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_EA_ATTR] = {
		.gfs2 = 1,
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_EA,
		.mh_format = GFS2_FORMAT_EA,
		.name = "gfs2_ea_attr",
		.fields = gfs2_ea_attr_fields,
		.nfields = ARRAY_SIZE(gfs2_ea_attr_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_EA_DATA] = {
		.gfs2 = 1,
		.gfs1 = 1,
		.header = 1,
		.mh_type = GFS2_METATYPE_ED,
		.mh_format = GFS2_FORMAT_ED,
		.name = "gfs2_ea_data",
		.fields = gfs2_ea_data_fields,
		.nfields = ARRAY_SIZE(gfs2_ea_data_fields),
		.size = sizeof(struct gfs2_meta_header),
	},
	[LGFS2_MT_GFS2_QUOTA_CHANGE] = {
		.gfs2 = 1,
		.name = "gfs2_quota_change",
		.fields = gfs2_quota_change_fields,
		.nfields = ARRAY_SIZE(gfs2_quota_change_fields),
		.size = sizeof(struct gfs2_quota_change),
	},
	[LGFS2_MT_DIRENT] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.name = "gfs2_dirent",
		.fields = gfs2_dirent_fields,
		.nfields = ARRAY_SIZE(gfs2_dirent_fields),
		.size = sizeof(struct gfs2_dirent),
	},
	[LGFS2_MT_EA_HEADER] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.name = "gfs2_ea_header",
		.fields = gfs2_ea_header_fields,
		.nfields = ARRAY_SIZE(gfs2_ea_header_fields),
		.size = sizeof(struct gfs2_ea_header),
	},
	[LGFS2_MT_GFS2_INUM_RANGE] = {
		.gfs2 = 1,
		.name = "gfs2_inum_range",
		.fields = gfs2_inum_range_fields,
		.nfields = ARRAY_SIZE(gfs2_inum_range_fields),
		.size = sizeof(struct gfs2_inum_range),
	},
	[LGFS2_MT_STATFS_CHANGE] = {
		.gfs1 = 1,
		.gfs2 = 1,
		.name = "gfs2_statfs_change",
		.fields = gfs2_statfs_change_fields,
		.nfields = ARRAY_SIZE(gfs2_statfs_change_fields),
		.size = sizeof(struct gfs2_statfs_change),
	},
	[LGFS2_MT_GFS_JINDEX] = {
		.gfs1 = 1,
		.name = "gfs_jindex",
		.fields = gfs_jindex_fields,
		.nfields = ARRAY_SIZE(gfs_jindex_fields),
		.size = sizeof(struct gfs_jindex),
	},
};

const unsigned lgfs2_metadata_size = ARRAY_SIZE(lgfs2_metadata);

static int check_metadata_sizes(void)
{
	unsigned offset;
	int i, j;
	int ret = 0;

	for (i = 0; i < lgfs2_metadata_size; i++) {
		const struct lgfs2_metadata *m = &lgfs2_metadata[i];
		offset = 0;
		for (j = 0; j < m->nfields; j++) {
			const struct lgfs2_metafield *f = &m->fields[j];
			if (f->offset != offset) {
				fprintf(stderr, "%s: %s: offset is %u, expected %u\n", m->name, f->name, f->offset, offset);
				ret = -1;
			}
			offset += f->length;
		}
		if (offset != m->size) {
			fprintf(stderr, "%s: size mismatch between struct %u and fields %u\n", m->name, m->size, offset);
			ret = -1;
		}
	}

	return ret;
}

static int check_symtab(void)
{
	int i, j;
	int ret = 0;

	for (i = 0; i < lgfs2_metadata_size; i++) {
		const struct lgfs2_metadata *m = &lgfs2_metadata[i];
		for (j = 0; j < m->nfields; j++) {
			const struct lgfs2_metafield *f = &m->fields[j];
			if (f->flags & (LGFS2_MFF_MASK|LGFS2_MFF_ENUM)) {
				if (f->symtab == NULL) {
					fprintf(stderr, "%s: Missing symtab for %s\n", m->name, f->name);
					ret = -1;
				}
			}
			if (f->symtab) {
				if (!(f->flags & (LGFS2_MFF_MASK|LGFS2_MFF_ENUM))) {
					fprintf(stderr, "%s: Symtab for non-enum and non-mask field %s\n", m->name, f->name);
					ret = -1;
				}
			}
		}
	}

	return ret;
}

int lgfs2_selfcheck(void)
{
	int ret = 0;

	ret |= check_metadata_sizes();
	ret |= check_symtab();

	return ret;
}

