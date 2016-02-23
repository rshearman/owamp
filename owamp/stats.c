/*
 *      $Id$
 */
/************************************************************************
 *                                                                      *
 *                             Copyright (C)  2003                      *
 *                                Internet2                             *
 *                             All Rights Reserved                      *
 *                                                                      *
 ************************************************************************/
/*
 *        File:         stats.c
 *
 *        Author:       Jeff W. Boote
 *                      Internet2
 *
 *        Date:         Fri Nov  4 14:42:29 MST 2005
 *
 *        Description:        
 *
 * This file contains the  convinience functions used to compute statistics
 * in many of the owamp tools.
 *
 * A fair amount of effort has been made to pre-allocate the memory needed
 * to support these summary statistics.  This is being done because powstream
 * in particular will be using the statistics functions to generate summary
 * information on the fly during tests, therefore it is important to minimize
 * the number of system calls.
 *
 */
#include <owamp/owamp.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <ctype.h>

/*
 * PacketBuffer utility functions:
 *
 * The packet buffer is basically a buffer that holds a record of every
 * packet of interest that can still effect summary statistics.
 *
 * It is a linked list with hash accesses to individual nodes. It is primarily
 * used to track loss and dups. The buffer needs to be large enough to hold
 * as many packets as can be seen within the loss-threshold (timeout) period.
 *
 */


/*
 * Function:    PacketFree
 *
 * Description:    
 *              Used to take a PacketRecord and put it back in the freelist.
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 */
static void
PacketFree(
        OWPStats    stats,
        OWPPacket   node
        )
{
    I2Datum k;

    k.dptr = &node->seq;
    k.dsize = sizeof(node->seq);

    if(I2HashDelete(stats->ptable,k) != 0){
        OWPError(stats->ctx,OWPErrWARNING,OWPErrUNKNOWN,
                "PacketFree: Unable to remove seq #%lu from OWPPacket table",
                node->seq);
    }

    node->seq = 0;
    node->seen = 0;
    node->next = stats->pfreelist;
    stats->pfreelist = node;

    return;
}

/*
 * Function:    PacketBufferClean
 *
 * Description:    
 *              Used to completly clear out the ring-buffer. This is used
 *              just before generating new statistics using the same
 *              stats object.
 *              
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 */
static I2Boolean
PacketBufferClean(
        I2Datum k   __attribute__((unused)),
        I2Datum v,
        void    *app_data
        )
{
    OWPStats   stats = app_data;
    OWPPacket  node = v.dptr;

    PacketFree(stats,node);

    return True;
}

/*
 * Function:    PacketAlloc
 *
 * Description:    
 *              Used to take a packet record off the freelist and add it
 *              into the current buffer.
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 */
static OWPPacket
PacketAlloc(
    OWPStats    stats,
    uint32_t   seq
    )
{
    OWPPacket   node;
    I2Datum     k,v;

    if((seq > stats->last) || (stats->pend && (seq <= stats->pend->seq))){
        OWPError(stats->ctx,OWPErrFATAL,OWPErrINVALID,
                "PacketAlloc: Invalid seq number for OWPPacket buffer");
        return NULL;
    }

    if(!stats->pfreelist){
        long int    i;

        /*
         * print out info message to inform that the "size" calculation was not
         * good enough. (dynamic memory allocations during parsing is to be
         * avoided if possible).
         *
         * Note that with two-way pings, we don't know the client's
         * send schedule so this is expected and we don't want to
         * print a message in that case.
         */
        if (!stats->hdr->twoway) {
            OWPError(stats->ctx,OWPErrINFO,OWPErrUNKNOWN,
                     "PacketAlloc: Allocating OWPPacket!: plistlen=%u, timeout=%g",
                     stats->plistlen,
                     OWPNum64ToDouble(stats->hdr->test_spec.loss_timeout));
        }

        if(!(node = calloc(sizeof(OWPPacketRec),stats->plistlen))){
            OWPError(stats->ctx,OWPErrFATAL,errno,"calloc(): %M");
            return NULL;
        }

        node[0].next = stats->pallocated;
        stats->pallocated = node;
        for(i=1;i<stats->plistlen;i++){
            node[i].next = stats->pfreelist;
            stats->pfreelist = &node[i];
        }
    }

    node = stats->pfreelist;
    stats->pfreelist = stats->pfreelist->next;

    node->next = NULL;
    node->seq = seq;
    node->seen = 0;
    node->lost = False;

    k.dptr = &node->seq;
    k.dsize = sizeof(node->seq);
    v.dptr = node;
    v.dsize = sizeof(*node);

    if(I2HashStore(stats->ptable,k,v) != 0){
        return NULL;
    }

    return node;
}

/*
 * Function:    PacketGet
 *
 * Description:    
 *              This function retrieves the packet record for a given
 *              sequence number. If necessary, it will call PacketAlloc
 *              to allocate the record if it does not exist yet. (Which
 *              will create records for all sequence numbers between
 *              the current end-of-list and this new sequence number
 *              in the process.
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 */
static OWPPacket
PacketGet(
        OWPStats    stats,
        uint32_t   seq
        )
{
    OWPPacket   node;
    I2Datum     k,v;

    /*
     * optimize for most frequent case.
     */
    if(seq == stats->pend->seq){
        return stats->pend;
    }

    /*
     * Ignore invalid seq num.
     */
    if((seq < stats->first) || (seq > stats->last)){
        return NULL;
    }

    /*
     * Need to build the list from current "end" to this seq num.
     */
    if(seq > stats->pend->seq){
        node = stats->pend;

        while(node->seq < seq){
            /* bug if context is not alligned with node allocation */
            assert(node->seq+1 == stats->isctx);

            /* update current schedule value */
            stats->endnum = OWPNum64Add(stats->endnum,
                    OWPScheduleContextGenerateNextDelta(stats->sctx));
            stats->isctx++;

            /* allocate and initialize next packet record */
            node->next = PacketAlloc(stats,node->seq+1);
            node->next->schedtime = stats->endnum;

            node = node->next;
        }

        stats->pend = node;

        return node;
    }

    /*
     * Shouldn't be requesting this seq num... It should already
     * be loss_timeout in the past.
     */
    if(seq < stats->pbegin->seq){
        OWPError(stats->ctx,OWPErrFATAL,OWPErrINVALID,
                "Invalid seq number request");
        return NULL;
    }

    /*
     * seq requested in within the begin<->end range, just fetch from
     * hash.
     */
    k.dptr = &seq;
    k.dsize = sizeof(seq);

    if(!I2HashFetch(stats->ptable,k,&v)){
        OWPError(stats->ctx,OWPErrFATAL,OWPErrUNKNOWN,
                "Unable to fetch seq (%lu) from packet hash",seq);
        return NULL;
    }

    return (OWPPacket)v.dptr;
}

/*
 * BucketBuffer utility functions:
 *
 * The BucketBuffer is basically just a hash of bucketed delay's that are
 * used to generate a histogram of the latency values for the given
 * summary session. (Basically used to quantile.)
 */

/*
 * Function:    BucketBufferClean
 *
 * Description:    
 *              Used to clean out the current hash of all existing values.
 *              Useful for re-using a stats object for a new summary
 *              period.
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 */
static I2Boolean
BucketBufferClean(
        I2Datum k,
        I2Datum v,
        void    *app_data
        )
{
    OWPStats    stats = app_data;
    OWPBucket   node = v.dptr;

    if(I2HashDelete(stats->btable,k) != 0){
        OWPError(stats->ctx,OWPErrWARNING,OWPErrUNKNOWN,
                "BucketBufferClean: Unable to remove bucket #%d",node->b);
    }

    node->b = 0;
    node->n = 0;
    node->next = stats->bfreelist;
    stats->bfreelist = node;

    return True;
}

