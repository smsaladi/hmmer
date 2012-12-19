/* Optional support for MPI parallelization.
 * 
 * Contents:
 *    4. Communicating P7_TOPHITS, list of high scoring alignments.
 *    5. Benchmark driver.
 *    6. Unit tests.
 *    7. Test driver.
 *    8. Copyright and license information.
 */
#include "p7_config.h"		

#ifdef HAVE_MPI
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <mpi.h>

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_mpi.h"
#include "esl_getopts.h"

#include "base/p7_bg.h"
#include "base/p7_domain.h"
#include "base/p7_hmm.h"
#include "base/p7_profile.h"
#include "base/p7_tophits.h"

static int p7_hit_MPISend(P7_HIT *hit, int dest, int tag, MPI_Comm comm, char **buf, int *nalloc);
static int p7_hit_MPIPackSize(P7_HIT *hit, MPI_Comm comm, int *ret_n);
static int p7_hit_MPIPack(P7_HIT *hit, char *buf, int n, int *pos, MPI_Comm comm);
static int p7_hit_MPIUnpack(char *buf, int n, int *pos, MPI_Comm comm, P7_HIT *hit);
static int p7_hit_MPIRecv(int source, int tag, MPI_Comm comm, char **buf, int *nalloc, P7_HIT *hit);

static int p7_dcl_MPISend(P7_DOMAIN *dcl, int dest, int tag, MPI_Comm comm, char **buf, int *nalloc);
static int p7_dcl_MPIPackSize(P7_DOMAIN *dcl, MPI_Comm comm, int *ret_n);
static int p7_dcl_MPIPack(P7_DOMAIN *dcl, char *buf, int n, int *pos, MPI_Comm comm);
static int p7_dcl_MPIUnpack(char *buf, int n, int *pos, MPI_Comm comm, P7_DOMAIN *dcl);
static int p7_dcl_MPIRecv(int source, int tag, MPI_Comm comm, char **buf, int *nalloc, P7_DOMAIN *dcl);




/*****************************************************************
 * 4. Communicating P7_TOPHITS
 *****************************************************************/

/* Function:  p7_tophits_MPISend()
 * Synopsis:  Send the TOPHITS as an MPI work unit.
 *
 * Purpose:   Sends the TOPHITS <th> as a work unit to MPI process
 *            <dest> (where <dest> ranges from 0..<nproc-1>), tagged
 *            with MPI tag <tag>, for MPI communicator <comm>, as 
 *            the sole workunit or result. 
 *            
 *            After the TOPHITS <th> information has been sent, send
 *            the each hit as an indepentant message.
 *            
 * Returns:   <eslOK> on success; <*buf> may have been reallocated and
 *            <*nalloc> may have been increased.
 * 
 * Throws:    <eslESYS> if an MPI call fails; <eslEMEM> if a malloc/realloc
 *            fails. In either case, <*buf> and <*nalloc> remain valid and useful
 *            memory (though the contents of <*buf> are undefined). 
 */
