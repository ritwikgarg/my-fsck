#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define stat stat_423     // avoid clash with host struct stat
#define dirent dirent_423 // avoid clash with host struct dirent
#include "types.h"

#include "fs.h"
#include "stat.h"
#undef stat
#undef dirent

typedef struct HardLinkMapNode {
    uint fsINode;
    uint hostINode;
    uint remainLink;
    struct HardLinkMapNode *next;
} HardLinkMapNode;

HardLinkMapNode *hardLinkMapRoot = NULL;

// Disk layout:
// [ boot block | sb block | inode blocks | free bit map | data blocks ]

#define FSSIZE 1024 // default size of file system in blocks
#define NINODES 200 // default maximum number of inodes

#define NINODEBLOCKS(ninodes)                                                  \
    (ninodes / IPB + 1) // maximum number of inodes block
#define BITBLOCKS(size)                                                        \
    (size / (BSIZE * 8) + 1) // maximum number of bitmap as block

int fsfd;
superblock sb;
char zeroes[BSIZE];
uint freeblock;
uint freeinode = 1; // start with 1
uint root_inode;

void balloc(int);
void wsect(uint, void *);
void winode(uint, dinode *);
void rinode(uint inum, dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);

uint fetchHardLink(uint hostINode, uint nlinks) {
    uint ret = 0;
    HardLinkMapNode *curr, *last;
    for (curr = hardLinkMapRoot, last = NULL; curr;
         last = curr, curr = curr->next) {
        if (curr->hostINode == hostINode) {
            ret = curr->fsINode;
            if (--curr->remainLink == 0) {
                if (last)
                    last->next = curr->next;
                else
                    hardLinkMapRoot = NULL;
                free(curr);
            }
            return ret;
        }
    }

    curr = malloc(sizeof(HardLinkMapNode));
    if (last)
        last->next = curr;
    else
        hardLinkMapRoot = curr;
    if (curr == NULL) {
        perror("malloc");
    }
    curr->hostINode = hostINode;
    curr->fsINode = ialloc(T_FILE);
    curr->remainLink = nlinks - 1;
    return curr->fsINode;
}

void freeHardLinkMap() {
    for (HardLinkMapNode *curr = hardLinkMapRoot; curr; curr = curr->next) {
        free(curr);
    }
    hardLinkMapRoot = NULL;
}

// convert to intel byte order
ushort xshort(ushort x) {
    ushort y;
    uchar *a = (uchar *)&y;
    a[0] = x;
    a[1] = x >> 8;
    return y;
}

uint xint(uint x) {
    uint y;
    uchar *a = (uchar *)&y;
    a[0] = x;
    a[1] = x >> 8;
    a[2] = x >> 16;
    a[3] = x >> 24;
    return y;
}

int mkfs(int ninodes, int size) {
    int i;
    char buf[BSIZE];
    uint bitblocks;
    int nblocks;

    bitblocks = BITBLOCKS(size);
    // 2: boot block + super block
    freeblock = 2 + NINODEBLOCKS(ninodes) + bitblocks;
    nblocks = size - freeblock;
    sb.size = xint(size);
    sb.nblocks = xint(nblocks); // so whole disk is size sectors
    sb.ninodes = xint(ninodes);

    printf("init: used %d (bit block %d + inode block %zu + 2) / %d blocks\n",
           freeblock, bitblocks, NINODEBLOCKS(ninodes), size);

    for (i = 0; i < size; i++)
        wsect(i, zeroes);

    memset(buf, 0, sizeof(buf));
    memmove(buf, &sb, sizeof(sb));
    wsect(1, buf);

    return 0;
}

int add_dir(DIR *cur_dir, int cur_inode, int parent_inode) {
    int r;
    int child_inode;
    int cur_fd, child_fd;
    dirent_423 de;
    dinode din;
    struct dirent *entry;
    struct stat st;
    int bytes_read;
    char buf[BSIZE];
    int off;

    bzero(&de, sizeof(de));
    de.inum = xshort(cur_inode);
    strcpy(de.name, ".");
    iappend(cur_inode, &de, sizeof(de));

    bzero(&de, sizeof(de));
    de.inum = xshort(parent_inode);
    strcpy(de.name, "..");

    if (cur_inode != parent_inode) {
        rinode(parent_inode, &din);
        ++din.nlink;
        winode(parent_inode, &din);
    }

    iappend(cur_inode, &de, sizeof(de));

    if (cur_dir == NULL) {
        return 0;
    }

    cur_fd = dirfd(cur_dir);
    if (cur_fd == -1) {
        perror("add_dir");
        exit(EXIT_FAILURE);
    }

    if (fchdir(cur_fd) != 0) {
        perror("add_dir");
        return -1;
    }

    while (true) {
        entry = readdir(cur_dir);

        // not quite proper error handling here, but oh well...
        if (entry == NULL)
            break;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        printf("%s\n", entry->d_name);

        child_fd = open(entry->d_name, O_RDONLY);
        if (child_fd == -1) {
            perror("open");
            return -1;
        }

        r = fstat(child_fd, &st);
        if (r != 0) {
            perror("stat");
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            child_inode = ialloc(T_DIR);
            r = add_dir(fdopendir(child_fd), child_inode, cur_inode);
            if (r != 0)
                return r;
            if (fchdir(cur_fd) != 0) {
                perror("chdir");
                return -1;
            }
        } else {
            bytes_read = 0;
            if (st.st_nlink > 0) {
                // printf("^ is hardlink\n");
                uint newINum = freeinode;
                child_inode = fetchHardLink(st.st_ino, st.st_nlink);
                if (child_inode != newINum) {
                    // no need to read
                    rinode(child_inode, &din);
                    ++din.nlink;
                    winode(child_inode, &din);

                    de.inum = xshort(child_inode);
                    strncpy(de.name, entry->d_name, DIRSIZ);
                    iappend(cur_inode, &de, sizeof(de));
                    continue;
                }
            } else {
                child_inode = ialloc(T_FILE);
            }
            bzero(&de, sizeof(de));
            while ((bytes_read = read(child_fd, buf, sizeof(buf))) > 0) {
                iappend(child_inode, buf, bytes_read);
            }
        }
        close(child_fd);

        de.inum = xshort(child_inode);
        strncpy(de.name, entry->d_name, DIRSIZ);
        iappend(cur_inode, &de, sizeof(de));
    }

    // fix size of inode cur_dir
    rinode(cur_inode, &din);
    off = xint(din.size);
    off = ((off / BSIZE) + 1) * BSIZE;
    din.size = xint(off);
    winode(cur_inode, &din);
    return 0;
}

