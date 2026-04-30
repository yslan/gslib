/*------------------------------------------------------------------------------
  
  Crystal Router
  
  Accomplishes all-to-all communication in log P msgs per proc
  The routine is low-level; the format of the input/output is an
  array of integers, consisting of a sequence of messages with format:
  
      target proc
      source proc
      m
      integer
      integer
      ...
      integer  (m integers in total)

  Before crystal_router is called, the source of each message should be
  set to this proc id; upon return from crystal_router, the target of each
  message will be this proc id.
  
  Example Usage:
  
    struct crystal cr;
    
    crystal_init(&cr, &comm);  // makes an internal copy of comm
    
    crystal.data.n = ... ;  // total number of integers (not bytes!)
    buffer_reserve(&cr.data, crystal.n * sizeof(uint));
    ... // fill cr.data.ptr with messages
    crystal_router(&cr);
    
    crystal_free(&cr);
    
  ----------------------------------------------------------------------------*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "c99.h"
#include "name.h"
#include "fail.h"
#include "types.h"
#include "gs_defs.h"
#include "comm.h"
#include "mem.h"
#include <stdio.h>

#define crystal_init   GS_PREFIXED_NAME(crystal_init  )
#define crystal_free   GS_PREFIXED_NAME(crystal_free  )
#define crystal_router GS_PREFIXED_NAME(crystal_router)

//#define CR_MAX_MSG ((ulong)INT_MAX / 2)
#define CR_MAX_MSG ((ulong)INT_MAX)
#define CR_MAX_N CR_MAX_MSG/sizeof(uint)

struct crystal {
  struct comm comm;
  buffer data, work;
};

void crystal_init(struct crystal *p, const struct comm *comm)
{
  comm_dup(&p->comm, comm);
  buffer_init(&p->data,1000);
  buffer_init(&p->work,1000);
}

void crystal_free(struct crystal *p)
{
  comm_free(&p->comm);
  buffer_free(&p->data);
  buffer_free(&p->work);
}

static void uintcpy(uint *dst, const uint *src, ulong n)
{
  if(dst+n<=src)    memcpy (dst,src,n*sizeof(uint));
  else if(dst!=src) memmove(dst,src,n*sizeof(uint));
}

static ulong crystal_move(struct crystal *p, uint cutoff, int send_hi)
{
  uint *src, *end;
  uint *keep = p->data.ptr, *send;
  ulong n = p->data.n, len;
  send = buffer_reserve(&p->work,n*sizeof(uint));
  if(send_hi) { /* send hi, keep lo */
    for(src=keep,end=keep+n; src<end; src+=len) {
      len = 3 + src[2];
      if(src[0]>=cutoff) memcpy (send,src,len*sizeof(uint)), send+=len;
      else               uintcpy(keep,src,len),              keep+=len;
    }
  } else      { /* send lo, keep hi */
    for(src=keep,end=keep+n; src<end; src+=len) {
      len = 3 + src[2];
      if(src[0]< cutoff) memcpy (send,src,len*sizeof(uint)), send+=len;
      else               uintcpy(keep,src,len),              keep+=len;
    }
  }
  p->data.n = keep - (uint*)p->data.ptr;
  return (ulong)(send - (uint*)p->work.ptr);
}

