/*
 * myfsck.c – File-system checker for the CS 423 MP4 file system format.
 *
 * Disk layout (from fs.h / mkfs.c):
 *   [ block 0: boot | block 1: superblock | inode blocks | bitmap blocks |
 *     data blocks ]
 *
 * Usage:
 *   myfsck <image>          – check only, exit 0 if clean
 *   myfsck -r <image>       – check + repair (extra credit)
 */


#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define stat   stat_423
#define dirent dirent_423
#include "types.h"
#include "fs.h"
#include "stat.h"
#undef stat
#undef dirent

#include "log.h"

/*
 * Set to 1 when the -r flag is given.  Lets checks #9 and #11 skip
 * their exit(1) so the repair pass can fix them instead.
 */
static int g_repair = 0;

/* ------------------------------------------------------------------ */
/* Layout helpers                                                       */
/* ------------------------------------------------------------------ */

/* Pointer to the first byte of block bn in the mapped image. */
static inline void *blkptr(void *img, uint bn)
{
	return (char *)img + (size_t)bn * BSIZE;
}

/* Pointer to on-disk inode inum (inodes are zero-indexed; 0 is unused). */
static inline dinode *inode_at(void *img, uint inum)
{
	return (dinode *)blkptr(img, IBLOCK(inum)) + (inum % IPB);
}

/*
 * Number of inode blocks for a filesystem with ni inodes.
 * Matches NINODEBLOCKS() in mkfs.c.
 */
#define INODE_BLOCKS(ni)  ((ni) / IPB + 1)

/*
 * Number of bitmap blocks for a filesystem of sz total blocks.
 * Matches BITBLOCKS() in mkfs.c.
 */
#define BITMAP_BLOCKS(sz) ((sz) / BPB + 1)

/* Index of the first allocatable data block. */
static inline uint first_data_blk(uint ninodes, uint size)
{
	return 2 + INODE_BLOCKS(ninodes) + BITMAP_BLOCKS(size);
}

/*
 * Return 1 if block bn is marked used in the bitmap, 0 otherwise.
 * BBLOCK(b, ninodes) from fs.h gives the bitmap-block number for block b.
 */
static inline int bitmap_is_used(void *img, uint bn, uint ninodes)
{
	uchar *bm  = (uchar *)blkptr(img, BBLOCK(bn, ninodes));
	uint   bit = bn % BPB;  /* bit offset within this bitmap block */
	return (bm[bit / 8] >> (bit % 8)) & 1;
}

/* ------------------------------------------------------------------ */
/* Check #1 – superblock sanity                                        */
/* ------------------------------------------------------------------ */

/*
 * The total image size must be large enough to hold all the regions:
 *   boot(1) + superblock(1) + inode-blocks + bitmap-blocks + data-blocks
 *
 * Also rejects a sb->size or sb->ninodes large enough that it overflows the
 * mapped image (e.g., a corrupt superblock with a wildly large size field).
 * Run BEFORE any sb-derived allocations / pointer math.
 */
static void check_superblock(superblock *sb, size_t imgsize)
{
	if (sb->size < 2 || sb->ninodes <= ROOTINO) {
		superblock_error();
		exit(1);
	}

	/* Checks if claimed filesystem size is larger than actual on-disk image size */
	if ((size_t)sb->size > imgsize / BSIZE) {
		superblock_error();
		exit(1);
	}

	size_t iblks = INODE_BLOCKS(sb->ninodes);
	size_t bblks = BITMAP_BLOCKS(sb->size);
	size_t first_data = 2 + iblks + bblks; /* computes where first blocks should begin */

	/* Checks if metadata exceeds filesystem size */
	if (first_data > sb->size) {
		superblock_error();
		exit(1);
	}

	/* Checks if declared datablocks fit after metadata */
	if ((size_t)sb->nblocks > (size_t)sb->size - first_data) {
		superblock_error();
		exit(1);
	}
}

/* ------------------------------------------------------------------ */
/* Check #2 – every inode is either free (type 0) or a valid type     */
/* ------------------------------------------------------------------ */

