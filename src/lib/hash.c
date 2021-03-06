/*
 * Based on libkmod-hash.c from libkmod - interface to kernel module operations
 * Copyright (C) 2011-2012  ProFUSION embedded systems
 * Copyright (C) 2013 Leandro Pereira <leandro@hardinfo.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "hash.h"
#include "murmur3.h"

struct hash_entry {
    const char *key;
    const void *value;
    unsigned hashval;
};

struct hash_bucket {
    struct hash_entry *entries;
    unsigned used;
    unsigned total;
};

struct hash {
    unsigned count;
    unsigned (*hash_value)(const void *key);
    int (*key_compare)(const void *k1, const void *k2);
    void (*free_value)(void *value);
    void (*free_key)(void *value);
    struct hash_bucket buckets[];
};

#define N_BUCKETS 512
#define STEPS 64
#define DEFAULT_ODD_CONSTANT 0x27d4eb2d

static inline unsigned int hash_int_shift_mult(const void *keyptr);

static unsigned int odd_constant = DEFAULT_ODD_CONSTANT;
static unsigned (*hash_str)(const void *key) = murmur3_simple;
static unsigned (*hash_int)(const void *key) = hash_int_shift_mult;

static unsigned int get_random_unsigned(void)
{
    unsigned int value;

#ifdef SYS_getrandom
    long int ret = syscall(SYS_getrandom, &value, sizeof(value), 0);
    if (ret == sizeof(value))
        return value;
#endif

    int fd = open("/dev/urandom", O_CLOEXEC | O_RDONLY);
    if (fd < 0) {
        fd = open("/dev/random", O_CLOEXEC | O_RDONLY);
        if (fd < 0)
            return DEFAULT_ODD_CONSTANT;
    }
    if (read(fd, &value, sizeof(value)) != sizeof(value))
        value = DEFAULT_ODD_CONSTANT;
    close(fd);

    return value;
}

static inline unsigned int hash_int_shift_mult(const void *keyptr)
{
    /* http://www.concentric.net/~Ttwang/tech/inthash.htm */
    unsigned int key = (unsigned int)(long)keyptr;
    unsigned int c2 = odd_constant;

    key = (key ^ 61) ^ (key >> 16);
    key += key << 3;
    key ^= key >> 4;
    key *= c2;
    key ^= key >> 15;
    return key;
}

#if defined(HAVE_BUILTIN_CPU_INIT) && defined(HAVE_BUILTIN_IA32_CRC32)
static inline unsigned int hash_str_crc32(const void *keyptr)
{
    unsigned int hash = odd_constant;
    const char *key = keyptr;
    size_t len = strlen(key);

#if __x86_64__
    while (len >= sizeof(uint64_t)) {
        uint64_t data;
        memcpy(&data, key, sizeof(data));
        hash = (unsigned int)__builtin_ia32_crc32di(hash, data);
        key += sizeof(uint64_t);
        len -= sizeof(uint64_t);
    }
#endif /* __x86_64__ */
    while (len >= sizeof(uint32_t)) {
        uint32_t data;
        memcpy(&data, key, sizeof(data));
        hash = __builtin_ia32_crc32si(hash, data);
        key += sizeof(uint32_t);
        len -= sizeof(uint32_t);
    }
    if (*key && *(key + 1)) {
        uint16_t data;
        memcpy(&data, key, sizeof(data));
        hash = __builtin_ia32_crc32hi(hash, data);
        key += sizeof(uint16_t);
    }
    /* Last byte might be the terminating NUL or the last character.
     * For a hash, this doesn't matter, and shaves off a branch.
     */
    hash = __builtin_ia32_crc32qi(hash, (unsigned char)*key);

    return hash;
}

static inline unsigned int hash_int_crc32(const void *keyptr)
{
    return __builtin_ia32_crc32si(odd_constant,
                                  (unsigned int)(uintptr_t)keyptr);
}
#endif

__attribute__((constructor)) static void initialize_odd_constant(void)
{
    /* This constant is randomized in order to mitigate the DDoS attack
     * described by Crosby and Wallach in UsenixSec2003.  */
    odd_constant = get_random_unsigned() | 1;
    murmur3_set_seed(odd_constant);

#if defined(HAVE_BUILTIN_CPU_INIT) && defined(HAVE_BUILTIN_IA32_CRC32)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sse4.2")) {
        hash_str = hash_str_crc32;
        hash_int = hash_int_crc32;
    }
#endif
}

static inline int hash_int_key_cmp(const void *k1, const void *k2)
{
    intptr_t a = (intptr_t)k1;
    intptr_t b = (intptr_t)k2;

    return (a > b) - (a < b);
}

static void no_op(void *arg __attribute__((unused))) {}

static struct hash *
hash_internal_new(unsigned int (*hash_value)(const void *key),
                  int (*key_compare)(const void *k1, const void *k2),
                  void (*free_key)(void *value),
                  void (*free_value)(void *value))
{
    struct hash *hash =
        calloc(1, sizeof(struct hash) + N_BUCKETS * sizeof(struct hash_bucket));

    if (hash == NULL)
        return NULL;

    hash->hash_value = hash_value;
    hash->key_compare = key_compare;
    hash->free_value = free_value;
    hash->free_key = free_key;
    return hash;
}

struct hash *hash_int_new(void (*free_key)(void *value),
                          void (*free_value)(void *value))
{
    return hash_internal_new(hash_int, hash_int_key_cmp,
                             free_key ? free_key : no_op,
                             free_value ? free_value : no_op);
}

