/* Redis benchmark utility.
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.

 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <assert.h>

#include "ae.h"
#include "hiredis.h"
#include "sds.h"
#include "adlist.h"
#include "zmalloc.h"

#define REDIS_NOTUSED(V) ((void) V)

static struct config {
    aeEventLoop *el;
    const char *hostip;
    int hostport;
    const char *hostsocket;
    int numclients;
    int liveclients;
    int requests;
    int requests_issued;
    int requests_finished;
    int keysize;
    int datasize;
    int randomkeys;
    int randomkeys_keyspacelen;
    int keepalive;
    int pipeline;
    long long start;
    long long totlatency;
    long long *latency;
    const char *title;
    list *clients;
    int quiet;
    int csv;
    int loop;
    int idlemode;
    char *tests;
    char *file;
    int mget_cnt;
    int needPrepareForVectoritem;
} config;

typedef struct _client {
    redisContext *context;
    sds obuf;
    char *randptr[3200]; /* needed for MSET against 10 keys and VMERGE 2000 keys */
    size_t randlen;
    unsigned int written; /* bytes of 'obuf' already written */
    long long start; /* start time of a request */
    long long latency; /* request latency */
    int pending;    /* Number of pending requests (sent but no reply received) */
} *client;

/* Prototypes */
static void writeHandler(aeEventLoop *el, int fd, void *privdata, int mask);
static void createMissingClients(client c);

/* Implementation */
static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

static long long mstime(void) {
    struct timeval tv;
    long long mst;

    gettimeofday(&tv, NULL);
    mst = ((long)tv.tv_sec)*1000;
    mst += tv.tv_usec/1000;
    return mst;
}

static void freeClient(client c) {
    listNode *ln;
    aeDeleteFileEvent(config.el,c->context->fd,AE_WRITABLE);
    aeDeleteFileEvent(config.el,c->context->fd,AE_READABLE);
    redisFree(c->context);
    sdsfree(c->obuf);
    zfree(c);
    config.liveclients--;
    ln = listSearchKey(config.clients,c);
    assert(ln != NULL);
    listDelNode(config.clients,ln);
}

static void freeAllClients(void) {
    listNode *ln = config.clients->head, *next;

    while(ln) {
        next = ln->next;
        freeClient(ln->value);
        ln = next;
    }
}

static void resetClient(client c) {
    aeDeleteFileEvent(config.el,c->context->fd,AE_WRITABLE);
    aeDeleteFileEvent(config.el,c->context->fd,AE_READABLE);
    aeCreateFileEvent(config.el,c->context->fd,AE_WRITABLE,writeHandler,c);
    c->written = 0;
    c->pending = config.pipeline;
}

static void randomizeClientKey(client c) {
    char buf[32];
    size_t i, r;

    for (i = 0; i < c->randlen; i++) {
        r = random() % config.randomkeys_keyspacelen;
        snprintf(buf,sizeof(buf),"%012zu",r);
        memcpy(c->randptr[i],buf,12);
    }
}

static void clientDone(client c) {
    if (config.requests_finished == config.requests) {
        freeClient(c);
        aeStop(config.el);
        return;
    }
    if (config.keepalive) {
        resetClient(c);
    } else {
        config.liveclients--;
        createMissingClients(c);
        config.liveclients++;
        freeClient(c);
    }
}

static void readHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    client c = privdata;
    void *reply = NULL;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(fd);
    REDIS_NOTUSED(mask);

    /* Calculate latency only for the first read event. This means that the
     * server already sent the reply and we need to parse it. Parsing overhead
     * is not part of the latency, so calculate it only once, here. */
    if (c->latency < 0) c->latency = ustime()-(c->start);

    if (redisBufferRead(c->context) != REDIS_OK) {
        fprintf(stderr,"Error: %s\n",c->context->errstr);
        exit(1);
    } else {
        while(c->pending) {
            if (redisGetReply(c->context,&reply) != REDIS_OK) {
                fprintf(stderr,"Error: %s\n",c->context->errstr);
                exit(1);
            }
            if (reply != NULL) {
                if (reply == (void*)REDIS_REPLY_ERROR) {
                    fprintf(stderr,"Unexpected error reply, exiting...\n");
                    exit(1);
                }

                freeReplyObject(reply);

                if (config.requests_finished < config.requests)
                    config.latency[config.requests_finished++] = c->latency;
                c->pending--;
                if (c->pending == 0) clientDone(c);
            } else {
                break;
            }
        }
    }
}

