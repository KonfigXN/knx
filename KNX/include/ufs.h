#pragma once
#include <stdint.h>

#define UFS_DEV_BSIZE 512
#define UFS_DEV_BSHIFT 9

#define UFS_SBLOCK_UFS1 8192
#define UFS_SBLOCK_UFS2 65536
#define UFS_SBLOCK_FLOPPY 0
#define UFS_SBLOCK_PIGGY 262144
#define UFS_SBLOCKSIZE 8192
#define UFS_SBLOCK_SECTORS (UFS_SBLOCK_UFS1 / UFS_DEV_BSIZE)

#define FS_UFS1_MAGIC 0x011954u
#define FS_UFS2_MAGIC 0x19540119u
#define CG_MAGIC 0x090255u

#define UFS_ROOTINO 2u
#define UFS_WINO 1u

#define UFS_NDADDR 12
#define UFS_NIADDR 3
#define UFS_NXADDR 2

#define UFS_NINDIR 1024

#define UFS_IFMT 0170000
#define UFS_IFIFO 0010000
#define UFS_IFCHR 0020000
#define UFS_IFDIR 0040000
#define UFS_IFBLK 0060000
#define UFS_IFREG 0100000
#define UFS_IFLNK 0120000
#define UFS_IFSOCK 0140000

#define UFS_DT_UNKNOWN 0
#define UFS_DT_FIFO 1
#define UFS_DT_CHR 2
#define UFS_DT_DIR 4
#define UFS_DT_BLK 6
#define UFS_DT_REG 8
#define UFS_DT_LNK 10
#define UFS_DT_SOCK 12

#define UFS_MAXNAMLEN 255
#define UFS_DIRBLKSIZ UFS_DEV_BSIZE

#define UFS_MINBSIZE 4096
#define UFS_MAXMNTLEN 468
#define UFS_MAXVOLLEN 32

#define UFS_FSOFF_FIRSTRFIELD 0
#define UFS_FSOFF_SBLKNO 8
#define UFS_FSOFF_CBLKNO 12
#define UFS_FSOFF_IBLKNO 16
#define UFS_FSOFF_DBLKNO 20
#define UFS_FSOFF_NCG 44
#define UFS_FSOFF_BSIZE 48
#define UFS_FSOFF_FSIZE 52
#define UFS_FSOFF_FRAG 56
#define UFS_FSOFF_BMASK 72
#define UFS_FSOFF_FMASK 76
#define UFS_FSOFF_BSHIFT 80
#define UFS_FSOFF_FSHIFT 84
#define UFS_FSOFF_FSBTODB 100
#define UFS_FSOFF_SBSIZE 104
#define UFS_FSOFF_NINDIR 116
#define UFS_FSOFF_INOPB 120
#define UFS_FSOFF_CGA 188
#define UFS_FSOFF_IPG 184
#define UFS_FSOFF_CSTOTAL 192
#define UFS_FSOFF_CSSIZE 156
#define UFS_FSOFF_CGSIZE 160
#define UFS_FSOFF_FSMNT 212
#define UFS_FSOFF_VOLNAME 680
#define UFS_FSOFF_SIZE 1080
#define UFS_FSOFF_DSIZE 1088
#define UFS_FSOFF_CSADDR 1096
#define UFS_FSOFF_INODEFMT 1324
#define UFS_FSOFF_MAGIC 1372

