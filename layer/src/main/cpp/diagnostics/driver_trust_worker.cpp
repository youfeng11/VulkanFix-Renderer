#include "../core/driver_probe_process.h"
#include <cstdio>
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if (argc!=3) return 2;
    return vkfix::driver_trust_worker(argv[1],argv[2]);
}
