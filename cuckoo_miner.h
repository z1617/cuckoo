// Cuckoo Cycle, a memory-hard proof-of-work
// Copyright (c) 2013-2014 John Tromp
// The edge=trimming time-memory trade-off is due to Dave Anderson:
// http://da-data.blogspot.com/2014/03/a-public-review-of-cuckoo-cycle.html

#include "cuckoo.h"
#ifdef __APPLE__
#include "osx_barrier.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <vector>
#include <atomic>
#include <set>

// algorithm parameters

// ok for size up to 2^32
#define MAXPATHLEN 8192
#ifndef PRESIP
#define PRESIP 1024
#endif
#ifndef PART_BITS
// #bits used to partition edge set processing to save memory
// a value of 0 does no partitioning and is fastest
// a value of 1 partitions in two, making twice_set the
// same size as shrinkingset at about 33% slowdown
// higher values are not that interesting
#define PART_BITS 0
#endif
#ifndef IDXSHIFT
// we want sizeof(cuckoo_hash) == sizeof(twice_set), so
// CUCKOO_SIZE * sizeof(u64) == TWICE_WORDS * sizeof(u32)
// CUCKOO_SIZE * 2 == TWICE_WORDS
// (SIZE >> IDXSHIFT) * 2 == 2 * ONCE_BITS / 32
// SIZE >> IDXSHIFT == HALFSIZE >> PART_BITS >> 5
// IDXSHIFT == 1 + PART_BITS + 5
#define IDXSHIFT (PART_BITS + 6)
#endif
#define CUCKOO_SIZE ((1+SIZE+(1<<IDXSHIFT)-1) >> IDXSHIFT)
#ifndef CLUMPSHIFT
// 2^CLUMPSHIFT should exceed maximum index drift (ui++) in cuckoo_hash
// SIZESHIFT-1 is limited to 64-KEYSHIFT
#define CLUMPSHIFT 9
#endif
#define KEYSHIFT (IDXSHIFT + CLUMPSHIFT)
#define KEYMASK ((1 << KEYSHIFT) - 1)
#define PART_MASK ((1 << PART_BITS) - 1)
#define ONCE_BITS ((HALFSIZE + PART_MASK) >> PART_BITS)
#define TWICE_WORDS ((2UL * ONCE_BITS + 31UL) / 32UL)

typedef std::atomic<u32> au32;
typedef std::atomic<u64> au64;

// set that starts out full and gets reset by threads on disjoint words
class shrinkingset {
public:
  std::vector<u32> bits;
  std::vector<u64> cnt;

  shrinkingset(nonce_t size, int nthreads) {
    bits.resize((size+31)/32);
    cnt.resize(nthreads);
    cnt[0] = size;
  }
  u64 count() {
    u64 sum = 0L;
    for (unsigned i=0; i<cnt.size(); i++)
      sum += cnt[i];
    return sum;
  }
  void reset(nonce_t n, int thread) {
    bits[n/32] |= 1 << (n%32);
    cnt[thread]--;
  }
  bool test(node_t n) {
    return !((bits[n/32] >> (n%32)) & 1);
  }
};

class twice_set {
public:
  au32 *bits;

  twice_set() {
    bits = (au32 *)calloc(TWICE_WORDS, sizeof(au32));
    assert(bits);
  }
  ~twice_set() {
    free(bits);
  }
  void reset() {
    for (unsigned i=0; i<TWICE_WORDS; i++)
      bits[i].store(0, std::memory_order_relaxed);
  }
  void set(node_t u) {
    node_t idx = u/16;
    u32 bit = 1 << (2 * (u%16));
    u32 old = std::atomic_fetch_or_explicit(&bits[idx], bit   , std::memory_order_relaxed);
    if (old & bit) std::atomic_fetch_or_explicit(&bits[idx], bit<<1, std::memory_order_relaxed);
  }
  u32 test(node_t u) {
    return (bits[u/16].load(std::memory_order_relaxed) >> (2 * (u%16))) & 2;
  }
};