struct hash *hash_str_new(void (*free_key)(void *value),
                          void (*free_value)(void *value))
{
    return hash_internal_new(
        hash_str, (int (*)(const void *, const void *))strcmp,
        free_key ? free_key : no_op, free_value ? free_value : no_op);
}

void hash_free(struct hash *hash)
{
    struct hash_bucket *bucket, *bucket_end;

    if (hash == NULL)
        return;

    bucket = hash->buckets;
    bucket_end = bucket + N_BUCKETS;
    for (; bucket < bucket_end; bucket++) {
        struct hash_entry *entry, *entry_end;
        entry = bucket->entries;
        entry_end = entry + bucket->used;
        for (; entry < entry_end; entry++) {
            hash->free_value((void *)entry->value);
            hash->free_key((void *)entry->key);
        }
        free(bucket->entries);
    }
    free(hash);
}

static struct hash_entry *hash_add_entry(struct hash *hash, const void *key)
{
    unsigned int hashval = hash->hash_value(key);
    unsigned int pos = hashval & (N_BUCKETS - 1);
    struct hash_bucket *bucket = hash->buckets + pos;
    struct hash_entry *entry, *entry_end;

    if (bucket->used + 1 >= bucket->total) {
        unsigned int new_total;
        struct hash_entry *tmp;

        if (__builtin_add_overflow(bucket->total, STEPS, &new_total)) {
            errno = EOVERFLOW;
            return NULL;
        }

        tmp = reallocarray(bucket->entries, new_total, sizeof(*tmp));
        if (tmp == NULL)
            return NULL;

        bucket->entries = tmp;
        bucket->total = new_total;
    }

    entry = bucket->entries;
    entry_end = entry + bucket->used;
    for (; entry < entry_end; entry++) {
        if (hashval != entry->hashval)
            continue;
        if (!hash->key_compare(key, entry->key))
            return entry;
    }

    bucket->used++;
    hash->count++;

    entry->hashval = hashval;
    entry->key = entry->value = NULL;

    return entry;
}

/*
 * add or replace key in hash map.
 *
 * none of key or value are copied, just references are remembered as is,
 * make sure they are live while pair exists in hash!
 */
int hash_add(struct hash *hash, const void *key, const void *value)
{
    struct hash_entry *entry = hash_add_entry(hash, key);

    if (!entry)
        return -errno;

    hash->free_value((void *)entry->value);
    hash->free_key((void *)entry->key);

    entry->key = key;
    entry->value = value;

    return 0;
}

/* similar to hash_add(), but fails if key already exists */
int hash_add_unique(struct hash *hash, const void *key, const void *value)
{
    struct hash_entry *entry = hash_add_entry(hash, key);

    if (!entry)
        return -errno;

    if (entry->key || entry->value)
        return -EEXIST;

    entry->key = key;
    entry->value = value;

    return 0;
}

static inline struct hash_entry *
hash_find_entry(const struct hash *hash, const char *key, unsigned int hashval)
{
    unsigned int pos = hashval & (N_BUCKETS - 1);
    const struct hash_bucket *bucket = hash->buckets + pos;
    struct hash_entry *entry, *entry_end;

    entry = bucket->entries;
    entry_end = entry + bucket->used;
    for (; entry < entry_end; entry++) {
        if (hashval != entry->hashval)
            continue;
        if (hash->key_compare(key, entry->key) == 0)
            return entry;
    }

    return NULL;
}

void *hash_find(const struct hash *hash, const void *key)
{
    const struct hash_entry *entry;

    entry = hash_find_entry(hash, key, hash->hash_value(key));
    if (entry)
        return (void *)entry->value;
    return NULL;
}

int hash_del(struct hash *hash, const void *key)
{
    unsigned int hashval = hash->hash_value(key);
    unsigned int pos = hashval & (N_BUCKETS - 1);
    unsigned int steps_used, steps_total;
    struct hash_bucket *bucket = hash->buckets + pos;
    struct hash_entry *entry, *entry_end;

    entry = hash_find_entry(hash, key, hashval);
    if (entry == NULL)
        return -ENOENT;

    hash->free_value((void *)entry->value);
    hash->free_key((void *)entry->key);

    entry_end = bucket->entries + bucket->used;
    memmove(entry, entry + 1,
            (size_t)(entry_end - entry) * sizeof(struct hash_entry));

    bucket->used--;
    hash->count--;

    steps_used = bucket->used / STEPS;
    steps_total = bucket->total / STEPS;
    if (steps_used + 1 < steps_total) {
        struct hash_entry *tmp =
            reallocarray(bucket->entries, steps_used + 1, STEPS * sizeof(*tmp));
        if (tmp) {
            bucket->entries = tmp;
            bucket->total = (steps_used + 1) * STEPS;
        }
    }

    return 0;
}

unsigned int hash_get_count(const struct hash *hash) { return hash->count; }

void hash_iter_init(const struct hash *hash, struct hash_iter *iter)
{
    iter->hash = hash;
    iter->bucket = 0;
    iter->entry = -1;
}

bool hash_iter_next(struct hash_iter *iter,
                    const void **key,
                    const void **value)
{
    const struct hash_bucket *b = iter->hash->buckets + iter->bucket;
    const struct hash_entry *e;

    iter->entry++;

    if ((unsigned int)iter->entry >= b->used) {
        iter->entry = 0;

        for (iter->bucket++; iter->bucket < N_BUCKETS; iter->bucket++) {
            b = iter->hash->buckets + iter->bucket;

            if (b->used > 0)
                break;
        }

        if (iter->bucket >= N_BUCKETS)
            return false;
    }

    e = b->entries + iter->entry;

    if (value != NULL)
        *value = e->value;
    if (key != NULL)
        *key = e->key;

    return true;
}
