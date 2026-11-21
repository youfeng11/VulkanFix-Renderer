#include "../core/driver_probe_process.h"
#include <cstdio>
#include <cstdlib>
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if (argc!=3) return 2;
#if !defined(_WIN32) && !defined(VKFIX_PROBE_TEST_CLIENT)
    if (setenv("VKFIX_PROBE_CHILD", "1", 1) != 0) return 2;
#endif
    return vkfix::driver_trust_worker(argv[1],argv[2]);
}