class cuckoo_hash {
public:
  au64 *cuckoo;

  cuckoo_hash() {
    cuckoo = (au64 *)calloc(CUCKOO_SIZE, sizeof(au64));
    assert(cuckoo);
  }
  ~cuckoo_hash() {
    free(cuckoo);
  }
  void set(node_t u, node_t v) {
    node_t ui = u >> IDXSHIFT;
    u64 niew = (u64)v << KEYSHIFT | (u & KEYMASK);;
    for (;;) {
      u64 old = 0;
      if (cuckoo[ui].compare_exchange_strong(old, niew, std::memory_order_relaxed))
        return;
      if (((u^old) & KEYMASK) == 0) {
        cuckoo[ui].store(niew, std::memory_order_relaxed);
        return;
      }
      if (++ui == CUCKOO_SIZE)
        ui = 0;
    }
  }
  node_t get(node_t u) {
    node_t ui = u >> IDXSHIFT;
    for (;;) {
      u64 cu = cuckoo[ui].load(std::memory_order_relaxed);
      if (!cu)
        return 0;
      if (((u^cu) & KEYMASK) == 0)
        return (node_t)(cu >> KEYSHIFT);
      if (++ui == CUCKOO_SIZE)
        ui = 0;
    }
  }
};

class cuckoo_ctx {
public:
  siphash_ctx sip_ctx;
  nonce_t easiness;
  shrinkingset *alive;
  twice_set *nonleaf;
  cuckoo_hash *cuckoo;
  node_t *fastcuckoo;
  nonce_t (*sols)[PROOFSIZE];
  unsigned maxsols;
  std::atomic<unsigned> nsols;
  int nthreads;
  int ntrims;
  pthread_barrier_t barry;

  cuckoo_ctx(const char* header, nonce_t easy_ness, int n_threads, int n_trims, int max_sols) {
    setheader(&sip_ctx, header);
    easiness = easy_ness;
    nthreads = n_threads;
#ifdef HUGEFAST
    assert(fastcuckoo = (node_t *)calloc(1+SIZE, sizeof(node_t)));
    alive = 0;
    nonleaf = 0;
#else
    alive = new shrinkingset(easiness, nthreads);
    nonleaf = new twice_set;
#endif
    ntrims = n_trims;
    assert(pthread_barrier_init(&barry, NULL, nthreads) == 0);
    assert(sols = (nonce_t (*)[PROOFSIZE])calloc(maxsols = max_sols, PROOFSIZE*sizeof(nonce_t)));
    nsols = 0;
  }
  ~cuckoo_ctx() {
#ifdef HUGEFAST
    // free(fastcuckoo);
#else
    delete alive;
#endif
    if (nonleaf)
      delete nonleaf;
    if (cuckoo)
      delete cuckoo;
  }
};

typedef struct {
  int id;
  pthread_t thread;
  cuckoo_ctx *ctx;
} thread_ctx;

void barrier(pthread_barrier_t *barry) {
  int rc = pthread_barrier_wait(barry);
  if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
    printf("Could not wait on barrier\n");
    pthread_exit(NULL);
  }
}