int
p7_tophits_MPISend(P7_TOPHITS *th, int dest, int tag, MPI_Comm comm, char **buf, int *nalloc)
{
  int   status;
  int   sz, n, pos;
  int   i, j, inx;

  P7_DOMAIN *dcl = NULL;
  P7_HIT    *hit = NULL;

  n  = 0;
  sz = 0;

  /* calculate the buffer size needed to hold the largest hit */
  hit = th->unsrt;
  for (i = 0; i < th->N; ++i) {
    for (j = 0; j < th->unsrt[i].ndom; ++j) {
      if (sz <= hit->dcl[j].ad->memsize) {
	sz = hit->dcl[j].ad->memsize;
	dcl = &hit->dcl[j];
      }
    }
    ++hit;
  }

  if (th->N > 0) {
    if ((status = p7_hit_MPIPackSize(th->unsrt, comm, &n)) != eslOK) goto ERROR;
    if (dcl != NULL) {
      if ((status = p7_dcl_MPIPackSize(dcl, comm, &sz))    != eslOK) goto ERROR;
      n = (n > sz) ? n : sz;
    }
  }

  if (MPI_Pack_size(3, MPI_LONG_LONG_INT, comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed");
  n = (n > sz) ? n : sz;

  /* Make sure the buffer is allocated appropriately */
  if (*buf == NULL || n > *nalloc) {
    void *tmp;
    ESL_RALLOC(*buf, tmp, sizeof(char) * n);
    *nalloc = n; 
  }

  pos = 0;
  if (MPI_Pack(&th->N,         1, MPI_LONG_LONG_INT, *buf, n, &pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&th->nreported, 1, MPI_LONG_LONG_INT, *buf, n, &pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&th->nincluded, 1, MPI_LONG_LONG_INT, *buf, n, &pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 

  /* Send the packed tophits information */
  if (MPI_Send(*buf, n, MPI_PACKED, dest, tag, comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi send failed");
  if (th->N == 0) return eslOK;

  /* loop through the hit list sending to dest */
  hit = th->unsrt;
  for (inx = 0; inx < th->N; ++inx) {
    if ((status = p7_hit_MPISend(hit, dest, tag, comm, buf, nalloc)) != eslOK) goto ERROR;
    ++hit;
  }

  return eslOK;

 ERROR:
  return status;
}

/* Function:  p7_tophits_MPIRecv()
 * Synopsis:  Receives an TOPHITS as a work unit from an MPI sender.
 *
 * Purpose:   Sends the TOPHITS <th> as a work unit to MPI process
 *            <dest> (where <dest> ranges from 0..<nproc-1>), tagged
 *            with MPI tag <tag>, for MPI communicator <comm>, as 
 *            the sole workunit or result. 
 *            
 *            After the TOPHITS <th> information has been sent, send
 *            the each hit as an indepentant message.
 *            
 * Returns:   <eslOK> on success; <*buf> may have been reallocated and
 *            <*nalloc> may have been increased.
 * 
 * Throws:    <eslESYS> if an MPI call fails; <eslEMEM> if a malloc/realloc
 *            fails. In either case, <*buf> and <*nalloc> remain valid and useful
 *            memory (though the contents of <*buf> are undefined). 
 */
int
p7_tophits_MPIRecv(int source, int tag, MPI_Comm comm, char **buf, int *nalloc, P7_TOPHITS **ret_th)
{
  int         n;
  int         status;
  int         pos;
  P7_TOPHITS *th    = NULL;
  P7_HIT     *hit   = NULL;
  MPI_Status  mpistatus;

  uint64_t    nhits;
  uint64_t    inx;

  /* Probe first, because we need to know if our buffer is big enough.
   */
  MPI_Probe(source, tag, comm, &mpistatus);
  MPI_Get_count(&mpistatus, MPI_PACKED, &n);

  /* make sure we are getting the tag we expect and from whom we expect if from */
  if (tag    != MPI_ANY_TAG    && mpistatus.MPI_TAG    != tag) {
    status = eslFAIL;
    goto ERROR;
  }
  if (source != MPI_ANY_SOURCE && mpistatus.MPI_SOURCE != source) {
    status = eslFAIL;
    goto ERROR;
  }

  /* set the source and tag */
  tag = mpistatus.MPI_TAG;
  source = mpistatus.MPI_SOURCE;

  /* Make sure the buffer is allocated appropriately */
  if (*buf == NULL || n > *nalloc) {
    void *tmp;
    ESL_RALLOC(*buf, tmp, sizeof(char) * n); 
    *nalloc = n; 
  }

  /* Receive the packed top hits */
  MPI_Recv(*buf, n, MPI_PACKED, source, tag, comm, &mpistatus);

  /* Unpack it - watching out for the EOD signal of M = -1. */
  pos = 0;
  if ((th = p7_tophits_Create(p7_TOPHITS_DEFAULT_INIT_ALLOC)) == NULL) { status = eslEMEM; goto ERROR; }
  if (MPI_Unpack(*buf, n, &pos, &nhits,         1, MPI_LONG_LONG_INT, comm) != 0) ESL_XEXCEPTION(eslESYS, "unpack failed");
  if (MPI_Unpack(*buf, n, &pos, &th->nreported, 1, MPI_LONG_LONG_INT, comm) != 0) ESL_XEXCEPTION(eslESYS, "unpack failed");
  if (MPI_Unpack(*buf, n, &pos, &th->nincluded, 1, MPI_LONG_LONG_INT, comm) != 0) ESL_XEXCEPTION(eslESYS, "unpack failed");

  /* loop through all of the hits sent */
  for (inx = 0; inx < nhits; ++inx) {
      if ((status = p7_tophits_CreateNextHit(th, &hit))                  != eslOK) goto ERROR;
      if ((status = p7_hit_MPIRecv(source, tag, comm, buf, nalloc, hit)) != eslOK) goto ERROR;
    }

  *ret_th = th;
  return eslOK;

 ERROR:
  if (th  != NULL) p7_tophits_Destroy(th);
  *ret_th = NULL;
  return status;
}


/* Function:  p7_hit_MPISend()
 */
int
p7_hit_MPISend(P7_HIT *hit, int dest, int tag, MPI_Comm comm, char **buf, int *nalloc)
{
  int   inx;
  int   status;
  int   pos;
  int   n = *nalloc;

  /* Pack the HIT into the buffer */
  pos  = 0;
  if ((status = p7_hit_MPIPack(hit, *buf, n, &pos, comm)) != eslOK) goto ERROR;

  /* Send the packed HIT to the destination. */
  if (MPI_Send(*buf, n, MPI_PACKED, dest, tag, comm) != 0)  ESL_EXCEPTION(eslESYS, "mpi send failed");

  /* loop through all of the domains */
  for (inx = 0; inx < hit->ndom; ++inx) {
    if ((status = p7_dcl_MPISend(hit->dcl + inx, dest, tag, comm, buf, nalloc)) != eslOK) goto ERROR;
  }

  return eslOK;

 ERROR:
  return status;
}

/* Function:  p7_hit_MPIRecv()
 */
int
p7_hit_MPIRecv(int source, int tag, MPI_Comm comm, char **buf, int *nalloc, P7_HIT *hit)
{
  int         n;
  int         status;
  int         pos;
  int         inx;
  MPI_Status  mpistatus;

  /* Probe first, because we need to know if our buffer is big enough.
   */
  MPI_Probe(source, tag, comm, &mpistatus);
  MPI_Get_count(&mpistatus, MPI_PACKED, &n);

  /* make sure we are getting the tag we expect and from whom we expect if from */
  if (tag    != MPI_ANY_TAG    && mpistatus.MPI_TAG    != tag) {
    status = eslFAIL;
    goto ERROR;
  }
  if (source != MPI_ANY_SOURCE && mpistatus.MPI_SOURCE != source) {
    status = eslFAIL;
    goto ERROR;
  }

  /* set the source and tag */
  tag = mpistatus.MPI_TAG;
  source = mpistatus.MPI_SOURCE;

  /* Make sure the buffer is allocated appropriately */
  if (*buf == NULL || n > *nalloc) {
    void *tmp;
    ESL_RALLOC(*buf, tmp, sizeof(char) * n); 
    *nalloc = n; 
  }

  /* Receive the packed top hits */
  MPI_Recv(*buf, n, MPI_PACKED, source, tag, comm, &mpistatus);

  /* Unpack it - watching out for the EOD signal of M = -1. */
  pos = 0;
  if ((status = p7_hit_MPIUnpack(*buf, n, &pos, comm, hit)) != eslOK) goto ERROR;
  ESL_ALLOC(hit->dcl, sizeof(P7_DOMAIN) * hit->ndom);


  /* loop through all of the hits sent */
  for (inx = 0; inx < hit->ndom; ++inx) {
     if ((status = p7_dcl_MPIRecv(source, tag, comm, buf, nalloc, hit->dcl + inx)) != eslOK) goto ERROR;
  }

  return eslOK;

 ERROR:
  return status;
}

/* Function:  p7_hit_MPIPackSize()
 * Synopsis:  Calculates size needed to pack a HIT.
 *
 * Purpose:   Calculate an upper bound on the number of bytes
 *            that <p7_hit_MPIPack()> will need to pack an P7_HIT
 *            <hit> in a packed MPI message for MPI communicator
 *            <comm>; return that number of bytes in <*ret_n>.
 *
 * Returns:   <eslOK> on success, and <*ret_n> contains the answer.
 *
 * Throws:    <eslESYS> if an MPI call fails, and <*ret_n> is 0.
 */
int
p7_hit_MPIPackSize(P7_HIT *hit, MPI_Comm comm, int *ret_n)
{
  int   status;
  int   n = 0;
  int   sz;

  /* P7_HIT data */
  if (MPI_Pack_size(1,            MPI_DOUBLE, comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* sortkey                 */
  if (MPI_Pack_size(3,            MPI_FLOAT,  comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* scores                  */
  if (MPI_Pack_size(3,            MPI_DOUBLE, comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* lnP                     */
  if (MPI_Pack_size(1,            MPI_FLOAT,  comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* expected                */
  if (MPI_Pack_size(5,            MPI_INT,    comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* region, envelopes, ndom */
  if (MPI_Pack_size(4,            MPI_INT,    comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* report info             */

  if ((status = esl_mpi_PackOptSize(hit->name, -1, MPI_CHAR, comm, &sz)) != eslOK) goto ERROR;  n += sz;
  if ((status = esl_mpi_PackOptSize(hit->acc,  -1, MPI_CHAR, comm, &sz)) != eslOK) goto ERROR;  n += sz; 
  if ((status = esl_mpi_PackOptSize(hit->desc, -1, MPI_CHAR, comm, &sz)) != eslOK) goto ERROR;  n += sz; 

  *ret_n = n;
  return eslOK;

 ERROR:
  *ret_n = 0;
  return status;

}

/* Function:  p7_hit_MPIPack()
 * Synopsis:  Packs the HIT into MPI buffer.
 *
 * Purpose:   Packs HIT <hit> into an MPI packed message buffer <buf>
 *            of length <n> bytes, starting at byte position <*position>,
 *            for MPI communicator <comm>.
 *            
 *            The caller must know that <buf>'s allocation of <n>
 *            bytes is large enough to append the packed HIT at
 *            position <*pos>. This typically requires a call to
 *            <p7_hit_MPIPackSize()> first, and reallocation if
 *            needed.
 *            
 * Returns:   <eslOK> on success; <buf> now contains the
 *            packed <hit>, and <*position> is set to the byte
 *            immediately following the last byte of the HIT
 *            in <buf>. 
 *
 * Throws:    <eslESYS> if an MPI call fails; or <eslEMEM> if the
 *            buffer's length <n> was overflowed in trying to pack
 *            <msa> into <buf>. In either case, the state of
 *            <buf> and <*position> is undefined, and both should
 *            be considered to be corrupted.
 */
int
p7_hit_MPIPack(P7_HIT *hit, char *buf, int n, int *pos, MPI_Comm comm)
{
  int             status;

  if (MPI_Pack(&hit->sortkey,        1, MPI_DOUBLE,   buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&hit->score,          1, MPI_FLOAT,    buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&hit->pre_score,      1, MPI_FLOAT,    buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&hit->sum_score,      1, MPI_FLOAT,    buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&hit->lnP,            1, MPI_DOUBLE,   buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&hit->pre_lnP,        1, MPI_DOUBLE,   buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&hit->sum_lnP,        1, MPI_DOUBLE,   buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&hit->nexpected,      1, MPI_FLOAT,    buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 
  if (MPI_Pack(&hit->nregions,       1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&hit->nclustered,     1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&hit->noverlaps,      1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&hit->nenvelopes,     1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&hit->ndom,           1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&hit->flags,          1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&hit->nreported,      1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&hit->nincluded,      1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&hit->best_domain,    1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");

  if ((status = esl_mpi_PackOpt(hit->name,        -1,      MPI_CHAR,  buf, n, pos, comm)) != eslOK) return status;
  if ((status = esl_mpi_PackOpt(hit->acc,         -1,      MPI_CHAR,  buf, n, pos, comm)) != eslOK) return status; 
  if ((status = esl_mpi_PackOpt(hit->desc,        -1,      MPI_CHAR,  buf, n, pos, comm)) != eslOK) return status; 

  if (*pos > n) ESL_EXCEPTION(eslEMEM, "buffer overflow");
  return eslOK;
  
 ERROR:
  return status;
}

/* Function:  p7_hit_MPIUnpack()
 * Synopsis:  Unpacks an HIT from an MPI buffer.
 *
 * Purpose:   Unpack a newly allocated HIT from MPI packed buffer
 *            <buf>, starting from position <*pos>, where the total length
 *            of the buffer in bytes is <n>. 
 *            
 * Returns:   <eslOK> on success. <*pos> is updated to the position of
 *            the next element in <buf> to unpack (if any). <*ret_hit>
 *            contains a newly allocated HIT, which the caller is
 *            responsible for free'ing.
 *            
 * Throws:    <eslESYS> on an MPI call failure. <eslEMEM> on allocation failure.
 *            In either case, <*ret_hit> is <NULL>, and the state of <buf>
 *            and <*pos> is undefined and should be considered to be corrupted.
 */
int
p7_hit_MPIUnpack(char *buf, int n, int *pos, MPI_Comm comm, P7_HIT *hit)
{
  int  status;

  if (MPI_Unpack(buf, n, pos, &hit->sortkey,     1, MPI_DOUBLE, comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed"); 
  if (MPI_Unpack(buf, n, pos, &hit->score,       1, MPI_FLOAT,  comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed"); 
  if (MPI_Unpack(buf, n, pos, &hit->pre_score,   1, MPI_FLOAT,  comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed"); 
  if (MPI_Unpack(buf, n, pos, &hit->sum_score,   1, MPI_FLOAT,  comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed"); 
  if (MPI_Unpack(buf, n, pos, &hit->lnP,         1, MPI_DOUBLE, comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed"); 
  if (MPI_Unpack(buf, n, pos, &hit->pre_lnP,     1, MPI_DOUBLE, comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed"); 
  if (MPI_Unpack(buf, n, pos, &hit->sum_lnP,     1, MPI_DOUBLE, comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed"); 
  if (MPI_Unpack(buf, n, pos, &hit->nexpected,   1, MPI_FLOAT,  comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed"); 
  if (MPI_Unpack(buf, n, pos, &hit->nregions,    1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hit->nclustered,  1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hit->noverlaps,   1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hit->nenvelopes,  1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hit->ndom,        1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hit->flags,       1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hit->nreported,   1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hit->nincluded,   1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hit->best_domain, 1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");

  if ((status = esl_mpi_UnpackOpt(buf, n, pos,   (void**)&(hit->name),        NULL, MPI_CHAR,  comm)) != eslOK) goto ERROR;
  if ((status = esl_mpi_UnpackOpt(buf, n, pos,   (void**)&(hit->acc),         NULL, MPI_CHAR,  comm)) != eslOK) goto ERROR;
  if ((status = esl_mpi_UnpackOpt(buf, n, pos,   (void**)&(hit->desc),        NULL, MPI_CHAR,  comm)) != eslOK) goto ERROR;

  return eslOK;

 ERROR:
  return status;
}

/* Function:  p7_dcl_MPISend()
 */
int
p7_dcl_MPISend(P7_DOMAIN *dcl, int dest, int tag, MPI_Comm comm, char **buf, int *nalloc)
{
  int   status;
  int   pos;
  int   n = *nalloc;

  /* Pack the DOMAIN into the buffer */
  pos  = 0;
  if ((status = p7_dcl_MPIPack(dcl, *buf, n, &pos, comm)) != eslOK) goto ERROR;

  /* Send the packed HIT to the destination. */
  if (MPI_Send(*buf, n, MPI_PACKED, dest, tag, comm) != 0)  ESL_EXCEPTION(eslESYS, "mpi send failed");

  return eslOK;

 ERROR:
  return status;
}


/* Function:  p7_dcl_MPIPackSize()
 */
int
p7_dcl_MPIPackSize(P7_DOMAIN *dcl, MPI_Comm comm, int *ret_n)
{
  int   status;
  int   n = 0;
  int   sz;

  P7_ALIDISPLAY *ad = dcl->ad;

  /* P7_DOMAIN data */
  if (MPI_Pack_size(4,            MPI_INT,    comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* alignment info          */
  if (MPI_Pack_size(5,            MPI_FLOAT,  comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* score info              */
  if (MPI_Pack_size(1,            MPI_DOUBLE, comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* lnP                     */
  if (MPI_Pack_size(2,            MPI_INT,    comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* thresholds              */

  /* P7_ALIDISPLAY data */
  if (MPI_Pack_size(16,          MPI_INT,     comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* offset info             */
  if (MPI_Pack_size(3,           MPI_LONG,    comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* sequence info           */
  if (MPI_Pack_size(1,           MPI_INT,     comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* string pool size        */
  if (MPI_Pack_size(ad->memsize, MPI_CHAR,    comm, &sz) != 0) ESL_XEXCEPTION(eslESYS, "pack size failed"); n += sz;  /* string pool             */

  *ret_n = n;
  return eslOK;

 ERROR:
  *ret_n = 0;
  return status;

}

/* Function:  p7_dcl_MPIPack()
 */
int
p7_dcl_MPIPack(P7_DOMAIN *dcl, char *buf, int n, int *pos, MPI_Comm comm)
{
  int             status;
  int             offset;

  P7_ALIDISPLAY  *ad       = dcl->ad;

  if (MPI_Pack(&dcl->ienv,           1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->jenv,           1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->iali,           1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->jali,           1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->envsc,          1, MPI_FLOAT,    buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->domcorrection,  1, MPI_FLOAT,    buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->dombias,        1, MPI_FLOAT,    buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->oasc,           1, MPI_FLOAT,    buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->bitscore,       1, MPI_FLOAT,    buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->lnP,            1, MPI_DOUBLE,   buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->is_reported,    1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&dcl->is_included,    1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");

  offset = (ad->rfline  == NULL)  ? -1 : ad->rfline - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->mmline  == NULL)  ? -1 : ad->mmline - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->csline  == NULL)  ? -1 : ad->csline - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->model   == NULL)  ? -1 : ad->model - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->mline   == NULL)  ? -1 : ad->mline - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->aseq    == NULL)     ? -1 : ad->aseq - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->ppline  == NULL)  ? -1 : ad->ppline - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&ad->N,               1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->hmmname == NULL)  ? -1 : ad->hmmname - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->hmmacc  == NULL)  ? -1 : ad->hmmacc - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->hmmdesc == NULL)  ? -1 : ad->hmmdesc - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&ad->hmmfrom,         1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&ad->hmmto,           1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&ad->M,               1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->sqname  == NULL)  ? -1 : ad->sqname - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->sqacc   == NULL)  ? -1 : ad->sqacc - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  offset = (ad->sqdesc  == NULL)  ? -1 : ad->sqdesc - ad->mem;
  if (MPI_Pack(&offset,              1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&ad->sqfrom,          1, MPI_LONG,     buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&ad->sqto,            1, MPI_LONG,     buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&ad->L,               1, MPI_LONG,     buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack(&ad->memsize,         1, MPI_INT,      buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed");
  if (MPI_Pack( ad->mem,   ad->memsize, MPI_CHAR,     buf, n, pos, comm) != 0) ESL_XEXCEPTION(eslESYS, "pack failed"); 

  if (*pos > n) ESL_EXCEPTION(eslEMEM, "buffer overflow");
  return eslOK;
  
 ERROR:
  return status;
}

/* Function:  p7_dcl_MPIUnpack()
 */
int
p7_dcl_MPIUnpack(char *buf, int n, int *pos, MPI_Comm comm, P7_DOMAIN *dcl)
{
  int  status;
  int  rfline, mmline, csline, model, mline, aseq, ppline;
  int  hmmname, hmmacc, hmmdesc;
  int  sqname, sqacc, sqdesc;

  P7_ALIDISPLAY *ad; 

  ESL_ALLOC(ad, sizeof(P7_ALIDISPLAY));

  if (MPI_Unpack(buf, n, pos, &dcl->ienv,          1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->jenv,          1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->iali,          1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->jali,          1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->envsc,         1, MPI_FLOAT,  comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->domcorrection, 1, MPI_FLOAT,  comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->dombias,       1, MPI_FLOAT,  comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->oasc,          1, MPI_FLOAT,  comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->bitscore,      1, MPI_FLOAT,  comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->lnP,           1, MPI_DOUBLE, comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->is_reported,   1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &dcl->is_included,   1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &rfline,             1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &mmline,             1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &csline,             1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &model,              1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &mline,              1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &aseq,               1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &ppline,             1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &ad->N,              1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hmmname,            1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hmmacc,             1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &hmmdesc,            1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &ad->hmmfrom,        1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &ad->hmmto,          1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &ad->M,              1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &sqname,             1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &sqacc,              1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &sqdesc,             1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &ad->sqfrom,         1, MPI_LONG,   comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &ad->sqto,           1, MPI_LONG,   comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &ad->L,              1, MPI_LONG,   comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");
  if (MPI_Unpack(buf, n, pos, &ad->memsize,        1, MPI_INT,    comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed");

  /* allocate the string pools for the alignments */
  ESL_ALLOC(ad->mem, ad->memsize);
  if (MPI_Unpack(buf, n, pos,  ad->mem,  ad->memsize, MPI_CHAR,   comm) != 0) ESL_XEXCEPTION(eslESYS, "mpi unpack failed"); 

  ad->rfline  = (rfline == -1)  ? NULL : ad->mem + rfline;
  ad->mmline  = (mmline == -1)  ? NULL : ad->mem + mmline;
  ad->csline  = (csline == -1)  ? NULL : ad->mem + csline;
  ad->model   = (model == -1)   ? NULL : ad->mem + model;
  ad->mline   = (mline == -1)   ? NULL : ad->mem + mline;
  ad->aseq    = (aseq == -1)    ? NULL : ad->mem + aseq;
  ad->ppline  = (ppline == -1)  ? NULL : ad->mem + ppline;

  ad->hmmname = (hmmname == -1) ? NULL : ad->mem + hmmname;
  ad->hmmacc  = (hmmacc == -1)  ? NULL : ad->mem + hmmacc;
  ad->hmmdesc = (hmmdesc == -1) ? NULL : ad->mem + hmmdesc;

  ad->sqname  = (sqname == -1)  ? NULL : ad->mem + sqname;
  ad->sqacc   = (sqacc == -1)   ? NULL : ad->mem + sqacc;
  ad->sqdesc  = (sqdesc == -1)  ? NULL : ad->mem + sqdesc;

  dcl->ad = ad;

  return eslOK;

 ERROR:
  if (ad  != NULL) {
    if (ad->mem != NULL) free(ad->mem);
    free(ad);
  }
  return status;
}

/* Function:  p7_dcl_MPIRecv()
 */
int
p7_dcl_MPIRecv(int source, int tag, MPI_Comm comm, char **buf, int *nalloc, P7_DOMAIN *dcl)
{
  int         status;
  int         n;
  int         pos;
  MPI_Status  mpistatus;

  /* Probe first, because we need to know if our buffer is big enough.
   */
  MPI_Probe(source, tag, comm, &mpistatus);
  MPI_Get_count(&mpistatus, MPI_PACKED, &n);

  /* make sure we are getting the tag we expect and from whom we expect if from */
  if (tag    != MPI_ANY_TAG    && mpistatus.MPI_TAG    != tag) {
    status = eslFAIL;
    goto ERROR;
  }
  if (source != MPI_ANY_SOURCE && mpistatus.MPI_SOURCE != source) {
    status = eslFAIL;
    goto ERROR;
  }

  /* set the source and tag */
  tag = mpistatus.MPI_TAG;
  source = mpistatus.MPI_SOURCE;

  /* Make sure the buffer is allocated appropriately */
  if (*buf == NULL || n > *nalloc) {
    void *tmp;
    ESL_RALLOC(*buf, tmp, sizeof(char) * n); 
    *nalloc = n; 
  }

  /* Receive the packed dcl */
  MPI_Recv(*buf, n, MPI_PACKED, source, tag, comm, &mpistatus);

  /* Unpack it, looking at the status code prefix for EOD/EOK  */
  pos = 0;
  return p7_dcl_MPIUnpack(*buf, *nalloc, &pos, comm, dcl);

 ERROR:
  return status;
}

/*----------------- end, P7_TOPHITS communication -------------------*/


/*****************************************************************
 * 5. Benchmark driver.
 *****************************************************************/

#ifdef p7MPISUPPORT_BENCHMARK
/* 
  mpicc -g -Wall -L. -I. -L ../easel -I ../easel -D p7MPISUPPORT_BENCHMARK -o benchmark-mpi mpisupport.c -lhmmer -leasel -lm
  qsub -N benchmark-mpi -j y -R y -b y -cwd -V -pe lam-mpi-tight 2 'mpirun C ./benchmark-mpi  ~/notebook/1227-msp-statistics/Pfam.hmm > bench.out'
  qsub -N benchmark-mpi -j y -R y -b y -cwd -V -pe lam-mpi-tight 2 'mpirun C ./benchmark-mpi -b ~/notebook/1227-msp-statistics/Pfam.hmm > bench.out'
 */
#include "p7_config.h"

#include <string.h>
#include <math.h>

#include "easel.h"
#include "esl_getopts.h"
#include "esl_alphabet.h"
#include "esl_random.h"
#include "esl_stopwatch.h"

#include "hmmer.h"

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "show brief help on version and usage",             0 },
  { "-b",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "baseline timing: don't send any HMMs",             0 },
  { "--stall",   eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "arrest after start: for debugging MPI under gdb",  0 },  
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <hmmfile>";
static char banner[] = "benchmark driver for MPI communication";

int
main(int argc, char **argv)
{
  ESL_GETOPTS    *go      = p7_CreateDefaultApp(options, 1, argc, argv, banner, usage);
  char           *hmmfile = esl_opt_GetArg(go, 1);
  ESL_ALPHABET   *abc     = esl_alphabet_Create(eslAMINO);
  P7_BG          *bg      = p7_bg_Create(abc);
  int             my_rank;
  int             nproc;
  char           *buf    = NULL;
  int             nbuf   = 0;
  int             subtotalM = 0;
  int             allM   = 0;
  int             stalling = esl_opt_GetBoolean(go, "--stall");

  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nproc);

  while (stalling); 

  /* Master MPI process: */
  if (my_rank == 0) 
    {
      ESL_STOPWATCH *w        = esl_stopwatch_Create();
      P7_HMMFILE    *hfp      = NULL;
      P7_PROFILE     *gm      = NULL;
      P7_HMM         *hmm     = NULL;


      /* Read HMMs from a file. */
      if (p7_hmmfile_OpenE(hmmfile, NULL, &hfp, NULL) != eslOK) p7_Fail("Failed to open HMM file %s", hmmfile);

      esl_stopwatch_Start(w);
      while (p7_hmmfile_Read(hfp, &abc, &hmm)     == eslOK) 
	{
	  gm = p7_profile_Create(hmm->M, abc);	  
	  p7_profile_ConfigLocal(gm, hmm, bg, 400);
	  if (!esl_opt_GetBoolean(go, "-b"))
	    p7_profile_MPISend(gm, 1, 0, MPI_COMM_WORLD, &buf, &nbuf); /* 1 = dest; 0 = tag */

	  p7_hmm_Destroy(hmm);
	  p7_profile_Destroy(gm);
	}
      p7_profile_MPISend(NULL, 1, 0, MPI_COMM_WORLD, &buf, &nbuf); /* send the "no more HMMs" sign */
      MPI_Reduce(&subtotalM, &allM, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

      printf("total: %d\n", allM);
      esl_stopwatch_Stop(w);
      esl_stopwatch_Display(stdout, w, "CPU Time: ");
      esl_stopwatch_Destroy(w);
    }
  /* Worker MPI process: */
  else 
    {
      P7_PROFILE     *gm_recd = NULL;      

      while (p7_profile_MPIRecv(0, 0, MPI_COMM_WORLD, abc, bg, &buf, &nbuf, &gm_recd) == eslOK) 
	{
	  subtotalM += gm_recd->M;
	  p7_profile_Destroy(gm_recd);  
	}
      MPI_Reduce(&subtotalM, &allM, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    }

  free(buf);
  p7_bg_Destroy(bg);
  esl_alphabet_Destroy(abc);
  esl_getopts_Destroy(go);
  MPI_Finalize();
  exit(0);
}

#endif /*p7MPISUPPORT_BENCHMARK*/
/*---------------------- end, benchmark -------------------------*/


/*****************************************************************
 * 6. Unit tests
 *****************************************************************/
#ifdef p7MPISUPPORT_TESTDRIVE


static void
utest_ProfileSendRecv(int my_rank, int nproc)
{
  ESL_RANDOMNESS *r    = esl_randomness_CreateFast(42);
  ESL_ALPHABET   *abc  = esl_alphabet_Create(eslAMINO);
  P7_HMM         *hmm  = NULL;
  P7_BG          *bg   = NULL;
  P7_PROFILE     *gm   = NULL;
  P7_PROFILE     *gm2  = NULL;
  int             M    = 200;
  int             L    = 400;
  char           *wbuf = NULL;
  int             wn   = 0;
  int             i;
  char            errbuf[eslERRBUFSIZE];

  p7_hmm_Sample(r, M, abc, &hmm); /* master and worker's sampled profiles are identical */
  bg = p7_bg_Create(abc);
  gm = p7_profile_Create(hmm->M, abc);
  p7_profile_ConfigLocal(gm, hmm, bg, L);
  p7_bg_SetLength  (bg, L);

  if (my_rank == 0)
    {
      for (i = 1; i < nproc; i++)
	{
	  ESL_DPRINTF1(("Master: receiving test profile\n"));
	  p7_profile_MPIRecv(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, abc, bg, &wbuf, &wn, &gm2);
	  ESL_DPRINTF1(("Master: test profile received\n"));

	  if (p7_profile_Validate(gm2, errbuf, 0.001) != eslOK) p7_Die("profile validation failed: %s", errbuf);
	  if (p7_profile_Compare(gm, gm2, 0.001) != eslOK) p7_Die("Received profile not identical to what was sent");

	  p7_profile_Destroy(gm2);
	}
    }
  else 
    {
      ESL_DPRINTF1(("Worker %d: sending test profile\n", my_rank));
      p7_profile_MPISend(gm, 0, 0, MPI_COMM_WORLD, &wbuf, &wn);
      ESL_DPRINTF1(("Worker %d: test profile sent\n", my_rank));
    }

  free(wbuf);
  p7_profile_Destroy(gm);
  p7_bg_Destroy(bg);
  p7_hmm_Destroy(hmm);
  esl_alphabet_Destroy(abc);
  esl_randomness_Destroy(r);
  return;
}



#endif /*p7MPISUPPORT_TESTDRIVE*/
/*---------------------- end, unit tests ------------------------*/


/*****************************************************************
 * 7. Test driver.
 *****************************************************************/
#ifdef p7MPISUPPORT_TESTDRIVE

/* mpicc -o mpisupport_utest -g -Wall -I../easel -L../easel -I. -L. -Dp7MPISUPPORT_TESTDRIVE mpisupport.c -lhmmer -leasel -lm
 * In an MPI environment: (qlogin -pe lam-mpi-tight 2; setenv JOB_ID <jobid>; setenv TMPDIR /tmp/<jobid>....
 *    mpirun C ./mpisupport_utest
 */
#include "esl_getopts.h"

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",        eslARG_NONE,   FALSE, NULL, NULL, NULL, NULL, NULL, "show brief help on version and usage",              0 },
  { "--stall",   eslARG_NONE,   FALSE, NULL, NULL, NULL, NULL, NULL, "arrest after start: for debugging MPI under gdb",   0 },  
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options]";
static char banner[] = "test driver for mpisupport.c";

int
main(int argc, char **argv)
{
  ESL_GETOPTS *go = p7_CreateDefaultApp(options, 0, argc, argv, banner, usage);
  int          stalling = FALSE;
  int          my_rank;
  int          nproc;

  /* For debugging: stall until GDB can be attached */
  if (esl_opt_GetBoolean(go, "--stall")) stalling = TRUE;
  while (stalling);

  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nproc);

  utest_HMMSendRecv(my_rank, nproc);
  utest_ProfileSendRecv(my_rank, nproc);

  MPI_Finalize();
  return 0;
}

#endif /*p7MPISUPPORT_TESTDRIVE*/
/*---------------------- end, test driver -----------------------*/


#else /*!HAVE_MPI*/

/* If we don't have MPI compiled in, provide some nothingness to:
 *   a. prevent Mac OS/X ranlib from bitching about .o file that "has no symbols" 
 *   b. prevent compiler from bitching about "empty compilation unit"
 *   c. automatically pass the automated tests.
 */
void p7_mpisupport_DoAbsolutelyNothing(void) { return; }
#if defined p7MPISUPPORT_TESTDRIVE || p7MPISUPPORT_EXAMPLE || p7MPISUPPORT_BENCHMARK
int main(void) { return 0; }
#endif
#endif /*HAVE_MPI*/

/*****************************************************************
 * @LICENSE@
 *
 * SVN $Id$
 * SVN $URL$
 *****************************************************************/