/*
 * Function:    BucketBufferPrint
 *
 * Description:    
 *              Used to print out the current hash of all existing values.
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 */
static I2Boolean
BucketBufferPrint(
        I2Datum k __attribute__((unused)),
        I2Datum v,
        void    *app_data
        )
{
    OWPBucket   node = v.dptr;
    FILE        *fp = app_data;

    fprintf(fp,"\t%d\t%u\n",node->b,node->n);

    return True;
}

/*
 * Function:    BucketBufferSortFill
 *
 * Description:    
 *              Used to copy bucket ref's into an array for sorting.
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 */
static I2Boolean
BucketBufferSortFill(
        I2Datum k   __attribute__((unused)),
        I2Datum v,
        void    *app_data
        )
{
    OWPStats    stats = app_data;
    OWPBucket   node = v.dptr;

    stats->bsort[stats->bsorti++] = node;

    return True;
}

/*
 * Function:    BucketIncrementDelay
 *
 * Description:    
 *              Used to record that fact that a given packet was recieved
 *              in a given delay time. Adds a new record into the hash
 *              if necessary.
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 */
static OWPBoolean
BucketIncrementDelay(
    OWPStats    stats,
    double      d       /* delay */
    )
{
    int         b;
    OWPBucket   node;
    I2Datum     k,v;

    /*
     * XXX: May eventually need to round these off to some significant number
     * of digits instead of using floor/ceil. (Currently, the algorithm is
     * biased away from 0.)
     */
    d /= stats->bucketwidth;
    b = (d<0)?floor(d):ceil(d);

    k.dsize = sizeof(b);
    k.dptr = &b;

    if(I2HashFetch(stats->btable,k,&v)){
        node = (OWPBucket)v.dptr;
    }
    else{
        if(!stats->bfreelist){
            long int    i;

            /*
             * print out info message to inform that the "size" calculation
             * was not good enough. (dynamic memory allocations during parsing
             * should be avoided if possible)
             */
            OWPError(stats->ctx,OWPErrINFO,OWPErrUNKNOWN,
                    "BucketIncrementDelay: Allocating additional memory for OWPBucket buffer!");

            if(!(node = calloc(sizeof(OWPBucketRec),stats->blistlen))){
                OWPError(stats->ctx,OWPErrFATAL,errno,"calloc(): %M");
                return False;
            }

            node[0].next = stats->ballocated;
            stats->ballocated = node;
            for(i=1;i<stats->blistlen;i++){
                node[i].next = stats->bfreelist;
                stats->bfreelist = &node[i];
            }
        }

        node = stats->bfreelist;
        stats->bfreelist = stats->bfreelist->next;

        node->next = NULL;
        node->b = b;
        node->n = 0;

        k.dptr = &node->b;
        k.dsize = sizeof(node->b);
        v.dptr = node;
        v.dsize = sizeof(*node);

        if(I2HashStore(stats->btable,k,v) != 0){
            return False;
        }
    }

    /*
     * Increment number of samples.
     */
    node->n++;

    return True;
}

/*
 * Stats utility functions:
 *
 * The Stats functions are used to create/free context for statistics
 * functions as well as providing those functions.
 */

/*
 * Function:    OWPStatsFree
 *
 * Description:    
 *              Used to free a Stats object.
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 */
void
OWPStatsFree(
        OWPStats    stats
        )
{
    if(!stats)
        return;

    if(stats->rn){
        free(stats->rn);
        stats->rn = NULL;
    }
    if(stats->rseqno){
        free(stats->rseqno);
        stats->rseqno = NULL;
    }

    if(stats->bsort){
        free(stats->bsort);
        stats->bsort = NULL;
        stats->bsortlen = 0;
    }
    if(stats->btable){
        I2HashClose(stats->btable);
    }
    while(stats->ballocated){
        OWPBucket   t;

        t = stats->ballocated->next;
        free(stats->ballocated);
        stats->ballocated = t;
    }

    if(stats->ptable){
        I2HashClose(stats->ptable);
    }
    while(stats->pallocated){
        OWPPacket   t;

        t = stats->pallocated->next;
        free(stats->pallocated);
        stats->pallocated = t;
    }

    if(stats->sctx){
        OWPScheduleContextFree(stats->sctx);
        stats->sctx = NULL;
    }
    if(stats->skips){
        free(stats->skips);
        stats->skips = NULL;
    }
    if(stats->hdr && stats->hdr->test_spec.slots){
        free(stats->hdr->test_spec.slots);
        stats->hdr->test_spec.slots = NULL;
    }

    free(stats);

    return;
}

/*
 * Function:    OWPStatsCreate
 *
 * Description:    
 *              used to create a stats object that is used to manage
 *              statistics parsing for a given owp file.
 *
 * In Args:    
 *
 * Out Args:    
 *
 * Scope:    
 * Returns:    
 * Side Effect:    
 *
 *
 * TODO: Create a more extensible interface for create. I'm in a hurry, so
 * for now I will just create an arg for every config option, but to
 * allow for other as yet unforseen stats it would be better to provide
 * a structure with some kind of bitmask to indicate which parts of the
 * structure are valid.
 */
