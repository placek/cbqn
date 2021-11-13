#pragma once


#if (defined(__x86_64) || defined(__amd64__)) && defined(MAP_32BIT) && MM!=0
  #ifndef JIT_START
    #define JIT_START 2 // number of calls for when to start JITting. -1: never JIT; 0: JIT everything, n: JIT after n non-JIT invocations; max ¯1+2⋆16
  #endif
#else
  #undef JIT_START
  #define JIT_START -1
#endif

enum {
  PUSH = 0x00, // N; push object from objs[N]
  DFND = 0x01, // N; push dfns[N], derived to current scope
  SYSV = 0x02, // N; get system function N
  
  POPS = 0x06, // pop object from stack
  RETN = 0x07, // returns top of stack
  RETD = 0x08, // return a namespace of exported items
  ARRO = 0x0B, // N; create a vector of top N items
  ARRM = 0x0C, // N; create a mutable vector of top N items
  
  FN1C = 0x10, // monadic function call ⟨…,x,f  ⟩ → F x
  FN2C = 0x11, //  dyadic function call ⟨…,x,f,w⟩ → w F x
  FN1O = 0x12, // optional monadic call (FN1C but checks for · at 𝕩)
  FN2O = 0x13, // optional  dyadic call (FN2C but checks for · at 𝕩 & 𝕨)
  TR2D = 0x14, // derive 2-train aka atop; ⟨…,  g,f⟩ → (f g)
  TR3D = 0x15, // derive 3-train aka fork; ⟨…,h,g,f⟩ → (f g h)
  CHKV = 0x16, // throw error if top of stack is ·
  TR3O = 0x17, // TR3D but creates an atop if F is ·
  
  MD1C = 0x1A, // call/derive 1-modifier; ⟨…,  _m,f⟩ → (f _m)
  MD2C = 0x1B, // call/derive 2-modifier; ⟨…,g,_m,f⟩ → (f _m_ g)
  MD2L = 0x1C, // derive 2-modifier to 1-modifier with 𝔽 ⟨…,_m_,f⟩ → (f _m_)
  MD2R = 0x1D, // derive 2-modifier to 1-modifier with 𝔾 ⟨…,g,_m_⟩ → (_m_ g)
  
  VARO = 0x20, // N0,N1; push variable at depth N0 and position N1
  VARM = 0x21, // N0,N1; push mutable variable at depth N0 and position N1
  VARU = 0x22, // N0,N1; like VARO but overrides the slot with bi_optOut
  DYNO = 0x26, // N; push variable with name objs[N]
  DYNM = 0x27, // N; push mutable variable with name objs[N]
  
  PRED = 0x2A, // pop item, go to next body if 0, continue if 1
  VFYM = 0x2B, // push a mutable version of ToS that fails if set to a non-equal value (for header assignment)
  SETH = 0x2F, // set header; acts like SETN, but it doesn't push to stack, and, instead of erroring in cases it would, it skips to the next body
  SETN = 0x30, // set new; _  ←_; ⟨…,x,  mut⟩ → mut←x
  SETU = 0x31, // set upd; _  ↩_; ⟨…,x,  mut⟩ → mut↩x
  SETM = 0x32, // set mod; _ F↩_; ⟨…,x,F,mut⟩ → mut F↩x
  SETC = 0x33, // set call; _ F↩; (…,  F,mut) → mut F↩
  FLDO = 0x40, // N; get field nameList[N] from ToS
  FLDM = 0x41, // N; get mutable field nameList[N] from ToS
  ALIM = 0x42, // N; replace ToS with one with a namespace field alias N
  EXTO, EXTM, EXTU, // alternate versions of VAR_ for extended variables
  ADDI, ADDU, // separate PUSH for refcounting needed/not needed (stores the object inline as 2 u32s, instead of reading from `objs`)
  FN1Ci, FN1Oi, FN2Ci, FN2Oi, // FN__ alternatives that don't take the function from the stack, but instead as an 2×u32 immediate in the bytecode
  SETNi, SETUi, SETMi, SETCi, // SET_ alternatives that expect the set variable as a depth-position pair like VAR_
  SETNv, SETUv, SETMv, SETCv, // SET_i alternatives that also don't return the result
  SETHi, PRED1, PRED2, // internal versions of SETH and PRED, with 2×u64 arguments (only 1 for PRED1) specifying bodies to jump to on fail (or NULL if is last)
  DFND0, DFND1, DFND2, // internal versions of DFND with a specific type, and a u64 argument representing the block pointer
  FAIL, // this body cannot be called monadically/dyadically
  BC_SIZE
};