static void writeHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    client c = privdata;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(fd);
    REDIS_NOTUSED(mask);

    /* Initialize request when nothing was written. */
    if (c->written == 0) {
        /* Enforce upper bound to number of requests. */
        if (config.requests_issued++ >= config.requests) {
            freeClient(c);
            return;
        }

        /* Really initialize: randomize keys and set start time. */
        if (config.randomkeys) randomizeClientKey(c);
        c->start = ustime();
        c->latency = -1;
    }

    if (sdslen(c->obuf) > c->written) {
        void *ptr = c->obuf+c->written;
        int nwritten = write(c->context->fd,ptr,sdslen(c->obuf)-c->written);
        if (nwritten == -1) {
            if (errno != EPIPE)
                fprintf(stderr, "Writing to socket: %s\n", strerror(errno));
            freeClient(c);
            return;
        }
        c->written += nwritten;
        if (sdslen(c->obuf) == c->written) {
            aeDeleteFileEvent(config.el,c->context->fd,AE_WRITABLE);
            aeCreateFileEvent(config.el,c->context->fd,AE_READABLE,readHandler,c);
        }
    }
}

static client createClient(char *cmd, size_t len) {
    int j;
    client c = zmalloc(sizeof(struct _client));

    if (config.hostsocket == NULL) {
        c->context = redisConnectNonBlock(config.hostip,config.hostport);
    } else {
        c->context = redisConnectUnixNonBlock(config.hostsocket);
    }
    if (c->context->err) {
        fprintf(stderr,"Could not connect to Redis at ");
        if (config.hostsocket == NULL)
            fprintf(stderr,"%s:%d: %s\n",config.hostip,config.hostport,c->context->errstr);
        else
            fprintf(stderr,"%s: %s\n",config.hostsocket,c->context->errstr);
        exit(1);
    }
    /* Queue N requests accordingly to the pipeline size. */
    c->obuf = sdsempty();
    for (j = 0; j < config.pipeline; j++)
        c->obuf = sdscatlen(c->obuf,cmd,len);
    c->randlen = 0;
    c->written = 0;
    c->pending = config.pipeline;

    /* Find substrings in the output buffer that need to be randomized. */
    if (config.randomkeys) {
        char *p = c->obuf;
        while ((p = strstr(p,":rand:")) != NULL) {
            assert(c->randlen < (signed)(sizeof(c->randptr)/sizeof(char*)));
            c->randptr[c->randlen++] = p+6;
            p += 6;
        }
    }

/*    redisSetReplyObjectFunctions(c->context,NULL); */
    aeCreateFileEvent(config.el,c->context->fd,AE_WRITABLE,writeHandler,c);
    listAddNodeTail(config.clients,c);
    config.liveclients++;
    return c;
}

static void createMissingClients(client c) {
    int n = 0;

    while(config.liveclients < config.numclients) {
        createClient(c->obuf,sdslen(c->obuf)/config.pipeline);

        /* Listen backlog is quite limited on most systems */
        if (++n > 64) {
            usleep(50000);
            n = 0;
        }
    }
}

static int compareLatency(const void *a, const void *b) {
    return (*(long long*)a)-(*(long long*)b);
}

static void showLatencyReport(void) {
    int i, curlat = 0;
    float perc, reqpersec;

    reqpersec = (float)config.requests_finished/((float)config.totlatency/1000);
    if (!config.quiet && !config.csv) {
        printf("====== %s ======\n", config.title);
        printf("  %d requests completed in %.2f seconds\n", config.requests_finished,
            (float)config.totlatency/1000);
        printf("  %d parallel clients\n", config.numclients);
        printf("  %d bytes payload\n", config.datasize);
        printf("  keep alive: %d\n", config.keepalive);
        printf("\n");

        qsort(config.latency,config.requests,sizeof(long long),compareLatency);
        for (i = 0; i < config.requests; i++) {
            if (config.latency[i]/1000 != curlat || i == (config.requests-1)) {
                curlat = config.latency[i]/1000;
                perc = ((float)(i+1)*100)/config.requests;
                printf("%.2f%% <= %d milliseconds\n", perc, curlat);
            }
        }
        printf("%.2f requests per second\n", reqpersec);
    } else if (config.csv) {
        printf("\"%s\",\"%.2f\"\n", config.title, reqpersec);
    } else {
        printf("%s: %.2f requests per second\n", config.title, reqpersec);
    }
}

