#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

#ifndef _WIN32
#include <err.h>
#endif

#include "mapi.h"

// Aligned memory operation macros.
#ifdef _WIN32
#define ALIGNED_ALLOC(a, sz) _aligned_malloc(sz, a)
#define ALIGNED_FREE(p) _aligned_free(p)
#else
#define ALIGNED_ALLOC(a, sz) aligned_alloc(a, sz)
#define ALIGNED_FREE(p) free(p)
#endif

/* internals */

static size_t mapi_roundup_size(size_t s) {
  s--;
  s |= s >> 1;
  s |= s >> 2;
  s |= s >> 4;
  s |= s >> 8;
  s |= s >> 16;
  s |= s >> 32;
  s++;

  return s;
}

/* do hash */
uint32_t hash(uint32_t a) {
    a = (a+0x7ed55d16) + (a<<12);
    a = (a^0xc761c23c) ^ (a>>19);
    a = (a+0x165667b1) + (a<<5);
    a = (a+0xd3a2646c) ^ (a<<9);
    a = (a+0xfd7046c5) + (a<<3);
    a = (a^0xb55a4f09) ^ (a>>16);
    return a;
}

/* allocators */

mapi *mapi_new(size_t s) {
  mapi *m;

  m = malloc(sizeof *m);
  if (!m) return NULL;
  mapi_init(m, s);

  return m;
}

void mapi_init(mapi *m, size_t s) {
  *m = (mapi){.object_size = s, .empty_key = MAPI_EMPTY_KEY};
}

void mapi_empty_key(mapi *m, uint32_t k) { m->empty_key = k; }

void mapi_release(mapi *m, void (*release)(void *)) { m->release = release; }

void mapi_clone(mapi *m, void (*clone)(void *, void *, size_t)) { m->clone = clone; }

void mapi_getKey(mapi *m, uint32_t (*getKey)(void *)) { m->getKey = getKey; }

void mapi_setKey(mapi *m, void (*setKey)(void *, uint32_t)) { m->setKey = setKey; }

void mapi_free(mapi *m) {
  mapi_clear(m);
  free(m);
}

/* capacity */

size_t mapi_size(mapi *m) { return m->size; }

/* Element access */

void *mapi_get(mapi *m, size_t p) {
  return (void *)(m->objects + (p * m->object_size));
}

void *mapi_at(mapi *m, uint32_t k) {
  uint32_t hk = hash(k);
  size_t p = hk;
  mapi_object *o;

  if (m->capacity == 0) return NULL;

  while (1) {
    p &= m->capacity - 1;
    o = mapi_super(mapi_get(m, p));
    if (__likely(o->key == m->empty_key) || __likely(m->getKey(o) == k)) return o;
    p++;
  }
}

mapi_object *mapi_super(void *o) { return (mapi_object *)o; }

int mapi_empty(mapi *m, void *o) { return mapi_super(o)->key == m->empty_key; }

/* element lookup */

void *mapi_find(mapi *m, uint32_t k) {
  void *o = mapi_at(m, k);

  return o && !mapi_empty(m, o) ? o : NULL;
}

/* modifiers */

void *mapi_insert(mapi *m, void *o) {
  mapi_object *mo;
  int e;

  e = mapi_reserve(m, m->size + 1);
  if (e == -1) return NULL;

  uint32_t gk = m->getKey(mapi_super(o));
  void* at = mapi_at(m, gk);
  mo = mapi_super(at);

  /* mo = mapi_super(mapi_at(m, m->getKey(mapi_super(o)))); */
  if (mo->key == m->empty_key) {
    m->clone(mo, o, m->object_size);
    m->size++;
    return mo;
  } else {
    if (m->release) m->release(o);
  }

  return NULL;
}

void mapi_erase(mapi *m, uint32_t k) {
  mapi_object *o, *i, *w;

  o = mapi_super(mapi_at(m, k));
  if (!o || mapi_super(o)->key == m->empty_key) return;

  if (m->release) m->release(o);
  m->size--;

  i = o;
  while (1) {
    i = mapi_super(mapi_inc(m, i));
    if (i == mapi_super(mapi_end(m))) i = mapi_super(m->objects);

    if (mapi_super(i)->key == m->empty_key) break;

    w = mapi_super(mapi_get(m, m->getKey(i) & (m->capacity - 1)));
    if ((i > o && (w <= o || w > i)) || (i < o && (w <= o && w > i))) {
      m->clone(o, i, m->object_size);
      o = i;
    }
  }

  mapi_super(o)->key = m->empty_key;
}

void mapi_clear(mapi *m) {
  void *o;

  if (m->release)
    for (o = mapi_begin(m); o < mapi_end(m); o = mapi_next(m, o)) m->release(o);

  ALIGNED_FREE(m->objects);

  m->objects = NULL;
  m->size = 0;
  m->capacity = 0;
}

/* buckets */

size_t mapi_bucket_count(mapi *m) { return m->capacity; }

/* hash policy */

int mapi_rehash(mapi *m, size_t c) {
  mapi nm;
  void *o;

  c = mapi_roundup_size(c);
  if (c > m->capacity) {
    if (m->capacity == 0) c = c < MAPI_MIN_CAPACITY ? MAPI_MIN_CAPACITY : c;

    mapi_init(&nm, m->object_size);
    mapi_empty_key(&nm, m->empty_key);
    mapi_release(&nm, m->release);
    mapi_clone(&nm, m->clone);
    mapi_getKey(&nm, m->getKey);
    mapi_setKey(&nm, m->setKey);

    nm.capacity = c;
    nm.objects =
        (char *)ALIGNED_ALLOC(64, MAPI_ROUNDUP(c * nm.object_size, 64));
    if (!nm.objects) return -1;

    for (o = mapi_begin(&nm); o < mapi_end(&nm); o = mapi_inc(&nm, o))
      mapi_super(o)->key = nm.empty_key;

    for (o = mapi_begin(m); o < mapi_end(m); o = mapi_next(m, o))
      mapi_insert(&nm, o);

    ALIGNED_FREE(m->objects);

    *m = nm;
  }

  return 0;
}

int mapi_reserve(mapi *m, size_t s) {
  s <<= 1;
  return s > m->capacity ? mapi_rehash(m, s) : 0;
}

/* iterators */

void *mapi_begin(mapi *m) { return mapi_skip(m, m->objects); }

void *mapi_next(mapi *m, void *o) { return mapi_skip(m, mapi_inc(m, o)); }

void *mapi_end(mapi *m) { return mapi_get(m, m->capacity); }

void *mapi_skip(mapi *m, void *o) {
  while (o < mapi_end(m) && mapi_super(o)->key == m->empty_key)
    o = mapi_inc(m, o);

  return o;
}

void *mapi_inc(mapi *m, void *o) {
  return (void *)((char *)o + m->object_size);
}