#define FORALL_LIVE_NONCES(NONCE) \
  for (nonce_t block = tp->id*32; block < ctx->easiness; block += ctx->nthreads*32) {\
    for (nonce_t NONCE = block; NONCE < block+32 && NONCE < ctx->easiness; NONCE++) {\
      if (ctx->alive->test(NONCE))

void trim_edges(thread_ctx *tp, unsigned part) {
  cuckoo_ctx *ctx = tp->ctx;
  if (tp->id == 0)
    ctx->nonleaf->reset();
  barrier(&ctx->barry);
  FORALL_LIVE_NONCES(nonce) {
    node_t u = sipedge_u(&ctx->sip_ctx, nonce);
    if ((u & PART_MASK) == part)
      ctx->nonleaf->set(u >> PART_BITS);
  }}}
  barrier(&ctx->barry);
  FORALL_LIVE_NONCES(nonce) {
    node_t u = sipedge_u(&ctx->sip_ctx, nonce);
    if ((u & PART_MASK) == part && !ctx->nonleaf->test(u >> PART_BITS))
    ctx->alive->reset(nonce, tp->id);
  }}}
  barrier(&ctx->barry);
  if (tp->id == 0)
    ctx->nonleaf->reset();
  barrier(&ctx->barry);
  FORALL_LIVE_NONCES(nonce) {
    node_t v = sipedge_v(&ctx->sip_ctx, nonce);
    if ((v & PART_MASK) == part)
      ctx->nonleaf->set(v >> PART_BITS);
  }}}
  barrier(&ctx->barry);
  FORALL_LIVE_NONCES(nonce) {
    node_t v = sipedge_v(&ctx->sip_ctx, nonce);
    if ((v & PART_MASK) == part && !ctx->nonleaf->test(v >> PART_BITS))
      ctx->alive->reset(nonce, tp->id);
  }}}
  barrier(&ctx->barry);
}