static void benchmark(char *title, char *cmd, int len) {
    client c;

    config.title = title;
    config.requests_issued = 0;
    config.requests_finished = 0;

    c = createClient(cmd,len);
    createMissingClients(c);

    config.start = mstime();
    aeMain(config.el);
    config.totlatency = mstime()-config.start;

    showLatencyReport();
    freeAllClients();
}

/* Returns number of consumed options. */
int parseOptions(int argc, const char **argv) {
    int i;
    int lastarg;
    int exit_status = 1;

    for (i = 1; i < argc; i++) {
        lastarg = (i == (argc-1));

        if (!strcmp(argv[i],"-c")) {
            if (lastarg) goto invalid;
            config.numclients = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-n")) {
            if (lastarg) goto invalid;
            config.requests = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-k")) {
            if (lastarg) goto invalid;
            config.keepalive = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-h")) {
            if (lastarg) goto invalid;
            config.hostip = strdup(argv[++i]);
        } else if (!strcmp(argv[i],"-p")) {
            if (lastarg) goto invalid;
            config.hostport = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-s")) {
            if (lastarg) goto invalid;
            config.hostsocket = strdup(argv[++i]);
        } else if (!strcmp(argv[i],"-d")) {
            if (lastarg) goto invalid;
            config.datasize = atoi(argv[++i]);
            if (config.datasize < 1) config.datasize=1;
            if (config.datasize > 1024*1024*1024) config.datasize = 1024*1024*1024;
        } else if (!strcmp(argv[i],"-P")) {
            if (lastarg) goto invalid;
            config.pipeline = atoi(argv[++i]);
            if (config.pipeline <= 0) config.pipeline=1;
        } else if (!strcmp(argv[i],"-r")) {
            if (lastarg) goto invalid;
            config.randomkeys = 1;
            config.randomkeys_keyspacelen = atoi(argv[++i]);
            if (config.randomkeys_keyspacelen < 0)
                config.randomkeys_keyspacelen = 0;
        } else if (!strcmp(argv[i],"-q")) {
            config.quiet = 1;
        } else if (!strcmp(argv[i],"--csv")) {
            config.csv = 1;
        } else if (!strcmp(argv[i],"-l")) {
            config.loop = 1;
        } else if (!strcmp(argv[i],"-I")) {
            config.idlemode = 1;
        } else if (!strcmp(argv[i],"-t")) {
            if (lastarg) goto invalid;
            /* We get the list of tests to run as a string in the form
             * get,set,lrange,...,test_N. Then we add a comma before and
             * after the string in order to make sure that searching
             * for ",testname," will always get a match if the test is
             * enabled. */
            config.tests = sdsnew(",");
            config.tests = sdscat(config.tests,(char*)argv[++i]);
            config.tests = sdscat(config.tests,",");
            sdstolower(config.tests);
        } else if (!strcmp(argv[i],"-f")) {
            if (lastarg) goto invalid;
            config.file = strdup(argv[++i]);
        } else if (!strcmp(argv[i],"-m")) {
            if (lastarg) goto invalid;
            config.mget_cnt = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"--help")) {
            exit_status = 0;
            goto usage;
        } else if (!strcmp(argv[i],"--pvec")) {
            if (lastarg) goto invalid;
            /** --pvec whether prepare data for vtest or not*/
            config.needPrepareForVectoritem = atoi(argv[++i]);
        } else {
            /* Assume the user meant to provide an option when the arg starts
             * with a dash. We're done otherwise and should use the remainder
             * as the command and arguments for running the benchmark. */
            if (argv[i][0] == '-') goto invalid;
            return i;
        }
    }

    return i;

invalid:
    printf("Invalid option \"%s\" or option argument missing\n\n",argv[i]);

