/*-
 * Copyright (c) 2011 Ganael LAPLANCHE <ganael.laplanche@martymac.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if defined(WITH_FILE_MEMORY)

#include "utils.h"
#include "file_memory.h"

/* mmap(2) */
#include <sys/mman.h>

/* open(2) */
#include <fcntl.h>

/* strerror(3) */
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* lseek(2) */
#include <unistd.h>

/* assert */
#include <assert.h>

/* Describes a file memory chunk */
struct file_memory;
struct file_memory {
    char *path;                     /* file name */
    int fd;                         /* file descriptor */

    size_t size;                    /* mmap size */
    void *start_addr;               /* mmap addr */
    size_t next_free_offset;        /* next free area offset within map */
    fnum_t ref_count;               /* reference counter */

    struct file_memory* nextp;      /* next file_entry */
    struct file_memory* prevp;      /* previous one */
};

/* Describes a malloc entry within a file_memory chunk */
struct file_malloc {
    struct file_memory* parentp;    /* pointer to parent file_memory */
    unsigned char data[1];          /* data starts here */
};
/* XXX keep this function up-to-date with file_malloc structure definition */
#define FILE_MALLOC_HEADER_SIZE \
    (sizeof(struct file_memory*))

/* Our main memory state descriptor */
static struct mem_manager {
    char *base_path;                /* base path name for each file memory */

    struct file_memory *currentp;   /* pointer to current (last) file_memory allocated */
    fnum_t next_chunk_index;        /* next file memory entry index */
    fnum_t max_chunks;              /* maximum number of file memory entries allowed (0 == illimited) */
} mem;

/* Add a file memory entry to a double-linked list of file_memories
   - if head is NULL, creates a new file memory entry ; if not, chains a new
     entry to it
   - returns with head set to the newly added element */
static int
add_file_memory(struct file_memory **head, char *path, size_t size)
{
    assert(head != NULL);
    assert(path != NULL);
    assert(size > 0);

    struct file_memory **current = head; /* current file_memory pointer address */
    struct file_memory *previous = NULL; /* previous file_memory pointer */

    /* backup current structure pointer and initialize a new structure */
    previous = *current;
    if((*current = malloc(sizeof(struct file_memory))) == NULL) {
        fprintf(stderr, "%s(): cannot allocate memory\n", __func__);
        return (1);
    }

    /* set head on first call */
    if(*head == NULL)
        *head = *current;

    /* set path */
    size_t malloc_size = strlen(path) + 1;
    if(((*current)->path = malloc(malloc_size)) == NULL) {
        fprintf(stderr, "%s(): cannot allocate memory\n", __func__);
        free(*current);
        *current = previous;
        return (1);
    }
    snprintf((*current)->path, malloc_size, "%s", path);

    /* open and create file, set fd */
    if (((*current)->fd = open((*current)->path, O_RDWR|O_CREAT|O_EXCL, 0660)) < 0) {
        fprintf(stderr, "%s: %s\n", (*current)->path, strerror(errno));
        free((*current)->path);
        free(*current);
        *current = previous;
        return (1);
    }
  
    /* set file size
       XXX fill file with zeroes to avoid fragmentation, see mmap(2) ? */
    (*current)->size = size;
    if(lseek((*current)->fd, (*current)->size - 1, SEEK_SET) != ((*current)->size - 1)) {
        fprintf(stderr, "%s: %s\n", (*current)->path, strerror(errno));
        close((*current)->fd);
        unlink((*current)->path);
        free((*current)->path);
        free(*current);
        *current = previous;
        return (1);
    }
    char zero = 0;
    if(write((*current)->fd, &zero, 1) != 1) {
        fprintf(stderr, "%s: %s\n", (*current)->path, strerror(errno));
        close((*current)->fd);
        unlink((*current)->path);
        free((*current)->path);
        free(*current);
        *current = previous;
        return (1);
    }

    /* mmap() our file */
    if(((*current)->start_addr =
        mmap(0, (*current)->size, PROT_READ|PROT_WRITE, MAP_SHARED, (*current)->fd, 0)) == MAP_FAILED) {
        fprintf(stderr, "%s(): cannot map memory\n", __func__);
        (*current)->start_addr = NULL;
        close((*current)->fd);
        unlink((*current)->path);
        free((*current)->path);
        free(*current);
        *current = previous;
        return (1);
    }

    /* initialize next free offset */
    (*current)->next_free_offset = 0;
    (*current)->ref_count = 0;

    /* set current pointers */
    (*current)->nextp = NULL;   /* set in next pass (see below) */
    (*current)->prevp = previous;

    /* set previous' nextp pointer */
    if(previous != NULL)
        previous->nextp = *current;

#if defined(DEBUG)
    fprintf(stderr, "%s(): memory file '%s' created (%zd bytes)\n", __func__, (*current)->path, (*current)->size);
    fprintf(stderr, "%s(): file mmaped @%p\n", __func__, (*current)->start_addr);
#endif

    return (0);
}

/* Un-initialize a double-linked list of file_memories */
static void
uninit_file_memories(struct file_memory *head)
{
    /* be sure to start from last entry */
    fastfw_list(head);

    struct file_memory *current = head;
    struct file_memory *prev = NULL;

    while(current != NULL) {
        if(current->start_addr != NULL)
            munmap(current->start_addr, current->size);
        if(current->fd >= 0)
            close(current->fd);
        if(current->path != NULL) {
            /* TODO : uncomment */
            unlink(current->path);
#if defined(DEBUG)
    fprintf(stderr, "%s(): memory file '%s' destroyed (%zd bytes)\n", __func__, current->path, current->size);
#endif
            free(current->path);
        }

        prev = current->prevp;
        free(current);
        current = prev;
    }
    return;
}

