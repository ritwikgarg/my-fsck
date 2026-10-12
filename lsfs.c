#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Avoid clashing with POSIX struct dirent from <dirent.h> / <stat.h>.
#define dirent dirent_423
#define stat stat_423
#include "fs.h"
#include "types.h"

#include "stat.h"
#undef dirent
#undef stat

// Return a pointer to block number bn in the image.
static inline void *blkptr(void *img, uint bn) {
    return img + (size_t)bn * BSIZE;
}

// Walk a single directory block
static void walk_dirblock(void *img, dinode *inodes, uint blkno,
                          const char *path);

// Recursively list directory inode inum
static void ls_dir(void *img, dinode *inodes, uint inum, const char *path) {
    dinode *din = inodes + inum;

    if (din->type != T_DIR)
        return;

    printf("\n- %s/ [nlink=%d]\n", path[0] ? path : "", din->nlink);

    // Direct blocks
    for (int i = 0; i < NDIRECT; i++) {
        if (din->addrs[i] == 0)
            continue;
        walk_dirblock(img, inodes, din->addrs[i], path);
    }

    // Indirect block
    if (din->addrs[NDIRECT] != 0) {
        uint *indirect = blkptr(img, din->addrs[NDIRECT]);
        for (int i = 0; i < NINDIRECT; i++) {
            if (indirect[i] == 0)
                continue;
            walk_dirblock(img, inodes, indirect[i], path);
        }
    }
}

static void walk_dirblock(void *img, dinode *inodes, uint blkno,
                          const char *path) {
    dirent_423 *dir_entry = (dirent_423 *)blkptr(img, blkno);
    int n_entries = BSIZE / sizeof(dirent_423);

    for (int k = 0; k < n_entries; k++, dir_entry++) {
        if (dir_entry->inum == 0)
            continue;
        // Skip diving into . and ..
        if (strncmp(dir_entry->name, ".", DIRSIZ) == 0 ||
            strncmp(dir_entry->name, "..", DIRSIZ) == 0)
            continue;

        dinode *child = inodes + dir_entry->inum;

        // buffers
        char childpath[4096];
        char name[DIRSIZ + 1];

        strncpy(name, dir_entry->name, DIRSIZ);
        name[DIRSIZ] = '\0';

        if (path[0])
            snprintf(childpath, sizeof(childpath), "%s/%s", path, name);
        else
            snprintf(childpath, sizeof(childpath), "/%s", name);

        switch (child->type) {
        case T_DIR:
            ls_dir(img, inodes, dir_entry->inum, childpath);
            break;
        case T_FILE:
            printf("%s  [file, %u bytes, nlink=%d]\n", childpath, child->size,
                   child->nlink);
            break;
        case T_DEV:
            printf("%s  [device %d:%d]\n", childpath, child->major,
                   child->minor);
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: lsfs <fs.img>\n");
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return 1;
    }

    // load image
    void *img = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (img == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    close(fd);

    superblock *sb = img + BSIZE; // block 1
    printf("= %s\n", argv[1]);
    printf("size=%u blocks  nblocks=%u  ninodes=%u\n\n", sb->size, sb->nblocks,
           sb->ninodes);

    // directory entries start at block 2
    ls_dir(img, img + 2 * BSIZE, ROOTINO, "");

    munmap(img, st.st_size);
    return 0;
}
