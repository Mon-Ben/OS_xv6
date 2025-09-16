#include "kernel/types.h"
#include "user.h"
int main(int argc,char *argv[])
{
    int c2f[2],f2c[2];//c2f:子读父写，f2c：父读子写
    char c[4];
    pipe(c2f);
    pipe(f2c);
    int fapid = getpid();
    int pid = fork();
    if(pid==0){
        close(0);
        dup(c2f[0]);
        close(c2f[0]);
        close(c2f[1]);
        read(0,c,4);
        printf("%d: received %s from pid %d\n",getpid(),c,fapid);
        //写入"pong"
        close(f2c[0]);
        write(f2c[1],"pong",4);
        close(f2c[1]);
    } else{//写入数据"ping"
        close(0);
        close(c2f[0]);
        write(c2f[1],"ping",4);
        close(c2f[1]);
        dup(f2c[0]);
        close(f2c[0]);
        close(f2c[1]);
        read(0,c,4);
        printf("%d: received %s from pid %d\n",getpid(),c,pid);
    }
    exit(0);
}