OWPStats
OWPStatsCreate(
        OWPContext          ctx,
        FILE                *fp,
        OWPSessionHeader    hdr,
        char                *fromhost,
        char                *tohost,
        char                scale,
        double              bucketwidth
        )
{
    char        *func = "OWPStatsCreate";
    OWPStats    stats=NULL;
    double      d;
    long int    i;
    size_t      s;

    /*
     * Verify args
     */
    if(!hdr->header || (hdr->version < 2)){
        uint32_t   version = 0;
        if(hdr->header){
            version = hdr->version;
        }
        OWPError(ctx,OWPErrFATAL,EINVAL,
                "%s: owp files must be version 2 or greater. (version = %lu)",
                func,version);
        return NULL;
    }

    /*
     * alloc base memory
     */
    if(! (stats = calloc(1,sizeof(OWPStatsRec)))){
        OWPError(ctx,OWPErrFATAL,errno,"%s: calloc(OWPStats): %M",func);
        return NULL;
    }

    stats->ctx = ctx;
    stats->fp = fp;

    /*
     * Pretty hostname/servname buffers
     */
    if( (getnameinfo((struct sockaddr*)&hdr->addr_sender,
                hdr->addr_len,
                stats->fromhost,NI_MAXHOST,
                stats->fromserv,NI_MAXSERV,
                NI_NUMERICSERV) != 0)){
        strcpy(stats->fromhost,"***");
        stats->fromserv[0] = '\0';
    }
    if(fromhost){
        strncpy(stats->fromhost,fromhost,NI_MAXHOST-1);
    }
    if( (getnameinfo((struct sockaddr*)&hdr->addr_sender,
                hdr->addr_len,stats->fromaddr,NI_MAXHOST,
                NULL,0,NI_NUMERICHOST) != 0)){
        strcpy(stats->fromaddr,"***");
    }

    if( (getnameinfo((struct sockaddr*)&hdr->addr_receiver,
                hdr->addr_len,
                stats->tohost,NI_MAXHOST,
                stats->toserv,NI_MAXSERV,
                NI_NUMERICSERV) != 0)){
        strcpy(stats->tohost,"***");
        stats->toserv[0] = '\0';
    }
    if(tohost){
        strncpy(stats->tohost,tohost,NI_MAXHOST-1);
    }
    if( (getnameinfo((struct sockaddr*)&hdr->addr_receiver,
                hdr->addr_len,stats->toaddr,NI_MAXHOST,
                NULL,0,NI_NUMERICHOST) != 0)){
        strcpy(stats->toaddr,"***");
    }

    /*
     * Scale for reports
     */
    s = sizeof(stats->scale_abrv);
    stats->scale_factor = OWPStatsScaleFactor(scale,stats->scale_abrv,&s);
    if(stats->scale_factor == 0.0){
        OWPError(ctx,OWPErrFATAL,EINVAL,"%s: Invalid scale \'%c\'",scale);
        goto error;
    }

    /*
     * Copy hdr record
     */
    memcpy(&stats->hdr_rec,hdr,sizeof(stats->hdr_rec));
    stats->hdr = &stats->hdr_rec;

    /*
     * Read slots from the file to be sure they are good.
     */
    stats->hdr->test_spec.slots = NULL;
    if(stats->hdr->test_spec.nslots){
        if( !(stats->hdr->test_spec.slots =
                    calloc(stats->hdr->test_spec.nslots,sizeof(OWPSlot)))){
            OWPError(stats->ctx,OWPErrFATAL,errno,"%s: calloc(%lu,OWPSlot): %M",
                    func,stats->hdr->test_spec.nslots);
            goto error;
        }

        if( !OWPReadDataHeaderSlots(stats->ctx,stats->fp,
                    stats->hdr->test_spec.nslots,stats->hdr->test_spec.slots)){
            OWPError(stats->ctx,OWPErrFATAL,OWPErrUNKNOWN,
                    "%s: Unable to read scheduling slots from file",func);
            goto error;
        }
    }

    /*
     * Copy skiprecs
     */
    if(stats->hdr->num_skiprecs){
        if( !(stats->skips = calloc(stats->hdr->num_skiprecs,
                        sizeof(OWPSkipRec)))){
            OWPError(stats->ctx,OWPErrFATAL,errno,
                    "%s: calloc(%lu,OWPSkipRec): %M",
                    func,stats->hdr->num_skiprecs);
            goto error;
        }
        if( !OWPReadDataSkips(stats->ctx,stats->fp,stats->hdr->num_skiprecs,
                    stats->skips)){
            OWPError(stats->ctx,OWPErrFATAL,errno,
                    "%s: Unable to read skip records from file",func);
            goto error;
        }
    }

    /*
     * Test schedule information
     */
    if( !(stats->sctx = OWPScheduleContextCreate(stats->ctx,stats->hdr->sid,
                    &stats->hdr->test_spec))){
        OWPError(stats->ctx,OWPErrFATAL,OWPErrUNKNOWN,
                "OWPStatsCreate: Unable to create schedule context");
        goto error;
    }
    stats->isctx = 0;
    stats->endnum = stats->hdr->test_spec.start_time;

    /*
     * pre-allocate packet records. Generally, the packet rate and the
     * loss-timeout can be used to determine how many packet records
     * are need. Because the exp distribution could actually produce
     * more packets than the rate allows for in a specific time period
     * a factor is needed to allocate something greater than this amount.
     * If this factor is too small, there will be entries in syslog and
     * it can be increased. (A dynmic allocation will happen in this event.)
     */
    if (hdr->twoway) {
        d = 0.0;
    } else {
#define PACKETBUFFERALLOCFACTOR   3.5
        d = OWPTestPacketRate(stats->ctx,&stats->hdr->test_spec) *
            OWPNum64ToDouble(stats->hdr->test_spec.loss_timeout) *
            PACKETBUFFERALLOCFACTOR;
    }
    if(d > 0x7fffffffL){
        OWPError(stats->ctx,OWPErrDEBUG,OWPErrUNKNOWN,
                "%s: Extreme packet rate (%g) requires excess memory usage",d);
        stats->plistlen = 0x7fffffffL;
    }
    else{
        stats->plistlen = d;
    }
    stats->plistlen = MAX(stats->plistlen,10); /* never alloc less than 10 */

    if( !(stats->pallocated = calloc(stats->plistlen,sizeof(OWPPacketRec)))){
            OWPError(stats->ctx,OWPErrFATAL,errno,
                    "%s: calloc(%lu,OWPPacketRec): %M",func,stats->plistlen);
            goto error;
    }

    /*
     * Packet buffer hash table
     */
    if( !(stats->ptable = I2HashInit(OWPContextErrHandle(stats->ctx),
                    stats->plistlen,NULL,NULL))){
        OWPError(stats->ctx,OWPErrFATAL,OWPErrUNKNOWN,
                "%s: Unable to allocate PacketRec hash");
        goto error;
    }

    /*
     * [0] is used to track the list of allocated arrays so they can
     * be freed. (So, only index 1 and above from each array are actually
     * used.)
     * This loop takes each one of those nodes and puts them in the freelist.
     */
    for(i=1; i<stats->plistlen; i++){
        stats->pallocated[i].next = stats->pfreelist;
        stats->pfreelist = &stats->pallocated[i];
    }

    /*
     * pre-allocate bucket records. Generally, the bucket width can
     * be used to estimate a reasonable number of buckets.
     *
     * The number of buckets needed is bounded by the loss timeout.
     *
     * XXX: It is further bounded by MIN_BUCKETS and MAX_BUCKETS. These
     * constants can be adjusted or even removed...)
     *
     */
#define MIN_BUCKETS 10
#define MAX_BUCKETS 2048
    assert(bucketwidth > 0.0);
    stats->bucketwidth = bucketwidth;
    d = stats->hdr->test_spec.loss_timeout / stats->bucketwidth;
    if(d > 0x7fffffffL){
        stats->blistlen = 0x7fffffffL;
    }
    else{
        stats->blistlen = d;
    }
    stats->blistlen = MAX(stats->blistlen,MIN_BUCKETS); /* never less than */
    stats->blistlen = MIN(stats->blistlen,MAX_BUCKETS); /* never more than */
    if( !(stats->ballocated = calloc(stats->blistlen,sizeof(OWPBucketRec)))){
            OWPError(stats->ctx,OWPErrFATAL,errno,
                    "%s: calloc(%lu,OWPBucketRec): %M",func,stats->blistlen);
            goto error;
    }

    /*
     * Bucket hash table
     */
    if( !(stats->btable = I2HashInit(OWPContextErrHandle(stats->ctx),
                    stats->blistlen,NULL,NULL))){
        OWPError(stats->ctx,OWPErrFATAL,OWPErrUNKNOWN,
                "%s: Unable to allocate BucketRec hash");
        goto error;
    }

    /*
     * [0] is used to track the list of allocated arrays so they can
     * be freed. (So, only index 1 and above from each array are actually
     * used.)
     * This loop takes each one of those nodes and puts them in the freelist.
     */
    for(i=1; i<stats->blistlen; i++){
        stats->ballocated[i].next = stats->bfreelist;
        stats->bfreelist = &stats->ballocated[i];
    }

    /*
     * reordering buffers
     */
    stats->rlistlen = stats->plistlen;
    if( !(stats->rseqno = calloc(stats->rlistlen,sizeof(uint32_t)))){
            OWPError(stats->ctx,OWPErrFATAL,errno,
                    "%s: calloc(%lu,uint32_t): %M",func,stats->rlistlen);
            goto error;
    }
    if( !(stats->rn = calloc(stats->rlistlen,sizeof(uint32_t)))){
            OWPError(stats->ctx,OWPErrFATAL,errno,
                    "%s: calloc(%lu,uint32_t): %M",func,stats->rlistlen);
            goto error;
    }

    return stats;

error:
    OWPStatsFree(stats);

    return NULL;
}

