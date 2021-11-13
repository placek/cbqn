#include <unistd.h>
#include "core.h"
#include "vm.h"
#include "ns.h"
#include "utils/utf.h"
#include "utils/talloc.h"
#include "utils/mut.h"

#ifndef UNWIND_COMPILER // whether to hide stackframes of the compiler in compiling errors
  #define UNWIND_COMPILER 1
#endif

#define FOR_BC(F) F(PUSH) F(DYNO) F(DYNM) F(ARRO) F(ARRM) F(FN1C) F(FN2C) F(MD1C) F(MD2C) F(TR2D) \
                  F(TR3D) F(SETN) F(SETU) F(SETM) F(SETC) F(POPS) F(DFND) F(FN1O) F(FN2O) F(CHKV) F(TR3O) \
                  F(MD2R) F(MD2L) F(VARO) F(VARM) F(VFYM) F(SETH) F(RETN) F(FLDO) F(FLDM) F(ALIM) F(RETD) F(SYSV) F(VARU) F(PRED) F(PRED1) F(PRED2) \
                  F(EXTO) F(EXTM) F(EXTU) F(ADDI) F(ADDU) F(FN1Ci)F(FN1Oi)F(FN2Ci)F(FN2Oi) \
                  F(SETNi)F(SETUi)F(SETMi)F(SETCi)F(SETNv)F(SETUv)F(SETMv)F(SETCv)F(SETHi) \
                  F(DFND0)F(DFND1)F(DFND2)F(FAIL)

u32 bL_m[BC_SIZE];
i32 sD_m[BC_SIZE];
i32 sC_m[BC_SIZE];
i32 sA_m[BC_SIZE];

char* bc_repr(u32* p) {
  switch(*p) { default: return "(unknown)";
    #define F(X) case X: return #X;
    FOR_BC(F)
    #undef F
  }
}
void print_BC(u32* p, i32 w) {
  char* str = bc_repr(p);
  printf("%s", str);
  u32* n = nextBC(p);
  p++;
  i32 len = strlen(str);
  while(p!=n) {
    u32 c = (u32)*p++;
    char buf[8];
    i32 clen = 0;
    do {
      buf[clen++] = (c&15)>9? 'A'+(c&15)-10 : '0'+(c&15);
      c>>= 4;
    } while(c);
    putchar(' ');
    for (i32 i = 0; i < clen; i++) putchar(buf[clen-i-1]);
    len+= clen+1;
  }
  len = w-len;
  while(len-->0) putchar(' ');
}
void print_BCStream(u32* p) {
  while(true) {
    print_BC(p, 10); putchar(10);
    if (*p == RETD || *p == RETN) return;
    p = nextBC(p);
  }
}


B catchMessage;
u64 envPrevHeight;

Env* envCurr; // pointer to current environment; included to make for simpler current position updating
Env* envStart;
Env* envEnd;

B* gStack; // points to after end
B* gStackStart;
B* gStackEnd;
NOINLINE void gsReserveR(u64 am) { gsReserve(am); }
void print_gStack() {
  B* c = gStackStart;
  i32 i = 0;
  printf("gStack %p, height "N64d":\n", gStackStart, gStack-gStackStart);
  while (c!=gStack) {
    printf("  %d: ", i); fflush(stdout);
    print(*c); fflush(stdout);
    if (isVal(*c)) printf(", refc=%d", v(*c)->refc);
    printf("\n");
    fflush(stdout);
    c++;
    i++;
  }
}