int
init_memory(char *base_path, fnum_t max_chunks)
{
    assert(base_path != NULL);

    /* set path */
    size_t malloc_size = strlen(base_path) + 1;
    if((mem.base_path = malloc(malloc_size)) == NULL) {
        fprintf(stderr, "%s(): cannot allocate memory\n", __func__);
        return (1);
    }
    snprintf(mem.base_path, malloc_size, "%s", base_path);

    mem.currentp = NULL;
    mem.next_chunk_index = 0;
    mem.max_chunks = max_chunks;

    return (0);
}

void
uninit_memory()
{
    /* un-initialize our file memory entries */
    uninit_file_memories(mem.currentp);

    /* clean our memory manager up */
    if(mem.base_path != NULL)
        free(mem.base_path);
    mem.currentp = NULL;
    mem.next_chunk_index = 0;
    mem.max_chunks = 0;

    return;
}

void *
file_malloc(size_t requested_size)
{
    /* invalid requested_size specified, reject call */
    if(requested_size == 0) {
        errno = EINVAL;
        return (NULL);
    }

    /* size aligned on pointer size */
    size_t aligned_size = round_num(requested_size + FILE_MALLOC_HEADER_SIZE, sizeof(void *));
    /* full size rounded on chunks' size */
    size_t chunked_size = round_num(aligned_size, FILE_MEMORY_CHUNK_SIZE);
    /* needed chunks to store data */
    size_t needed_chunks = chunked_size / FILE_MEMORY_CHUNK_SIZE;

#if defined(DEBUG)
    fprintf(stderr, "%s(): FILE_MEMORY_CHUNK_SIZE = %d\n", __func__, FILE_MEMORY_CHUNK_SIZE);
    fprintf(stderr, "%s(): FILE_MALLOC_HEADER_SIZE = %zd\n", __func__, FILE_MALLOC_HEADER_SIZE);
    fprintf(stderr, "%s(): requested_size = %zd\n", __func__, requested_size);
    fprintf(stderr, "%s(): aligned_size (including header) = %zd\n", __func__, aligned_size);
    fprintf(stderr, "%s(): chunked_size = %zd\n", __func__, chunked_size);
    fprintf(stderr, "%s(): needed_chunks = %zd\n", __func__, needed_chunks);
#endif

    /* if first call... */
    if((mem.currentp == NULL) ||
        /* ...or not enough space in current chunk */
        ((mem.currentp->next_free_offset + aligned_size) >
        (mem.currentp->size))) {

        /* allocate a new chunk, can we proceed ? */
        if ((mem.max_chunks > 0) && ((mem.next_chunk_index + needed_chunks) > (mem.max_chunks))) {
            fprintf(stderr, "%s(): cannot allocate memory\n", __func__);
            errno = ENOMEM;
            return (NULL);
        }

        /* compute tmp_path "base_path.i\0" */
        char *tmp_path = NULL;
        size_t malloc_size = strlen(mem.base_path) + 1 +
            get_num_digits(mem.next_chunk_index) + 1;
        if((tmp_path = malloc(malloc_size)) == NULL) {
            fprintf(stderr, "%s(): cannot allocate memory\n", __func__);
            errno = ENOMEM;
            return (NULL);
        }
        snprintf(tmp_path, malloc_size, "%s.%lld", mem.base_path, mem.next_chunk_index);

        /* add chunk */
        if(add_file_memory(&mem.currentp, tmp_path, needed_chunks * FILE_MEMORY_CHUNK_SIZE) != 0) {
            fprintf(stderr, "%s(): cannot allocate memory\n", __func__);
            free(tmp_path);
            errno = ENOMEM;
            return (NULL);
        }
        free(tmp_path);
        mem.next_chunk_index++;
    }

    /* current chunk OK */
    struct file_malloc *fmallocp =
        (struct file_malloc *)((char *)mem.currentp->start_addr + mem.currentp->next_free_offset);
    fmallocp->parentp = mem.currentp;

    /* update memory manager status */
    mem.currentp->next_free_offset += aligned_size;
    mem.currentp->ref_count++;

#if defined(DEBUG)
    fprintf(stderr, "%s(): %zd bytes requested\n", __func__, requested_size);
    fprintf(stderr, "%s(): %zd bytes allocated @%p in %zd chunk(s)\n", __func__, aligned_size, &(fmallocp->data[0]), needed_chunks);
#endif

    return (&(fmallocp->data[0]));
}

void
file_free(void *ptr)
{
/*
    XXX Not implemented yet :
    - Find file memory where ptr is allocated (use a pointer to parent file_memory)
    - Decrement a reference counter for this structure
      (file_malloc() should increment this counter)
    - Write a remove_file_memory() function that, for a given file memory :
      - Calls munmap()
      - Closes fd()
      - Unlinks file
      - If necessary, link previous and next file memory chunks
      (and call this function from uninit_file_memories())
    - If ref counter is 0 and either there is no byte left in the file memory or nextp is used
        - Call remove_file_memory() on file memory pointer
    - Update Memory Manager's currentp if nextp was NULL (last file memory was removed)
    - If last file_malloc removed, reuse-space by changing next_free_offset
*/

#if defined(DEBUG)
    fprintf(stderr, "%s(): memory free'ed @%p\n", __func__, ptr);
#endif

    return;
}

#endif /* WITH_FILE_MEMORY */