struct ufs_sb {
    int32_t fs_firstfield;
    int32_t fs_unused_1;
    int32_t fs_sblkno;
    int32_t fs_cblkno;
    int32_t fs_iblkno;
    int32_t fs_dblkno;
    int32_t fs_old_cgoffset;
    int32_t fs_old_cgmask;
    int32_t fs_old_time;
    int32_t fs_old_size;
    int32_t fs_old_dsize;
    uint32_t fs_ncg;
    int32_t fs_bsize;
    int32_t fs_fsize;
    int32_t fs_frag;
    int32_t fs_minfree;
    int32_t fs_old_rotdelay;
    int32_t fs_old_rps;
    int32_t fs_bmask;
    int32_t fs_fmask;
    int32_t fs_bshift;
    int32_t fs_fshift;
    int32_t fs_maxcontig;
    int32_t fs_maxbpg;
    int32_t fs_fragshift;
    int32_t fs_fsbtodb;
    int32_t fs_sbsize;
    int32_t fs_spare1[2];
    int32_t fs_nindir;
    uint32_t fs_inopb;
    int32_t fs_old_nspf;
    int32_t fs_optim;
    int32_t fs_old_npsect;
    int32_t fs_old_interleave;
    int32_t fs_old_trackskew;
    int32_t fs_id[2];
    int32_t fs_old_csaddr;
    int32_t fs_cssize;
    int32_t fs_cgsize;
    int32_t fs_spare2;
    int32_t fs_old_nsect;
    int32_t fs_old_spc;
    int32_t fs_old_ncyl;
    int32_t fs_old_cpg;
    uint32_t fs_ipg;
    int32_t fs_fpg;
};

struct ufs1_dinode {
    uint16_t di_mode;
    uint16_t di_nlink;
    uint32_t di_freelink;
    uint64_t di_size;
    uint32_t di_atime;
    int32_t di_atimensec;
    uint32_t di_mtime;
    int32_t di_mtimensec;
    uint32_t di_ctime;
    int32_t di_ctimensec;
    int32_t di_db[UFS_NDADDR];
    int32_t di_ib[UFS_NIADDR];
    uint32_t di_flags;
    uint32_t di_blocks;
    uint32_t di_gen;
    uint32_t di_uid;
    uint32_t di_gid;
    uint64_t di_modrev;
};

struct ufs_cg_hdr {
    int32_t cg_firstfield;
    int32_t cg_magic;
    int32_t cg_old_time;
    uint32_t cg_cgx;
    int16_t cg_old_ncyl;
    int16_t cg_old_niblk;
    uint32_t cg_ndblk;
    int32_t cg_cs_ndir;
    int32_t cg_cs_nbfree;
    int32_t cg_cs_nifree;
    int32_t cg_cs_nffree;
    uint32_t cg_rotor;
    uint32_t cg_frotor;
    uint32_t cg_irotor;
    uint32_t cg_frsum[8];
    int32_t cg_old_btotoff;
    int32_t cg_old_boff;
    uint32_t cg_iusedoff;
    uint32_t cg_freeoff;
    uint32_t cg_nextfreeoff;
    uint32_t cg_clustersumoff;
    uint32_t cg_clusteroff;
    uint32_t cg_nclusterblks;
    uint32_t cg_niblk;
    uint32_t cg_initediblk;
    uint32_t cg_unrefs;
    int32_t cg_sparecon32;
    uint32_t cg_ckhash;
    int64_t cg_time;
    int64_t cg_sparecon64[3];
};

struct ufs_direct {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t d_type;
    uint8_t d_namlen;
    char d_name[UFS_MAXNAMLEN + 1];
};

#define UFS_INOPB(fs_bsize) ((unsigned)(fs_bsize) / sizeof(struct ufs1_dinode))
#define UFS_CGSTART(fs_fpg, c) ((unsigned)(fs_fpg) * (unsigned)(c))
#define UFS_CGDMIN(sb, c) (UFS_CGSTART((sb)->fs_fpg, (c)) + (sb)->fs_dblkno)
#define UFS_CGIMIN(sb, c) (UFS_CGSTART((sb)->fs_fpg, (c)) + (sb)->fs_iblkno)
#define UFS_CGTOD(sb, c) (UFS_CGSTART((sb)->fs_fpg, (c)) + (sb)->fs_cblkno)
#define UFS_CGSBLOCK(sb, c) (UFS_CGSTART((sb)->fs_fpg, (c)) + (sb)->fs_sblkno)
