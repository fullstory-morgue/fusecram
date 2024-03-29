//
// C Implementation: cramfs
//
// Description: 
//
//
// Author: Dmitry Morozhnikov <dmiceman@mail.ru>, (C) 2005
// Auto-endianess conversion by Renzo Davoli <renzo@cs.unibo.it> (C) 2005
//
// Copyright: Released under the GPLv2. 
// See COPYING file that comes with this distribution
//
//

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <glib.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <zlib.h>
#include <dirent.h>
#include <pthread.h>
#include <byteswap.h>

#include "cramfs.h"

static cramfs_context context;
static cramfs_inode_context *root_context;
static GHashTable *lookup_table;
static GHashTable *negative_lookup_table;
static char *negative_value = "not exists";
static int last_node;
static pthread_mutex_t main_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;
#define NOSWAP 0
#define BE2LE 1
#define LE2BE 2

static int byteswap;

#define SW32(X) ((byteswap)?bswap_32(X):(X))
#define SW16(X) ((byteswap)?bswap_16(X):(X))
#define SWSIZE(X) ((byteswap)?(bswap_32(X<<8)):(X))
#define SWNAMELEN(PX) ((byteswap)? \
		((byteswap==BE2LE)?(bswap_32(((u32*)(PX))[2]) >> 26):(bswap_32(((u32*)(PX))[2]) & 0x3F))\
		:(PX)->namelen)
#define SWOFFSET(PX) ((byteswap)? \
		((byteswap==BE2LE)?(bswap_32(((u32*)(PX))[2] & 0xffffff03)):(bswap_32(((u32*)(PX))[2]) >> 6))\
		:(PX)->offset)

void *cramfs_real_init(char *imagefile) {
//     pthread_mutex_init(&fd_mutex, NULL);
    int fd = open(imagefile, O_RDONLY);
    if(fd == -1) {
        perror("open image file");
        exit(EINVAL);
    };
    context.imagefile = imagefile;
    context.fd = fd;
    ssize_t size = read(fd, & context.super, sizeof(context.super));
    if(size != sizeof(context.super)) {
        fprintf(stderr, "only %d bytes read from superblock, %d required\n", size, sizeof(context.super));
        exit(EIO);
    };
    
    // filesystem check code directly copied from linux kernel
    // thanks to Linus Torvalds, original author!
    if(context.super.magic != CRAMFS_MAGIC &&
				context.super.magic != bswap_32(CRAMFS_MAGIC)) {
        /* check at 512 byte offset */
        lseek(fd, 512, SEEK_SET);
        ssize_t size = read(fd, & context.super, sizeof(context.super));
        if(size != sizeof(context.super)) {
            fprintf(stderr, "only %d bytes read from [possible] shifted superblock, %d required\n", 
                size, sizeof(context.super));
            exit(EIO);
        };
        if(context.super.magic != CRAMFS_MAGIC &&
						bswap_32(context.super.magic) != CRAMFS_MAGIC) {
            fprintf(stderr, "wrong magic! is it really cramfs image file?\n");
            exit(EINVAL);
        };
    };

		if (bswap_32(context.super.magic) == CRAMFS_MAGIC) {
			if (*((char *)(&(context.super.magic))) != CRAMFS_MAGIC >> 24) {
				fprintf(stderr, "Swap endianess: reading LE file system on a BE machine\n");
				byteswap=LE2BE;
			} else {
				fprintf(stderr, "Swap endianess: reading BE file system on a LE machine\n");
				byteswap=BE2LE;
			}
		}

    /* get feature flags first */
    if(SW32(context.super.flags) & ~CRAMFS_SUPPORTED_FLAGS) {
        fprintf(stderr, "unsupported filesystem features, sorry :-(\n");
        exit(EINVAL);
    };

    /* Check that the root inode is in a sane state */
    if(!S_ISDIR(SW16(context.super.root.mode))) {
        fprintf(stderr, "init: root is not a directory\n");
        exit(EINVAL);
    };
    unsigned long root_offset = SWOFFSET(&(context.super.root)) << 2;
    if (!(SW32(context.super.flags) & CRAMFS_FLAG_FSID_VERSION_2)) {
        // nothing to do with this?
    };
    if(root_offset == 0) {
        fprintf(stderr, "warning: empty filesystem\n");
    } else if(!(SW32(context.super.flags) & CRAMFS_FLAG_SHIFTED_ROOT_OFFSET) &&
         ((root_offset != sizeof(struct cramfs_super)) &&
          (root_offset != 512 + sizeof(struct cramfs_super))))
    {
        fprintf(stderr, "init: bad root offset %lu\n", root_offset);
        exit(EINVAL);
    };
    
    lookup_table = g_hash_table_new(g_str_hash, g_str_equal);
    negative_lookup_table = g_hash_table_new(g_str_hash, g_str_equal);
    root_context = (cramfs_inode_context *) malloc(sizeof(cramfs_inode_context));
    root_context->inode = & context.super.root;
    root_context->ino = 1;
    last_node = 2;
    g_hash_table_insert(lookup_table, "/", root_context);
    return &context;
};

