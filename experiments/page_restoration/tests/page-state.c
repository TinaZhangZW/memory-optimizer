#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
int main(int argc,char **argv) {
 if(argc!=3) return 2;
 char path[128];snprintf(path,sizeof(path),"/proc/%d/pagemap",atoi(argv[1]));
 int fd=open(path,O_RDONLY);FILE *f=fopen(argv[2],"r");if(fd<0||!f){perror("open");return 1;}
 unsigned long a;unsigned count=0,present=0,swapped=0; uint64_t e;
 puts("hva\tpresent\tswapped\tpfn_or_swap");
 while(fscanf(f,"%lx",&a)==1) {
  if(a%4096 || pread(fd,&e,8,(a/4096)*8)!=8)return 1;
  unsigned p=(e>>63)&1,s=(e>>62)&1;count++;present+=p;swapped+=s;
  printf("0x%lx\t%u\t%u\t0x%" PRIx64 "\n",a,p,s,(uint64_t)(e&((1ULL<<55)-1)));
 }
 fprintf(stderr,"pages=%u present=%u swapped=%u absent=%u\n",count,present,swapped,count-present-swapped);
 fclose(f);close(fd);return 0;
}