usage:
    printf(
"Usage: redis-benchmark [-h <host>] [-p <port>] [-c <clients>] [-n <requests]> [-k <boolean>]\n\n"
" -h <hostname>      Server hostname (default 127.0.0.1)\n"
" -p <port>          Server port (default 6379)\n"
" -s <socket>        Server socket (overrides host and port)\n"
" -c <clients>       Number of parallel connections (default 50)\n"
" -n <requests>      Total number of requests (default 10000)\n"
" -d <size>          Data size of SET/GET value in bytes (default 2)\n"
" -k <boolean>       1=keep alive 0=reconnect (default 1)\n"
" -r <keyspacelen>   Use random keys for SET/GET/INCR, random values for SADD\n"
" -f <filename>      Call GET against keys listed in file\n"
" -m <count>         Call MGET against every key listed in file specified by '-f',\n"
"                    each MGET contains the same key for count times\n"
"  Using this option the benchmark will get/set keys\n"
"  in the form mykey_rand:000000012456 instead of constant\n"
"  keys, the <keyspacelen> argument determines the max\n"
"  number of values for the random number. For instance\n"
"  if set to 10 only rand:000000000000 - rand:000000000009\n"
"  range will be allowed.\n"
" -P <numreq>        Pipeline <numreq> requests. Default 1 (no pipeline).\n"
" -q                 Quiet. Just show query/sec values\n"
" --csv              Output in CSV format\n"
" -l                 Loop. Run the tests forever\n"
" -t <tests>         Only run the comma separated list of tests. The test\n"
"                    names are the same as the ones produced as output.\n"
" -I                 Idle mode. Just open N idle connections and wait.\n\n"
" --pvec             Prepare data like schema and vectoritems for vtest.\n"
"Examples:\n\n"
" Run the benchmark with the default configuration against 127.0.0.1:6379:\n"
"   $ redis-benchmark\n\n"
" Use 20 parallel clients, for a total of 100k requests, against 192.168.1.1:\n"
"   $ redis-benchmark -h 192.168.1.1 -p 6379 -n 100000 -c 20\n\n"
" Fill 127.0.0.1:6379 with about 1 million keys only using the SET test:\n"
"   $ redis-benchmark -t set -n 1000000 -r 100000000\n\n"
" Benchmark 127.0.0.1:6379 for a few commands producing CSV output:\n"
"   $ redis-benchmark -t ping,set,get -n 100000 --csv\n\n"
" Fill a list with 10000 random elements:\n"
"   $ redis-benchmark -r 10000 -n 10000 lpush mylist ele:rand:000000000000\n\n"
    );
    exit(exit_status);
}

int showThroughput(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);

    if (config.csv) return 250;
    float dt = (float)(mstime()-config.start)/1000.0;
    float rps = (float)config.requests_finished/dt;
    printf("%s: %.2f\r", config.title, rps);
    fflush(stdout);
    return 250; /* every 250ms */
}

/* Return true if the named test was selected using the -t command line
 * switch, or if all the tests are selected (no -t passed by user). */
int test_is_selected(char *name) {
    char buf[256];
    int l = strlen(name);

    if (config.tests == NULL) return 1;
    buf[0] = ',';
    memcpy(buf+1,name,l);
    buf[l+1] = ',';
    buf[l+2] = '\0';
    return strstr(config.tests,buf) != NULL;
}

static redisContext *getRedisContext(){
    redisContext *c;
    c = redisConnect(config.hostip, config.hostport);
    if (c != NULL && c->err) {
        printf("Error: %s\n", c->errstr);
        exit(1);
    }
    
    return c;
}

#define DEFAULT_SCHEMA "test_schema"
static long randNum(long max) {
    return rand() % max; 
}

static long currentTime(){
    struct timeval tv; 
    gettimeofday(&tv,NULL);
    return tv.tv_sec;
}

static void prepareDataForVectoritem(int count, int itemCount){

    redisContext *c = getRedisContext();

    redisCommand(c, "riskauth psd@retsulcsider");
    redisCommand(c, "flushdb");
    /* prepare schema info */
    redisCommand(c, "config schema add %s", DEFAULT_SCHEMA);
    redisCommand(c, "config column add %s col 4", DEFAULT_SCHEMA);

    /* prepare fixed vectoritem */
    sds idmetaStr = sdsempty();
    int i, j, totalCount = count * itemCount;
    long *ids = (long *)zmalloc(totalCount*sizeof(long));
    if(!ids) { 
        fprintf(stderr, "malloc failed when prepare datas.\n");
        exit(1);
    }

    printf("\n Generate and Add Vectoritem start....\n");
    long cur = currentTime()*1000000;
    for(i = 0; i < totalCount; i++){
        ids[i] = randNum(totalCount * 1000) + cur;
    }
    
    for(i = 0; i < count; i++){
        idmetaStr = sdscatprintf(idmetaStr, "vadd :rand:%012d.%s ", i, DEFAULT_SCHEMA);;
        for(j = 0; j < itemCount; j++){
            idmetaStr = sdscatprintf(idmetaStr, " %lu 9", ids[i*itemCount+j]); 
        }
        redisCommand(c, idmetaStr);
        sdsclear(idmetaStr);
    }
    printf("\n Generate and Add Vectoritem end....\n");
    sdsfree(idmetaStr);
    zfree(ids);
    redisFree(c);
}