cramfs_inode_context *cramfs_lookup(const char *path) {
    if(path[0] == '\0') {
        return NULL;
    };
    cramfs_inode_context *inode = g_hash_table_lookup(lookup_table, path);
    if(inode) {
        return inode;
    };
    if(g_hash_table_lookup(negative_lookup_table, path)) {
        return NULL;
    };
//     printf("start search for %s\n", path);
    gchar **parts = g_strsplit(path, "/", -1);
    guint parts_len = g_strv_length(parts);
    int partno = 1;
    gchar *rpath = g_strdup("/");
    gchar *rpath1 = "";
    gchar *part = parts[partno];
    while(part && partno < parts_len) {
        rpath1 = g_strconcat(rpath1, "/", part, NULL);
//         printf("looking for %s in %s...\n", rpath1, rpath);
        inode = g_hash_table_lookup(lookup_table, rpath1);
        if(!inode) {
//             printf("trying to load %s...\n", rpath);
            int rc = cramfs_real_readdir(rpath, NULL, NULL);
            if(rc) {
                fprintf(stderr, "lookup: error %d from readdir: %s\n", rc, strerror(-rc));
                g_strfreev(parts);
                g_free(rpath);
                return NULL;
            };
        };
        partno++;
        part = parts[partno];
        g_free(rpath);
        rpath = rpath1;
    };
    g_strfreev(parts);
    g_free(rpath);
    inode = g_hash_table_lookup(lookup_table, path);
    if(!inode) {
        g_hash_table_insert(negative_lookup_table, g_strdup(path), negative_value);
    };
    return inode;
};

int cramfs_real_opendir(const char *path) {
    cramfs_inode_context *current = cramfs_lookup(path);
    if(!current) {
        fprintf(stderr, "opendir: know nothing about %s\n", path);
        return -ENOENT;
    };
    if(!S_ISDIR(SW16(current->inode->mode))) {
        fprintf(stderr, "opendir: %s not a dir\n", path);
        return -ENOTDIR;
    };
    return 0;
};

int cramfs_real_readdir(const char *path, void *buf, cramfs_dir_fill_t filler) {
//     printf("readdir: entering with %s\n", path);
    if(path[0] == '\0') {
        fprintf(stderr, "readdir: attempt to read empty path name\n");
        return -EINVAL;
    };
    cramfs_inode_context *current = cramfs_lookup(path);
    if(!current) {
        fprintf(stderr, "readdir: know nothing about %s\n", path);
        return -ENOENT;
    };
    if(!S_ISDIR(SW16(current->inode->mode))) {
        fprintf(stderr, "readdir: %s not a dir\n", path);
        return -ENOTDIR;
    };
    if(filler) {
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);
    };