static Body* m_body(i32 vam, i32 pos, u32 maxStack, u16 maxPSC) { // leaves varIDs and nsDesc uninitialized
  Body* body = mm_alloc(fsizeof(Body,varIDs,i32,vam), t_body);
  
  #if JIT_START != -1
    body->nvm = NULL;
    body->nvmRefs = m_f64(0);
  #endif
  #if JIT_START > 0
    body->callCount = 0;
  #endif
  body->bcTmp = pos;
  body->maxStack = maxStack;
  body->maxPSC = maxPSC;
  body->bl = NULL;
  body->varAm = (u16)vam;
  return body;
}
typedef struct NextRequest {
  u32 off; // offset into bytecode where the two integers must be inserted
  u32 pos1; // offset into bodyI/bodyMap of what's wanted for monadic
  u32 pos2; // ↑ for dyadic; U32_MAX if not wanted
} NextRequest;
Block* compileBlock(B block, Comp* comp, bool* bDone, u32* bc, usz bcIA, B allBlocks, B allBodies, B nameList, Scope* sc, i32 depth) {
  usz blIA = a(block)->ia;
  if (blIA!=3) thrM("VM compiler: Bad block info size");
  SGetU(block)
  usz  ty  = o2s(GetU(block,0)); if (ty>2) thrM("VM compiler: Bad type");
  bool imm = o2b(GetU(block,1));
  B    bodyObj = GetU(block,2);
  i32 argAm = argCount(ty, imm);
  
  i32* bodyI;
  i32 bodyAm1, bodyAm2, bodyILen;
  if (isArr(bodyObj)) {
    usz boia = a(bodyObj)->ia;
    if (boia!=1 && boia!=2) thrM("VM compiler: Unexpected body list length");
    // print(bodyObj); putchar('\n');
    SGetU(bodyObj)
    B b1 =               GetU(bodyObj,0);
    B b2 = boia==1? b1 : GetU(bodyObj,1);
    if (!isArr(b1) || !isArr(b2)) thrM("VM compiler: Body list contained non-arrays");
    bodyAm1 = a(b1)->ia; SGetU(b1)
    bodyAm2 = a(b2)->ia; SGetU(b2)
    bodyILen = bodyAm1+bodyAm2;
    TALLOC(i32, bodyInds_, bodyILen+2); bodyI = bodyInds_; i32* bodyI2 = bodyInds_+bodyAm1+1;
    for (i32 i = 0; i < bodyAm1; i++) bodyI [i] = o2i(GetU(b1, i));
    for (i32 i = 0; i < bodyAm2; i++) bodyI2[i] = o2i(GetU(b2, i));
    for (i32 i = 1; i < bodyAm1; i++) if (bodyI [i]<=bodyI [i-1]) thrM("VM compiler: Expected body indices to be sorted");
    for (i32 i = 1; i < bodyAm2; i++) if (bodyI2[i]<=bodyI2[i-1]) thrM("VM compiler: Expected body indices to be sorted");
    bodyI[bodyAm1] = bodyI[bodyILen+1] = I32_MAX;
  } else {
    bodyILen = 2;
    TALLOC(i32, bodyInds_, bodyILen+2); bodyI = bodyInds_;
    bodyI[0] = bodyI[2] = o2i(bodyObj);
    bodyI[1] = bodyI[3] = I32_MAX;
    bodyAm1 = 1;
    bodyAm2 = 1;
  }
  // for (int i = 0; i < bodyILen+2; i++) printf("%d ", bodyI[i]); putchar('\n'); printf("things: %d %d\n", bodyAm1, bodyAm2);
  TSALLOC(i32, newBC, 20); // transformed bytecode
  TSALLOC(i32, mapBC, 20); // map of original bytecode to transformed
  TSALLOC(Block*, usedBlocks, 2); // list of blocks to be referenced by DFND, stored in result->blocks
  TSALLOC(Body*, bodies, 2); // list of bodies of this block
  TALLOC(Body*, bodyMap, bodyILen+2); // map from index in bodyI to the corresponding body
  TSALLOC(NextRequest, bodyReqs, 10); // list of SETH/PRED-s to fill out when bodyMap is complete
  
  i32 pos1 = 0; // pos1 and pos2 always stay valid indexes in bodyI because bodyI is padded with -1s
  i32 pos2 = bodyAm1+1;
  i32 index1 = -1;
  i32 index2 = -1;
  if (bodyAm1==0 || bodyAm2==0) {
    i32 sz = TSSIZE(bodies);
    if (bodyAm1==0) index1 = sz;
    if (bodyAm2==0) index2 = sz;
    i32 bcStart = TSSIZE(newBC);
    TSADD(newBC, FAIL);
    TSADD(mapBC, 0);
    
    Body* body = m_body(6, bcStart, 1, 0);
    body->nsDesc = NULL;
    TSADD(bodies, body);
  }
  bodyMap[bodyAm1] = bodyMap[bodyILen+1] = NULL;
  
  while (true) {
    i32 curr1 = bodyI[pos1];
    i32 curr2 = bodyI[pos2];
    i32 currBody = curr1<curr2? curr1 : curr2;
    if (currBody==I32_MAX) break;
    // printf("step %d %d:  %d %d %d\n", pos1, pos2, curr1, curr2, currBody);
    u64 bodyIdx = TSSIZE(bodies);
    bool is1 = curr1==currBody; if (is1) { if (index1==-1) index1=bodyIdx; pos1++; }
    bool is2 = curr2==currBody; if (is2) { if (index2==-1) index2=bodyIdx; pos2++; }
    // printf("idxs: %d %d\n", index1, index2);
    
    
    B bodyRepr = IGetU(allBodies, currBody); if (!isArr(bodyRepr)) thrM("VM compiler: Body array contained non-array");
    usz boIA = a(bodyRepr)->ia; if (boIA!=2 && boIA!=4) thrM("VM compiler: Body array had invalid length");
    SGetU(bodyRepr)
    usz idx = o2s(GetU(bodyRepr,0)); if (idx>=bcIA) thrM("VM compiler: Bytecode index out of bounds");
    usz vam = o2s(GetU(bodyRepr,1)); if (vam!=(u16)vam) thrM("VM compiler: >2⋆16 variables not supported"); // TODO any reason for this? 2⋆32 vars should just work, no? // oh, some size fields are u16s. but i doubt those change much, or even make things worse
    
    i32 h = 0; // stack height
    i32 hM = 0; // max stack height
    i32 mpsc = 0;
    if (depth==0 && sc && vam > sc->varAm) {
      if (boIA==2) thrM("VM compiler: Full block info must be provided for extending scopes");
      u32 regAm = sc->varAm;
      ScopeExt* oE = sc->ext;
      if (oE==NULL || vam > regAm+oE->varAm) {
        i32 nSZ = vam - regAm;
        ScopeExt* nE = mm_alloc(fsizeof(ScopeExt, vars, B, nSZ*2), t_scopeExt);
        nE->varAm = nSZ;
        i32 oSZ = 0;
        if (oE) {
          oSZ = oE->varAm;
          memcpy(nE->vars    , oE->vars    , oSZ*sizeof(B));
          memcpy(nE->vars+nSZ, oE->vars+oSZ, oSZ*sizeof(B));
          mm_free((Value*)oE);
        }
        B varIDs = GetU(bodyRepr,2);
        for (i32 i = oSZ; i < nSZ; i++) {
          nE->vars[i] = bi_noVar;
          nE->vars[i+nSZ] = IGet(nameList, o2s(IGetU(varIDs, regAm+i)));
        }
        sc->ext = nE;
      }
    }
    i32 bcStart = TSSIZE(newBC);
    u32* c;
    
    bool remapArgs = false;
    c = bc+idx;
    while (*c!=RETN & *c!=RETD) {
      if (*c==PRED) { remapArgs = true; break; }
      c = nextBC(c);
    }
    if (remapArgs) {
      if (sc && depth==0) thrM("Predicates cannot be used directly in a REPL");
      c = bc+idx;
      bool argUsed[6] = {0,0,0,0,0,0};
      while (*c!=RETN & *c!=RETD) {
        if (*c==VARO | *c==VARM | *c==VARU) if (c[1]==0 && c[2]<argAm) argUsed[c[2]] = true;
        c = nextBC(c);
      }
      for (i32 i = 0; i < 6; i++) if (argUsed[i]) {
        TSADDA(newBC, ((u32[]){ VARO,0,i, VARM,0,vam+i, SETN, POPS }), 8);
        TSADDA(mapBC, ((u32[]){ 0,0,0,    0,0,0,        0   , 0    }), 8);
      }
    }
    
    c = bc+idx;
    while (true) {
      u32* n = nextBC(c);
      if (n-bc-1 >= bcIA) thrM("VM compiler: No RETN/RETD found before end of bytecode");
      bool ret = false;
      #define A64(X) { u64 a64=(X); TSADD(newBC, (u32)a64); TSADD(newBC, a64>>32); }
      switch (*c) {
        case PUSH:;
          B obj = comp->objs->a[c[1]];
          TSADD(newBC, isVal(obj)? ADDI : ADDU);
          A64(obj.u);
          break;
        case RETN: if(h!=1) thrM("VM compiler: RETN expected to be called with one item on the stack");
          TSADD(newBC, RETN);
          ret = true;
          break;
        case RETD: if(h!=1&h!=0) thrM("VM compiler: RETD expected to be called with no more than 1 item on the stack");
          if (h==1) TSADD(newBC, POPS);
          TSADD(newBC, RETD);
          ret = true;
          break;
        case DFND: {
          u32 id = c[1];
          if ((u32)id >= a(allBlocks)->ia) thrM("VM compiler: DFND index out-of-bounds");
          if (bDone[id]) thrM("VM compiler: DFND of the same block in multiple places");
          bDone[id] = true;
          Block* bl = compileBlock(IGetU(allBlocks,id), comp, bDone, bc, bcIA, allBlocks, allBodies, nameList, sc, depth+1);
          TSADD(newBC, bl->ty==0? DFND0 : bl->ty==1? DFND1 : DFND2);
          A64((u64)bl);
          TSADD(usedBlocks, bl);
          break;
        }
        case VARO: case VARM: case VARU: {
          i32 ins = c[0];
          i32 cdepth = c[1];
          i32 cpos = c[2];
          if (cdepth+1 > mpsc) mpsc = cdepth+1;
          if (sc && cdepth>=depth) {
            Scope* csc = sc;
            for (i32 i = depth; i < cdepth; i++) if (!(csc = csc->psc)) thrM("VM compiler: VAR_ has an out-of-bounds depth");
            if (cpos >= csc->varAm) {
              cpos-= csc->varAm;
              ins = ins==VARO? EXTO : ins==VARM? EXTM : EXTO;
            }
          }
          if (remapArgs && cpos<argAm && cdepth==0) cpos+= vam;
          TSADD(newBC, ins);
          TSADD(newBC, cdepth);
          TSADD(newBC, cpos);
          break;
        }
        case SETH: case PRED:
          if (*c==PRED && h!=1) thrM("VM compiler: PRED expected to be called with one item on the stack");
          if (mpsc<1) mpsc=1; // SETH and PRED may want to have a parent scope pointer
          TSADD(newBC, *c==SETH? SETHi : imm? PRED1 : PRED2);
          TSADD(bodyReqs, ((NextRequest){.off = TSSIZE(newBC), .pos1 = pos1, .pos2 = imm? U32_MAX : pos2}));
          A64(0); if(*c==SETH || !imm)A64(0); // to be filled in by later bodyReqs handling
          break;
        default: {
          u32* ccpy = c;
          while (ccpy!=n) TSADD(newBC, *ccpy++);
          break;
        }
      }
      #undef A64
      usz nlen = TSSIZE(newBC)-TSSIZE(mapBC);
      for (usz i = 0; i < nlen; i++) TSADD(mapBC, c-bc);
      h+= stackDiff(c);
      if (h<0) thrM("VM compiler: Stack size goes negative");
      if (h>hM) hM = h;
      if (ret) break;
      c = n;
    }
    
    if (mpsc>U16_MAX) thrM("VM compiler: Block too deep");
    
    Body* body = m_body(vam+(remapArgs? argAm : 0), bcStart, (u32)hM, mpsc);
    if (boIA>2) {
      m_nsDesc(body, imm, ty, inc(nameList), GetU(bodyRepr,2), GetU(bodyRepr,3));
    } else {
      body->nsDesc = NULL;
      for (u64 i = 0; i < vam; i++) body->varIDs[i] = -1;
    }
    
    TSADD(bodies, body);
    if (is1) bodyMap[pos1-1] = body;
    if (is2) bodyMap[pos2-1] = body;
  }
  u64 bodyReqAm = TSSIZE(bodyReqs);
  for (u64 i = 0; i < bodyReqAm; i++) {
    NextRequest r = bodyReqs[i];
    /*ugly, but whatever*/ u64 v1 = (u64)bodyMap[r.pos1]; newBC[r.off+0] = (u32)v1; newBC[r.off+1] = v1>>32;
    if (r.pos2!=U32_MAX) { u64 v2 = (u64)bodyMap[r.pos2]; newBC[r.off+2] = (u32)v2; newBC[r.off+3] = v2>>32; }
  }
  TSFREE(bodyReqs);
  TFREE(bodyMap);
  TFREE(bodyI);
  
  usz blC = TSSIZE(usedBlocks);
  BlBlocks* nBl = NULL;
  if (blC) {
    nBl = mm_alloc(fsizeof(BlBlocks,a,Block*,blC), t_blBlocks);
    nBl->am = blC;
    memcpy(nBl->a, usedBlocks, blC*sizeof(Block*));
  }
  TSFREE(usedBlocks);
  
  usz nbcC = TSSIZE(newBC); i32* nbc; m_i32arrv(&nbc, nbcC); memcpy(nbc, newBC, nbcC*4); TSFREE(newBC);
  usz mapC = TSSIZE(mapBC); i32* map; m_i32arrv(&map, mapC); memcpy(map, mapBC, mapC*4); TSFREE(mapBC);
  
  i32 bodyCount = TSSIZE(bodies);
  Block* bl = mm_alloc(fsizeof(Block,bodies,Body*,bodyCount), t_block);
  bl->comp = comp; ptr_inc(comp);
  bl->ty = (u8)ty;
  bl->bc = nbc;
  bl->blocks = nBl==NULL? NULL : nBl->a;
  bl->map = map;
  bl->imm = imm;
  
  bl->bodyCount = bodyCount;
  if (index1 != 0) { // this is a _mess_
    i32 sw0 = 0; i32 sw1 = index1;
    
    Body* t = bodies[sw0]; bodies[sw0] = bodies[sw1]; bodies[sw1] = t;
    index1 = sw0;
    
    if      (index2==sw0) index2 = sw1;
    else if (index2==sw1) index2 = sw0;
  }
  for (i32 i = 0; i < bodyCount; i++) {
    bl->bodies[i] = bodies[i];
    bodies[i]->bc = (u32*)nbc + bodies[i]->bcTmp;
    bodies[i]->bl = bl;
  }
  bl->dyBody = bodies[index2];
  TSFREE(bodies);
  return bl;
}

