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

static void uintcpy(uint *dst, const uint *src, uint n)
{
  if(dst+n<=src)    memcpy (dst,src,n*sizeof(uint));
  else if(dst!=src) memmove(dst,src,n*sizeof(uint));
}

static ulong crystal_move(struct crystal *p, uint cutoff, int send_hi)
{
  uint len, *src, *end;
  uint *keep = p->data.ptr, *send;
  uint n = p->data.n;
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
  //return      send - (uint*)p->work.ptr;
  return (ulong)(send - (uint*)p->work.ptr);
}

static ulong crystal_exchange(struct crystal *p, ulong send_n_long, uint targ,
                             int recvn, int tag)
{
  comm_req req[3];
  ulong count_long[2] = {0,0}, sum_long;
  uint count[2] = {0,0}, sum, *recv[2];

  if(recvn)   
    comm_irecv(&req[1],&p->comm, &count_long[0],sizeof(ulong), targ        ,tag);
  if(recvn==2)
    comm_irecv(&req[2],&p->comm, &count_long[1],sizeof(ulong), p->comm.id-1,tag);
  comm_isend(&req[0],&p->comm, &send_n_long,sizeof(ulong), targ,tag);
  comm_wait(req,recvn+1);
  
  sum_long = p->data.n + count_long[0] + count_long[1];
  buffer_reserve(&p->data,sum_long*sizeof(uint));
  recv[0] = (uint*)p->data.ptr + p->data.n, recv[1] = recv[0] + count_long[0];
  p->data.n = sum_long;
  
  // TODO loop if n_send*sizeof(uint) > INT_MAX
  if(recvn)    comm_irecv(&req[1],&p->comm,
                          recv[0],count_long[0]*sizeof(uint), targ        ,tag+1);
  if(recvn==2) comm_irecv(&req[2],&p->comm,
                          recv[1],count_long[1]*sizeof(uint), p->comm.id-1,tag+1);
  comm_isend(&req[0],&p->comm, p->work.ptr,send_n_long*sizeof(uint), targ,tag+1);
  comm_wait(req,recvn+1);
  return sum_long;
}

void crystal_router(struct crystal *p)
{
  uint bl=0, bh, nl;
  uint id = p->comm.id, n=p->comm.np;
  uint targ, tag = 0;
  ulong send_n_long;
  slong send_n_long_b;
  sint overflow;
  int send_hi, recvn;
  while(n>1) {
    nl = (n+1)/2, bh = bl+nl;
    send_hi = id<bh;
    send_n_long = crystal_move(p,bh,send_hi);

    send_n_long_b = send_n_long * sizeof(uint);
    overflow = (send_n_long_b >= INT_MAX || send_n_long_b < 0);
    if (overflow) {
      fprintf(stderr, "Error in crystal_router1: rank = %d send_n = %lld (> "
        "INT_MAX)\n", p->comm.id, send_n_long_b);
      fflush(stderr);
      die(EXIT_FAILURE);
    }

    recvn = 1, targ = n-1-(id-bl)+bl;
    if(id==targ) targ=bh, recvn=0;
    if(n&1 && id==bh) recvn=2;
    send_n_long = crystal_exchange(p,send_n_long,targ,recvn,tag);
    if(id<bh) n=nl; else n-=nl,bl=bh;
    tag += 2;

    send_n_long_b = send_n_long * sizeof(uint);
    overflow = (send_n_long_b >= INT_MAX || send_n_long_b < 0);
    if (overflow) {
      fprintf(stderr, "Error in crystal_router2: rank = %d send_n = %lld (> "
        "INT_MAX)\n", p->comm.id, send_n_long_b);
      fflush(stderr);
      die(EXIT_FAILURE);
    }
  }
}