//     printf("%s, offset %d, size %d\n", path, current->offset * 4, current->size);
    off_t ioff = SWOFFSET(current->inode) * 4;
    unsigned char *ibuf = (unsigned char *) malloc(sizeof(struct cramfs_inode) + CRAMFS_MAXPATHLEN + 1);
    size_t dsize = SWSIZE(current->inode->size);
    int count = 0;
    while(dsize > 0) {
        if(pthread_mutex_lock(& fd_mutex)) {
            int err = errno;
            pthread_mutex_unlock(& main_mutex);
            perror("readdir: can`l lock fd_mutex");
            return -err;
        };
        if(lseek(context.fd, ioff, SEEK_SET) == -1) {
            perror("readdir: can`t lseek()");
            pthread_mutex_unlock(& fd_mutex);
            return -EIO;
        };
        if(read(context.fd, ibuf, sizeof(struct cramfs_inode)) != sizeof(struct cramfs_inode)) {
            fprintf(stderr, "readdir: can`t read full inode, errno %d, message %s\n", 
                errno, strerror(errno));
            pthread_mutex_unlock(& fd_mutex);
            return -EIO;
        };
        char *entry = (char *) malloc(CRAMFS_MAXPATHLEN + 1);
        memset(entry, 0, CRAMFS_MAXPATHLEN + 1);
        struct cramfs_inode *inode = (struct cramfs_inode *) malloc(sizeof(struct cramfs_inode));
        memcpy(inode, ibuf, sizeof(struct cramfs_inode));
        int namelen = SWNAMELEN(inode);
        if(namelen * 4 > CRAMFS_MAXPATHLEN) {
            pthread_mutex_unlock(& fd_mutex);
            fprintf(stderr, "readdir: too long filename found! namelen %d\n", 
                namelen);
            return -EIO;
        };
        if(read(context.fd, entry, namelen * 4) != namelen * 4) {
            fprintf(stderr, "readdir: can`t read full direntry, errno %d, message %s\n", 
                errno, strerror(errno));
            pthread_mutex_unlock(& fd_mutex);
            return -EIO;
        };
        pthread_mutex_unlock(& fd_mutex);
        entry[namelen * 4] = '\0';
/*        printf("%s: size %d, mode %d namelen %d, offset %d\n", 
            entry, SWSIZE(inode->size), SW16(inode->mode), namelen * 4, SWOFFSET(inode));*/
        int esize = sizeof(struct cramfs_inode) + namelen * 4;
        ioff += esize;
        dsize -= esize;
        count++;
        if(entry[0] == '\0') {
            fprintf(stderr, "readdir: empty size name found! namelen %d, dsize %d, ioff %d\n", 
                namelen, dsize, (int) ioff);
            return 0;
        };
        if(filler) {
            int filler_rc = filler(buf, entry, NULL, 0);
            if(filler_rc) {
                return filler_rc;
            };
        };
        char absolute_entry[PATH_MAX];
        strcpy(absolute_entry, path);
        if(path[1] != '\0') { // not root dir
            strcat(absolute_entry, "/");
        };
        strcat(absolute_entry, entry);
        if(g_hash_table_lookup(lookup_table, absolute_entry)) {
            // already in lookup cache
            free(inode);
        } else {
            cramfs_inode_context *node_context = (cramfs_inode_context *) malloc(sizeof(cramfs_inode_context));
            node_context->ino = last_node;
            last_node++;
            node_context->inode = inode;
            g_hash_table_insert(lookup_table, g_strdup(absolute_entry), node_context);
        };
        free(entry);
    };
    free(ibuf);
//     printf("readdir: exiting from %s, count %d\n", path, count);
    return 0;
};

int cramfs_real_getattr(const char *path, struct stat *stbuf) {
    cramfs_inode_context *context = cramfs_lookup(path);
    if(!context) {
        // this is too common to report each one
        // fprintf(stderr, "getattr: know nothing about %s\n", path);
        // fprintf(stderr, "getattr: know nothing about %s\n", path);
        return -ENOENT;
    } else {
//         printf("getattr: found %s, size %d\n", path, inode->size);
    };
    memset(stbuf, 0, sizeof(struct stat));
    /// @todo may be it is not to follow kernel cramfs and set ino to value of file offset?
//     stbuf->st_ino = context->ino;
    stbuf->st_mode = SW16(context->inode->mode);
    /// @todo may be set it to current user uid/gid?
    stbuf->st_uid = SW16(context->inode->uid);
    stbuf->st_gid = context->inode->gid;
    stbuf->st_size = SWSIZE(context->inode->size);
    stbuf->st_blksize = PAGE_CACHE_SIZE;
    stbuf->st_blocks = (stbuf->st_size  - 1) / PAGE_CACHE_SIZE + 1;
    stbuf->st_nlink = 1;
    /// @todo may be set atime/mtime/ctime to current time?
//     stbuf->st_atime = time(NULL);
//     stbuf->st_mtime = time(NULL);
//     stbuf->st_ctime = time(NULL);
    return 0;
};