static OWPBoolean
PacketBeginFlush(
        OWPStats    stats
        )
{
    OWPPacket   node = stats->pbegin;
    OWPBoolean  keep_parsing = True;

    if(!node){
        OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                "PacketBeginFlush: begin node empty?");
        return False;
    }

    /*
     * Move begin skip to next skip if needed (based on node->seq).
     */
    while(stats->skips && (stats->iskip < (long int)stats->hdr->num_skiprecs) &&
            (node->seq > stats->skips[stats->iskip].end)){
        stats->iskip++;
    }

    /*
     * Check if in "skip" range. If so, then skip aggregation information
     * and flush the packet. (iskip has been forwarded to guarentee the
     * first skip range is the only possible match.)
     */
    if(stats->skips && (stats->iskip < (long int)stats->hdr->num_skiprecs) &&
            (node->seq >= stats->skips[stats->iskip].begin)){
        goto flush;
    }

    /*
     * Loss and Dup Stats Happen Here
     */
    if(node->lost){
        /* count lost packets */
        stats->lost++;
    }
    else if(node->seen){
        /* count dups */
        stats->dups += (node->seen - 1);
    }

flush:

    /* Retain the last scheduled timestamp */
    stats->end_time = node->schedtime;

    if(node->next){
        stats->pbegin = node->next;
    }
    else if((node->seq+1) < stats->last){
        stats->pbegin = PacketGet(stats,node->seq+1);
    }
    else{
        keep_parsing = False;
    }

    PacketFree(stats,node);

    return keep_parsing;
}

static int
IterateSummarizeSession(
        OWPDataRec  *rec,
        void        *cdata
        )
{
    OWPStats    stats = cdata;
    OWPPacket   node;
    double      d;
    double      derr;
    long int    i;

    /*
     * Mark the first offset that has a seq greater than currently
     * interested in. This allows the caller to know what offset to
     * use for the "beginning" of the next summary.
     */
    if(!stats->next_oset && (rec->seq_no >= stats->last)){
        stats->next_oset = stats->begin_oset + stats->i * stats->hdr->rec_size;
    }

    /* increase file index */
    stats->i++;

    /*
     * return (cont processing) if this record is not part of this sum-session
     *
     * XXX: This may not be completely correct with respect to reordering...
     * If the first packet of the "next" session takes place before the
     * last packet of the "previous" session - should reordering be counted?
     *
     */ 
    if((rec->seq_no < stats->first) || (rec->seq_no >= stats->last)){
        return 0;
    }

    /*
     * Flush OWPPacket buffer before dealing with this packet so the buffer
     * only holds as many records as is needed.
     *
     */
    if(OWPIsLostRecord(rec)){
        /*
         * if current rec is lost, then all seq nums less than this one
         * can be flushed.
         */
        while(stats->pbegin->seq < rec->seq_no){
            if(!PacketBeginFlush(stats)) {
                OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                         "IterateSummarizeSession: Unable to flush lost packets");
                return -1;
            }
        }
    }else{
        /*
         * If this packet is not lost, then compute recv-lossThresh
         * and flush all packets with "sent" before this time.
         */
        OWPNum64    thresh = OWPNum64Sub(rec->recv.owptime,
                stats->hdr->test_spec.loss_timeout);

        while(OWPNum64Cmp(stats->pbegin->schedtime,thresh) < 0){
            if(!PacketBeginFlush(stats)) {
                OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                         "IterateSummarizeSession: Unable to flush packets");
                return -1;
            }
        }
    }

    /*
     * Fetch current packet record
     */
    if( !(node = PacketGet(stats,rec->seq_no))){
        OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                "IterateSummarizeSession: Unable to fetch packet #%lu",
                rec->seq_no);
        return -1;
    }

    /*
     * Check if in "skip" range. If so, then skip aggregation information
     * for this record.
     */
    i = stats->iskip;
    while(stats->skips && (i < (long int)stats->hdr->num_skiprecs)){
        if((node->seq >= stats->skips[i].begin) &&
                (node->seq <= stats->skips[i].end)){
            return 0;
        }
        i++;
    }

    if( OWPIsLostRecord(rec)){
        /*
         * If this has been seen before, then we have a problem.
         */
        if(node->seen){
            OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                    "IterateSummarizeSession: Unexpected lost packet record");
            return -1;
        }
        node->lost = True;
        stats->sent++;

        /* sync */
        if(!rec->recv.sync){
            stats->sync = 0;
        }

        /*
         * Time error
         */
        derr = OWPGetTimeStampError(&rec->recv);
        stats->maxerr = MAX(stats->maxerr,derr);

        if(stats->output){
            fprintf(stats->output,"seq_no=%-10u *LOST*\n", rec->seq_no);
        }

        return 0;
    }
    else{
        /*
         * If this has already been declared lost, we have a problem.
         */
        if(node->lost){
            OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                    "IterateSummarizeSession: Unexpected duplicate packet record (for lost one)");
            return -1;
        }

        if(!node->seen){
            stats->sent++;
        }
        node->seen++;
    }

    /*
     * j-reordering. See:
     * http://www.internet2.edu/~shalunov/ippm/\
     *                          draft-shalunov-reordering-definition-02.txt
     */
#define rseqindex(x)    ((x) >= 0? x: x + stats->rlistlen)
    for(i=0;i < MIN(stats->rnumseqno,stats->rlistlen) &&
            rec->seq_no < stats->rseqno[rseqindex(stats->rindex-i-1)];i++){
        stats->rn[i]++;
    }
    stats->rseqno[stats->rindex] = rec->seq_no;
    stats->rnumseqno++;
    stats->rindex++;
    stats->rindex %= stats->rlistlen;
#undef rseqindex

    /* sync */
    if(!rec->send.sync || !rec->recv.sync){
        stats->sync = 0;
    }

    /*
     * compute delay for this packet
     */
    d = OWPDelay(&rec->send, &rec->recv);

    /*
     * compute total error from send/recv
     */
    derr = OWPGetTimeStampError(&rec->send) + OWPGetTimeStampError(&rec->recv);
    stats->maxerr = MAX(stats->maxerr,derr);

    /*
     * Print individual packet record
     */
    if(stats->output){
        if(rec->send.sync && rec->recv.sync){
	  if (stats->display_unix_ts == True) {
	    /* print using unix timestamp */
	    double epochdiff = (OWPULongToNum64(OWPJAN_1970))>>32;
	    fprintf(stats->output,
		    "seq_no=%d delay=%e %s (sync, err=%.3g %s) sent=%f recv=%f\n",
		    rec->seq_no, d*stats->scale_factor, stats->scale_abrv,
		    derr*stats->scale_factor, stats->scale_abrv,
		    OWPNum64ToDouble(rec->send.owptime) - epochdiff,
		    OWPNum64ToDouble(rec->recv.owptime) - epochdiff
		    );
	  } 
	  else {
	    /* print the default */
	    fprintf(stats->output,
		    "seq_no=%-10u delay=%.3g %s\t(sync, err=%.3g %s)\n",
		    rec->seq_no, d*stats->scale_factor, stats->scale_abrv,
		    derr*stats->scale_factor,stats->scale_abrv);
	  }
        }
        else{
            fprintf(stats->output,
                    "seq_no=%-10u delay=%.3g %s\t(unsync)\n",
                    rec->seq_no, d*stats->scale_factor,stats->scale_abrv);
        }
    }

    /*
     * Save max/min delays
     */
    stats->min_delay = MIN(stats->min_delay,d);
    stats->max_delay = MAX(stats->max_delay,d);

    /*
     * Delay and TTL stats not computed on duplicates
     */
    if(node->seen > 1){
        return 0;
    }

    /*
     * Increment histogram for this delay
     */
    if( !BucketIncrementDelay(stats,d)){
        /* error return */
        OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                "IterateSummarizeSession: Unable to increment delay bucket");
        return -1;
    }

    /*
     * TTL info
     */
    stats->ttl_count[rec->ttl]++;

    return 0;
}