// consumes all; assumes arguments are valid (verifies some stuff, but definitely not everything)
// if sc isn't NULL, this block must only be evaluated directly in that scope precisely once
NOINLINE Block* compile(B bcq, B objs, B allBlocks, B allBodies, B indices, B tokenInfo, B src, B path, Scope* sc) {
  usz bIA = a(allBlocks)->ia;
  I32Arr* bca = toI32Arr(bcq);
  u32* bc = (u32*)bca->a;
  usz bcIA = bca->ia;
  Comp* comp = mm_alloc(sizeof(Comp), t_comp);
  comp->bc = taga(bca);
  comp->indices = indices;
  comp->src = src;
  comp->path = path;
  HArr* objArr = comp->objs = cpyHArr(objs);
  usz objAm = objArr->ia;
  for (usz i = 0; i < objAm; i++) objArr->a[i] = squeeze_deep(objArr->a[i]);
  comp->blockAm = 0;
  B nameList;
  if (q_N(tokenInfo)) {
    nameList = bi_emptyHVec;
  } else {
    B t = IGetU(tokenInfo,2);
    nameList = IGetU(t,0);
  }
  if (!q_N(src) && !q_N(indices)) {
    if (isAtm(indices) || rnk(indices)!=1 || a(indices)->ia!=2) thrM("VM compiler: Bad indices");
    for (i32 i = 0; i < 2; i++) {
      B ind = IGetU(indices,i);
      if (isAtm(ind) || rnk(ind)!=1 || a(ind)->ia!=bcIA) thrM("VM compiler: Bad indices");
      SGetU(ind)
      for (usz j = 0; j < bcIA; j++) o2i(GetU(ind,j));
    }
  }
  TALLOC(bool,bDone,bIA);
  for (usz i = 0; i < bIA; i++) bDone[i] = false;
  Block* ret = compileBlock(IGetU(allBlocks, 0), comp, bDone, bc, bcIA, allBlocks, allBodies, nameList, sc, 0);
  TFREE(bDone);
  ptr_dec(comp); dec(allBlocks); dec(allBodies); dec(tokenInfo);
  return ret;
}




NOINLINE void v_setR(Scope* pscs[], B s, B x, bool upd) {
  if (isExt(s)) {
    Scope* sc = pscs[(u16)(s.u>>32)];
    B prev = sc->ext->vars[(u32)s.u];
    if (upd) {
      if (prev.u==bi_noVar.u) thrM("↩: Updating undefined variable");
      dec(prev);
    } else dec(prev);
    sc->ext->vars[(u32)s.u] = inc(x);
  } else {
    VTY(s, t_harr);
    B* sp = harr_ptr(s);
    usz ia = a(s)->ia;
    if (isAtm(x) || !eqShape(s, x)) {
      if (!isNsp(x)) thrM("Assignment: Mismatched shape for spread assignment");
      for (u64 i = 0; i < ia; i++) {
        B c = sp[i];
        if (isVar(c)) {
          Scope* sc = pscs[(u16)(c.u>>32)];
          i32 nameID = sc->body->varIDs[(u32)c.u];
          v_set(pscs, c, ns_getU(x, sc->body->nsDesc->nameList, nameID), upd);
        } else if (isExt(c)) {
          ScopeExt* ext = pscs[(u16)(c.u>>32)]->ext;
          v_set(pscs, c, ns_getNU(x, ext->vars[(u32)c.u + ext->varAm], true), upd);
        } else if (isObj(c)) {
          assert(v(c)->type == t_fldAlias);
          Scope* sc = pscs[0];
          FldAlias* cf = c(FldAlias,c);
          v_set(pscs, cf->obj, ns_getU(x, sc->body->nsDesc->nameList, cf->p), upd);
        } else thrM("Assignment: extracting non-name from namespace");
      }
      return;
    }
    SGetU(x)
    for (u64 i = 0; i < ia; i++) v_set(pscs, sp[i], GetU(x,i), upd);
  }
}
NOINLINE bool v_sethR(Scope* pscs[], B s, B x) {
  assert(isVal(s));
  if (v(s)->type==t_vfyObj) return equal(c(VfyObj,s)->obj,x);
  VTY(s, t_harr);
  B* sp = harr_ptr(s);
  usz ia = a(s)->ia;
  if (isAtm(x) || !eqShape(s, x)) {
    if (!isNsp(x)) return false;
    for (u64 i = 0; i < ia; i++) {
      B c = sp[i];
      if (isVar(c)) {
        Scope* sc = pscs[(u16)(c.u>>32)];
        i32 nameID = sc->body->varIDs[(u32)c.u];
        B g = ns_qgetU(x, sc->body->nsDesc->nameList, nameID);
        if (q_N(g) || !v_seth(pscs, c, g)) return false;
      } else if (isObj(c) && v(c)->type==t_fldAlias) {
        assert(v(c)->type == t_fldAlias);
        Scope* sc = pscs[0];
        FldAlias* cf = c(FldAlias,c);
        B g = ns_qgetU(x, sc->body->nsDesc->nameList, cf->p);
        if (q_N(g) || !v_seth(pscs, cf->obj, g)) return false;
      } else return false;
    }
    return true;
  }
  SGetU(x)
  for (u64 i = 0; i < ia; i++) if (!v_seth(pscs, sp[i], GetU(x,i))) return false;
  return true;
}



NOINLINE B v_getR(Scope* pscs[], B s) {
  if (isExt(s)) {
    Scope* sc = pscs[(u16)(s.u>>32)];
    B r = sc->ext->vars[(u32)s.u];
    sc->ext->vars[(u32)s.u] = bi_optOut;
    return r;
  } else {
    VTY(s, t_harr);
    usz ia = a(s)->ia;
    B* sp = harr_ptr(s);
    HArr_p r = m_harrUv(ia);
    for (u64 i = 0; i < ia; i++) r.a[i] = v_get(pscs, sp[i]);
    return r.b;
  }
}