int cramfs_read_block(off_t offset, size_t bsize, char *data, size_t *size) {
    char ibuf[PAGE_CACHE_SIZE * 2];
    if(pthread_mutex_lock(& fd_mutex)) {
        int err = errno;
        perror("read_block: can`l lock fd_mutex");
        return -err;
    };
    if(lseek(context.fd, offset, SEEK_SET) == -1) {
        perror("read_block: can`t lseek()");
        pthread_mutex_unlock(& fd_mutex);
        return -EIO;
    };
    if(read(context.fd, ibuf, bsize) != bsize) {
        fprintf(stderr, "readdir: can`t read full block, errno %d, message %s\n", 
            errno, strerror(errno));
        pthread_mutex_unlock(& fd_mutex);
        return -EIO;
    };
    pthread_mutex_unlock(& fd_mutex);
    int rc = uncompress(data, (uLongf *) size, ibuf, bsize);
//     printf("read_block: offset %d, bsize %d, size %d, rc %d\n", (int) offset, bsize, *size, rc);
    return rc;
};

int cramfs_real_readlink(const char *path, char *target, size_t size) {
    cramfs_inode_context *ncontext = cramfs_lookup(path);
    if(!ncontext) {
        fprintf(stderr, "readlink: know nothing about %s\n", path);
        return -ENOENT;
    };
    struct cramfs_inode *inode = ncontext->inode;
    if(!S_ISLNK(SW16(inode->mode))) {
        fprintf(stderr, "readlink: %s not a link\n", path);
        return -EINVAL;
    };
    size_t fsize = SWSIZE(inode->size);
    char *obuf = (char *) malloc(PAGE_CACHE_SIZE * 2);
    int nblocks = (fsize - 1) / PAGE_CACHE_SIZE + 1;
    int *bbuf = (int *) malloc(nblocks * 4);
    if(pthread_mutex_lock(& fd_mutex)) {
        int err = errno;
        perror("readlink: can`l lock fd_mutex");
        return -err;
    };
    if(lseek(context.fd, SWOFFSET(inode) * 4, SEEK_SET) == -1) {
        perror("read_block: can`t lseek()");
        pthread_mutex_unlock(& fd_mutex);
        return -EIO;
    };
    if(read(context.fd, bbuf, nblocks * 4) != nblocks * 4) {
        fprintf(stderr, "readdir: can`t read full block table, errno %d, message %s\n", 
            errno, strerror(errno));
        pthread_mutex_unlock(& fd_mutex);
        return -EIO;
    };
    pthread_mutex_unlock(& fd_mutex);
    int i;
    off_t offset = SWOFFSET(inode) * 4 + nblocks * 4;
    off_t ooff = 0;
    for(i = 0; i < nblocks; i++) {
        int block = SW32(bbuf[i]);
        size_t bsize = block - offset;
        if(bsize > PAGE_CACHE_SIZE * 2) {
            free(bbuf);
            free(obuf);
            fprintf(stderr, "read: block size bigger than PAGE_CACHE_SIZE * 2 while reading block %i from symlink %s, bsize %i, offset %i, block %i\n", 
                i, path, bsize, (int) offset, block);
            return -EIO;
        };
        size_t osize = PAGE_CACHE_SIZE;
        int rc = cramfs_read_block(offset, bsize, obuf + ooff, & osize);
        if(rc != Z_OK) {
            free(bbuf);
            free(obuf);
            fprintf(stderr, "readlink: read block error %i: %s\n", rc, strerror(rc));
            return -rc;
        };
        offset = block;
        ooff += osize;
        obuf[ooff] = '\0';
    };
    strncpy(target, obuf, size - 1);
    target[size - 1] = '\0';
//     printf("readlink: %s -> %s\n", path, target);
    free(bbuf);
    free(obuf);
    return 0;
};

