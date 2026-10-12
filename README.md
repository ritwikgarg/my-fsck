Name: Ritwik Garg

NetID: ritwikg3

## Overview

`myfsck` is a file-system consistency checker for the CS 423 MP4 filesystem format (similar to VSFS). It memory-maps the image with `mmap()`, runs all checks in a single pass over inodes and directories, and exits with code 1 on the first error detected. With the `-r` flag it additionally repairs orphaned inodes (placing them in `/lost_found/`) and corrects incorrect reference counts.

## Usage

```
./myfsck <file_system_image>        # check only
./myfsck -r <file_system_image>     # check + repair 
```

## Checks implemented

| # | Description | Error message |
|---|---|---|
| 1 | Superblock size covers all regions | `superblock is corrupted.` |
| 2 | Inode type is 0, T_FILE, T_DIR, or T_DEV | `bad inode.` |
| 3 | Direct/indirect block addresses are in the data region | `bad direct/indirect address in inode.` |
| 4 | Every directory has valid `.` (self) and `..` entries | `directory not properly formatted.` |
| 5 | Every block used by an inode is marked used in the bitmap | `address used by inode but marked free in bitmap.` |
| 6 | Every bitmap-marked block is actually used by an inode | `bitmap marks block in use but it is not in use.` |
| 7 | No direct block is shared across two inodes | `direct address used more than once.` |
| 8 | File size is consistent with the number of allocated blocks | `incorrect file size in inode.` |
| 9 | Every in-use inode appears in at least one directory entry | `inode marked used but not found in a directory.` |
| 10 | Every directory entry references an in-use inode | `inode referred to in directory but marked free.` |
| 11 | Regular-file nlink equals the number of directory entries pointing to it | `bad reference count for file.` |
| 12 | Each directory appears as a named entry in exactly one parent | `directory appears more than once in file system.` |
| EC1 | `..` entry matches the actual naming parent | `parent directory mismatch.` |
| EC2 | Every directory is reachable from root via BFS | `inaccessible directory.` |

## Repairs (`-r`)

- **Orphan recovery (EC #3/4):** creates `/lost_found/` under root and adds each unreferenced inode as `#<inum>`. Orphaned directories have their `..` entry rewritten to point at `lost_found` so EC check #1 remains consistent.
- **nlink fix (EC #5):** overwrites each regular file's `nlink` with the actual directory-reference count.

## Design notes

- The entire image is accessed through a single `mmap()` — no intermediate buffers. Writes in repair mode go directly back to disk via `MAP_SHARED`.
- All tracking arrays (`blks_used`, `direct_seen`, `inode_in_use`, `ref_count`, `named_ref_count`, `dotdot_of`, `named_parent_of`) are `calloc`'d after the superblock is validated so their sizes are correct.
- Checks run strictly in spec order (1 → 12 → EC1 → EC2), exiting on the first error.

## Testing

Thirteen purpose-built corrupted images (one per check, plus two for EC #1 and #2) are included in the repo. The repair tests copy a repairable image to `/tmp`, run `myfsck -r`, then re-run `myfsck` and verify exit 0.

## Some reminders

- Don't print to stderr except error reporting.
- Return 0 if no error, and 1 if any error detected.