static int
IterateSummarizeTWSession(
        OWPTWDataRec *rec,
        void        *cdata
        )
{
    OWPStats    stats = cdata;
    OWPPacket   node;
    double      proc_d;
    double      d;
    double      derr;
    long int    i;

    /*
     * Mark the first offset that has a seq greater than currently
     * interested in. This allows the caller to know what offset to
     * use for the "beginning" of the next summary.
     */
    if(!stats->next_oset && (rec->sent.seq_no >= stats->last)){
        stats->next_oset = stats->begin_oset + stats->i * stats->hdr->rec_size;
    }

    /* increase file index */
    stats->i++;

    /*
     * return (cont processing) if this record is not part of this sum-session
     *
     * XXX: This may not be completely correct with respect to reordering...
     * If the first packet of the "next" session takes place before the
     * last packet of the "previous" session - should reordering be counted?
     *
     */
    if((rec->sent.seq_no < stats->first) || (rec->sent.seq_no >= stats->last)){
        return 0;
    }

    /*
     * Flush OWPPacket buffer before dealing with this packet so the buffer
     * only holds as many records as is needed.
     *
     */
    if(OWPIsLostRecord(&rec->sent)){
        /*
         * if current rec is lost, then all seq nums less than this one
         * can be flushed.
         */
        while(stats->pbegin->seq < rec->sent.seq_no){
            if(!PacketBeginFlush(stats)) {
                OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                         "IterateTWSummarizeSession: Unable to flush lost packets");
                return -1;
            }
        }
    }else{
        /*
         * If this packet is not lost, then compute recv-lossThresh
         * and flush all packets with "sent" before this time.
         */
        OWPNum64    thresh = OWPNum64Sub(rec->sent.send.owptime,
                stats->hdr->test_spec.loss_timeout);

        while(OWPNum64Cmp(stats->pbegin->schedtime,thresh) < 0){
            if(!PacketBeginFlush(stats)) {
                OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                         "IterateTWSummarizeSession: Unable to flush packets");
                return -1;
            }
        }
    }

    /*
     * Fetch current packet record
     */
    if( !(node = PacketGet(stats,rec->sent.seq_no))){
        OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                "IterateSummarizeTWSession: Unable to fetch packet #%lu",
                rec->sent.seq_no);
        return -1;
    }

    /*
     * Check if in "skip" range. If so, then skip aggregation information
     * for this record.
     */
    i = stats->iskip;
    while(stats->skips && (i < (long int)stats->hdr->num_skiprecs)){
        if((node->seq >= stats->skips[i].begin) &&
                (node->seq <= stats->skips[i].end)){
            return 0;
        }
        i++;
    }

    if( OWPIsLostRecord(&rec->sent)){
        /*
         * If this has been seen before, then we have a problem.
         */
        if(node->seen){
            OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                    "IterateSummarizeTWSession: Unexpected lost packet record");
            return -1;
        }
        node->lost = True;
        stats->sent++;

        /* sync */
        if(!rec->sent.recv.sync){
            stats->sync = 0;
        }

        /*
         * Time error
         */
        derr = OWPGetTimeStampError(&rec->sent.recv);
        stats->maxerr = MAX(stats->maxerr,derr);

        if(stats->output){
            fprintf(stats->output,"seq_no=%-10u *LOST*\n", rec->sent.seq_no);
        }

        return 0;
    }
    else{
        /*
         * If this has already been declared lost, we have a problem.
         */
        if(node->lost){
            OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                    "IterateSummarizeTWSession: Unexpected duplicate packet record (for lost one)");
            return -1;
        }

        if(!node->seen){
            stats->sent++;
        }
        node->seen++;
    }

    /*
     * j-reordering. See:
     * http://www.internet2.edu/~shalunov/ippm/\
     *                          draft-shalunov-reordering-definition-02.txt
     */
#define rseqindex(x)    ((x) >= 0? x: x + stats->rlistlen)
    for(i=0;i < MIN(stats->rnumseqno,stats->rlistlen) &&
            rec->sent.seq_no < stats->rseqno[rseqindex(stats->rindex-i-1)];i++){
        stats->rn[i]++;
    }
    stats->rseqno[stats->rindex] = rec->sent.seq_no;
    stats->rnumseqno++;
    stats->rindex++;
    stats->rindex %= stats->rlistlen;
#undef rseqindex

    /*
     * compute processing delay on far end
     */
    proc_d = OWPDelay(&rec->sent.recv, &rec->reflected.send);
    /*
     * compute best possible estimate for network round-trip time for this packet
     */
    d = OWPDelay(&rec->sent.send, &rec->reflected.recv) - proc_d;

    /*
     * compute total error from send/recv.
     */
    derr = OWPGetTimeStampError(&rec->sent.send) +
        OWPGetTimeStampError(&rec->reflected.recv) +
        OWPGetTimeStampError(&rec->sent.recv);
    stats->maxerr = MAX(stats->maxerr,derr);

    /*
     * Print individual packet record
     */
    if(stats->output){
        if (stats->display_unix_ts == True) {
            /* print using unix timestamp */
            double epochdiff = (OWPULongToNum64(OWPJAN_1970))>>32;
            fprintf(stats->output,
                    "seq_no=%d delay=%e %s proc_delay=%e %s (err=%.3g %s) sent=%f reflected=%f recv=%f\n",
                    rec->sent.seq_no, d*stats->scale_factor, stats->scale_abrv,
                    proc_d*stats->scale_factor, stats->scale_abrv,
                    derr*stats->scale_factor, stats->scale_abrv,
                    OWPNum64ToDouble(rec->sent.send.owptime) - epochdiff,
                    OWPNum64ToDouble(rec->reflected.send.owptime) - epochdiff,
                    OWPNum64ToDouble(rec->reflected.recv.owptime) - epochdiff
                );
        }
        else {
            /* print the default */
            fprintf(stats->output,
                    "seq_no=%-10u delay=%.3g %s proc_delay=%.3g %s\t(err=%.3g %s)\n",
                    rec->sent.seq_no, d*stats->scale_factor, stats->scale_abrv,
                    proc_d*stats->scale_factor, stats->scale_abrv,
                    derr*stats->scale_factor,stats->scale_abrv);
        }
    }

    /*
     * Save max/min delays
     */
    stats->min_delay = MIN(stats->min_delay,d);
    stats->max_delay = MAX(stats->max_delay,d);
    stats->min_proc_delay = MIN(stats->min_proc_delay,proc_d);
    stats->max_proc_delay = MAX(stats->max_proc_delay,proc_d);

    /*
     * Delay and TTL stats not computed on duplicates
     */
    if(node->seen > 1){
        return 0;
    }

    /*
     * Increment histogram for this delay
     */
    if( !BucketIncrementDelay(stats,d)){
        /* error return */
        OWPError(stats->ctx,OWPErrFATAL,EINVAL,
                "IterateSummarizeTWSession: Unable to increment delay bucket");
        return -1;
    }

    /*
     * TTL info
     */
    stats->ttl_count[rec->sent.ttl]++;

    return 0;
}