typedef struct Comp Comp;
typedef struct BlBlocks BlBlocks;
typedef struct Block Block;
typedef struct Body Body;
typedef struct Scope Scope;
typedef struct ScopeExt ScopeExt;

typedef struct Comp {
  struct Value;
  B bc;
  B src;
  B path;
  B indices;
  HArr* objs;
  u32 blockAm;
} Comp;

struct BlBlocks {
  struct Value;
  u32 am;
  Block* a[];
};

typedef struct NSDesc NSDesc;
struct Block {
  struct Value;
  Comp* comp;
  bool imm;
  u8 ty;
  Block** blocks; // pointer in an owned BlBlocks, or null
  i32* map; // pointer in an owned I32Arr
  i32* bc; // pointer in an owned I32Arr
  i32 bodyCount;
  Body* dyBody; // pointer within bodies; not owned; TODO move to the second item of bodies or something
  Body* bodies[]; // bodies[0] is the first monadic body (also niladic body)
};
struct Body {
  struct Value;
#if JIT_START != -1
  u8* nvm; // either NULL or a pointer to machine code
#endif
#if JIT_START > 0
  u16 callCount;
#endif
  union { u32* bc; i32 bcTmp; }; // pointer in bl->bc; bcTmp to make ubsan happy (god dammit C)
  u32 maxStack;
  u16 maxPSC;
#if JIT_START != -1
  B nvmRefs;
#endif
  Block* bl; // non-owned pointer to corresponding block
  NSDesc* nsDesc;
  u16 varAm;
  i32 varIDs[];
};

struct ScopeExt {
  struct Value;
  u16 varAm;
  B vars[]; // vars has length varAm*2; position varAm and onwards are corresponding names to variables at regular indexes
};

struct Scope {
  struct Value;
  Scope* psc;
  Body* body; // last place where code was executed in this scope; also used for variable name resolution, so can only safely be replaced if varAm==0
  u16 varAm;
  ScopeExt* ext; // will probably be NULL
  B vars[];
};

Block* bqn_comp(B str, B path, B args);
Block* bqn_compSc(B str, B path, B args, Scope* sc, bool repl);
Block* compile(B bcq, B objs, B blocks, B bodies, B indices, B tokenInfo, B src, B path, Scope* sc);
Scope* m_scope(Body* body, Scope* psc, u16 varAm, i32 initVarAm, B* initVars);

typedef struct Nvm_res { u8* p; B refs; } Nvm_res;
Nvm_res m_nvm(Body* b);
void nvm_free(u8* ptr);

B evalJIT(Body* b, Scope* sc, u8* ptr);
B evalBC(Block* bl, Body* b, Scope* sc);
B execBlockInline(Block* block, Scope* sc); // doesn't consume; executes bytecode of the monadic body directly in the scope
FORCE_INLINE B execBodyInlineI(Block* block, Body* body, Scope* sc) { // consumes sc, unlike execBlockInline
  #if JIT_START != -1
    if (body->nvm) { toJIT: return evalJIT(body, sc, body->nvm); }
    bool jit = true;
    #if JIT_START > 0
      jit = body->callCount++ >= JIT_START;
    #endif
    // jit = body->bc[2]==m_f64(123456).u>>32; // enable JIT for blocks starting with `123456⋄`
    if (jit) {
      Nvm_res r = m_nvm(body);
      body->nvm = r.p;
      body->nvmRefs = r.refs;
      goto toJIT;
    }
  #endif
  return evalBC(block, body, sc);
}

extern u32 bL_m[BC_SIZE];
extern i32 sD_m[BC_SIZE];
extern i32 sC_m[BC_SIZE];
extern i32 sA_m[BC_SIZE];
static u32* nextBC       (u32* p) { return p + bL_m[*p]; }
static i32  stackAdded   (u32* p) { return sA_m[*p]; }
static i32  stackDiff    (u32* p) { if (*p==ARRO|*p==ARRM) return 1-p[1]; return sD_m[*p]; }
static i32  stackConsumed(u32* p) { if (*p==ARRO|*p==ARRM) return   p[1]; return sC_m[*p]; }

char* bc_repr(u32* p);