void prepareDataForVdel(int total, int itemCount){
    /* prepare data for vdel, we should prepare this datas everytime*/
    redisContext *c = getRedisContext();

    printf("Preparing data for vel start...\n");
    sds idmetaStr = sdsempty();
    int i,j, idmetas;
    long cur = currentTime() * 1000000;
    for(i = 0; i < total; i++){
        redisCommand(c, "del delkey:rand:%012d.%s", i, DEFAULT_SCHEMA);
        idmetaStr = sdscatprintf(idmetaStr, "vadd delkey:rand:%012d.%s ", i, DEFAULT_SCHEMA);;
        idmetas = randNum(itemCount)+1;
        for(j = 0; j < itemCount; j++){
            idmetaStr = sdscatprintf(idmetaStr, " %lu 9", randNum(total*1000) + cur); 
        }
        redisCommand(c, idmetaStr);
        sdsclear(idmetaStr);
    }
    printf("Preparing data for vel end...\n");
    sdsfree(idmetaStr);
    redisFree(c);
}

/**
 * BenchMark Function For Vadd.
 * @title    : output title 
 * @startId  : id of idmeta start from startId
 * @idmetaNum: how many idmetas to add
 **/
#define VADD_SCHEMA "vadd_schema"
static void vaddBenchTest(char *title, long startId, int idmetaNum){
    int i, len;
    char *cmd;

    sds viKey = sdsnew("vadd");
    viKey = sdscatprintf(viKey, "  _vi%d_:rand:000000000000.%s ", idmetaNum, VADD_SCHEMA);
    for(i = 0; i < idmetaNum; i++){
        viKey = sdscatprintf(viKey, " %lu %d ", startId+randNum(10000000), idmetaNum);
    } 
    len = redisFormatCommand(&cmd, viKey); 

    benchmark(title, cmd, len);
    sdsfree(viKey);
    free(cmd);
}
#define VREM_SCHEMA "vrem_schema"
static void vremBenchTest(char *title, long startId, int idmetaNum){
	// add before rem, same key and same idmeta for vadd and vrem.
	int i, len;
    char *cmd;
	sds viKey = sdsnew("vadd");
    viKey = sdscatprintf(viKey, "  _vi%d_:rand:000000000000.%s ", idmetaNum, VREM_SCHEMA);
    for(i = 0; i < idmetaNum; i++){
        viKey = sdscatprintf(viKey, " %lu %d ", startId+i, idmetaNum);
    }
    len = redisFormatCommand(&cmd, viKey);
	sds buf = sdsnew(title);
	buf = sdscatprintf(buf, " %s %d", "vadd", idmetaNum);
	benchmark(buf, cmd, len);
	sdsfree(viKey);
	sdsfree(buf);
	free(cmd);

    viKey = sdsnew("vrem");
    viKey = sdscatprintf(viKey, "  _vi%d_:rand:000000000000.%s ", idmetaNum, VREM_SCHEMA);
    for(i = 0; i < idmetaNum; i++){
        viKey = sdscatprintf(viKey, " %lu", startId+i);
    }
    len = redisFormatCommand(&cmd, viKey);

    benchmark(title, cmd, len);
    sdsfree(viKey);
    free(cmd);
}
#define VREMRANGE_SCHEMA "vremrange_schema"
static void vremrangeBenchTest(char *title, long startId, int idmetaNum){
	// add before rem, same key and same idmeta for vadd and vrem.
	int i, len;
    char *cmd;
	sds viKey = sdsnew("vadd");
	// use idmetaNum + idmetaNum to avoid influence from previous test
    viKey = sdscatprintf(viKey, "  _vi%d_:rand:000000000000.%s ", idmetaNum + idmetaNum, VREMRANGE_SCHEMA);
	for(i = 0; i < idmetaNum; i++){
		viKey = sdscatprintf(viKey, " %lu %d ", startId+randNum(100000), idmetaNum);
	}
	
	len = redisFormatCommand(&cmd, viKey);
	sds buf = sdsnew(title);
	buf = sdscatprintf(buf, " %s %d", "vadd", idmetaNum);
	benchmark(buf, cmd, len);
	sdsfree(viKey);
	sdsfree(buf);
	free(cmd);


    viKey = sdsnew("vremrange");
    viKey = sdscatprintf(viKey, "  _vi%d_:rand:000000000000.%s -1 -1 ", idmetaNum + idmetaNum, VREMRANGE_SCHEMA);
    len = redisFormatCommand(&cmd, viKey);

    benchmark(title, cmd, len);
    sdsfree(viKey);
    free(cmd);
}
static void vmergeBenchTest(int totalCount, int mergeCount, int topCount){

    assert(totalCount > mergeCount);
    int partCount = totalCount / mergeCount;
    int i, randInd, len;
    char *cmd;
    sds mergeKeys = sdscatprintf(sdsempty(), "vmerge ");
    for(i = 0; i < mergeCount; i++){
        randInd = randNum(partCount); 
        mergeKeys = sdscatprintf(mergeKeys, " :rand:000000000000.%s ", DEFAULT_SCHEMA); 
    }

    sds title = sdscatprintf(sdsempty(), "VMERGE %d keys and get Top :%d", mergeCount, topCount);
    mergeKeys = sdscatprintf(mergeKeys, " 0 0 %d -1 -1", topCount);
    len = redisFormatCommand(&cmd, mergeKeys); 
    benchmark(title, cmd, len);
    sdsfree(mergeKeys);
    sdsfree(title);
    free(cmd);
}

