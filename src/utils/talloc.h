#pragma once

typedef struct TAlloc {
  struct Value;
  u8 data[];
} TAlloc;
#define TOFF offsetof(TAlloc, data)
#define TALLOC(T,N,AM) T* N = (T*) ((TAlloc*)mm_alloc(TOFF + (AM)*sizeof(T) + 8, t_temp))->data; // +8 so mm is happy
#define TOBJ(N) (void*)((u8*)(N) - TOFF)
#define TFREE(N) mm_free((Value*)TOBJ(N));
#define TREALLOC(N, AM) talloc_realloc(TOBJ(N), AM)
#define TSIZE(N) (mm_size(TOBJ(N))-TOFF)
static inline void* talloc_realloc(TAlloc* t, u64 am) { // TODO maybe shouldn't be inline?
  u64 stored = mm_size((Value*)t)-TOFF;
  if (stored > am) return t->data;
  TALLOC(u8,r,am);
  memcpy(r, t->data, stored);
  mm_free((Value*)t);
  return r;
}

typedef struct TStack {
  struct Value;
  usz size;
  usz cap;
  u8 data[];
} TStack;
#define TSALLOC(T,N,I) usz N##_dc=(I); u32 N##_e=sizeof(T); TStack* N##_o = (TStack*)mm_alloc(sizeof(TStack)+N##_e*N##_dc, t_temp); N##_o->size=0; N##_o->cap=N##_dc; T* N = (T*)N##_o->data;
#define TSFREE(N) mm_free((Value*)N##_o);
#define TSUPD(N,AM) { N##_o = ts_e(N##_o, N##_e, AM); N = (void*)N##_o->data; }
#define TSADD(N,X) { if (N##_o->size==N##_o->cap) TSUPD(N, 1); N[N##_o->size++] = X; }
#define TSADDA(N,P,AM) { u64 n=AM; if(N##_o->size+n>N##_o->cap) TSUPD(N, n); memcpy(N+N##_o->size,P,n*N##_e); N##_o->size+= n; }
#define TSFREEP(N) mm_free((void*)RFLD(N, TStack, data));
#define TSSIZE(N) (N##_o->size)
TStack* ts_e(TStack* o, u32 elsz, u64 am);
