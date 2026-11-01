"""Check production connect results even when cleanup changes errno."""
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[1]/'source/carplay_services.m').read_text()
function=source[source.index('static int try_connect('):source.index('/* ═',source.index('static int try_connect('))]
harness=r'''
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
static int fail_option;
static void trim_dot(const char*s,char*d,size_t n){snprintf(d,n,"%s",s);}
static int noisy_close(int fd){int r=close(fd);errno=EIO;return r;}
static void noisy_free(struct addrinfo*a){freeaddrinfo(a);errno=EIO;}
static int option(int fd,int level,int opt,void*v,socklen_t*n){
 if(fail_option){errno=EACCES;return -1;}
 return getsockopt(fd,level,opt,v,n);
}
#define close noisy_close
#define freeaddrinfo noisy_free
#define getsockopt option
''' + function + r'''
#undef close
int main(void){
 int listener=socket(AF_INET,SOCK_STREAM,0); assert(listener>=0);
 struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(bind(listener,(struct sockaddr*)&a,sizeof a)==0);
 socklen_t n=sizeof a;assert(getsockname(listener,(struct sockaddr*)&a,&n)==0);
 assert(listen(listener,2)==0);
 int fd=try_connect("127.0.0.1",ntohs(a.sin_port),0,0,1);assert(fd>=0);close(fd);
 fail_option=1;
 assert(try_connect("127.0.0.1",ntohs(a.sin_port),0,0,1)==-1);assert(errno==EACCES);
 fail_option=0;close(listener);
 assert(try_connect("127.0.0.1",ntohs(a.sin_port),0,0,1)==-1);assert(errno==ECONNREFUSED);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'check.c').write_text(harness)
 subprocess.run(['clang',str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
print('PASS: successful connection, socket-option failure, refused port survives cleanup')