int main(int argc, const char **argv) {
    int i;
    char *data, *cmd;
    int len;

    client c;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    config.numclients = 50;
    config.requests = 10000;
    config.liveclients = 0;
    config.el = aeCreateEventLoop();
    aeCreateTimeEvent(config.el,1,showThroughput,NULL,NULL);
    config.keepalive = 1;
    config.datasize = 3;
    config.pipeline = 1;
    config.randomkeys = 0;
    config.randomkeys_keyspacelen = 0;
    config.quiet = 0;
    config.csv = 0;
    config.loop = 0;
    config.idlemode = 0;
    config.latency = NULL;
    config.clients = listCreate();
    config.hostip = "127.0.0.1";
    config.hostport = 6379;
    config.hostsocket = NULL;
    config.tests = NULL;
    config.file = NULL;
    config.mget_cnt = 1;
    config.needPrepareForVectoritem = 1;

    i = parseOptions(argc,argv);
    argc -= i;
    argv += i;

    config.latency = zmalloc(sizeof(long long)*config.requests);

    if (config.keepalive == 0) {
        printf("WARNING: keepalive disabled, you probably need 'echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse' for Linux and 'sudo sysctl -w net.inet.tcp.msl=1000' for Mac OS X in order to use a lot of clients/requests\n");
    }

    if (config.idlemode) {
        printf("Creating %d idle connections and waiting forever (Ctrl+C when done)\n", config.numclients);
        c = createClient("",0); /* will never receive a reply */
        createMissingClients(c);
        aeMain(config.el);
        /* and will wait for every */
    }

    if (config.file) {
        FILE *fp = fopen(config.file, "r");
        if (!fp) {
            fprintf(stderr, "Error opening %s: %s\n", config.file, strerror(errno));
            exit(1);
        }
        char key[2550], ccmd[2550];
        while (fgets(key, 255, fp) != NULL) {
            key[strlen(key)-1] = '\0';
            if (config.mget_cnt == 1) {
                sprintf(ccmd, "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n", strlen(key), key);
            } else {
                sds tmp = sdsempty();
                for (int j = 0; j < config.mget_cnt; ++j)
                    tmp = sdscatprintf(tmp, "$%zu\r\n%s\r\n", strlen(key), key);
                sprintf(ccmd, "*%d\r\n$4\r\nMGET\r\n%s", config.mget_cnt+1, tmp);
                sdsfree(tmp);
            }
            benchmark("GET", ccmd, strlen(ccmd));
        }

        fclose(fp);
        return 0;
    }

    /* Run benchmark with command in the remainder of the arguments. */
    if (argc) {
        sds title = sdsnew(argv[0]);
        for (i = 1; i < argc; i++) {
            title = sdscatlen(title, " ", 1);
            title = sdscatlen(title, (char*)argv[i], strlen(argv[i]));
        }

        do {
            len = redisFormatCommandArgv(&cmd,argc,argv,NULL);
            benchmark(title,cmd,len);
            free(cmd);
        } while(config.loop);

        return 0;
    }

    srand(time(0));    
    /* Run default benchmark suite. */
    do {
        data = zmalloc(config.datasize+1);
        memset(data,'x',config.datasize);
        data[config.datasize] = '\0';

        if (test_is_selected("ping_inline") || test_is_selected("ping"))
            benchmark("PING_INLINE","PING\r\n",6);

        if (test_is_selected("ping_mbulk") || test_is_selected("ping")) {
            len = redisFormatCommand(&cmd,"PING");
            benchmark("PING_BULK",cmd,len);
            free(cmd);
        }

        if (test_is_selected("set")) {
            len = redisFormatCommand(&cmd,"SET foo:rand:000000000000 %s",data);
            benchmark("SET",cmd,len);
            free(cmd);
        }

        if (test_is_selected("get")) {
            len = redisFormatCommand(&cmd,"GET foo:rand:000000000000");
            benchmark("GET",cmd,len);
            free(cmd);
        }

        if (test_is_selected("incr")) {
            len = redisFormatCommand(&cmd,"INCR counter:rand:000000000000");
            benchmark("INCR",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lpush")) {
            len = redisFormatCommand(&cmd,"LPUSH mylist %s",data);
            benchmark("LPUSH",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lpop")) {
            len = redisFormatCommand(&cmd,"LPOP mylist");
            benchmark("LPOP",cmd,len);
            free(cmd);
        }

        if (test_is_selected("sadd")) {
            len = redisFormatCommand(&cmd,
                "SADD myset counter:rand:000000000000");
            benchmark("SADD",cmd,len);
            free(cmd);
        }

        if (test_is_selected("spop")) {
            len = redisFormatCommand(&cmd,"SPOP myset");
            benchmark("SPOP",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") ||
            test_is_selected("lrange_100") ||
            test_is_selected("lrange_300") ||
            test_is_selected("lrange_500") ||
            test_is_selected("lrange_600"))
        {
            len = redisFormatCommand(&cmd,"LPUSH mylist %s",data);
            benchmark("LPUSH (needed to benchmark LRANGE)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_100")) {
            len = redisFormatCommand(&cmd,"LRANGE mylist 0 99");
            benchmark("LRANGE_100 (first 100 elements)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_300")) {
            len = redisFormatCommand(&cmd,"LRANGE mylist 0 299");
            benchmark("LRANGE_300 (first 300 elements)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_500")) {
            len = redisFormatCommand(&cmd,"LRANGE mylist 0 449");
            benchmark("LRANGE_500 (first 450 elements)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("lrange") || test_is_selected("lrange_600")) {
            len = redisFormatCommand(&cmd,"LRANGE mylist 0 599");
            benchmark("LRANGE_600 (first 600 elements)",cmd,len);
            free(cmd);
        }

        if (test_is_selected("mset")) {
            const char *argv[21];
            argv[0] = "MSET";
            for (i = 1; i < 21; i += 2) {
                argv[i] = "foo:rand:000000000000";
                argv[i+1] = data;
            }
            len = redisFormatCommandArgv(&cmd,21,argv,NULL);
            benchmark("MSET (10 keys)",cmd,len);
            free(cmd);
        }
        
        /* Vectoritem BenchMark Start. */
        int totalCount = 100000;
        if(test_is_selected("vtest")){
            /* because prepare data maybe too long, so if we test many times, 
            * we can set --pvec 0, to flag we don't need prepare data again. */
            if(config.needPrepareForVectoritem == 1){
                prepareDataForVectoritem(100000, 200);
            }   

        }
        if(test_is_selected("vadd") || test_is_selected("vadd_1")){
            vaddBenchTest("VADD 1 IdMeta ", 40000000000000000, 1);
        }
                                
        if(test_is_selected("vadd") || test_is_selected("vadd_40")){
            vaddBenchTest("VADD 40 IdMeta ", 50000000000000000, 40);
        }
        if(test_is_selected("vadd") || test_is_selected("vadd_80")){
            vaddBenchTest("VADD 80 IdMeta ", 50000000000000000, 80);
        }
        if(test_is_selected("vadd") || test_is_selected("vadd_120")){
            vaddBenchTest("VADD 120 IdMeta ",60000000000000000, 120);
        }
        if(test_is_selected("vadd") || test_is_selected("vadd_160")){
            vaddBenchTest("VADD 160 IdMeta ",60000000000000000, 160);
        }
        if(test_is_selected("vadd") || test_is_selected("vadd_200")){
            vaddBenchTest("VADD 200 IdMeta ",60000000000000000, 200);
        }
	// vrem
	if(test_is_selected("vrem") || test_is_selected("vrem_1")){
		vremBenchTest("VREM 1",60000000000000000, 1);
	}
	if(test_is_selected("vrem") || test_is_selected("vrem_40")){
		vremBenchTest("VREM 40",60000000000000000, 40);
	}
	if(test_is_selected("vrem") || test_is_selected("vrem_80")){
		vremBenchTest("VREM 80",60000000000000000, 80);
	}
	if(test_is_selected("vrem") || test_is_selected("vrem_120")){
		vremBenchTest("VREM 120",60000000000000000, 120);
	}
	if(test_is_selected("vrem") || test_is_selected("vrem_160")){
		vremBenchTest("VREM 160",60000000000000000, 160);
	}
	if(test_is_selected("vrem") || test_is_selected("vrem_200")){
		vremBenchTest("VREM 200",60000000000000000, 200);
	}

	// vremrange
	if(test_is_selected("vremrange") || test_is_selected("vremrange_1")){
		vremrangeBenchTest("VREMRANGE 1",60000000000000000, 1);
	}
	if(test_is_selected("vremrange") || test_is_selected("vremrange_40")){
		vremrangeBenchTest("VREMRANGE 40",60000000000000000, 40);
	}
	if(test_is_selected("vremrange") || test_is_selected("vremrange_80")){
		vremrangeBenchTest("VREMRANGE 80",60000000000000000, 80);
	}
	if(test_is_selected("vremrange") || test_is_selected("vremrange_120")){
		vremrangeBenchTest("VREMRANGE 120",60000000000000000, 120);
	}
	if(test_is_selected("vremrange") || test_is_selected("vremrange_160")){
		vremrangeBenchTest("VREMRANGE 160",60000000000000000, 160);
	}
	if(test_is_selected("vremrange") || test_is_selected("vremrange_200")){
		vremrangeBenchTest("VREMRANGE 200",60000000000000000, 200);
	}
        if(test_is_selected("vtest") || test_is_selected("vremrange_o")){
            prepareDataForVdel(10000, 200);
            len = redisFormatCommand(&cmd, "vremrange delkey:rand:000000000000.%s -1 -1", DEFAULT_SCHEMA);
            benchmark("VREMRANGE All",cmd,len);
            free(cmd);
        }

        if(test_is_selected("vtest") || test_is_selected("vrange")){
            len = redisFormatCommand(&cmd, "vrange :rand:000000000000.%s 0 0 -1 -1", DEFAULT_SCHEMA);
            benchmark("VRANGE All",cmd,len);
            free(cmd);
        }
        
        if(test_is_selected("vtest") || test_is_selected("vcard")){
            len = redisFormatCommand(&cmd, "vcard :rand:000000000000.%s  -1 -1", DEFAULT_SCHEMA);
            benchmark("VCARD",cmd,len);
            free(cmd);
        }
        
        if(test_is_selected("vtest") || test_is_selected("vcount")){
            len = redisFormatCommand(&cmd, "vcount :rand:000000000000.%s 0 0 -1 -1", DEFAULT_SCHEMA);
            benchmark("VCOUNT",cmd,len);
            free(cmd);
        }
        
        if(test_is_selected("vtest") || test_is_selected("vmerge_50_45")){
            vmergeBenchTest(totalCount, 50, 45);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_50_200")){
            vmergeBenchTest(totalCount, 50, 200);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_50_1000")){
            vmergeBenchTest(totalCount, 50, 1000);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_50_2000")){
            vmergeBenchTest(totalCount, 50, 2000);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_100_45")){
            vmergeBenchTest(totalCount, 100, 45);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_100_200")){
            vmergeBenchTest(totalCount, 100, 200);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_100_1000")){
            vmergeBenchTest(totalCount, 100, 1000);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_100_2000")){
            vmergeBenchTest(totalCount, 100, 2000);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_200_45")){
            vmergeBenchTest(totalCount, 200, 45);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_200_200")){
            vmergeBenchTest(totalCount, 200, 200);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_200_1000")){
            vmergeBenchTest(totalCount, 200, 1000);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_200_2000")){
            vmergeBenchTest(totalCount, 200, 2000);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_2000_45")){
            vmergeBenchTest(totalCount, 2000, 45);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_2000_200")){
            vmergeBenchTest(totalCount, 2000, 200);                
        }

        if(test_is_selected("vtest") || test_is_selected("vmerge_2000_1000")){
            vmergeBenchTest(totalCount, 2000, 1000);                
        }
        if(test_is_selected("vtest") || test_is_selected("vmerge_2000_2000")){
            vmergeBenchTest(totalCount, 2000, 2000);                
        }
        /* Vectoritem BenchMark End. */

        if (!config.csv) printf("\n");
    } while(config.loop);

    return 0;
}