static ulong crystal_exchange(struct crystal *p, ulong send_n_long, uint targ,
                             int recvn, int tag)
{

  uint *recv[2];
  ulong count_long[2] = {0,0}, sum_long;
  const int nr_max=3*4;
  comm_req req[nr_max];

  if(recvn) // 1 or 2=recv
    comm_irecv(&req[1],&p->comm, &count_long[0],sizeof(ulong), targ        ,tag);
  if(recvn==2) // 2=recv more
    comm_irecv(&req[2],&p->comm, &count_long[1],sizeof(ulong), p->comm.id-1,tag);
  comm_isend(&req[0],&p->comm, &send_n_long,sizeof(ulong), targ,tag);
  comm_wait(req,recvn+1);
  
  sum_long = p->data.n + count_long[0] + count_long[1];
  buffer_reserve(&p->data,sum_long*sizeof(uint));
  recv[0] = (uint*)p->data.ptr + p->data.n, recv[1] = recv[0] + count_long[0];
  p->data.n = sum_long;

  ulong soff=0, sleft=send_n_long;
  ulong r1off=0, r1left=count_long[0];
  ulong r2off=0, r2left=count_long[1];
  
  int nr = 0;
  while(sleft || r1left || r2left) {
    ulong sn=0, r1n=0, r2n=0;
  
    if(nr+3>nr_max){
      comm_wait(req,nr);
      nr = 0;
    }

    if(recvn && r1left) {
      r1n = (r1left > CR_MAX_N) ? CR_MAX_N : r1left;
      comm_irecv(&req[nr++],&p->comm,
                 recv[0]+r1off,r1n*sizeof(uint),targ,tag+1);
    }
  
    if(recvn==2 && r2left) {
      r2n = (r2left > CR_MAX_N) ? CR_MAX_N : r2left;
      comm_irecv(&req[nr++],&p->comm,
                 recv[1]+r2off,r2n*sizeof(uint),p->comm.id-1,tag+1);
    }
  
    if(sleft) {
      sn = (sleft > CR_MAX_N) ? CR_MAX_N : sleft;
      comm_isend(&req[nr++],&p->comm,
                 (uint*)p->work.ptr+soff,sn*sizeof(uint),targ,tag+1);
    }

    r1off += r1n; r1left -= r1n;
    r2off += r2n; r2left -= r2n;
    soff  += sn;  sleft  -= sn;
  }
  if(nr>0) comm_wait(req,nr);
//  if(recvn)    comm_irecv(&req[1],&p->comm,
//                          recv[0],count_long[0]*sizeof(uint), targ        ,tag+1);
//  if(recvn==2) comm_irecv(&req[2],&p->comm,
//                          recv[1],count_long[1]*sizeof(uint), p->comm.id-1,tag+1);
//  comm_isend(&req[0],&p->comm, p->work.ptr,send_n_long*sizeof(uint), targ,tag+1);
//  comm_wait(req,recvn+1);
  return sum_long;
}

void crystal_router(struct crystal *p)
{
  uint bl=0, bh, nl;
  uint id = p->comm.id, n=p->comm.np;
  uint targ, tag = 0;
  ulong send_n_long, send_n_long_b;
  sint overflow;
  int send_hi, recvn;
  while(n>1) {
    nl = (n+1)/2, bh = bl+nl; // if uneven, low has more
    send_hi = id<bh;
    send_n_long = crystal_move(p,bh,send_hi);

    send_n_long_b = send_n_long * sizeof(uint);
    overflow = (send_n_long_b >= CR_MAX_MSG);
    if (overflow) {
      fprintf(stderr, "Error in crystal_router1: rank = %d send_n = %llu (> "
        "INT_MAX = %llu)\n", p->comm.id,
        (unsigned long long)send_n_long_b, (unsigned long long)CR_MAX_MSG);
      fflush(stderr);
      //die(EXIT_FAILURE);
    }

    recvn = 1, targ = n-1-(id-bl)+bl; // ideal: low - high pariwise
    if(id==targ) targ=bh, recvn=0; // recv nothing
    if(n&1 && id==bh) recvn=2; // recv from 2 partners
    send_n_long = crystal_exchange(p,send_n_long,targ,recvn,tag);
    if(id<bh) n=nl; else n-=nl,bl=bh;
    tag += 2;

    send_n_long_b = send_n_long * sizeof(uint);
    overflow = (send_n_long_b >= CR_MAX_MSG);
    if (overflow) {
      fprintf(stderr, "Error in crystal_router2: rank = %d send_n = %llu (> "
        "INT_MAX = %llu)\n", p->comm.id,
        (unsigned long long)send_n_long_b, (unsigned long long)CR_MAX_MSG);
      fflush(stderr);
      //die(EXIT_FAILURE);
    }
  }
}
