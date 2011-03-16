/*
 * Copyright (c) 2004-2006 Maxim Sobolev <sobomax@FreeBSD.org>
 * Copyright (c) 2006-2007 Sippy Software, Inc., http://www.sippysoft.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id$
 *
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <g729_decoder.h>

#include "decoder.h"
#include "session.h"
#include "../rtp.h"
#include "g711.h"

/* static char i[] = "0"; */

void *
decoder_new(struct session *sp)
{
    struct decoder_stream *dp;

    dp = malloc(sizeof(*dp));
    if (dp == NULL)
        return NULL;
    memset(dp, 0, sizeof(*dp));
    dp->pp = MYQ_FIRST(sp);
    if (dp->pp == NULL)
        /*
         * If the queue is empty return "as is", decoder_get() then'll be
         * responsible for generating DECODER_EOF.
         */
        return (void *)dp;
    dp->sp = sp;
    dp->obp = dp->obuf;
    dp->oblen = 0;
    dp->stime = dp->pp->pkt->time;
    dp->nticks = dp->sticks = dp->pp->parsed.ts;
    dp->dticks = 0;
    dp->lpt = RTP_PCMU;
    /* dp->f = fopen(i, "w"); */
    /* i[0]++; */

    return (void *)dp;
}

int32_t
decoder_get(struct decoder_stream *dp)
{
    unsigned int cticks, t;
    int j;

    if (dp->oblen == 0) {
        if (dp->pp == NULL)
            return DECODER_EOF;
        cticks = dp->pp->parsed.ts;
        /*
         * First of all check if we can trust timestamp contained in the
         * packet. If it's off by more than 1 second than the device
         * probably gone nuts and we can't trust it anymore.
         */
        if ((double)((cticks - dp->sticks) / 8000) > (dp->pp->pkt->time - dp->stime + 1.0)) {
            dp->nticks = cticks;
            dp->sticks = cticks - (dp->pp->pkt->time - dp->stime) * 8000;
        }
        if (dp->nticks < cticks) {
            t = cticks - dp->nticks;
            if (t > 4000)
                t = 4000;
            j = generate_silence(dp, dp->obuf, t);
            if (j <= 0)
                return DECODER_ERROR;
            dp->nticks += t;
            dp->dticks += t;
            dp->oblen = j / 2;
            dp->obp = dp->obuf;
        } else if ((dp->pp->pkt->time - dp->stime - (double)dp->dticks / 8000.0) > 0.2) {
            t = (((dp->pp->pkt->time - dp->stime) * 8000) - dp->dticks) / 2;
            if (t > 4000)
                t = 4000;
            j = generate_silence(dp, dp->obuf, t);
            if (j <= 0)
                return DECODER_ERROR;
            dp->dticks += t;
            dp->oblen = j / 2;
            dp->obp = dp->obuf;
        } else {
            j = decode_frame(dp, dp->obuf, RPLOAD(dp->pp), RPLEN(dp->pp));
            if (j > 0)
                dp->lpt = dp->pp->rpkt->pt;
            dp->pp = MYQ_NEXT(dp->pp);
            if (j <= 0)
                return decoder_get(dp);
            dp->oblen = j / 2;
            dp->obp = dp->obuf;
        }
    }
    dp->oblen--;
    dp->obp += sizeof(int16_t);
    return *(((int16_t *)(dp->obp)) - 1);
}

int
decode_frame(struct decoder_stream *dp, unsigned char *obuf, unsigned char *ibuf, unsigned int ibytes)
{
    int fsize;
    unsigned int obytes;
    void *bp;

    switch (dp->pp->rpkt->pt) {
    case RTP_PCMU:
        ULAW2SL(obuf, ibuf, ibytes);
        dp->nticks += ibytes;
        dp->dticks += ibytes;
        return ibytes * 2;

    case RTP_PCMA:
        ALAW2SL(obuf, ibuf, ibytes);
        dp->nticks += ibytes;
        dp->dticks += ibytes;
        return ibytes * 2;

    case RTP_G729:
        /* fwrite(ibuf, ibytes, 1, dp->f); */
        /* fflush(dp->f); */
        if (ibytes % 10 == 0)
            fsize = 10;
        else if (ibytes % 8 == 0)
            fsize = 8;
        else if (ibytes % 15 == 0)
            fsize = 15;
        else if (ibytes % 2 == 0)
            fsize = 2;
        else
            return -1;
        if (dp->g729_ctx == NULL)
            dp->g729_ctx = g729_decoder_new();
        if (dp->g729_ctx == NULL)
            return -1;
        for (obytes = 0; ibytes > 0; ibytes -= fsize) {
            bp = g729_decode_frame(dp->g729_ctx, ibuf, fsize);
            ibuf += fsize;
            memcpy(obuf, bp, 160);
            obuf += 160;
            obytes += 160;
            dp->nticks += 80;
            dp->dticks += 80;
        }
        return obytes;

    case RTP_CN:
    case RTP_TSE:
    case RTP_TSE_CISCO:
        return 0;

    default:
        return -1;
    }
}

int
generate_silence(struct decoder_stream *dp, unsigned char *obuf, unsigned int iticks)
{
    unsigned int obytes;
    void *bp;

    switch (dp->lpt) {
    case RTP_PCMU:
    case RTP_PCMA:
    case RTP_G723:
        memset(obuf, 0, iticks * 2);
        return iticks * 2;

    case RTP_G729:
        if (dp->g729_ctx == NULL)
            dp->g729_ctx = g729_decoder_new();
        if (dp->g729_ctx == NULL) {
            memset(obuf, 0, iticks * 2);
            return iticks * 2;
        }
        for (obytes = 0; iticks >= 80; iticks -= 80) {
            bp = g729_decode_frame(dp->g729_ctx, NULL, 0);
            memcpy(obuf, bp, 160);
            obuf += 160;
            obytes += 160;
        }
        if (iticks > 0) {
            memset(obuf, 0, iticks * 2);
            obytes += iticks * 2;
        }
        return obytes;

    default:
        return -1;
    }
}