#ifdef HUGEFAST
int path(node_t *cuckoo, node_t u, node_t *us) {
  int nu;
  for (nu = 0; u; u = cuckoo[u]) {
#else
int path(cuckoo_hash *cuckoo, node_t u, node_t *us) {
  int nu;
  for (nu = 0; u; u = cuckoo->get(u)) {
#endif
    if (++nu >= MAXPATHLEN) {
      while (nu-- && us[nu] != u) ;
      if (nu < 0)
        printf("maximum path length exceeded\n");
      else printf("illegal % 4d-cycle\n", MAXPATHLEN-nu);
      pthread_exit(NULL);
    }
    us[nu] = u;
  }
  return nu;
}

typedef std::pair<node_t,node_t> edge;

void solution(cuckoo_ctx *ctx, node_t *us, int nu, node_t *vs, int nv) {
  std::set<edge> cycle;
  unsigned n;
  cycle.insert(edge(*us, *vs));
  while (nu--)
    cycle.insert(edge(us[(nu+1)&~1], us[nu|1])); // u's in even position; v's in odd
  while (nv--)
    cycle.insert(edge(vs[nv|1], vs[(nv+1)&~1])); // u's in odd position; v's in even
  unsigned soli = std::atomic_fetch_add_explicit(&ctx->nsols, 1U, std::memory_order_relaxed);
  for (nonce_t nonce = n = 0; nonce < ctx->easiness; nonce++) {
#ifdef HUGEFAST
    {
#else
    if (ctx->alive->test(nonce)) {
#endif
      edge e(1+sipedge_u(&ctx->sip_ctx, nonce), 1+HALFSIZE+sipedge_v(&ctx->sip_ctx, nonce));
      if (cycle.find(e) != cycle.end()) {
        ctx->sols[soli][n++] = nonce;
        cycle.erase(e);
      }
    }
  }
}

#ifndef HUGEFAST
void *worker(void *vp) {
  thread_ctx *tp = (thread_ctx *)vp;
  cuckoo_ctx *ctx = tp->ctx;

  int load = 100;
  for (int round=1; round <= ctx->ntrims; round++) {
    for (unsigned part = 0; part <= PART_MASK; part++)
      trim_edges(tp, part);
    if (tp->id == 0) {
      load = (int)(100 * ctx->alive->count() / CUCKOO_SIZE);
      printf("%d trims: load %d%%\n", round, load);
    }
  }
  if (tp->id == 0) {
    if (load >= 90) {
      printf("overloaded! exiting...");
      exit(0);
    }
    delete ctx->nonleaf;
    ctx->nonleaf = 0;
    ctx->cuckoo = new cuckoo_hash();
  }
  barrier(&ctx->barry);
  cuckoo_hash *cuckoo = ctx->cuckoo;
  node_t us[MAXPATHLEN], vs[MAXPATHLEN];
#ifdef SINGLE
  if (tp->id != 0)
    pthread_exit(NULL);
  for (nonce_t nonce = 0; nonce < ctx->easiness; nonce++) {{
    if (ctx->alive->test(nonce)) {
#else
  FORALL_LIVE_NONCES(nonce) {
#endif
    node_t u0, v0;
    sipedge(&ctx->sip_ctx, nonce, &u0, &v0);
    u0 += 1        ;  // make non-zero
    v0 += 1 + HALFSIZE;  // make v's different from u's
    node_t u = cuckoo->get(u0), v = cuckoo->get(v0);
    if (u == v0 || v == u0)
      continue; // ignore duplicate edges
    us[0] = u0;
    vs[0] = v0;
    int nu = path(cuckoo, u, us), nv = path(cuckoo, v, vs);
    if (us[nu] == vs[nv]) {
      int min = nu < nv ? nu : nv;
      for (nu -= min, nv -= min; us[nu] != vs[nv]; nu++, nv++) ;
      int len = nu + nv + 1;
      printf("% 4d-cycle found at %d:%d%%\n", len, tp->id, (int)(nonce*100L/ctx->easiness));
      if (len == PROOFSIZE && ctx->nsols < ctx->maxsols)
        solution(ctx, us, nu, vs, nv);
      continue;
    }
    if (nu < nv) {
      while (nu--)
        cuckoo->set(us[nu+1], us[nu]);
      cuckoo->set(u0, v0);
    } else {
      while (nv--)
        cuckoo->set(vs[nv+1], vs[nv]);
      cuckoo->set(v0, u0);
    }
  }}}
  pthread_exit(NULL);
}
#else
void *worker(void *vp) {
  thread_ctx *tp = (thread_ctx *)vp;
  cuckoo_ctx *ctx = tp->ctx;

  node_t *cuckoo = ctx->fastcuckoo;
  node_t us[MAXPATHLEN], vs[MAXPATHLEN], uvpre[2*PRESIP], npre = 0;
  for (node_t nonce = tp->id; nonce < ctx->easiness; nonce += ctx->nthreads) {
    node_t u0, v0;
#if PRESIP==0
    u0 = sipedge_u(&ctx->sip_ctx, nonce);
    v0 = sipedge_v(&ctx->sip_ctx, nonce);
#else
    if (!npre) {
      for (unsigned n = nonce; npre < PRESIP; npre++, n += ctx->nthreads) {
        uvpre[2*npre  ] = sipedge_u(&ctx->sip_ctx, n);
        uvpre[2*npre+1] = sipedge_v(&ctx->sip_ctx, n);
      }
    }
    unsigned i = PRESIP - npre--;
    u0 = uvpre[2*i];
    v0 = uvpre[2*i+1];
#endif
    u0 += 1        ;  // make non-zero
    v0 += 1 + HALFSIZE;  // make v's different from u's
    node_t u = cuckoo[u0], v = cuckoo[v0];
    if (u == v0 || v == u0)
      continue; // ignore duplicate edges
    us[0] = u0;
    vs[0] = v0;
    int nu = path(cuckoo, u, us), nv = path(cuckoo, v, vs);
    if (us[nu] == vs[nv]) {
      int min = nu < nv ? nu : nv;
      for (nu -= min, nv -= min; us[nu] != vs[nv]; nu++, nv++) ;
      int len = nu + nv + 1;
      printf("% 4d-cycle found at %d:%d%%\n", len, tp->id, (int)(nonce*100L/ctx->easiness));
      if (len == PROOFSIZE && ctx->nsols < ctx->maxsols)
        solution(ctx, us, nu, vs, nv);
      continue;
    }
    if (nu < nv) {
      while (nu--)
        cuckoo[us[nu+1]] = us[nu];
      cuckoo[*us] = *vs;
    } else {
      while (nv--)
        cuckoo[vs[nv+1]] = vs[nv];
      cuckoo[*vs] = *us;
    }
  }
  pthread_exit(NULL);
}
#endif
