#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdlib.h>

static inline void superblock_error() {
    fprintf(stderr, "MP4_ERR: superblock is corrupted.\n");
}

static inline void bad_inode_error() {
    fprintf(stderr, "MP4_ERR: bad inode.\n");
}

static inline void bad_directory_format_error() {
    fprintf(stderr, "MP4_ERR: directory not properly formatted.\n");
}

static inline void bad_direct_address_error() {
    fprintf(stderr, "MP4_ERR: bad direct address in inode.\n");
}

static inline void bad_direct_address_once_error() {
    fprintf(stderr, "MP4_ERR: direct address used more than once.\n");
}

static inline void bad_inode_referred_marked_error() {
    fprintf(stderr,
            "MP4_ERR: inode referred to in directory but marked free.\n");
}

static inline void bad_indirect_address_error() {
    fprintf(stderr, "MP4_ERR: bad indirect address in inode.\n");
}

static inline void bad_indirect_address_once_error() {
    fprintf(stderr, "MP4_ERR: indirect address used more than once.\n");
}

static inline void bad_file_size_error() {
    fprintf(stderr, "MP4_ERR: incorrect file size in inode.\n");
}

static inline void bad_parent_directory_error() {
    fprintf(stderr, "MP4_ERR: parent directory mismatch.\n");
}

static inline void bad_directory_error() {
    fprintf(stderr, "MP4_ERR: inaccessible directory.\n");
}

static inline void bad_directory_once_error() {
    fprintf(stderr,
            "MP4_ERR: directory appears more than once in file system.\n");
}

static inline void bad_reference_count_error() {
    fprintf(stderr, "MP4_ERR: bad reference count for file.\n");
}

static inline void bad_bitmap_marked_used_error() {
    fprintf(stderr,
            "MP4_ERR: bitmap marks block in use but it is not in use.\n");
}

static inline void bad_used_inode_not_found_error() {
    fprintf(stderr,
            "MP4_ERR: inode marked used but not found in a directory.\n");
}

static inline void bad_used_address_error() {
    fprintf(stderr,
            "MP4_ERR: address used by inode but marked free in bitmap.\n");
}

#endif // LOG_H