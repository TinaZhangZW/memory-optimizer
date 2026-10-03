// SPDX-License-Identifier: GPL-2.0
// Guest-side controlled pages and first-touch latency; run as root in the guest.
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
static volatile sig_atomic_t stop;
static volatile uint64_t sink;
static void stopping(int sig) { (void)sig; stop=1; }
static uint64_t now(void) { struct timespec t; if(clock_gettime(CLOCK_MONOTONIC_RAW,&t)) abort(); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static uint64_t random64(uint64_t *s) { *s^=*s<<13; *s^=*s>>7; *s^=*s<<17; return *s; }
static uint64_t hash(const uint8_t *p) { uint64_t h=1469598103934665603ULL; for(int i=0;i<4096;i++) h=(h^p[i])*1099511628211ULL; return h; }
int main(int argc,char **argv) {
    if(argc!=3 || sysconf(_SC_PAGESIZE)!=4096) return 2;
    char *end; unsigned long mib=strtoul(argv[1],&end,10);
    if(*end || mib<1 || mib>256) return 2;
    int mode=!strcmp(argv[2],"sparse")?0:!strcmp(argv[2],"mixed")?1:!strcmp(argv[2],"random")?2:-1;
    if(mode<0) return 2;
    struct rlimit lim={RLIM_INFINITY,RLIM_INFINITY};
    if(setrlimit(RLIMIT_MEMLOCK,&lim)) {perror("rlimit");return 1;}
    size_t bytes=mib<<20,n=bytes/4096;
    uint8_t *p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED || madvise(p,bytes,MADV_NOHUGEPAGE) || mlock(p,bytes)) {perror("memory");return 1;}
    uint64_t *expected=calloc(n,8),*first=calloc(n,8),*second=calloc(n,8);
    size_t *order=calloc(n,sizeof(*order));
    if(!expected||!first||!second||!order) return 1;
    uint64_t rng=0x912b473caf31ULL;
    for(size_t i=0;i<n;i++) {
        uint64_t *words=(uint64_t *)(p+i*4096);
        size_t count=mode==0?1:mode==1?256:512;
        for(size_t k=0;k<count;k++) words[k]=random64(&rng);
        expected[i]=hash(p+i*4096); order[i]=i; first[i]=second[i]=0;
    }
    for(size_t i=n-1;i>0;i--) {size_t j=random64(&rng)%(i+1),tmp=order[i];order[i]=order[j];order[j]=tmp;}
    if(mlock(expected,n*8)||mlock(first,n*8)||mlock(second,n*8)||mlock(order,n*sizeof(*order))) return 1;
    int fd=open("/proc/self/pagemap",O_RDONLY); if(fd<0) return 1;
    FILE *f=fopen("pages.tmp","w"); if(!f) return 1;
    for(size_t i=0;i<n;i++) {uint64_t e; if(pread(fd,&e,8,((uintptr_t)p/4096+i)*8)!=8 || !(e>>63) || !(e&((1ULL<<55)-1))) return 1; fprintf(f,"%zu 0x%" PRIx64 "\n",i,(uint64_t)((e&((1ULL<<55)-1))*4096));}
    close(fd); if(fclose(f)||rename("pages.tmp","pages.tsv")) return 1;
    signal(SIGTERM,stopping);signal(SIGINT,stopping);
    for(int i=0;i<100;i++) sink+=now();
    printf("ready pid=%d pages=%zu pattern=%s\n",getpid(),n,argv[2]);fflush(stdout);
    while(!stop) {
        f=fopen("command","r");
        if(!f) {usleep(10000);continue;}
        char tag[64]; if(fscanf(f,"%63s",tag)!=1) return 1; fclose(f);unlink("command");
        if(strspn(tag,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=strlen(tag)) return 2;
        uint64_t totals[2];
        for(int pass=0;pass<2;pass++) {
            uint64_t begin=now();
            for(size_t j=0;j<n;j++) {
                size_t i=order[j]; uint64_t a=now();
                uint8_t v=*(volatile uint8_t *)(p+i*4096);
                uint64_t elapsed=now()-a;
                sink+=v; (pass?second:first)[i]=elapsed;
            }
            totals[pass]=now()-begin;
        }
        size_t errors=0;
        for(size_t i=0;i<n;i++) errors+=hash(p+i*4096)!=expected[i];
        char path[128]; snprintf(path,sizeof(path),"%s.latency.tsv",tag);
        f=fopen(path,"w");if(!f)return 1;
        fprintf(f,"page\tfirst_ns\tsecond_ns\n");
        for(size_t i=0;i<n;i++) fprintf(f,"%zu\t%" PRIu64 "\t%" PRIu64 "\n",i,first[i],second[i]);
        if(fclose(f)) return 1;
        snprintf(path,sizeof(path),"%s.done",tag);f=fopen(path,"w");if(!f)return 1;
        fprintf(f,"pages=%zu first_total_ns=%" PRIu64 " second_total_ns=%" PRIu64 " errors=%zu pattern=%s\n",n,totals[0],totals[1],errors,argv[2]);
        if(fclose(f)) return 1;
        if(errors)return 1;
    }
    munmap(p,bytes);return 0;
}