FORCE_INLINE B gotoNextBody(Block* bl, Scope* sc, Body* body) {
  if (body==NULL) thrF("No header matched argument%S", q_N(sc->vars[2])?"":"s");
  
  popEnv();
  
  i32 ga = blockGivenVars(bl);
  
  for (u64 i = 0; i < ga; i++) inc(sc->vars[i]);
  Scope* nsc = m_scope(body, sc->psc, body->varAm, ga, sc->vars);
  scope_dec(sc);
  return execBodyInlineI(bl, body, nsc);
}

#ifdef DEBUG_VM
i32 bcDepth=-2;
i32* vmStack;
i32 bcCtr = 0;
#endif
#define BCPOS(B,P) (B->bl->map[(P)-(u32*)B->bl->bc])
B evalBC(Block* bl, Body* b, Scope* sc) { // doesn't consume
  #ifdef DEBUG_VM
    bcDepth+= 2;
    if (!vmStack) vmStack = malloc(400);
    i32 stackNum = bcDepth>>1;
    vmStack[stackNum] = -1;
    printf("new eval\n");
    B* origStack = gStack;
  #endif
  B* objs = bl->comp->objs->a;
  u32* bc = b->bc;
  pushEnv(sc, bc);
  gsReserve(b->maxStack);
  Scope* pscs[b->maxPSC];
  if (b->maxPSC) {
    pscs[0] = sc;
    for (i32 i = 1; i < b->maxPSC; i++) pscs[i] = pscs[i-1]->psc;
  }
  #ifdef GS_REALLOC
    #define POP (*--gStack)
    #define P(N) B N=POP;
    #define ADD(X) { B tr=X; *(gStack++) = tr; }
    #define PEEK(X) gStack[-(X)]
    #define STACK_HEIGHT 
    #define GS_UPD
  #else
    B* lgStack = gStack;
    #define POP (*--lgStack)
    #define P(N) B N=POP;
    #define ADD(X) { *(lgStack++) = X; } // fine, as, if an error occurs, lgStack is ignored anyways
    #define PEEK(X) lgStack[-(X)]
    #define GS_UPD { gStack = lgStack; }
  #endif
  #define L64 ({ u64 r = bc[0] | ((u64)bc[1])<<32; bc+= 2; r; })
  #if VM_POS
    #define POS_UPD envCurr->pos = (u64)(bc-1);
  #else
    #define POS_UPD
  #endif
  
  while(true) {
    #ifdef DEBUG_VM
      u32* sbc = bc;
      i32 bcPos = BCPOS(b,sbc);
      vmStack[stackNum] = bcPos;
      for(i32 i = 0; i < bcDepth; i++) printf(" ");
      print_BC(sbc,20); printf("@%d  in: ", bcPos);
      for (i32 i = 0; i < lgStack-origStack; i++) { if(i)printf("; "); print(origStack[i]); } putchar('\n'); fflush(stdout);
      bcCtr++;
      for (i32 i = 0; i < sc->varAm; i++) VALIDATE(sc->vars[i]);
    #endif
    switch(*bc++) {
      case POPS: dec(POP); break;
      case PUSH: {
        ADD(inc(objs[*bc++]));
        break;
      }
      case ADDI: {
        ADD(incG(b(L64)));
        break;
      }
      case ADDU: {
        ADD(b(L64));
        break;
      }
      case FN1C: { P(f)P(x)
        GS_UPD;POS_UPD;
        ADD(c1(f, x)); dec(f);
        break;
      }
      case FN1O: { P(f)P(x)
        GS_UPD;POS_UPD;
        ADD(q_N(x)? x : c1(f, x)); dec(f);
        break;
      }
      case FN2C: { P(w)P(f)P(x)
        GS_UPD;POS_UPD;
        ADD(c2(f, w, x)); dec(f);
        break;
      }
      case FN2O: { P(w)P(f)P(x)
        GS_UPD;POS_UPD;
        if (q_N(x)) { dec(w); ADD(x); }
        else ADD(q_N(w)? c1(f, x) : c2(f, w, x));
        dec(f);
        break;
      }
      case ARRO: case ARRM: {
        u32 sz = *bc++;
        if (sz==0) {
          ADD(emptyHVec());
        } else {
          HArr_p r = m_harrUv(sz);
          bool allNum = true;
          for (i64 i = 0; i < sz; i++) if (!isNum(r.a[sz-i-1] = POP)) allNum = false;
          if (allNum) {
            GS_UPD;
            ADD(withFill(r.b, m_f64(0)));
          } else ADD(r.b);
        }
        break;
      }
      case DFND0: { GS_UPD;POS_UPD; ADD(m_funBlock((Block*)L64, sc)); break; }
      case DFND1: { GS_UPD;POS_UPD; ADD(m_md1Block((Block*)L64, sc)); break; }
      case DFND2: { GS_UPD;POS_UPD; ADD(m_md2Block((Block*)L64, sc)); break; }
      // case DFND: {
      //   GS_UPD;POS_UPD;
      //   Block* cbl = blocks[*bc++];
      //   switch(cbl->ty) { default: UD;
      //     case 0: ADD(m_funBlock(cbl, sc)); break;
      //     case 1: ADD(m_md1Block(cbl, sc)); break;
      //     case 2: ADD(m_md2Block(cbl, sc)); break;
      //   }
      //   break;
      // }
      case MD1C: { P(f)P(m)     GS_UPD;POS_UPD; ADD(m1_d  (m,f  )); break; }
      case MD2C: { P(f)P(m)P(g) GS_UPD;POS_UPD; ADD(m2_d  (m,f,g)); break; }
      case MD2R: {     P(m)P(g)                 ADD(m2_h  (m,  g)); break; }
      case TR2D: {     P(g)P(h)                 ADD(m_atop(  g,h)); break; }
      case TR3D: { P(f)P(g)P(h)                 ADD(m_fork(f,g,h)); break; }
      case TR3O: { P(f)P(g)P(h)
        if (q_N(f)) { ADD(m_atop(g,h)); dec(f); }
        else ADD(m_fork(f,g,h));
        break;
      }
      case VARM: { u32 d = *bc++; u32 p = *bc++;
        ADD(tag((u64)d<<32 | (u32)p, VAR_TAG));
        break;
      }
      case VARO: { u32 d = *bc++; u32 p = *bc++;
        B l = pscs[d]->vars[p];
        if(l.u==bi_noVar.u) { POS_UPD; thrM("Reading variable before its defined"); }
        ADD(inc(l));
        break;
      }
      case VARU: { u32 d = *bc++; u32 p = *bc++;
        B* vars = pscs[d]->vars;
        ADD(vars[p]);
        vars[p] = bi_optOut;
        break;
      }
      case EXTM: { u32 d = *bc++; u32 p = *bc++;
        ADD(tag((u64)d<<32 | (u32)p, EXT_TAG));
        break;
      }
      case EXTO: { u32 d = *bc++; u32 p = *bc++;
        B l = pscs[d]->ext->vars[p];
        if(l.u==bi_noVar.u) { POS_UPD; thrM("Reading variable before its defined"); }
        ADD(inc(l));
        break;
      }
      case EXTU: { u32 d = *bc++; u32 p = *bc++;
        B* vars = pscs[d]->ext->vars;
        ADD(vars[p]);
        vars[p] = bi_optOut;
        break;
      }
      case SETN: { P(s)    P(x) GS_UPD; POS_UPD; v_set(pscs, s, x, false); dec(s); ADD(x); break; }
      case SETU: { P(s)    P(x) GS_UPD; POS_UPD; v_set(pscs, s, x, true ); dec(s); ADD(x); break; }
      case SETM: { P(s)P(f)P(x) GS_UPD; POS_UPD;
        B w = v_get(pscs, s);
        B r = c2(f,w,x); dec(f);
        v_set(pscs, s, r, true); dec(s);
        ADD(r);
        break;
      }
      case SETC: { P(s)P(f) GS_UPD; POS_UPD;
        B x = v_get(pscs, s);
        B r = c1(f,x); dec(f);
        v_set(pscs, s, r, true); dec(s);
        ADD(r);
        break;
      }
      case SETHi:{ P(s)    P(x) GS_UPD; POS_UPD; u64 v1 = L64; u64 v2 = L64;
        bool ok = v_seth(pscs, s, x); dec(x); dec(s);
        if (!ok) { GS_UPD; return gotoNextBody(bl, sc, (Body*)(q_N(sc->vars[2])? v1 : v2)); }
        break;
      }
      case PRED1:{ P(x) GS_UPD; POS_UPD; u64 v1 = L64;
        if (!o2b(x)) { GS_UPD; return gotoNextBody(bl, sc, (Body*)v1); }
        break;
      }
      case PRED2:{ P(x) GS_UPD; POS_UPD; u64 v1 = L64; u64 v2 = L64;
        if (!o2b(x)) { GS_UPD; return gotoNextBody(bl, sc, (Body*)(q_N(sc->vars[2])? v1 : v2)); }
        break;
      }
      case FLDO: { P(ns) GS_UPD; u32 p = *bc++; POS_UPD;
        if (!isNsp(ns)) thrM("Trying to read a field from non-namespace");
        ADD(inc(ns_getU(ns, sc->body->nsDesc->nameList, p)));
        dec(ns);
        break;
      }
      case RETD: {
        ptr_inc(sc);
        ptr_inc(b->nsDesc);
        ADD(m_ns(sc, b->nsDesc));
        goto end;
      }
      case ALIM: { P(o) u32 l = *bc++;
        FldAlias* a = mm_alloc(sizeof(FldAlias), t_fldAlias);
        a->obj = o;
        a->p = l;
        ADD(tag(a,OBJ_TAG));
        break;
      }
      case RETN: goto end;
      case CHKV: {
        if (q_N(PEEK(1))) { GS_UPD; POS_UPD; thrM("Unexpected Nothing (·)"); }
        break;
      }
      case VFYM: { P(o)
        VfyObj* a = mm_alloc(sizeof(VfyObj), t_vfyObj);
        a->obj = o;
        ADD(tag(a,OBJ_TAG));
        break;
      }
      case FAIL: thrM(q_N(sc->vars[2])? "This block cannot be called monadically" : "This block cannot be called dyadically");
      // not implemented: DYNO DYNM FLDM SYSV
      default:
        #ifdef DEBUG
          printf("todo %d\n", bc[-1]); bc++; break;
        #else
          UD;
        #endif
    }
    #ifdef DEBUG_VM
      for(i32 i = 0; i < bcDepth; i++) printf(" ");
      print_BC(sbc,20); printf("@%d out: ", BCPOS(b, sbc));
      for (i32 i = 0; i < lgStack-origStack; i++) { if(i)printf("; "); print(origStack[i]); } putchar('\n'); fflush(stdout);
    #endif
  }
  end:;
  #ifdef DEBUG_VM
    bcDepth-= 2;
  #endif
  B r = POP;
  GS_UPD;
  popEnv();
  scope_dec(sc);
  return r;
  #undef L64
  #undef P
  #undef ADD
  #undef POP
  #undef POS_UPD
  #undef GS_UPD
}

