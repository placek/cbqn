#include "../core.h"
#include "../nfns.h"
#include "../builtins.h"

DEF_FREE(md1D) { dec(((Md1D*)x)->m1); dec(((Md1D*)x)->f);                     }
DEF_FREE(md2D) { dec(((Md2D*)x)->m2); dec(((Md2D*)x)->f); dec(((Md2D*)x)->g); }
DEF_FREE(md2H) { dec(((Md2H*)x)->m2); dec(((Md2H*)x)->g);                     }
DEF_FREE(fork) { dec(((Fork*)x)->f ); dec(((Fork*)x)->g); dec(((Fork*)x)->h); }
DEF_FREE(atop) {                      dec(((Atop*)x)->g); dec(((Atop*)x)->h); }

static void md1D_visit(Value* x) { mm_visit(((Md1D*)x)->m1); mm_visit(((Md1D*)x)->f);                          }
static void md2D_visit(Value* x) { mm_visit(((Md2D*)x)->m2); mm_visit(((Md2D*)x)->f); mm_visit(((Md2D*)x)->g); }
static void md2H_visit(Value* x) { mm_visit(((Md2H*)x)->m2); mm_visit(((Md2H*)x)->g);                          }
static void fork_visit(Value* x) { mm_visit(((Fork*)x)->f ); mm_visit(((Fork*)x)->g); mm_visit(((Fork*)x)->h); }
static void atop_visit(Value* x) {                           mm_visit(((Atop*)x)->g); mm_visit(((Atop*)x)->h); }

static void md1D_print(B x) { printf("(md1D ");print(c(Md1D,x)->f);printf(" ");print(c(Md1D,x)->m1);                                printf(")"); }
static void md2D_print(B x) { printf("(md2D ");print(c(Md2D,x)->f);printf(" ");print(c(Md2D,x)->m2);printf(" ");print(c(Md2D,x)->g);printf(")"); }
static void md2H_print(B x) { printf("(md2H ");                                print(c(Md2H,x)->m2);printf(" ");print(c(Md2H,x)->g);printf(")"); }
static void fork_print(B x) { printf("(fork ");print(c(Fork,x)->f);printf(" ");print(c(Fork,x)->g );printf(" ");print(c(Fork,x)->h);printf(")"); }
static void atop_print(B x) { printf("(atop ");                                print(c(Atop,x)->g );printf(" ");print(c(Atop,x)->h);printf(")"); }

B md1D_c1(B t,      B x) { Md1D* tc = c(Md1D, t); return c(Md1,tc->m1)->c1(tc,    x); }
B md1D_c2(B t, B w, B x) { Md1D* tc = c(Md1D, t); return c(Md1,tc->m1)->c2(tc, w, x); }
B md2D_c1(B t,      B x) { Md2D* tc = c(Md2D, t); return c(Md2,tc->m2)->c1(tc,    x); }
B md2D_c2(B t, B w, B x) { Md2D* tc = c(Md2D, t); return c(Md2,tc->m2)->c2(tc, w, x); }
B tr2D_c1(B t,      B x) { return c1(c(Atop,t)->g, c1(c(Atop,t)->h,    x)); }
B tr2D_c2(B t, B w, B x) { return c1(c(Atop,t)->g, c2(c(Atop,t)->h, w, x)); }
B fork_c1(B t, B x) {
  B f = c(Fork,t)->f; errMd(f);
  B h = c(Fork,t)->h;
  if (isFun(f)) {
    B hr = c1(h, inc(x));
    return c2(c(Fork,t)->g, c(Fun,f)->c1(f,x), hr);
  } else {
    return c2(c(Fork,t)->g, inc(f), c1(h,x));
  }
}
B fork_c2(B t, B w, B x) {
  B f = c(Fork,t)->f; errMd(f);
  B h = c(Fork,t)->h;
  if (isFun(f)) {
    B hr = c2(h, inc(w), inc(x));
    return c2(c(Fork,t)->g, c(Fun,f)->c2(f,w,x), hr);
  } else {
    return c2(c(Fork,t)->g, inc(f), c2(h,w,x));
  }
}
B md2H_c1(Md1D* m,      B x) { Md2H* t=c(Md2H,m->m1); return md2D_c1(m_md2D(t->m2, m->f, t->g),    x); }
B md2H_c2(Md1D* m, B w, B x) { Md2H* t=c(Md2H,m->m1); return md2D_c2(m_md2D(t->m2, m->f, t->g), w, x); }