int cramfs_real_open(const char *path) {
    cramfs_inode_context *ncontext = cramfs_lookup(path);
    if(!ncontext) {
        fprintf(stderr, "read: know nothing about %s\n", path);
        return -ENOENT;
    };
    struct cramfs_inode *inode = ncontext->inode;
    if(!S_ISREG(SW16(inode->mode))) {
        fprintf(stderr, "read: %s not a file\n", path);
        return -EINVAL;
    };
    return 0;
};

int cramfs_real_read(const char *path, char *buf, size_t size, off_t offset) {
    cramfs_inode_context *ncontext = cramfs_lookup(path);
    if(!ncontext) {
        fprintf(stderr, "read: know nothing about %s\n", path);
        return -ENOENT;
    };
    struct cramfs_inode *inode = ncontext->inode;
    if(!S_ISREG(SW16(inode->mode))) {
        fprintf(stderr, "read: %s not a file\n", path);
        return -EINVAL;
    };
    size_t fsize = SWSIZE(inode->size);
    int start = offset / PAGE_CACHE_SIZE;
    int end = (offset + size) / PAGE_CACHE_SIZE;
    char *obuf = (char *) malloc((end - start + 1) * PAGE_CACHE_SIZE);
    int nblocks = (fsize - 1) / PAGE_CACHE_SIZE + 1;
    int *bbuf = (int *) malloc(nblocks * 4);
    if(pthread_mutex_lock(& fd_mutex)) {
        int err = errno;
        perror("read: can`l lock fd_mutex");
        return -err;
    };
    if(lseek(context.fd, SWOFFSET(inode) * 4, SEEK_SET) == -1) {
        perror("read_block: can`t lseek()");
        pthread_mutex_unlock(& fd_mutex);
        return -EIO;
    };
    if(read(context.fd, bbuf, nblocks * 4) != nblocks * 4) {
        fprintf(stderr, "read: can`t read full block table, errno %d, message %s\n", 
            errno, strerror(errno));
        pthread_mutex_unlock(& fd_mutex);
        return -EIO;
    };
    pthread_mutex_unlock(& fd_mutex);
    int i;
    off_t foffset = SWOFFSET(inode) * 4 + nblocks * 4;
    off_t ooff = 0;
    size_t osize;
    size_t real_size = 0;
//     printf("read: reading blocks from %d to %d, shift %d, fsize %d, nblocks %d\n", 
//         start, end, (int) (offset % PAGE_CACHE_SIZE), fsize, nblocks);
    for(i = start; i < end; i++) {
        int block = SW32(bbuf[i]);
        int boffset = (i ? SW32(bbuf[i - 1]) : (int) foffset);
        size_t bsize = block - boffset;
        if(bsize > PAGE_CACHE_SIZE * 2) {
            free(bbuf);
            free(obuf);
            fprintf(stderr, "read: block size bigger than PAGE_CACHE_SIZE * 2 while reading block %i from %s, bsize %i, foffset %i, block %i\n", 
                i, path, bsize, (int) foffset, block);
            return -EIO;
        };
        osize = PAGE_CACHE_SIZE;
        if(!bsize) {
            // hole
        } else {
            int rc = cramfs_read_block(boffset, bsize, obuf + ooff, &osize);
            if(rc != Z_OK) {
                free(bbuf);
                free(obuf);
                fprintf(stderr, "read: read block error %i: %s\n", rc, strerror(rc));
                return -rc;
            };
        };
        ooff += osize;
        real_size += osize;
    };
    if(real_size < size) {
        size = real_size;
    };
    memcpy(buf, obuf + (offset % PAGE_CACHE_SIZE), size);
//     fwrite(buf, size, 1, stdout);
    free(bbuf);
    free(obuf);
    return size;
};

int cramfs_real_statfs(struct statfs *stbuf) {
    stbuf->f_type = CRAMFS_MAGIC;
    stbuf->f_bsize = PAGE_CACHE_SIZE;
    stbuf->f_blocks = SW32(context.super.fsid.blocks);
    stbuf->f_bfree = 0;
    stbuf->f_bavail = 0;
    stbuf->f_files = SW32(context.super.fsid.files);
    stbuf->f_ffree = 0;
    stbuf->f_namelen = CRAMFS_MAXPATHLEN;
    return 0;
};