Scope* m_scope(Body* body, Scope* psc, u16 varAm, i32 initVarAm, B* initVars) { // consumes initVarAm items of initVars
  Scope* sc = mm_alloc(fsizeof(Scope, vars, B, varAm), t_scope);
  sc->body = body; ptr_inc(body);
  sc->psc = psc; if(psc) ptr_inc(psc);
  sc->varAm = varAm;
  sc->ext = NULL;
  i32 i = 0;
  while (i<initVarAm) { sc->vars[i] = initVars[i]; i++; }
  while (i<varAm) sc->vars[i++] = bi_noVar;
  return sc;
}

B execBlockInline(Block* block, Scope* sc) { ptr_inc(sc); return execBodyInlineI(block, block->bodies[0], sc); }

FORCE_INLINE B execBlock(Block* block, Body* body, Scope* psc, i32 ga, B* svar) { // consumes svar contents
  u16 varAm = body->varAm;
  assert(varAm>=ga);
  assert(ga == blockGivenVars(block));
  Scope* sc = m_scope(body, psc, varAm, ga, svar);
  B r = execBodyInlineI(block, body, sc);
  return r;
}

B funBl_c1(B     t,      B x) { FunBlock* b=c(FunBlock, t    ); ptr_inc(b); return execBlock(b->bl, b->bl->bodies[0], b->sc, 3, (B[]){t,              x, bi_N                                  }); }
B funBl_c2(B     t, B w, B x) { FunBlock* b=c(FunBlock, t    ); ptr_inc(b); return execBlock(b->bl, b->bl->dyBody,    b->sc, 3, (B[]){t,              x, w                                     }); }
B md1Bl_c1(Md1D* d,      B x) { Md1Block* b=c(Md1Block, d->m1); ptr_inc(d); return execBlock(b->bl, b->bl->bodies[0], b->sc, 5, (B[]){tag(d,FUN_TAG), x, bi_N, inc(d->m1), inc(d->f)           }); }
B md1Bl_c2(Md1D* d, B w, B x) { Md1Block* b=c(Md1Block, d->m1); ptr_inc(d); return execBlock(b->bl, b->bl->dyBody,    b->sc, 5, (B[]){tag(d,FUN_TAG), x, w   , inc(d->m1), inc(d->f)           }); }
B md2Bl_c1(Md2D* d,      B x) { Md2Block* b=c(Md2Block, d->m2); ptr_inc(d); return execBlock(b->bl, b->bl->bodies[0], b->sc, 6, (B[]){tag(d,FUN_TAG), x, bi_N, inc(d->m2), inc(d->f), inc(d->g)}); }
B md2Bl_c2(Md2D* d, B w, B x) { Md2Block* b=c(Md2Block, d->m2); ptr_inc(d); return execBlock(b->bl, b->bl->dyBody,    b->sc, 6, (B[]){tag(d,FUN_TAG), x, w   , inc(d->m2), inc(d->f), inc(d->g)}); }

B md1Bl_d(B m, B f     ) { Md1Block* c = c(Md1Block,m); Block* bl=c(Md1Block, m)->bl; return c->bl->imm? execBlock(bl, bl->bodies[0], c(Md1Block, m)->sc, 2, (B[]){m, f   }) : m_md1D(m,f  ); }
B md2Bl_d(B m, B f, B g) { Md2Block* c = c(Md2Block,m); Block* bl=c(Md2Block, m)->bl; return c->bl->imm? execBlock(bl, bl->bodies[0], c(Md2Block, m)->sc, 3, (B[]){m, f, g}) : m_md2D(m,f,g); }

B m_funBlock(Block* bl, Scope* psc) { // doesn't consume anything
  if (bl->imm) return execBlock(bl, bl->bodies[0], psc, 0, NULL);
  FunBlock* r = mm_alloc(sizeof(FunBlock), t_fun_block);
  r->bl = bl; ptr_inc(bl);
  r->sc = psc; ptr_inc(psc);
  r->c1 = funBl_c1;
  r->c2 = funBl_c2;
  return tag(r,FUN_TAG);
}
B m_md1Block(Block* bl, Scope* psc) {
  Md1Block* r = mm_alloc(sizeof(Md1Block), t_md1_block);
  r->bl = bl; ptr_inc(bl);
  r->sc = psc; ptr_inc(psc);
  r->c1 = md1Bl_c1;
  r->c2 = md1Bl_c2;
  return tag(r,MD1_TAG);
}
B m_md2Block(Block* bl, Scope* psc) {
  Md2Block* r = mm_alloc(sizeof(Md2Block), t_md2_block);
  r->bl = bl; ptr_inc(bl);
  r->sc = psc; ptr_inc(psc);
  r->c1 = md2Bl_c1;
  r->c2 = md2Bl_c2;
  return tag(r,MD2_TAG);
}