static void
PrintStatsHeader(
        OWPStats    stats,
        FILE        *output
        )
{
    char        sid_name[sizeof(OWPSID)*2+1];

    if(!output)
        return;

    fprintf(output,"\n--- %s statistics from [%s]:%s to [%s]:%s ---\n",
            stats->hdr->twoway ? "twping" : "owping",
            stats->fromhost,stats->fromserv,stats->tohost,stats->toserv);
    I2HexEncode(sid_name,stats->hdr->sid,sizeof(OWPSID));
    fprintf(output,"SID:\t%s\n",sid_name);

    return;
}

static int
BucketBufferSortCmp(
        const void *bp1,
        const void *bp2)
{
    OWPBucket   b1 = *(OWPBucket *)bp1;
    OWPBucket   b2 = *(OWPBucket *)bp2;

    return (b1->b - b2->b);
}

static OWPBoolean
BucketBufferSortPercentile(
        OWPStats    stats,
        double      alpha,
        double      *delay_ret
        )
{
    uint32_t    i;
    double      sum=0;

    assert((0.0 <= alpha) && (alpha <= 1.0));

    for(i=0;
            (i < stats->bsortsize) &&
            ((stats->bsort[i]->n + sum) < (alpha * stats->sent));
            i++){
        sum += stats->bsort[i]->n;
    }

    if(i >= stats->bsortsize){
        return False;
    }

    *delay_ret = stats->bsort[i]->b * stats->bucketwidth;
    return True;
}

OWPBoolean
OWPStatsParse(
        OWPStats    stats,
        FILE        *output,
        off_t       begin_oset,
        uint32_t    first,
        uint32_t    last
        )
{
    off_t       fileend;
    uint32_t    nrecs;
    long int    i;

    if(last == (uint32_t)~0){
        last = stats->hdr->test_spec.npackets;
    }
    if((first > last) || (last > stats->hdr->test_spec.npackets)){
        OWPError(stats->ctx,OWPErrFATAL,OWPErrINVALID,
                "OWPStatsParse: Invalid sample range [%lu,%lu]",first,last);
        return False;
    }

    stats->begin_oset = begin_oset;
    stats->next_oset = 0;
    stats->first = first;
    stats->last = last;
    stats->iskip = 0;
    stats->sent = 0;

    /*
     * Initialize file record information: oset's/ record index/ nrecs
     */

    stats->i = 0;

    /*
     * determine end of packet records in file.
     */
    if(stats->hdr->oset_skiprecs > stats->hdr->oset_datarecs){
        fileend = stats->hdr->oset_skiprecs;
    }
    else{
        if(fseeko(stats->fp,0,SEEK_END) != 0){
            OWPError(stats->ctx,OWPErrFATAL,errno,
                    "OWPStatsParse: fseeko(): %M");
            return False;
        }
        if((fileend = ftello(stats->fp)) < 0){
            OWPError(stats->ctx,OWPErrFATAL,errno,
                    "OWPStatsParse: ftello(): %M");
            return False;
        }
    }

    /* determine position of first record */
    if(stats->begin_oset < stats->hdr->oset_datarecs){
        stats->begin_oset = stats->hdr->oset_datarecs;
    }

    /* position fp to start */
    if(fseeko(stats->fp,stats->begin_oset,SEEK_SET) != 0){
        OWPError(stats->ctx,OWPErrFATAL,errno,
                "OWPStatsParse: fseeko(): %M");
        return False;
    }

    /* determine how many records to look through */
    nrecs = (fileend - stats->begin_oset) / stats->hdr->rec_size;

    if ((stats->rec_limit > 0) && (stats->rec_limit < nrecs))
      nrecs = stats->rec_limit;

    /*
     * Initialize statistics variables
     */

    /* Schedule information: advance sctx to appropriate value */
    if( !first || (first < stats->isctx)){
        OWPScheduleContextReset(stats->sctx,NULL,NULL);
        stats->isctx = 0;
        stats->endnum = stats->hdr->test_spec.start_time;
    }
    while(stats->isctx <= first){
        stats->endnum = OWPNum64Add(stats->endnum,
                OWPScheduleContextGenerateNextDelta(stats->sctx));
        stats->isctx++;
    }
    stats->start_time = stats->endnum;

    /*
     * PacketBuffer stuff (used for dups,lost)
     * First clear out any existing data from the packet buffer, then
     * initialize with first record needed.
     */

    /* clean up */
    I2HashIterate(stats->ptable,PacketBufferClean,stats);

    /* alloc first node */
    stats->pbegin = stats->pend = PacketAlloc(stats,first);

    /*
     * update sctx/isctx to approprate place
     */

    /* initialize first node with appropriate sched time */
    stats->pbegin->schedtime = stats->endnum;

    /*
     *
     * Clear btable
     *  iterate with BucketBufferClean
     *
     * init stats (min/max/ttl stuff)
     */

    /* clean up */
    I2HashIterate(stats->btable,BucketBufferClean,stats);

    /* ttl */
    for(i=0;i<256;i++){
        stats->ttl_count[i] = 0;
    }

    /* re-order buffers */
    for(i=0;i<stats->rlistlen;i++){
        stats->rseqno[i]=0;
        stats->rn[i]=0;
    }

    /* init min_delay to +inf, max_delay to -inf */
    stats->inf_delay = OWPNum64ToDouble(stats->hdr->test_spec.loss_timeout + 1);
    stats->min_delay = stats->inf_delay;
    stats->max_delay = -stats->inf_delay;
    stats->min_proc_delay = stats->inf_delay;
    stats->max_proc_delay = -stats->inf_delay;

    /* timestamp quality */
    stats->sync = 1;
    stats->maxerr = 0.0;

    /* dups/lost */
    stats->dups = stats->lost = 0;

    /*
     * Iterate function to read all data
     */
    PrintStatsHeader(stats,output);
    stats->output = output;
    if (stats->hdr->twoway) {
        if(OWPParseTWRecords(stats->ctx,stats->fp,nrecs,stats->hdr->version,
                             IterateSummarizeTWSession,(void*)stats) != OWPErrOK){
            OWPError(stats->ctx,OWPErrFATAL,OWPErrUNKNOWN,
                     "OWPStatsParse: iteration of twoway data records failed");
            stats->output = NULL;
            return False;
        }
    } else {
        if(OWPParseRecords(stats->ctx,stats->fp,nrecs,stats->hdr->version,
                           IterateSummarizeSession,(void*)stats) != OWPErrOK){
            OWPError(stats->ctx,OWPErrFATAL,OWPErrUNKNOWN,
                     "OWPStatsParse: iteration of data records failed");
            stats->output = NULL;
            return False;
        }
    }
    stats->output = NULL;

    /*
     * Process remaining buffered packet records
     */
    while(stats->pbegin && PacketBeginFlush(stats));

    /*
     * Sort Delay histogram
     */

    /* alloc sort array */
    stats->bsortsize = I2HashNumEntries(stats->btable);
    if(stats->bsortlen < stats->bsortsize){
        OWPBucket   *tbp;
        if( (tbp = realloc(stats->bsort,sizeof(OWPBucket)*stats->bsortsize))){
            stats->bsortlen = stats->bsortsize;
            stats->bsort = tbp;
        }
        else{
            OWPError(stats->ctx,OWPErrFATAL,errno,
                    "OWPStatsParse: allocating memory for sort array: %M");
            return False;
        }
    }

    /* fill sort array with entries */
    stats->bsorti = 0;
    I2HashIterate(stats->btable,BucketBufferSortFill,stats);
    assert(stats->bsorti == stats->bsortsize);

    /* sort */
    qsort(stats->bsort,stats->bsortsize,sizeof(OWPBucket),BucketBufferSortCmp);

    /*
     * Stats structure now holds complete statistics information
     */

    return True;
}

