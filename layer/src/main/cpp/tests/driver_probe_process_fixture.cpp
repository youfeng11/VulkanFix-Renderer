// Test-only child process. Never included in the APK.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if (argc!=3) return 2;
    const char *fault=std::getenv("VKFIX_TEST_PROCESS_FAULT");
    if (!fault) fault="healthy";
    const bool subject=!std::strcmp(argv[1],"push-subject");
    if (!std::strcmp(fault,"setup-crash")) raise(SIGSEGV);
    if (!std::strcmp(fault,"bad-protocol")) return 0;
    std::puts("VKFIX_PROBE_READY");
    if (!std::strcmp(fault,"closed-output-hang")) { close(STDOUT_FILENO); for (;;) pause(); }
    if (!std::strcmp(fault,"flood")) { char data[512]; std::memset(data,'x',sizeof data); for (;;) write(STDOUT_FILENO,data,sizeof data); }
    if (!std::strcmp(fault,"hang")) for (;;) pause();
    if (subject && !std::strcmp(fault,"subject-crash")) raise(SIGSEGV);
    if ((subject && !std::strcmp(fault,"subject-mismatch")) ||
        (!subject && !std::strcmp(fault,"control-mismatch"))) {
        std::puts("VKFIX_PROBE_MISMATCH"); return 3;
    }
    std::puts("VKFIX_PROBE_PASS"); return 0;
}