DEF_FREE(scope) {
  Scope* c = (Scope*)x;
  if (LIKELY(c->psc!=NULL)) ptr_decR(c->psc);
  if (RARE  (c->ext!=NULL)) ptr_decR(c->ext);
  ptr_decR(c->body);
  u16 am = c->varAm;
  for (u32 i = 0; i < am; i++) dec(c->vars[i]);
}
DEF_FREE(body) {
  Body* c = (Body*)x;
  #if JIT_START!=-1
    if(c->nvm) nvm_free(c->nvm);
    dec(c->nvmRefs);
  #endif
  if(c->nsDesc) ptr_decR(c->nsDesc);
}
DEF_FREE(block) {
  Block* c = (Block*)x;
  ptr_decR(c->comp);
  if(c->blocks) ptr_decR(RFLD(c->blocks,BlBlocks,a));
  ptr_decR(RFLD(c->bc, I32Arr,a));
  ptr_decR(RFLD(c->map,I32Arr,a));
  i32 am = c->bodyCount;
  for (i32 i = 0; i < am; i++) ptr_decR(c->bodies[i]);
}
DEF_FREE(comp) { Comp*     c = (Comp    *)x; ptr_decR(c->objs); decR(c->bc); decR(c->src); decR(c->indices); decR(c->path); }
DEF_FREE(funBl) { FunBlock* c = (FunBlock*)x; ptr_dec(c->sc); ptr_decR(c->bl); }
DEF_FREE(md1Bl) { Md1Block* c = (Md1Block*)x; ptr_dec(c->sc); ptr_decR(c->bl); }
DEF_FREE(md2Bl) { Md2Block* c = (Md2Block*)x; ptr_dec(c->sc); ptr_decR(c->bl); }
DEF_FREE(alias) { dec(((FldAlias*)x)->obj); }
DEF_FREE(vfymO) { dec(((VfyObj*  )x)->obj); }
DEF_FREE(bBlks) { BlBlocks* c = (BlBlocks*)x; u16 am = c->am; for (i32 i = 0; i < am; i++) ptr_dec(c->a[i]); }
DEF_FREE(scExt) { ScopeExt* c = (ScopeExt*)x; u16 am = c->varAm*2; for (i32 i = 0; i < am; i++) dec(c->vars[i]); }

void scope_visit(Value* x) {
  Scope* c = (Scope*)x;
  if (c->psc) mm_visitP(c->psc);
  if (c->ext) mm_visitP(c->ext);
  mm_visitP(c->body);
  u16 am = c->varAm;
  for (u32 i = 0; i < am; i++) mm_visit(c->vars[i]);
}
void body_visit(Value* x) {
  Body* c = (Body*) x;
  #if JIT_START != -1
    mm_visit(c->nvmRefs);
  #endif
  if(c->nsDesc) mm_visitP(c->nsDesc);
}
void block_visit(Value* x) {
  Block* c = (Block*)x;
  mm_visitP(c->comp);
  if(c->blocks) mm_visitP(RFLD(c->blocks,BlBlocks,a));
  mm_visitP(RFLD(c->bc, I32Arr,a));
  mm_visitP(RFLD(c->map,I32Arr,a));
  i32 am = c->bodyCount;
  for (i32 i = 0; i < am; i++) mm_visitP(c->bodies[i]);
}
void  comp_visit(Value* x) { Comp*     c = (Comp    *)x; mm_visitP(c->objs); mm_visit(c->bc); mm_visit(c->src); mm_visit(c->indices); mm_visit(c->path); }
void funBl_visit(Value* x) { FunBlock* c = (FunBlock*)x; mm_visitP(c->sc); mm_visitP(c->bl); }
void md1Bl_visit(Value* x) { Md1Block* c = (Md1Block*)x; mm_visitP(c->sc); mm_visitP(c->bl); }
void md2Bl_visit(Value* x) { Md2Block* c = (Md2Block*)x; mm_visitP(c->sc); mm_visitP(c->bl); }
void alias_visit(Value* x) { mm_visit(((FldAlias*)x)->obj); }
void vfymO_visit(Value* x) { mm_visit(((VfyObj*  )x)->obj); }
void bBlks_visit(Value* x) { BlBlocks* c = (BlBlocks*)x; u16 am = c->am; for (i32 i = 0; i < am; i++) mm_visitP(c->a[i]); }
void scExt_visit(Value* x) { ScopeExt* c = (ScopeExt*)x; u16 am = c->varAm*2; for (i32 i = 0; i < am; i++) mm_visit(c->vars[i]); }

void comp_print (B x) { printf("(%p: comp)",v(x)); }
void body_print (B x) { printf("(%p: body varam=%d)",v(x),c(Body,x)->varAm); }
void block_print(B x) { printf("(%p: block)",v(x)); }
void scope_print(B x) { printf("(%p: scope; vars:",v(x));Scope*sc=c(Scope,x);for(u64 i=0;i<sc->varAm;i++){printf(" ");print(sc->vars[i]);}printf(")"); }
void alias_print(B x) { printf("(alias %d of ", c(FldAlias,x)->p); print(c(FldAlias,x)->obj); printf(")"); }
void vfymO_print(B x) { print(c(FldAlias,x)->obj); }
void bBlks_print(B x) { printf("(block list)"); }
void scExt_print(B x) { printf("(scope extension with %d vars)", c(ScopeExt,x)->varAm); }

// void funBl_print(B x) { printf("(%p: function"" block bl=%p sc=%p)",v(x),c(FunBlock,x)->bl,c(FunBlock,x)->sc); }
// void md1Bl_print(B x) { printf("(%p: 1-modifier block bl=%p sc=%p)",v(x),c(Md1Block,x)->bl,c(Md1Block,x)->sc); }
// void md2Bl_print(B x) { printf("(%p: 2-modifier block bl=%p sc=%p)",v(x),c(Md2Block,x)->bl,c(Md2Block,x)->sc); }
// void funBl_print(B x) { printf("(function"" block @%d)",c(FunBlock,x)->bl->body->map[0]); }
// void md1Bl_print(B x) { printf("(1-modifier block @%d)",c(Md1Block,x)->bl->body->map[0]); }
// void md2Bl_print(B x) { printf("(2-modifier block @%d)",c(Md2Block,x)->bl->body->map[0]); }
void funBl_print(B x) { printf("{function"" block}"); }
void md1Bl_print(B x) { printf("{1-modifier block}"); }
void md2Bl_print(B x) { printf("{2-modifier block}"); }

B block_decompose(B x) { return m_v2(m_i32(1), x); }

static usz pageSizeV;
usz getPageSize() {
  if (pageSizeV==0) pageSizeV = sysconf(_SC_PAGESIZE);
  return pageSizeV;
}
static void allocStack(void** curr, void** start, void** end, i32 elSize, i32 count) {
  usz ps = getPageSize();
  u64 sz = (elSize*count + ps-1)/ps * ps;
  assert(sz%elSize == 0);
  *curr = *start = mmap(NULL, sz+ps, PROT_READ|PROT_WRITE, MAP_NORESERVE|MAP_PRIVATE|MAP_ANON, -1, 0);
  *end = ((char*)*start)+sz;
  mprotect(*end, ps, PROT_NONE); // idk first way i found to force erroring on overflow
}
void print_vmStack() {
  #ifdef DEBUG_VM
    printf("vm stack:");
    for (i32 i = 0; i < (bcDepth>>1) + 1; i++) { printf(" %d", vmStack[i]); fflush(stdout); }
    printf("\n"); fflush(stdout);
  #endif
}