typedef struct FunBlock { struct Fun; Scope* sc; Block* bl; } FunBlock;
typedef struct Md1Block { struct Md1; Scope* sc; Block* bl; } Md1Block;
typedef struct Md2Block { struct Md2; Scope* sc; Block* bl; } Md2Block;
// all don't consume anything
B m_funBlock(Block* bl, Scope* psc); // may return evaluated result, whatever
B m_md1Block(Block* bl, Scope* psc);
B m_md2Block(Block* bl, Scope* psc);






typedef struct Env {
  u64 pos; // if top bit set, ((u32)pos)>>1 is an offset into bytecode; otherwise, it's a pointer in the bytecode
  Scope* sc;
} Env;
extern Env* envCurr;
extern Env* envStart;
extern Env* envEnd;
extern u64 envPrevHeight; // envStart+prevEnvHeight gives envCurr+1 from before the error
static inline void pushEnv(Scope* sc, u32* bc) {
  if (envCurr+1==envEnd) thrM("Stack overflow");
  envCurr++;
  envCurr->sc = sc;
  envCurr->pos = (u64)bc;
}
static inline void popEnv() {
  assert(envCurr>=envStart);
  envCurr--;
}
FORCE_INLINE void scope_dec(Scope* sc) { // version of ptr_dec for scopes, that tries to also free trivial cycles
  i32 varAm = sc->varAm;
  if (sc->refc>1) {
    i32 innerRef = 1;
    for (i32 i = 0; i < varAm; i++) {
      B c = sc->vars[i];
      if (isVal(c) && v(c)->refc==1) {
        u8 t = v(c)->type;
        if      (t==t_fun_block && c(FunBlock,c)->sc==sc) innerRef++;
        else if (t==t_md1_block && c(Md1Block,c)->sc==sc) innerRef++;
        else if (t==t_md2_block && c(Md2Block,c)->sc==sc) innerRef++;
      }
    }
    assert(innerRef <= sc->refc);
    if (innerRef==sc->refc) {
      value_free((Value*)sc);
      return;
    }
  }
  ptr_dec(sc);
}
FORCE_INLINE i32 argCount(u8 ty, bool imm) { return (imm?0:3) + ty + (ty>0); }
FORCE_INLINE i32 blockGivenVars(Block* bl) { return argCount(bl->ty, bl->imm); }
void vm_pst(Env* s, Env* e);
void vm_pstLive(void);
void vm_printPos(Comp* comp, i32 bcPos, i64 pos);
NOINLINE B vm_fmtPoint(B src, B prepend, B path, usz cs, usz ce); // consumes prepend
NOINLINE void printErrMsg(B msg);
NOINLINE void unwindEnv(Env* envNew); // envNew==envStart-1 for emptying the env stack
NOINLINE void unwindCompiler(void); // unwind to the env of the invocation of the compiler; UB when not in compiler!
usz getPageSize();


typedef struct FldAlias {
  struct Value;
  B obj;
  i32 p;
} FldAlias;
typedef struct VfyObj {
  struct Value;
  B obj;
} VfyObj;


NOINLINE B v_getR(Scope* pscs[], B s); // doesn't consume
static inline B v_getI(Scope* sc, u32 p) {
  B r = sc->vars[p];
  sc->vars[p] = bi_optOut;
  return r;
}
static inline B v_get(Scope* pscs[], B s) { // get value representing s, replacing with bi_optOut; doesn't consume
  if (RARE(!isVar(s))) return v_getR(pscs, s);
  return v_getI(pscs[(u16)(s.u>>32)], (u32)s.u);
}

NOINLINE void v_setR(Scope* pscs[], B s, B x, bool upd); // doesn't consume
NOINLINE bool v_sethR(Scope* pscs[], B s, B x); // doesn't consume
static inline void v_setI(Scope* sc, u32 p, B x, bool upd) { // consumes x
  B prev = sc->vars[p];
  if (upd) {
    if (prev.u==bi_noVar.u) thrM("↩: Updating undefined variable");
    dec(prev);
  }
  sc->vars[p] = x;
}
static inline void v_set(Scope* pscs[], B s, B x, bool upd) { // doesn't consume
  if (RARE(!isVar(s))) v_setR(pscs, s, x, upd);
  else v_setI(pscs[(u16)(s.u>>32)], (u32)s.u, inc(x), upd);
}

static inline bool v_seth(Scope* pscs[], B s, B x) { // doesn't consume; s cannot contain extended variables
  if (RARE(!isVar(s))) return v_sethR(pscs, s, x);
  v_setI(pscs[(u16)(s.u>>32)], (u32)s.u, inc(x), false);
  return true;
}