static B md1D_decompose(B x) { B r=m_v3(m_i32(4),inc(c(Md1D,x)->f),inc(c(Md1D,x)->m1)                   ); decR(x); return r; }
static B md2D_decompose(B x) { B r=m_v4(m_i32(5),inc(c(Md2D,x)->f),inc(c(Md2D,x)->m2), inc(c(Md2D,x)->g)); decR(x); return r; }
static B md2H_decompose(B x) { B r=m_v3(m_i32(6),                  inc(c(Md2H,x)->m2), inc(c(Md2H,x)->g)); decR(x); return r; }
static B fork_decompose(B x) { B r=m_v4(m_i32(3),inc(c(Fork,x)->f),inc(c(Fork,x)->g ), inc(c(Fork,x)->h)); decR(x); return r; }
static B atop_decompose(B x) { B r=m_v3(m_i32(2),                  inc(c(Atop,x)->g ), inc(c(Atop,x)->h)); decR(x); return r; }

static B md2D_uc1(B t, B o, B x) {
  B m = c(Md2D, t)->m2;
  B f = c(Md2D, t)->f;
  B g = c(Md2D, t)->g;
  if (v(t)->flags || !isMd2(m)) return def_fn_uc1(t, o, x); // flags check to not deconstruct builtins
  return TI(m,m2_uc1)(m, o, f, g, x);
}

static B toConstant(B x) { // doesn't consume x
  if (!isCallable(x)) return inc(x);
  if (v(x)->type == t_md1D) {
    Md1D* d = c(Md1D,x);
    B m1 = d->m1;
    if (v(m1)->type==t_md1BI && v(m1)->flags==n_const) return inc(d->f);
  }
  return bi_N;
}
static NFnDesc* ucwWrapDesc;

static B fork_uc1(B t, B o, B x) {
  B f = toConstant(c(Fork, t)->f);
  B g = c(Fork, t)->g;
  B h = c(Fork, t)->h;
  if (RARE(q_N(f) | !isFun(g) | !isFun(h))) { dec(f); return def_fn_uc1(t, o, x); } // flags check to not deconstruct builtins
  B args[] = {g, o, f};
  B tmp = m_nfn(ucwWrapDesc, tag(args, RAW_TAG));
  B r = TI(h,fn_uc1)(h,tmp,x);
  // f is consumed by the eventual ucwWrap call. this hopes that everything is nice and calls o only once, and within the under call, so any under interface must make they can't
  dec(tmp);
  return r;
}

static B ucwWrap_c1(B t, B x) {
  B* args = c(B, nfn_objU(t));
  B g = args[0];
  return TI(g,fn_ucw)(g, args[1], args[2], x);
}


void derv_init() {
  TIi(t_md1D,freeO) = md1D_freeO; TIi(t_md1D,freeF) = md1D_freeF; TIi(t_md1D,visit) = md1D_visit; TIi(t_md1D,print) = md1D_print; TIi(t_md1D,decompose) = md1D_decompose;
  TIi(t_md2D,freeO) = md2D_freeO; TIi(t_md2D,freeF) = md2D_freeF; TIi(t_md2D,visit) = md2D_visit; TIi(t_md2D,print) = md2D_print; TIi(t_md2D,decompose) = md2D_decompose;
  TIi(t_md2H,freeO) = md2H_freeO; TIi(t_md2H,freeF) = md2H_freeF; TIi(t_md2H,visit) = md2H_visit; TIi(t_md2H,print) = md2H_print; TIi(t_md2H,decompose) = md2H_decompose;
  TIi(t_fork,freeO) = fork_freeO; TIi(t_fork,freeF) = fork_freeF; TIi(t_fork,visit) = fork_visit; TIi(t_fork,print) = fork_print; TIi(t_fork,decompose) = fork_decompose;
  TIi(t_atop,freeO) = atop_freeO; TIi(t_atop,freeF) = atop_freeF; TIi(t_atop,visit) = atop_visit; TIi(t_atop,print) = atop_print; TIi(t_atop,decompose) = atop_decompose;
  TIi(t_md1BI,m1_d) = m_md1D;
  TIi(t_md2BI,m2_d) = m_md2D;
  TIi(t_md2D,fn_uc1) = md2D_uc1; // not in post so later init can utilize it
}
void dervPost_init() {
  ucwWrapDesc = registerNFn(m_str8l("(temporary function for ⌾)"), ucwWrap_c1, c2_bad);
  TIi(t_fork,fn_uc1) = fork_uc1;
}