void comp_init() {
  TIi(t_comp     ,freeO) =  comp_freeO; TIi(t_comp     ,freeF) =  comp_freeF; TIi(t_comp     ,visit) = comp_visit;  TIi(t_comp     ,print) =  comp_print;
  TIi(t_body     ,freeO) =  body_freeO; TIi(t_body     ,freeF) =  body_freeF; TIi(t_body     ,visit) = body_visit;  TIi(t_body     ,print) =  body_print;
  TIi(t_block    ,freeO) = block_freeO; TIi(t_block    ,freeF) = block_freeF; TIi(t_block    ,visit) = block_visit; TIi(t_block    ,print) = block_print;
  TIi(t_scope    ,freeO) = scope_freeO; TIi(t_scope    ,freeF) = scope_freeF; TIi(t_scope    ,visit) = scope_visit; TIi(t_scope    ,print) = scope_print;
  TIi(t_scopeExt ,freeO) = scExt_freeO; TIi(t_scopeExt ,freeF) = scExt_freeF; TIi(t_scopeExt ,visit) = scExt_visit; TIi(t_scopeExt ,print) = scExt_print;
  TIi(t_blBlocks ,freeO) = bBlks_freeO; TIi(t_blBlocks ,freeF) = bBlks_freeF; TIi(t_blBlocks ,visit) = bBlks_visit; TIi(t_blBlocks ,print) = bBlks_print;
  TIi(t_fldAlias ,freeO) = alias_freeO; TIi(t_fldAlias ,freeF) = alias_freeF; TIi(t_fldAlias ,visit) = alias_visit; TIi(t_fldAlias ,print) = alias_print;
  TIi(t_vfyObj   ,freeO) = vfymO_freeO; TIi(t_vfyObj   ,freeF) = vfymO_freeF; TIi(t_vfyObj   ,visit) = vfymO_visit; TIi(t_vfyObj   ,print) = vfymO_print;
  TIi(t_fun_block,freeO) = funBl_freeO; TIi(t_fun_block,freeF) = funBl_freeF; TIi(t_fun_block,visit) = funBl_visit; TIi(t_fun_block,print) = funBl_print; TIi(t_fun_block,decompose) = block_decompose;
  TIi(t_md1_block,freeO) = md1Bl_freeO; TIi(t_md1_block,freeF) = md1Bl_freeF; TIi(t_md1_block,visit) = md1Bl_visit; TIi(t_md1_block,print) = md1Bl_print; TIi(t_md1_block,decompose) = block_decompose; TIi(t_md1_block,m1_d)=md1Bl_d;
  TIi(t_md2_block,freeO) = md2Bl_freeO; TIi(t_md2_block,freeF) = md2Bl_freeF; TIi(t_md2_block,visit) = md2Bl_visit; TIi(t_md2_block,print) = md2Bl_print; TIi(t_md2_block,decompose) = block_decompose; TIi(t_md2_block,m2_d)=md2Bl_d;
  #ifndef GS_REALLOC
    allocStack((void**)&gStack, (void**)&gStackStart, (void**)&gStackEnd, sizeof(B), GS_SIZE);
  #endif
  allocStack((void**)&envCurr, (void**)&envStart, (void**)&envEnd, sizeof(Env), ENV_SIZE);
  envCurr--;
  
  for (i32 i = 0; i < BC_SIZE; i++) bL_m[i] = sD_m[i] = sC_m[i] = sA_m[i] = 1<<29;
  
  // bytecode length map
  bL_m[FN1C]=1; bL_m[FN2C]=1; bL_m[FN1O]=1; bL_m[FN2O]=1;
  bL_m[MD1C]=1; bL_m[MD2C]=1; bL_m[MD2R]=1;
  bL_m[TR2D]=1; bL_m[TR3D]=1; bL_m[TR3O]=1;
  bL_m[SETN]=1; bL_m[SETU]=1; bL_m[SETM]=1; bL_m[SETH]=1; bL_m[SETC]=1;
  bL_m[POPS]=1; bL_m[CHKV]=1; bL_m[VFYM]=1; bL_m[RETN]=1; bL_m[RETD]=1;
  bL_m[FAIL]=1; bL_m[PRED]=1;
  
  bL_m[PUSH]=2; bL_m[DFND]=2; bL_m[ARRO]=2; bL_m[ARRM]=2;
  bL_m[DYNO]=2; bL_m[DYNM]=2; bL_m[FLDO]=2; bL_m[FLDM]=2;
  bL_m[SYSV]=2; bL_m[ALIM]=2;
  
  bL_m[VARO]=3; bL_m[VARM]=3; bL_m[VARU]=3;
  bL_m[EXTO]=3; bL_m[EXTM]=3; bL_m[EXTU]=3;
  bL_m[ADDI]=3; bL_m[ADDU]=3;
  bL_m[FN1Ci]=3; bL_m[FN1Oi]=3; bL_m[FN2Ci]=3; bL_m[DFND0]=3; bL_m[DFND1]=3; bL_m[DFND2]=3;
  bL_m[SETNi]=3; bL_m[SETUi]=3; bL_m[SETMi]=3; bL_m[SETCi]=3;
  bL_m[SETNv]=3; bL_m[SETUv]=3; bL_m[SETMv]=3; bL_m[SETCv]=3; bL_m[PRED1]=3;
  
  bL_m[FN2Oi]=5; bL_m[SETHi]=5; bL_m[PRED2]=5;
  
  // stack diff map
  sD_m[PUSH ]= 1; sD_m[DYNO ]= 1; sD_m[DYNM]= 1; sD_m[DFND]= 1; sD_m[VARO]= 1; sD_m[VARM]= 1; sD_m[DFND0]= 1; sD_m[DFND1]=1; sD_m[DFND2]=1;
  sD_m[VARU ]= 1; sD_m[EXTO ]= 1; sD_m[EXTM]= 1; sD_m[EXTU]= 1; sD_m[SYSV]= 1; sD_m[ADDI]= 1; sD_m[ADDU ]= 1;
  sD_m[FN1Ci]= 0; sD_m[FN1Oi]= 0; sD_m[CHKV]= 0; sD_m[VFYM]= 0; sD_m[FLDO]= 0; sD_m[FLDM]= 0; sD_m[RETD ]= 0; sD_m[ALIM ]=0;
  sD_m[FN2Ci]=-1; sD_m[FN2Oi]=-1; sD_m[FN1C]=-1; sD_m[FN1O]=-1; sD_m[MD1C]=-1; sD_m[TR2D]=-1; sD_m[POPS ]=-1; sD_m[MD2R ]=-1; sD_m[RETN]=-1; sD_m[PRED]=-1; sD_m[PRED1]=-1; sD_m[PRED2]=-1;
  sD_m[MD2C ]=-2; sD_m[TR3D ]=-2; sD_m[FN2C]=-2; sD_m[FN2O]=-2; sD_m[TR3O]=-2; sD_m[SETH]=-2; sD_m[SETHi]=-2;
  
  sD_m[SETN]=-1; sD_m[SETNi]= 0; sD_m[SETNv]=-1;
  sD_m[SETU]=-1; sD_m[SETUi]= 0; sD_m[SETUv]=-1;
  sD_m[SETC]=-1; sD_m[SETCi]= 0; sD_m[SETCv]=-1;
  sD_m[SETM]=-2; sD_m[SETMi]=-1; sD_m[SETMv]=-2;
  
  sD_m[FAIL]=0;
  
  // stack consumed map
  sC_m[PUSH]=0; sC_m[DYNO]=0; sC_m[DYNM]=0; sC_m[DFND]=0; sC_m[VARO ]=0; sC_m[VARM]=0; sC_m[VARU]=0; sC_m[EXTO]=0; sC_m[EXTM]=0;
  sC_m[EXTU]=0; sC_m[SYSV]=0; sC_m[ADDI]=0; sC_m[ADDU]=0; sC_m[DFND0]=0;sC_m[DFND1]=0;sC_m[DFND2]=0;
  
  sC_m[CHKV ]=0;sC_m[RETD ]=0;
  sC_m[FN1Ci]=1;sC_m[FN1Oi]=1; sC_m[FLDO]=1; sC_m[FLDM]=1; sC_m[ALIM]=1; sC_m[RETN]=1; sC_m[POPS]=1; sC_m[PRED]=1; sC_m[PRED1]=1; sC_m[PRED2]=1; sC_m[VFYM]=1;
  sC_m[FN2Ci]=2;sC_m[FN2Oi]=2; sC_m[FN1C]=2; sC_m[FN1O]=2; sC_m[MD1C]=2; sC_m[TR2D]=2; sC_m[MD2R]=2; sC_m[SETH]=2; sC_m[SETHi]=2;
  sC_m[MD2C ]=3;sC_m[TR3D ]=3; sC_m[FN2C]=3; sC_m[FN2O]=3; sC_m[TR3O]=3;
  
  sC_m[SETN]=2; sC_m[SETNi]=1; sC_m[SETNv]=1;
  sC_m[SETU]=2; sC_m[SETUi]=1; sC_m[SETUv]=1;
  sC_m[SETC]=2; sC_m[SETCi]=1; sC_m[SETCv]=1;
  sC_m[SETM]=3; sC_m[SETMi]=2; sC_m[SETMv]=2;
  
  sC_m[FAIL]=0;
  
  // stack added map
  for (i32 i = 0; i < BC_SIZE; i++) sA_m[i] = sD_m[i] + sC_m[i];
  sA_m[ARRO]=1;
  sA_m[ARRM]=1;
}