/*
 * Return the correct scale factor to use for the given scale indication.
 * The abbreviation used for this scale is returned, if abrv is non-null
 * and *abrv_len is long enough.
 *
 * Returns 0.0 on error.
 */
float
OWPStatsScaleFactor(
        char    scale,
        char    *abrv,
        size_t  *abrv_len)
{
    double  factor = 1.0;

    /* 3 (2 char abrv and nul byte) */
    if(abrv && abrv_len && (*abrv_len >= 3)){
        *abrv_len = 2;
    }
    else{
        abrv = NULL;
    }

    switch(tolower(scale)){
        case 'n':
            factor *= 1000000000; /* 1e-9 */
            if(abrv) strcpy(abrv,"ns");
            break;
        case 'u':
            factor *= 1000000; /* 1e-6 */
            if(abrv) strcpy(abrv,"us");
            break;
        case 'm':
            factor *= 1000; /* 1e-3 */
            if(abrv) strcpy(abrv,"ms");
        case 's':
            break;
        default:
            if(abrv_len){
                *abrv_len = 0;
            }
            return 0.0;
    }

    return factor;
}

/*
 * Human-readable statistics summary
 */
OWPBoolean
OWPStatsPrintSummary(
        OWPStats    stats,
        FILE        *output,
        float       *percentiles,
        uint32_t   npercentiles
        )
{
    long int        i;
    uint32_t        ui;
    uint8_t    	    nttl=0;
    uint8_t    	    minttl=255;
    uint8_t    	    maxttl=0;
    char            minval[80];
    char            maxval[80];
    char            n1val[80];
    double          d1, d2;
    struct timespec sspec;
    struct timespec espec;
    struct timespec *sspecp,*especp;
    struct tm       stm,etm;
    struct tm       *stmp,*etmp;
    char            stval[50],etval[50];
    OWPTimeStamp    ttstamp;

    PrintStatsHeader(stats,output);

    /*
     * Print out timerange
     */
    memset(&stm,0,sizeof(stm));
    memset(&etm,0,sizeof(etm));
    memset(&sspec,0,sizeof(sspec));
    memset(&espec,0,sizeof(espec));

    /* set start-time string */
    ttstamp.owptime = stats->start_time;
    if( !(sspecp = OWPTimestampToTimespec(&sspec,&ttstamp))){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: OWPTimestampToTimespec(): Unable to convert time value");
        strncpy(stval,"XXX",sizeof(stval));
    }
    else if( !(stmp = localtime_r(&sspecp->tv_sec,&stm))){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: localtime_r(): Unable to convert time value");
        strncpy(stval,"XXX",sizeof(stval));
    }
    else if( !strftime(stval,sizeof(stval),"%FT%T",stmp)){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: strftime(): Unable to convert time value");
        strncpy(stval,"XXX",sizeof(stval));
    }

    /* set end-time string */
    ttstamp.owptime = stats->end_time;
    if( !(especp = OWPTimestampToTimespec(&espec,&ttstamp))){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: OWPTimestampToTimespec(): Unable to convert time value");
        strncpy(etval,"XXX",sizeof(etval));
    }
    else if( !(etmp = localtime_r(&especp->tv_sec,&etm))){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: localtime_r(): Unable to convert time value");
        strncpy(etval,"XXX",sizeof(etval));
    }
    else if( !strftime(etval,sizeof(etval),"%FT%T",etmp)){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: strftime(): Unable to convert time value");
        strncpy(etval,"XXX",sizeof(etval));
    }

    /*
     * Divide the integer nanoseconds by 1 million to get 3 significant
     * digits of the fractional seconds to the left of the decimal point.
     */
#define MILLION (1000000)
    fprintf(output,"first:\t%s.%03.0f\nlast:\t%s.%03.0f\n",
            stval,((float)sspec.tv_nsec)/MILLION,
            etval,((float)espec.tv_nsec)/MILLION);

    /*
     * lost % is 0 if sent == 0.
     */
    if(stats->sent > 0){
        d1 = (double)stats->lost/stats->sent;
    }
    else{
        d1 = 0.0;
    }
    fprintf(output,"%u sent, %u lost (%.3f%%), %u duplicates\n",
            stats->sent,stats->lost,100.0*d1,stats->dups);

    /*
     * Min, Median
     */

    /*
     * parse min/max - Sure would be easier if C99 soft-float were portable...
     * XXX: Just use NAN as the float value once that works everywhere!
     *      (BucketBufferSortPercentile would be WAY easier!!!)
     */
    if(stats->min_delay >= stats->inf_delay){
        strncpy(minval,"nan",sizeof(minval));
    }
    else if( (snprintf(minval,sizeof(minval),"%.3g",
                    stats->min_delay * stats->scale_factor) < 0)){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: snprintf(): %M");
        strncpy(minval,"XXX",sizeof(minval));
    }
    if(stats->max_delay <= -stats->inf_delay){
        strncpy(maxval,"nan",sizeof(maxval));
    }
    else if( (snprintf(maxval,sizeof(maxval),"%.3g",
                    stats->max_delay * stats->scale_factor) < 0)){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: snprintf(): %M");
        strncpy(maxval,"XXX",sizeof(maxval));
    }

    if( !BucketBufferSortPercentile(stats,0.5,&d1)){
        strncpy(n1val,"nan",sizeof(n1val));
    }
    else if(snprintf(n1val,sizeof(n1val),"%.3g",
                      d1 * stats->scale_factor) < 0){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: snprintf(): %M");
        strncpy(n1val,"XXX",sizeof(n1val));
    }


    fprintf(output,"%s min/median/max = %s/%s/%s %s, ",
            stats->hdr->twoway ? "round-trip time" : "one-way delay",
            minval,n1val,maxval,stats->scale_abrv);
    if(stats->sync){
        fprintf(output,"(err=%.3g %s)\n",stats->maxerr * stats->scale_factor,
                stats->scale_abrv);
    }
    else{
        fprintf(output,"(unsync)\n");
    }
    if (stats->hdr->twoway) {
        if(stats->min_proc_delay >= stats->inf_delay){
            strncpy(minval,"nan",sizeof(minval));
        }
        else if( (snprintf(minval,sizeof(minval),"%.3g",
                           stats->min_proc_delay * stats->scale_factor) < 0)){
            OWPError(stats->ctx,OWPErrWARNING,errno,
                     "OWPStatsPrintSummary: snprintf(): %M");
            strncpy(minval,"XXX",sizeof(minval));
        }
        if(stats->max_proc_delay <= -stats->inf_delay){
            strncpy(maxval,"nan",sizeof(maxval));
        }
        else if( (snprintf(maxval,sizeof(maxval),"%.3g",
                           stats->max_proc_delay * stats->scale_factor) < 0)){
            OWPError(stats->ctx,OWPErrWARNING,errno,
                     "OWPStatsPrintSummary: snprintf(): %M");
            strncpy(maxval,"XXX",sizeof(maxval));
        }
        fprintf(output,"reflector processing time min/max = %s/%s %s\n",
                minval,maxval,stats->scale_abrv);
    }


    /*
     * "jitter"
     */
    if( !BucketBufferSortPercentile(stats,0.95,&d1) ||
        !BucketBufferSortPercentile(stats,0.5,&d2)){
        strncpy(n1val,"nan",sizeof(n1val));
    }
    else if(snprintf(n1val,sizeof(n1val),"%.3g",
                      (d1-d2) * stats->scale_factor) < 0){
        OWPError(stats->ctx,OWPErrWARNING,errno,
                    "OWPStatsPrintSummary: snprintf(): %M");
        strncpy(n1val,"XXX",sizeof(n1val));
    }
    fprintf(output,"%s = %s %s (P95-P50)\n",
            stats->hdr->twoway ? "two-way PDV" : "one-way jitter",
            n1val,stats->scale_abrv);

    /*
     * Print out random percentiles
     */
    if(npercentiles){
        fprintf(output,"Percentiles:\n");
        for(ui=0;ui<npercentiles;ui++){
            if( !BucketBufferSortPercentile(stats,percentiles[ui]/100.0,&d1)){
                strncpy(n1val,"nan",sizeof(n1val));
            }
            else if(snprintf(n1val,sizeof(n1val),"%.3g",
                        d1 * stats->scale_factor) < 0){
                OWPError(stats->ctx,OWPErrWARNING,errno,
                        "OWPStatsPrintSummary: snprintf(): %M");
                strncpy(n1val,"XXX",sizeof(n1val));
            }
            fprintf(output,"\t%.1f: %s %s\n",
                    percentiles[ui],n1val,stats->scale_abrv);
        }
    }

    /*
     * Report ttl's
     */
    for(i=0;i<255;i++){
        if(!stats->ttl_count[i])
            continue;
        nttl++;
        if(i<minttl)
            minttl = i;
        if(i>maxttl)
            maxttl = i;
    }

    if(nttl < 1){
        fprintf(output,"TTL not reported\n");
    }
    else if(nttl == 1){
        fprintf(output,"Hops = %d (consistently)\n",255-minttl);
    }
    else{
        fprintf(output,"Hops takes %d values; Min Hops = %d, Max Hops = %d\n",
                nttl,255-maxttl,255-minttl);
    }

    /*
     * Report j-reordering
     */
    for(i=0;((i<stats->rlistlen) && (stats->rn[i]));i++){
        fprintf(output,"%ld-reordering = %f%%\n",i+1,
                100.0*stats->rn[i]/(stats->rnumseqno));
    }
    if(i==0){
        fprintf(output,"no reordering\n");
    }
    else if(i < stats->rlistlen){
        fprintf(output,"no %ld-reordering\n", i+1);
    }
    else{
        fprintf(output,"%ld-reordering not handled\n",stats->rlistlen+1);
    }

    fprintf(output,"\n");

    return True;
}

