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
    const bool subject=std::strstr(argv[1],"-subject")!=nullptr;
    const bool graphics=std::strstr(argv[1],"push-graphics-")!=nullptr;
    const bool vertex=std::strstr(argv[1],"push-vertex-")!=nullptr;
    const bool stage_mask=std::strstr(argv[1],"push-vertex-compute-")!=nullptr;
    if (!std::strcmp(fault,"setup-crash")) raise(SIGSEGV);
    if (!std::strcmp(fault,"bad-protocol")) return 0;
    if (!std::strcmp(fault,"pass-without-ready")) { std::puts("VKFIX_PROBE_PASS"); return 0; }
    std::puts("VKFIX_PROBE_READY");
    if (!std::strcmp(fault,"repair")) { std::puts("VKFIX_PROBE_DETAIL:controlled repair"); std::puts("VKFIX_PROBE_REPAIR"); return 4; }
    if (!std::strcmp(fault,"closed-output-hang")) { close(STDOUT_FILENO); for (;;) pause(); }
    if (!std::strcmp(fault,"flood")) { char data[512]; std::memset(data,'x',sizeof data); for (;;) write(STDOUT_FILENO,data,sizeof data); }
    if (!std::strcmp(fault,"hang")) for (;;) pause();
    if (subject && !std::strcmp(fault,"known-mismatch")) {
        std::puts("VKFIX_PROBE_KNOWN_MISMATCH"); return 3;
    }
    if (!std::strcmp(fault,"reset-control-mismatch")) {
        if (!std::strcmp(argv[1],"sync2-events-subject")) {
            std::puts("VKFIX_PROBE_KNOWN_MISMATCH"); return 3;
        }
        if (!std::strcmp(argv[1],"sync2-events-reset-control")) {
            std::puts("VKFIX_PROBE_MISMATCH"); return 3;
        }
    }
    if (subject && !std::strcmp(fault,"subject-crash")) raise(SIGSEGV);
    if (graphics && subject && !std::strcmp(fault,"graphics-subject-crash")) raise(SIGSEGV);
    if (graphics && subject && !std::strcmp(fault,"graphics-subject-mismatch")) { std::puts("VKFIX_PROBE_MISMATCH"); return 3; }
    if (graphics && !subject && !std::strcmp(fault,"graphics-control-mismatch")) { std::puts("VKFIX_PROBE_MISMATCH"); return 3; }
    if (vertex && subject && !std::strcmp(fault,"vertex-subject-crash")) raise(SIGSEGV);
    if (vertex && subject && !std::strcmp(fault,"vertex-subject-mismatch")) { std::puts("VKFIX_PROBE_MISMATCH"); return 3; }
    if (vertex && !subject && !std::strcmp(fault,"vertex-control-mismatch")) { std::puts("VKFIX_PROBE_MISMATCH"); return 3; }
    if (stage_mask && subject && !std::strcmp(fault,"stage-mask-subject-mismatch")) { std::puts("VKFIX_PROBE_MISMATCH"); return 3; }
    if ((subject && !std::strcmp(fault,"subject-mismatch")) ||
        (!subject && !std::strcmp(fault,"control-mismatch"))) {
        std::puts("VKFIX_PROBE_MISMATCH"); return 3;
    }
    std::puts("VKFIX_PROBE_PASS"); return 0;
}