typedef struct CatchFrame {
  jmp_buf jmp;
  u64 gsDepth;
  u64 envDepth;
  u64 cfDepth;
} CatchFrame;
CatchFrame* cf; // points to after end
CatchFrame* cfStart;
CatchFrame* cfEnd;

jmp_buf* prepareCatch() { // in the case of returning false, must call popCatch();
  if (cf==cfEnd) {
    u64 n = cfEnd-cfStart;
    n = n<8? 8 : n*2;
    u64 d = cf-cfStart;
    cfStart = realloc(cfStart, n*sizeof(CatchFrame));
    cf    = cfStart+d;
    cfEnd = cfStart+n;
  }
  cf->cfDepth = cf-cfStart;
  cf->gsDepth = gStack-gStackStart;
  cf->envDepth = (envCurr+1)-envStart;
  return &(cf++)->jmp;
}
void popCatch() {
  #if CATCH_ERRORS
    assert(cf>cfStart);
    cf--;
  #endif
}

NOINLINE B vm_fmtPoint(B src, B prepend, B path, usz cs, usz ce) { // consumes prepend
  SGetU(src)
  usz srcL = a(src)->ia;
  usz srcS = cs;
  while (srcS>0 && o2cu(GetU(src,srcS-1))!='\n') srcS--;
  usz srcE = srcS;
  while (srcE<srcL) { if(o2cu(GetU(src, srcE))=='\n') break; srcE++; }
  if (ce>srcE) ce = srcE;
  
  i64 ln = 1;
  for (usz i = 0; i < srcS; i++) if(o2cu(GetU(src, i))=='\n') ln++;
  B s = prepend;
  if (isArr(path) && (a(path)->ia>1 || (a(path)->ia==1 && IGetU(path,0).u!=m_c32('.').u))) AFMT("%R:%l:\n  ", path, ln);
  else AFMT("at ");
  i64 padEnd = (i64)a(s)->ia;
  i64 padStart = padEnd;
  SGetU(s)
  while (padStart>0 && o2cu(GetU(s,padStart-1))!='\n') padStart--;
  
  Arr* slice = TI(src,slice)(inc(src),srcS, srcE-srcS); arr_shVec(slice);
  AJOIN(taga(slice));
  cs-= srcS;
  ce-= srcS;
  ACHR('\n');
  for (i64 i = padStart; i < padEnd; i++) ACHR(' ');
  for (u64 i = 0; i < cs; i++) ACHR(o2cu(GetU(src, srcS+i))=='\t'? '\t' : ' '); // ugh tabs
  for (u64 i = cs; i < ce; i++) ACHR('^');
  return s;
}

NOINLINE void vm_printPos(Comp* comp, i32 bcPos, i64 pos) {
  B src = comp->src;
  if (!q_N(src) && !q_N(comp->indices)) {
    B inds = IGetU(comp->indices, 0); usz cs = o2s(IGetU(inds,bcPos));
    B inde = IGetU(comp->indices, 1); usz ce = o2s(IGetU(inde,bcPos))+1;
    // printf("  bcPos=%d\n", bcPos);       // in case the pretty error generator is broken
    // printf(" inds:%d…%d\n", cs, ce);
    // int start = pos==-1? 0 : printf(N64d": ", pos);
    // usz srcL = a(src)->ia;
    // SGetU(src)
    // usz srcS = cs;   while (srcS>0 && o2cu(GetU(src,srcS-1))!='\n') srcS--;
    // usz srcE = srcS; while (srcE<srcL) { u32 chr = o2cu(GetU(src, srcE)); if(chr=='\n')break; printUTF8(chr); srcE++; }
    // if (ce>srcE) ce = srcE;
    // cs-= srcS; ce-= srcS;
    // putchar('\n');
    // for (i32 i = 0; i < cs+start; i++) putchar(' ');
    // for (i32 i = cs; i < ce; i++) putchar('^');
    // putchar('\n');
    B s = emptyCVec();
    printRaw(vm_fmtPoint(src, s, comp->path, cs, ce));
    putchar('\n');
    //print_BCStream((u32*)i32arr_ptr(comp->bc)+bcPos);
  } else {
    #ifdef DEBUG
      if (pos!=-1) printf(N64d": ", pos);
      printf("source unknown\n");
    #endif
  }
}

NOINLINE void vm_pst(Env* s, Env* e) { // e not included
  assert(s<=e);
  i64 l = e-s;
  i64 i = l-1;
  while (i>=0) {
    Env* c = s+i;
    if (l>30 && i==l-10) {
      printf("("N64d" entries omitted)\n", l-20);
      i = 10;
    }
    Comp* comp = c->sc->body->bl->comp;
    i32 bcPos = c->pos&1? ((u32)c->pos)>>1 : BCPOS(c->sc->body, (u32*)c->pos);
    vm_printPos(comp, bcPos, i);
    i--;
  }
}
NOINLINE void vm_pstLive() {
  vm_pst(envStart, envCurr+1);
}

void unwindEnv(Env* envNew) {
  assert(envNew<=envCurr);
  while (envCurr!=envNew) {
    // if ((envCurr->pos&1) == 0) printf("unwinding %ld\n", (u32*)envCurr->pos - (u32*)envCurr->sc->body->bl->bc);
    // else printf("not unwinding %ld", envCurr->pos>>1);
    if ((envCurr->pos&1) == 0) envCurr->pos = (BCPOS(envCurr->sc->body, (u32*)envCurr->pos)<<1) | 1;
    envCurr--;
  }
}
void unwindCompiler() {
  #if UNWIND_COMPILER
    unwindEnv(envStart+comp_currEnvPos);
  #endif
}

NOINLINE void printErrMsg(B msg) {
  if (isArr(msg)) {
    SGetU(msg)
    usz msgLen = a(msg)->ia;
    for (usz i = 0; i < msgLen; i++) if (!isC32(GetU(msg,i))) goto base;
    printRaw(msg);
    return;
  }
  base:
  print(msg);
}


NOINLINE NORETURN void thr(B msg) {
  // printf("gStack %p-%p:\n", gStackStart, gStack); B* c = gStack;
  // while (c>gStackStart) { print(*--c); putchar('\n'); } printf("gStack printed\n");
  if (cf>cfStart) {
    catchMessage = msg;
    cf--;
    
    B* gStackNew = gStackStart + cf->gsDepth;
    assert(gStackNew<=gStack);
    while (gStack!=gStackNew) dec(*--gStack);
    envPrevHeight = envCurr-envStart + 1;
    unwindEnv(envStart + cf->envDepth - 1);
    
    
    if (cfStart+cf->cfDepth > cf) err("bad catch cfDepth");
    cf = cfStart+cf->cfDepth;
    longjmp(cf->jmp, 1);
  }
  assert(cf==cfStart);
  printf("Error: "); printErrMsg(msg); putchar('\n'); fflush(stdout);
  Env* envEnd = envCurr+1;
  unwindEnv(envStart-1);
  vm_pst(envCurr+1, envEnd);
  #ifdef DEBUG
  __builtin_trap();
  #else
  exit(1);
  #endif
}

NOINLINE NORETURN void thrM(char* s) {
  thr(fromUTF8(s, strlen(s)));
}
NOINLINE NORETURN void thrOOM() { thrM("Out of memory"); }