int main(int argc, char *argv[]) {
    int r;
    DIR *root_dir;

    if (argc < 2) {
        fprintf(stderr, "Usage: mkfs fs.img [file_tree_root]\n");
        exit(1);
    }

    assert((BSIZE % sizeof(dinode)) == 0);
    assert((BSIZE % sizeof(dirent_423)) == 0);

    fsfd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fsfd < 0) {
        perror(argv[1]);
        exit(1);
    }

    mkfs(NINODES, FSSIZE);

    root_inode = ialloc(T_DIR);
    assert(root_inode == ROOTINO);

    if (argc == 3) {
        root_dir = opendir(argv[2]);
        r = add_dir(root_dir, root_inode, root_inode);
        if (r != 0) {
            exit(EXIT_FAILURE);
        }
    }

    balloc(freeblock);
    if (hardLinkMapRoot) {
        // printf("warning: nonempty hard link map");
        freeHardLinkMap();
    }

    printf("final: used %d / %d blocks\n", freeblock, FSSIZE);
    exit(0);
}

void wsect(uint sec, void *buf) {
    if (lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE) {
        perror("lseek");
        exit(1);
    }
    if (write(fsfd, buf, BSIZE) != BSIZE) {
        perror("write");
        exit(1);
    }
}

uint i2b(uint inum) { return (inum / IPB) + 2; }

void winode(uint inum, dinode *ip) {
    char buf[BSIZE];
    uint bn;
    dinode *dip;

    bn = i2b(inum);
    rsect(bn, buf);
    dip = ((dinode *)buf) + (inum % IPB);
    *dip = *ip;
    wsect(bn, buf);
}

void rinode(uint inum, dinode *ip) {
    char buf[BSIZE];
    uint bn;
    dinode *dip;

    bn = i2b(inum);
    rsect(bn, buf);
    dip = ((dinode *)buf) + (inum % IPB);
    *ip = *dip;
}

void rsect(uint sec, void *buf) {
    if (lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE) {
        perror("lseek");
        exit(1);
    }
    if (read(fsfd, buf, BSIZE) != BSIZE) {
        perror("read");
        exit(1);
    }
}

uint ialloc(ushort type) {
    uint inum = freeinode++;
    dinode din;

    bzero(&din, sizeof(din));
    din.type = xshort(type);
    // 2 + n
    din.nlink = xshort(type == T_DIR ? 2 : 1);
    din.size = xint(0);
    winode(inum, &din);
    return inum;
}

void balloc(int used) {
    uchar buf[BSIZE];
    int i;

    printf("balloc: first %d blocks have been allocated\n", used);
    assert(used < BSIZE * 8);
    bzero(buf, BSIZE);
    for (i = 0; i < used; i++) {
        buf[i / 8] = buf[i / 8] | (0x1 << (i % 8));
    }
    printf("balloc: write bitmap block at sector %zu\n",
           NINODEBLOCKS(NINODES) + 2);
    wsect(NINODEBLOCKS(NINODES) + 2, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void iappend(uint inum, void *xp, int n) {
    char *p = xp;
    uint fbn, off, n1;
    dinode din;
    char buf[BSIZE];
    uint indirect[NINDIRECT];
    uint x;

    rinode(inum, &din);

    off = xint(din.size);
    while (n > 0) {
        fbn = off / BSIZE;
        assert(fbn < MAXFILE);
        if (fbn < NDIRECT) {
            if (xint(din.addrs[fbn]) == 0) {
                din.addrs[fbn] = xint(freeblock++);
            }
            x = xint(din.addrs[fbn]);
        } else {
            if (xint(din.addrs[NDIRECT]) == 0) {
                // printf("allocate indirect block\n");
                din.addrs[NDIRECT] = xint(freeblock++);
            }
            // printf("read indirect block\n");
            rsect(xint(din.addrs[NDIRECT]), indirect);
            if (indirect[fbn - NDIRECT] == 0) {
                indirect[fbn - NDIRECT] = xint(freeblock++);
                wsect(xint(din.addrs[NDIRECT]), indirect);
            }
            x = xint(indirect[fbn - NDIRECT]);
        }
        n1 = min(n, (fbn + 1) * BSIZE - off);
        rsect(x, buf);
        bcopy(p, buf + off - (fbn * BSIZE), n1);
        wsect(x, buf);
        n -= n1;
        off += n1;
        p += n1;
    }
    din.size = xint(off);
    winode(inum, &din);
}