/*
 * Program-readable statistics summary
 */
OWPBoolean
OWPStatsPrintMachine(
        OWPStats    stats,
        FILE        *output
        )
{
    /* Version 3.0 of stats output */
    float       version=3.0;
    char        sid_name[sizeof(OWPSID)*2+1];
    uint32_t    i;
    long int    j;
    uint8_t     nttl=0;
    uint8_t     minttl=255;
    uint8_t     maxttl=0;

    I2HexEncode(sid_name,stats->hdr->sid,sizeof(OWPSID));


    /*
     * Basic session information
     */
    fprintf(output,"SUMMARY\t%.2f\n",version);
    fprintf(output,"SID\t%s\n",sid_name);
    fprintf(output,"FROM_HOST\t%s\n",stats->fromhost);
    fprintf(output,"FROM_ADDR\t%s\n",stats->fromaddr);
    fprintf(output,"FROM_PORT\t%s\n",stats->fromserv);
    fprintf(output,"TO_HOST\t%s\n",stats->tohost);
    fprintf(output,"TO_ADDR\t%s\n",stats->toaddr);
    fprintf(output,"TO_PORT\t%s\n",stats->toserv);

    fprintf(output,"START_TIME\t" OWP_TSTAMPFMT "\n",stats->start_time);
    fprintf(output,"END_TIME\t" OWP_TSTAMPFMT "\n",stats->end_time);
    
    /* print unix versions of timestamp */
    if (stats->display_unix_ts == True) {
        double epochdiff = (OWPULongToNum64(OWPJAN_1970))>>32;
        fprintf(output,"UNIX_START_TIME\t%f\n", OWPNum64ToDouble(stats->start_time) - epochdiff);
        fprintf(output,"UNIX_END_TIME\t%f\n", OWPNum64ToDouble(stats->end_time) - epochdiff);
    }

    /*
     * If typeP is specified as a DSCP code-byte, then output it too.
     * (If any bits are set outside of the low-order 6 bits of the
     * high-order byte, then it is not a DSCP.)
     */
    if( !(stats->hdr->test_spec.typeP & ~0x3F000000)){
        uint8_t dscp = stats->hdr->test_spec.typeP >> 24;
        fprintf(output,"DSCP\t0x%2.2x\n",dscp);
    }
    fprintf(output,"LOSS_TIMEOUT\t%"PRIu64"\n",stats->hdr->test_spec.loss_timeout);
    fprintf(output,"PACKET_PADDING\t%u\n",
            stats->hdr->test_spec.packet_size_padding);
    fprintf(output,"SESSION_PACKET_COUNT\t%u\n",stats->hdr->test_spec.npackets);
    fprintf(output,"SAMPLE_PACKET_COUNT\t%u\n", stats->last - stats->first);
    fprintf(output,"BUCKET_WIDTH\t%g\n",stats->bucketwidth);
    fprintf(output,"SESSION_FINISHED\t%d\n",
            (stats->hdr->finished == OWP_SESSION_FINISHED_NORMAL)?1:0);

    /*
     * Summary results
     */
    fprintf(output,"SENT\t%u\n",stats->sent);
    fprintf(output,"SYNC\t%" PRIuPTR "\n",stats->sync);
    fprintf(output,"MAXERR\t%g\n",stats->maxerr);
    fprintf(output,"DUPS\t%u\n",stats->dups);
    fprintf(output,"LOST\t%u\n",stats->lost);

    if(stats->min_delay < stats->inf_delay){
        fprintf(output,"MIN\t%g\n",stats->min_delay);
    }
    if(stats->max_delay > -stats->inf_delay){
        fprintf(output,"MAX\t%g\n",stats->max_delay);
    }

    /*
     * Delay histogram
     */
    if(stats->sent > stats->lost){
        fprintf(output,"<BUCKETS>\n");
        I2HashIterate(stats->btable,BucketBufferPrint,output);
        fprintf(output,"</BUCKETS>\n");
    }

    /*
     * TTL histogram
     */
    for(i=0;i<255;i++){
        if(!stats->ttl_count[i])
            continue;
        nttl++;
        if(i<minttl)
            minttl = i;
        if(i>maxttl)
            maxttl = i;
    }

    if(nttl > 0){
        fprintf(output,"MINTTL\t%u\n",minttl);
        fprintf(output,"MAXTTL\t%u\n",minttl);
        fprintf(output,"<TTLBUCKETS>\n");
        for(i=0;i<255;i++){
            if(!stats->ttl_count[i])
                continue;
            fprintf(output,"\t%u\t%u\n",i,stats->ttl_count[i]);
        }
        fprintf(output,"</TTLBUCKETS>\n");

    }

    fprintf(output,"\n");

    /*
     * Reordering histogram
     */
    fprintf(output,"<NREORDERING>\n");
    for(j=0;((j<stats->rlistlen) && (stats->rn[j]));j++){
        fprintf(output,"\t%u\t%u\n",(uint32_t)j+1,
                stats->rn[j]);
    }
    if((j==0) || (j >= stats->rlistlen)){
        fprintf(output,"\t%u\t%u\n",(uint32_t)j+1,0);
    }
    fprintf(output,"</NREORDERING>\n");

    return True;
}