static void check_inode_types(void *img, superblock *sb)
{
	uint i;

	for (i = 1; i < sb->ninodes; i++) {
		short type = inode_at(img, i)->type;
		if (type != 0 && type != T_FILE && type != T_DIR && type != T_DEV) {
			bad_inode_error();
			exit(1);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Checks #3, #5, #7, #8 – per-inode address and size validation      */
/* ------------------------------------------------------------------ */

/*
 * Validate one block address that is directly referenced by an inode.
 *
 * first_data    : first allocatable data block number
 * fs_size       : total blocks in the image (from superblock)
 * ninodes       : ninodes field from superblock (bitmap lookup)
 * blks_used     : array[fs_size]; set to 1 when a block is referenced
 *                 (used by check_bitmap for #6).
 * direct_seen   : array[fs_size]; tracks blocks seen via direct slots.
 * indirect_seen : array[fs_size]; tracks blocks seen via the indirect
 *                 pointer block or its entries.
 * is_direct     : non-zero → direct block; zero → indirect path.
 *
 * Duplicate detection looks at BOTH arrays so cross-direct/indirect
 * sharing is also caught – the error reported matches the caller side.
 */
static void validate_addr(void *img, uint addr,
			   uint first_data, uint fs_size, uint ninodes,
			   uchar *blks_used,
			   uchar *direct_seen, uchar *indirect_seen,
			   int is_direct)
{
	/* check #3 – address within the data-block region */
	if (addr < first_data || addr >= fs_size) {
		if (is_direct)
			bad_direct_address_error();
		else
			bad_indirect_address_error();
		exit(1);
	}

	/* check #5 – block must be marked used in the bitmap */
	if (!bitmap_is_used(img, addr, ninodes)) {
		bad_used_address_error();
		exit(1);
	}

	/* check #7 – no inode reference may share a block with another */
	if (direct_seen[addr] || indirect_seen[addr]) {
		if (is_direct)
			bad_direct_address_once_error();
		else
			bad_indirect_address_once_error();
		exit(1);
	}

	/* Mark the block as seen */
	if (is_direct)
		direct_seen[addr] = 1;
	else
		indirect_seen[addr] = 1;

	blks_used[addr] = 1;
}

/*
 * Scan all in-use inodes.  For each one:
 *   • validate every direct and indirect block address (#3, #5, #7)
 *   • verify the stored file size is consistent with the block count (#8)
 *
 * Side effects:
 *   blks_used[b]    is set to 1 for every block b actually referenced by
 *                   an inode (direct data, indirect pointer, or indirect data).
 *   direct_seen[b]  is set to 1 for every block b used as a direct block.
 *   inode_in_use[i] is set to 1 for every inode i that has a non-zero type.
 */
static void scan_inodes(void *img, superblock *sb,
			uchar *blks_used,
			uchar *direct_seen, uchar *indirect_seen,
			uchar *inode_in_use)
{
	uint first_data = first_data_blk(sb->ninodes, sb->size);
	uint i, j;

	/* inode 0 is unused, root inode starts from 1 */
	for (i = 1; i < sb->ninodes; i++) {
		dinode *dip = inode_at(img, i);

		if (dip->type == 0)
			continue;  /* free inode – skip */

		inode_in_use[i] = 1;

		uint nblks = 0;  /* data blocks actually allocated */

		/* --- direct block slots (addrs[0..NDIRECT-1]) --- */
		for (j = 0; j < NDIRECT; j++) {
			uint addr = dip->addrs[j];
			if (addr == 0)
				continue;

			validate_addr(img, addr, first_data, sb->size,
				      sb->ninodes, blks_used,
				      direct_seen, indirect_seen, 1);
			nblks++;
		}

		/* --- indirect block pointer (addrs[NDIRECT]) --- */
		uint iaddr = dip->addrs[NDIRECT];
		if (iaddr != 0) {
			/*
			 * The indirect block itself is a real block; we validate
			 * it but do not count it in nblks (it stores pointers,
			 * not file data).
			 */
			validate_addr(img, iaddr, first_data, sb->size,
				      sb->ninodes, blks_used,
				      direct_seen, indirect_seen, 0);

			/* walk the indirect entries */
			uint *indir = (uint *)blkptr(img, iaddr);
			for (j = 0; j < NINDIRECT; j++) {
				uint addr = indir[j];
				if (addr == 0)
					continue;

				validate_addr(img, addr, first_data, sb->size,
					      sb->ninodes, blks_used,
					      direct_seen, indirect_seen, 0);
				nblks++;
			}
		}

		/* --- check #8 – file size vs. allocated block count --- */
		/*
		 * With b data blocks of size s, size must satisfy:
		 *   b == 0 → size == 0
		 *   b > 0  → (b-1)*s < size <= b*s
		 */
		uint sz = dip->size;
		if (nblks == 0) {
			if (sz != 0) {
				bad_file_size_error();
				exit(1);
			}
		} else {
			if (sz == 0 || sz > (uint)nblks * BSIZE ||
			    sz <= (uint)(nblks - 1) * BSIZE) {
				bad_file_size_error();
				exit(1);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Check #6 – bitmap-marked blocks that no inode actually uses        */
/* ------------------------------------------------------------------ */

/*
 * For every data block (first_data..size-1), if the bitmap claims the
 * block is in use but no inode references it, report the error.
 *
 * Metadata blocks (0..first_data-1) are legitimately marked used by
 * mkfs and are excluded from this check.
 */
static void check_bitmap(void *img, superblock *sb, uchar *blks_used)
{
	uint first_data = first_data_blk(sb->ninodes, sb->size);
	uint b;

	for (b = first_data; b < sb->size; b++) {
		if (bitmap_is_used(img, b, sb->ninodes) && !blks_used[b]) {
			bad_bitmap_marked_used_error();
			exit(1);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Checks #4, #9, #10, #11, #12 – directory structure                 */
/* ------------------------------------------------------------------ */

/*
 * Process one directory data block (blkno) belonging to directory inode
 * dir_inum.  For each non-empty entry:
 *
 *   check #10: referenced inum must belong to an in-use inode
 *   check #4:  '.' entry must reference the directory itself;
 *              presence of '.' and '..' is tracked for the caller
 *
 * ref_count[]        incremented for every entry (including '.' / '..').
 * named_ref_count[]  incremented only for non-dot/dotdot entries.
 * dotdot_of[D]       set to inum when the '..' entry of directory D is seen.
 * named_parent_of[C] set to dir_inum when a named entry names a T_DIR child C.
 */
static void scan_dir_block(void *img, superblock *sb, uint dir_inum,
			   uint blkno, uchar *inode_in_use,
			   uint *ref_count, uint *named_ref_count,
			   uint *dotdot_of, uint *named_parent_of,
			   int *found_dot, int *found_dotdot)
{
	dirent_423 *de = (dirent_423 *)blkptr(img, blkno);
	int nent = BSIZE / sizeof(dirent_423);
	int k;

	for (k = 0; k < nent; k++, de++) {
		if (de->inum == 0)
			continue;  /* unused directory slot */

		uint inum = de->inum;

		/* check #10 – referenced inode must be in-use */
		if (inum >= sb->ninodes || !inode_in_use[inum]) {
			bad_inode_referred_marked_error();
			exit(1);
		}

		/* total references, including dot/dotdot; not used for orphan detection */
		ref_count[inum]++;

		int is_dot    = strncmp(de->name, ".",  DIRSIZ) == 0;
		int is_dotdot = strncmp(de->name, "..", DIRSIZ) == 0;

		if (is_dot) {
			/* check #4 – '.' must refer back to this directory */
			if (inum != dir_inum) {
				bad_directory_format_error();
				exit(1);
			}
			*found_dot = 1;
		} else if (is_dotdot) {
			/* record claimed parent for EC check #1 */
			dotdot_of[dir_inum] = inum;
			*found_dotdot = 1;
		} else {
			/*
			 * Named entry: counts toward nlink (#11) and the
			 * directory hard-link check (#12).  Also record the
			 * naming parent for each child directory (EC check #1).
			 */
			named_ref_count[inum]++;
			if (inode_at(img, inum)->type == T_DIR)
				named_parent_of[inum] = dir_inum;
		}
	}
}

/*
 * Walk every in-use T_DIR inode, scanning its data blocks.
 * Enforces check #4 after each directory's blocks are fully scanned.
 * check #10 is enforced inline inside scan_dir_block().
 * Populates dotdot_of[] and named_parent_of[] for the EC checks.
 */
static void scan_directories(void *img, superblock *sb,
			     uchar *inode_in_use,
			     uint *ref_count, uint *named_ref_count,
			     uint *dotdot_of, uint *named_parent_of)
{
	uint i, j;

	for (i = 1; i < sb->ninodes; i++) {
		dinode *dip = inode_at(img, i);
		if (dip->type != T_DIR)
			continue;

		int found_dot    = 0;
		int found_dotdot = 0;

		/* direct data blocks */
		for (j = 0; j < NDIRECT; j++) {
			if (dip->addrs[j] == 0)
				continue;
			scan_dir_block(img, sb, i, dip->addrs[j],
				       inode_in_use, ref_count,
				       named_ref_count,
				       dotdot_of, named_parent_of,
				       &found_dot, &found_dotdot);
		}

		/* indirect data blocks */
		if (dip->addrs[NDIRECT] != 0) {
			uint *indir = (uint *)blkptr(img, dip->addrs[NDIRECT]);
			for (j = 0; j < NINDIRECT; j++) {
				if (indir[j] == 0)
					continue;
				scan_dir_block(img, sb, i, indir[j],
					       inode_in_use, ref_count,
					       named_ref_count,
					       dotdot_of, named_parent_of,
					       &found_dot, &found_dotdot);
			}
		}

		/* check #4 – directory must contain both '.' and '..' */
		if (!found_dot || !found_dotdot) {
			bad_directory_format_error();
			exit(1);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Check #9 – every in-use inode must appear in at least one entry    */
/* ------------------------------------------------------------------ */

/*
 * Iterate all in-use inodes.  named_ref_count[i] was incremented for every
 * named directory entry that references inode i.  A count of zero
 * means no directory ever referenced this inode.
 */
static void check_inodes_referenced(superblock *sb,
				    uchar *inode_in_use,
				    uint *named_ref_count)
{
	for (uint i = 1; i < sb->ninodes; i++) {
		if (i == ROOTINO)
			continue;

		if (inode_in_use[i] && named_ref_count[i] == 0) {
			if (!g_repair) {
				bad_used_inode_not_found_error();
				exit(1);
			}
			/* repair mode: orphan will be moved to lost_found */
		}
	}
}

/* ------------------------------------------------------------------ */
/* Checks #11 and #12 – nlink and directory uniqueness                */
/* ------------------------------------------------------------------ */

/*
 * check #11: for T_FILE inodes, the on-disk nlink must equal the number
 *            of named directory entries that reference the inode.
 *            (Files never appear as '.' or '..', so named_ref_count
 *            equals total ref_count for files.)
 *
 * check #12: a T_DIR inode must appear as a named entry in at most one
 *            parent directory – directories cannot be hard-linked.
 *            named_ref_count > 1 means the directory was added to more
 *            than one parent.  (Root legitimately has named_ref_count==0.)
 */
static void check_ref_counts(void *img, superblock *sb,
			     uint *named_ref_count)
{
	uint i;

	for (i = 1; i < sb->ninodes; i++) {
		dinode *dip = inode_at(img, i);
		if (dip->type == 0)
			continue;

		if (dip->type == T_FILE) {
			/* check #11 */
			if ((uint)dip->nlink != named_ref_count[i]) {
				if (!g_repair) {
					bad_reference_count_error();
					exit(1);
				}
				/* repair mode: nlink will be corrected */
			}
		} else if (dip->type == T_DIR) {
			/* check #12 */
			if (named_ref_count[i] > 1) {
				bad_directory_once_error();
				exit(1);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* EC check #1 – '..' must point to the directory's actual parent     */
/* ------------------------------------------------------------------ */

/*
 * For every non-root T_DIR inode i, dotdot_of[i] records which inode i's
 * '..' entry names.  named_parent_of[i] records which directory actually
 * contains a named entry for i (i.e., i's true parent in the tree).
 *
 * A mismatch means the '..' is lying about the parent, or the parent
 * doesn't agree that i is its child.
 *
 * Root is skipped: root's '..' legitimately points to itself and it has
 * no external named parent.
 */
static void check_parent_mismatch(void *img, superblock *sb,
				  uchar *inode_in_use,
				  uint *dotdot_of, uint *named_parent_of)
{
	uint i;

	for (i = 1; i < sb->ninodes; i++) {
		if (!inode_in_use[i])
			continue;
		if (inode_at(img, i)->type != T_DIR)
			continue;
		if (i == ROOTINO)
			continue;  /* root's '..' → itself is intentional */

		if (dotdot_of[i] != named_parent_of[i]) {
			/*
			 * In repair mode, an orphan directory (named_parent_of==0)
			 * will be re-linked into lost_found and have its '..'
			 * rewritten by repair_lost_found(), so silence here.
			 * A real (non-orphan) parent mismatch is not repairable.
			 */
			if (g_repair && named_parent_of[i] == 0)
				continue;
			bad_parent_directory_error();
			exit(1);
		}
	}
}

static void check_parent_chains(void *img, superblock *sb,
				uchar *inode_in_use,
				uint *dotdot_of,
				uint *named_ref_count)
{
	for (uint start = 1; start < sb->ninodes; start++) {
		if (!inode_in_use[start])
			continue;
		if (inode_at(img, start)->type != T_DIR)
			continue;

		if (g_repair && start != ROOTINO && named_ref_count[start] == 0)
			continue;

		uchar *seen = calloc(sb->ninodes, sizeof(uchar));
		if (!seen) {
			perror("calloc");
			exit(1);
		}

		uint cur = start;

		while (cur != ROOTINO) {
			if (cur == 0 || cur >= sb->ninodes ||
			    !inode_in_use[cur] ||
			    inode_at(img, cur)->type != T_DIR) {
				free(seen);
				bad_directory_error();
				exit(1);
			}

			if (seen[cur]) {
				free(seen);
				bad_directory_error();
				exit(1);
			}

			seen[cur] = 1;
			cur = dotdot_of[cur];
		}

		free(seen);
	}
}

/* ------------------------------------------------------------------ */
/* EC check #2 – every directory must be reachable from root          */
/* ------------------------------------------------------------------ */

/*
 * BFS from root, following only named (non-dot/dotdot) entries that lead
 * to T_DIR children.  Any in-use T_DIR not reached is inaccessible.
 *
 * Also detects cycles that involve root: if any directory has a named
 * entry whose target is ROOTINO (i.e., named_ref_count[ROOTINO] > 0),
 * the directory tree contains a back-edge to root.  Cycles that don't
 * involve root are caught by check #12 (a node in such a cycle gets
 * named more than once) or by leaving the BFS unvisited.
 *
 * In repair mode the "unvisited" path is silenced for orphan dirs that
 * repair_lost_found() will adopt.  Cycle errors are NOT silenced – they
 * are not repairable by this checker.
 */
static void check_dir_reachable(void *img, superblock *sb,
				uchar *inode_in_use, uint *named_ref_count)
{
	/* root must never appear as a named entry – that would be a cycle */
	if (named_ref_count[ROOTINO] > 0) {
		bad_directory_error();
		exit(1);
	}

	uchar *visited = calloc(sb->ninodes, sizeof(uchar));
	uint  *queue   = calloc(sb->ninodes, sizeof(uint));
	uint   head = 0, tail = 0;
	uint   cur, j, k;

	if (!visited || !queue) {
		perror("calloc");
		exit(1);
	}

	/* seed BFS at root */
	visited[ROOTINO] = 1;
	queue[tail++]    = ROOTINO;

	while (head < tail) {
		cur = queue[head++];

		dinode *dip = inode_at(img, cur);

		/* walk every data block of directory cur */
		for (j = 0; j <= NDIRECT; j++) {
			uint blkno;
			uint *indir = NULL;
			int  nslots;

			if (j < NDIRECT) {
				blkno  = dip->addrs[j];
				nslots = 1;
			} else {
				/* indirect block — iterate its entries */
				blkno  = dip->addrs[NDIRECT];
				nslots = 0;  /* handled below */
			}

			if (j == NDIRECT) {
				if (blkno == 0)
					continue;
				indir  = (uint *)blkptr(img, blkno);
				nslots = NINDIRECT;
				blkno  = 0;  /* suppress direct-block path */
			}

			/* process one direct block or all indirect entries */
			for (k = 0; k < (indir ? (uint)nslots : 1); k++) {
				uint b = indir ? indir[k] : blkno;
				if (b == 0)
					continue;

				dirent_423 *de = (dirent_423 *)blkptr(img, b);
				int e;
				for (e = 0; e < BSIZE / (int)sizeof(dirent_423); e++, de++) {
					uint inum = de->inum;
					if (inum == 0 || inum >= sb->ninodes)
						continue;
					if (strncmp(de->name, ".",  DIRSIZ) == 0 ||
					    strncmp(de->name, "..", DIRSIZ) == 0)
						continue;
					if (inode_at(img, inum)->type != T_DIR)
						continue;
					if (!visited[inum]) {
						visited[inum]    = 1;
						queue[tail++]    = inum;
					}
				}
			}
		}
	}

	/*
	 * Any in-use T_DIR not reached from root is inaccessible.
	 */
	for (cur = 1; cur < sb->ninodes; cur++) {
		if (inode_in_use[cur] &&
		    inode_at(img, cur)->type == T_DIR &&
		    !visited[cur]) {

			if (g_repair && named_ref_count[cur] == 0) {
				continue;   // repair_lost_found can adopt it
			}

			bad_directory_error();
			exit(1);
		}
	}

	free(visited);
	free(queue);
}

/* ------------------------------------------------------------------ */
/* EC repair helpers – image mutation (MAP_SHARED, PROT_WRITE)        */
/* ------------------------------------------------------------------ */

/* Return the inum of the first free (type==0) inode, or 0 if full. */
static uint alloc_inode_slot(void *img, superblock *sb)
{
	uint i;
	for (i = 2; i < sb->ninodes; i++) {   /* skip root (1) */
		if (inode_at(img, i)->type == 0)
			return i;
	}
	return 0;
}

/*
 * Find the first free data block, mark it used in the bitmap, zero its
 * content, and return its block number.  Returns 0 if the image is full.
 */
static uint alloc_data_block(void *img, superblock *sb)
{
	uint first = first_data_blk(sb->ninodes, sb->size);
	uint b;

	for (b = first; b < sb->size; b++) {
		if (!bitmap_is_used(img, b, sb->ninodes)) {
			uchar *bm  = (uchar *)blkptr(img, BBLOCK(b, sb->ninodes));
			uint   bit = b % BPB;
			bm[bit / 8] |= (1 << (bit % 8));
			memset(blkptr(img, b), 0, BSIZE);
			return b;
		}
	}
	return 0;
}

/*
 * Append (child_inum, name) as a new entry in directory dir_inum.
 * Scans existing direct blocks for an empty slot (inum==0); allocates a
 * new block if every existing block is full.
 * Returns 0 on success, -1 if the directory has no room.
 */
static int add_entry_to_dir(void *img, superblock *sb,
			    uint dir_inum, uint child_inum, const char *name)
{
	dinode *dip = inode_at(img, dir_inum);
	uint    j;

	for (j = 0; j < NDIRECT; j++) {
		if (dip->addrs[j] == 0) {
			/* allocate a new data block for this directory */
			uint nb = alloc_data_block(img, sb);
			if (!nb)
				return -1;
			dip->addrs[j] = nb;
			dip->size     = (j + 1) * BSIZE;
		}

		dirent_423 *de   = (dirent_423 *)blkptr(img, dip->addrs[j]);
		int          nent = BSIZE / sizeof(dirent_423);
		int          k;

		for (k = 0; k < nent; k++, de++) {
			if (de->inum == 0) {
				de->inum = (ushort)child_inum;
				strncpy(de->name, name, DIRSIZ);
				return 0;
			}
		}
		/* block is full – try next slot */
	}
	return -1;  /* all NDIRECT blocks are full */
}

/*
 * Rewrite the '..' entry of directory dir_inum so it points to
 * new_parent_inum.  Used when an orphaned directory is adopted by
 * lost_found so that EC check #1 (parent mismatch) keeps passing.
 */
static void fix_dotdot(void *img, uint dir_inum, uint new_parent_inum)
{
	dinode *dip = inode_at(img, dir_inum);
	uint    j;

	for (j = 0; j < NDIRECT; j++) {
		if (dip->addrs[j] == 0)
			continue;
		dirent_423 *de   = (dirent_423 *)blkptr(img, dip->addrs[j]);
		int          nent = BSIZE / sizeof(dirent_423);
		int          k;
		for (k = 0; k < nent; k++, de++) {
			if (de->inum != 0 &&
			    strncmp(de->name, "..", DIRSIZ) == 0) {
				de->inum = (ushort)new_parent_inum;
				return;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* EC repair #3/#4 – move orphaned inodes into /lost_found            */
/* ------------------------------------------------------------------ */

/*
 * Find every in-use inode that has no directory reference (ref_count==0),
 * create a 'lost_found' directory under root, and add each orphan as an
 * entry named '#<inum>'.
 *
 * For orphaned T_DIR inodes, also rewrite their '..' entry to point at
 * lost_found so that the parent-mismatch EC check passes after repair.
 *
 * named_ref_count[] is updated inline so that repair_nlinks() sees the
 * correct counts.
 */
static void repair_lost_found(void *img, superblock *sb,
			      uchar *inode_in_use, uint *ref_count,
			      uint *named_ref_count)
{
	uint i;
	int  has_orphans = 0;

	/*
	 * An inode is orphaned iff it is in use and no NAMED directory entry
	 * (excluding '.' and '..') references it.  Using named_ref_count
	 * catches orphan T_DIR inodes too; ref_count would have been ≥ 1
	 * for any directory because of its own '.' self-reference.
	 */
	for (i = 1; i < sb->ninodes; i++) {
		if (i == ROOTINO)
			continue;
		if (inode_in_use[i] && named_ref_count[i] == 0) {
			has_orphans = 1;
			break;
		}
	}
	if (!has_orphans)
		return;

	/* allocate inode and one data block for lost_found */
	uint lf_inum = alloc_inode_slot(img, sb);
	uint lf_blk  = lf_inum ? alloc_data_block(img, sb) : 0;
	if (!lf_inum || !lf_blk) {
		bad_used_inode_not_found_error();
		exit(1);
	}

	/* initialise the lost_found directory inode */
	dinode *lf = inode_at(img, lf_inum);
	memset(lf, 0, sizeof(dinode));
	lf->type     = T_DIR;
	lf->nlink    = 2;
	lf->size     = BSIZE;
	lf->addrs[0] = lf_blk;

	/* write '.' and '..' into the first block */
	dirent_423 *de = (dirent_423 *)blkptr(img, lf_blk);
	de[0].inum = (ushort)lf_inum; strncpy(de[0].name, ".",  DIRSIZ);
	de[1].inum = (ushort)ROOTINO;  strncpy(de[1].name, "..", DIRSIZ);

	if (add_entry_to_dir(img, sb, ROOTINO, lf_inum, "lost_found") < 0) {
		bad_used_inode_not_found_error();
		exit(1);
	}

	named_ref_count[lf_inum] = 1;
	inode_at(img, ROOTINO)->nlink++;

	/* link each orphan into lost_found */
	for (i = 1; i < sb->ninodes; i++) {
		if (i == ROOTINO)
			continue;

		if (!inode_in_use[i] || named_ref_count[i] != 0)
			continue;

		char name[DIRSIZ];
		snprintf(name, DIRSIZ, "#%u", i);

		if (add_entry_to_dir(img, sb, lf_inum, i, name) < 0) {
			bad_used_inode_not_found_error();
			exit(1);
		}

		named_ref_count[i]++;   /* now referenced once */
		ref_count[i]++;

		/* fix '..' of orphaned dirs so EC check #1 still passes */
		if (inode_at(img, i)->type == T_DIR)
			fix_dotdot(img, i, lf_inum);
	}
}

/* ------------------------------------------------------------------ */
/* EC repair #5 – fix incorrect nlink values for regular files        */
/* ------------------------------------------------------------------ */

/*
 * For every in-use T_FILE inode, overwrite the on-disk nlink with the
 * count of named directory entries that reference it (named_ref_count[i]).
 * named_ref_count was updated by repair_lost_found() before this runs,
 * so previously-orphaned files get the correct value of 1.
 */
static void repair_nlinks(void *img, superblock *sb, uint *named_ref_count)
{
	uint i;
	for (i = 1; i < sb->ninodes; i++) {
		dinode *dip = inode_at(img, i);
		if (dip->type != T_FILE)
			continue;
		if ((uint)dip->nlink != named_ref_count[i])
			dip->nlink = (short)named_ref_count[i];
	}
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	int    repair  = 0;
	char  *imgpath = NULL;

	/* Parse arguments: myfsck <img> or myfsck -r <img> */
	if (argc == 2) {
		imgpath = argv[1];
	} else if (argc == 3 && strcmp(argv[1], "-r") == 0) {
		repair  = 1;
		imgpath = argv[2];
	} else {
		fprintf(stderr, "Usage: myfsck <file_system_image>\n");
		exit(1);
	}

	/* Open the image (writable only when repair mode is requested). */
	int fd = open(imgpath, repair ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "image not found.\n");
		exit(1);
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		perror("fstat");
		exit(1);
	}
	size_t imgsize = (size_t)st.st_size;

	/*
	 * Image must be at least two blocks (boot + superblock) before we can
	 * even read the superblock safely.
	 */
	if (imgsize < 2 * BSIZE) {
		superblock_error();
		exit(1);
	}

	/*
	 * Map the whole image into memory.  MAP_SHARED lets repairs write back
	 * to disk; MAP_PRIVATE is sufficient for read-only checking.
	 */
	int   prot  = PROT_READ | (repair ? PROT_WRITE : 0);
	int   flags = repair ? MAP_SHARED : MAP_PRIVATE;
	void *img   = mmap(NULL, imgsize, prot, flags, fd, 0);
	if (img == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	close(fd);

	g_repair = repair;   /* allow checks #9/#11 to skip exit in repair mode */

	superblock *sb = (superblock *)blkptr(img, 1);

	/*
	 * Validate the superblock FIRST – before allocating any per-block /
	 * per-inode tracking arrays sized from sb->size or sb->ninodes.  A
	 * corrupted size field could otherwise drive calloc() to a huge or
	 * impossible request.
	 */
	check_superblock(sb, imgsize);                      /* check #1          */

	/*
	 * Per-block tracking arrays, indexed 0..sb->size-1.
	 * Per-inode tracking arrays, indexed 0..sb->ninodes-1.
	 */
	uchar *blks_used       = calloc(sb->size,    sizeof(uchar));
	uchar *direct_seen     = calloc(sb->size,    sizeof(uchar));
	uchar *indirect_seen   = calloc(sb->size,    sizeof(uchar));
	uchar *inode_in_use    = calloc(sb->ninodes, sizeof(uchar));
	uint  *ref_count       = calloc(sb->ninodes, sizeof(uint));
	uint  *named_ref_count = calloc(sb->ninodes, sizeof(uint));
	/* EC: claimed parent (via '..') and actual naming parent per directory */
	uint  *dotdot_of       = calloc(sb->ninodes, sizeof(uint));
	uint  *named_parent_of = calloc(sb->ninodes, sizeof(uint));

	/* Check if allocation succeeded */
	if (!blks_used || !direct_seen || !indirect_seen || !inode_in_use ||
	    !ref_count || !named_ref_count ||
	    !dotdot_of || !named_parent_of) {
		perror("calloc");
		exit(1);
	}

	/* ---- continue checks in assignment order ---- */
	check_inode_types(img, sb);                         /* check #2          */
	/* root must be a directory */
	if (inode_at(img, ROOTINO)->type != T_DIR) {
		bad_directory_format_error();
		exit(1);
	}
	scan_inodes(img, sb, blks_used,
		    direct_seen, indirect_seen,
		    inode_in_use);                          /* checks #3,#5,#7,#8 */
	check_bitmap(img, sb, blks_used);                   /* check #6          */
	scan_directories(img, sb, inode_in_use,
			 ref_count, named_ref_count,
			 dotdot_of, named_parent_of);       /* checks #4, #10    */
	check_inodes_referenced(sb, inode_in_use,
				named_ref_count);                 /* check #9          */
	check_ref_counts(img, sb, named_ref_count);         /* checks #11, #12   */
	check_parent_mismatch(img, sb, inode_in_use,
			      dotdot_of, named_parent_of);  /* EC check #1       */
	check_parent_chains(img, sb, inode_in_use,
			    dotdot_of, named_ref_count);    /* EC check #2 parent chain */
	check_dir_reachable(img, sb, inode_in_use,
			    named_ref_count);               /* EC check #2       */

	/* ---- repair pass (only when -r is given) ---- */
	if (repair) {
		repair_lost_found(img, sb, inode_in_use,
				  ref_count, named_ref_count);  /* EC repair #3/#4 */
		repair_nlinks(img, sb, named_ref_count);        /* EC repair #5    */

		/* re-run checks after repair */
		memset(blks_used, 0, sb->size * sizeof(uchar));
		memset(direct_seen, 0, sb->size * sizeof(uchar));
		memset(indirect_seen, 0, sb->size * sizeof(uchar));
		memset(inode_in_use, 0, sb->ninodes * sizeof(uchar));
		memset(ref_count, 0, sb->ninodes * sizeof(uint));
		memset(named_ref_count, 0, sb->ninodes * sizeof(uint));
		memset(dotdot_of, 0, sb->ninodes * sizeof(uint));
		memset(named_parent_of, 0, sb->ninodes * sizeof(uint));

		scan_inodes(img, sb, blks_used,
			    direct_seen, indirect_seen,
			    inode_in_use);
		check_bitmap(img, sb, blks_used);
		scan_directories(img, sb, inode_in_use,
				 ref_count, named_ref_count,
				 dotdot_of, named_parent_of);
		check_inodes_referenced(sb, inode_in_use, named_ref_count);
		check_ref_counts(img, sb, named_ref_count);
		check_parent_mismatch(img, sb, inode_in_use,
				      dotdot_of, named_parent_of);
		check_parent_chains(img, sb, inode_in_use,
				    dotdot_of, named_ref_count);
		check_dir_reachable(img, sb, inode_in_use,
				    named_ref_count);
	}

	free(blks_used);
	free(direct_seen);
	free(indirect_seen);
	free(inode_in_use);
	free(ref_count);
	free(named_ref_count);
	free(dotdot_of);
	free(named_parent_of);
	munmap(img, imgsize);
	return 0;
}
