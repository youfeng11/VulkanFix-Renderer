#include "driver_probe_process.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cerrno>
#ifndef _WIN32
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace vkfix {
std::string driver_probe_identity(const VkPhysicalDeviceProperties &p,uint64_t revision) {
    char fields[128];
    std::snprintf(fields,sizeof fields,"%x:%x:%x:%x:%llx:",p.vendorID,p.deviceID,p.driverVersion,p.apiVersion,
                  static_cast<unsigned long long>(revision));
    std::string result=fields;
    for (auto byte:p.pipelineCacheUUID) {
        char hex[3]; std::snprintf(hex,sizeof hex,"%02x",byte); result+=hex;
    }
    return result;
}

ProbeProcessResult run_driver_probe_process(const char *mode,const std::string &identity,unsigned timeout_ms) {
#ifdef _WIN32
    return {ProbeProcessStatus::Unavailable,"isolated probe worker requires Android"};
#else
    // A worker can load a forwarding driver during tests. Its instance and
    // device setup must never launch another worker.
    if (std::getenv("VKFIX_PROBE_CHILD"))
        return {ProbeProcessStatus::Unavailable, "nested isolated probe suppressed"};
    std::string path;
    if (auto override_path=std::getenv("VULKAN_FIX_DRIVER_PROBE_WORKER")) path=override_path;
    else {
        Dl_info location{};
        if (dladdr(reinterpret_cast<void*>(&run_driver_probe_process),&location) && location.dli_fname) {
            path=location.dli_fname;
            auto slash=path.find_last_of('/');
            if (slash!=std::string::npos) path.resize(slash+1); else path.clear();
            path+="libvulkanfix_probe.so";
        }
    }
    if (path.empty() || path[0]!='/' || access(path.c_str(),X_OK))
        return {ProbeProcessStatus::Unavailable,"isolated probe worker unavailable"};
    int descriptors[2];
    if (pipe2(descriptors,O_CLOEXEC|O_NONBLOCK))
        return {ProbeProcessStatus::Unavailable,"cannot create isolated probe pipe"};
    auto normalize=[](int fd) {
        if (fd<0 || fd>=3) return fd;
        const int moved=fcntl(fd,F_DUPFD_CLOEXEC,3); close(fd); return moved;
    };
    descriptors[0]=normalize(descriptors[0]); descriptors[1]=normalize(descriptors[1]);
    const int errors=normalize(open("/dev/null",O_WRONLY|O_CLOEXEC));
    if (errors<0 || descriptors[0]<0 || descriptors[1]<0) {
        close(descriptors[0]); close(descriptors[1]);
        if (errors>=0) close(errors);
        return {ProbeProcessStatus::Unavailable,"cannot initialize isolated probe"};
    }
    char *arguments[]{const_cast<char*>(path.c_str()),const_cast<char*>(mode),const_cast<char*>(identity.c_str()),nullptr};
    const char *executable=path.c_str();
    // Android 8 has no posix_spawn. The fork child performs ONLY async-signal-
    // safe descriptor operations and execve; no allocator, C++, Vulkan or JVM
    // work runs in the forked address space. The executed worker starts fresh.
    const pid_t pid=fork();
    if (pid==0) {
        close(descriptors[0]);
        if (dup2(descriptors[1],STDOUT_FILENO)<0 || dup2(errors,STDERR_FILENO)<0) _exit(2);
        if (descriptors[1]!=STDOUT_FILENO) close(descriptors[1]);
        if (errors!=STDERR_FILENO) close(errors);
        execve(executable,arguments,environ);
        _exit(2);
    }
    const int launch_error=errno;
    close(errors); close(descriptors[1]);
    if (pid<0) {
        close(descriptors[0]);
        return {ProbeProcessStatus::Unavailable,"isolated probe fork failed: "+std::to_string(launch_error)};
    }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
    std::string output;
    int status=0; bool finished=false;
    auto read_output=[&] {
        char chunk[512]; ssize_t bytes;
        // A noisy or corrupt worker must not prevent the outer deadline from
        // being checked. Bound both retained output AND each drain iteration.
        for (unsigned reads=0;reads<16 && (bytes=read(descriptors[0],chunk,sizeof chunk))>0;++reads)
            if (output.size()<4096) output.append(chunk,std::min<size_t>(size_t(bytes),4096-output.size()));
    };
    bool closed_output=false;
    while (std::chrono::steady_clock::now()<deadline) {
        read_output();
        const auto result=waitpid(pid,&status,WNOHANG);
        if (result==pid) { finished=true; break; }
        if (result<0 && errno!=EINTR) {
            close(descriptors[0]);
            return {ProbeProcessStatus::Unavailable,"isolated child no longer waitable"};
        }
        pollfd item{closed_output ? -1 : descriptors[0],POLLIN,0};
        poll(&item,1,10);
        closed_output|=(item.revents&POLLHUP)!=0;
    }
    if (!finished) {
        kill(pid,SIGKILL);
        // Never wait indefinitely for a broken GPU kernel's process teardown.
        std::thread([pid]{ int value; while (waitpid(pid,&value,0)<0 && errno==EINTR) {} }).detach();
        close(descriptors[0]);
        return {ProbeProcessStatus::TimedOut,"isolated probe exceeded "+std::to_string(timeout_ms)+" ms"};
    }
    read_output();
    close(descriptors[0]);
    // READY is emitted only after identity, legal resources and pipeline setup.
    // A loader/setup crash is inconclusive, rather than a subject failure.
    const bool ready=output.find("VKFIX_PROBE_READY\n")!=std::string::npos;
    auto detail=[&](const char *fallback) {
        const std::string marker="VKFIX_PROBE_DETAIL:";
        const auto begin=output.find(marker);
        if (begin==std::string::npos) return std::string(fallback);
        const auto value=begin+marker.size();
        return output.substr(value,std::min<size_t>(512,output.find('\n',value)-value));
    };
    if (WIFSIGNALED(status))
        return {ready ? ProbeProcessStatus::Crashed : ProbeProcessStatus::Unavailable,
                "isolated probe signal "+std::to_string(WTERMSIG(status))};
    if (ready && WIFEXITED(status) && WEXITSTATUS(status)==0 && output.find("VKFIX_PROBE_PASS\n")!=std::string::npos)
        return {ProbeProcessStatus::Passed,detail("isolated GPU readback matched")};
    if (ready && WIFEXITED(status) && WEXITSTATUS(status)==4 && output.find("VKFIX_PROBE_REPAIR\n")!=std::string::npos)
        return {ProbeProcessStatus::RepairConfirmed,detail("isolated differential controls established repair")};
    if (ready && WIFEXITED(status) && WEXITSTATUS(status)==3 && output.find("VKFIX_PROBE_KNOWN_MISMATCH\n")!=std::string::npos)
        return {ProbeProcessStatus::KnownMismatch,detail("isolated GPU readback matched a known failure")};
    if (ready && WIFEXITED(status) && WEXITSTATUS(status)==3 && output.find("VKFIX_PROBE_MISMATCH\n")!=std::string::npos)
        return {ProbeProcessStatus::Mismatch,detail("isolated GPU readback mismatched")};
    return {ProbeProcessStatus::Unavailable,"isolated probe returned no valid measurement: "+output.substr(0,512)};
#endif
}

DriverProbeAssessment isolated_driver_assessment(const char *mode,const VkPhysicalDeviceProperties &p,uint64_t revision) {
    const auto result=run_driver_probe_process(mode,driver_probe_identity(p,revision));
    if (result.status==ProbeProcessStatus::Passed) return {DriverProbeVerdict::Healthy,result.detail};
    if (result.status==ProbeProcessStatus::RepairConfirmed) return {DriverProbeVerdict::RepairConfirmed,result.detail};
    return {DriverProbeVerdict::Inconclusive,std::string(mode)+": "+result.detail};
}

ProbeProcessResult run_controlled_driver_probe(const char *subject, const char *control,
                                               const char *control_name, const std::string &identity,
                                               unsigned timeout_ms) {
    const auto before = run_driver_probe_process(control, identity, timeout_ms);
    if (before.status != ProbeProcessStatus::Passed)
        return {ProbeProcessStatus::Unavailable, std::string(control_name) + " control before subject: " + before.detail};
    auto result = run_driver_probe_process(subject, identity, timeout_ms);
    if (result.status == ProbeProcessStatus::Unavailable) return result;
    const auto after = run_driver_probe_process(control, identity, timeout_ms);
    if (after.status != ProbeProcessStatus::Passed)
        return {ProbeProcessStatus::Unavailable, std::string(control_name) + " control after subject: " + after.detail};
    result.detail += std::string("; ") + control_name + " controls passed before and after";
    return result;
}

DriverProbeAssessment isolated_push_descriptor_probe(const VkPhysicalDeviceProperties &p,uint64_t revision) {
    auto identity=driver_probe_identity(p,revision);
    for (const char *name:{"push","push-graphics","push-vertex","push-vertex-compute"}) {
        const std::string prefix=name;
        auto subject=run_controlled_driver_probe((prefix+"-subject").c_str(), (prefix+"-control").c_str(),
                                                 "equivalent descriptor-set", identity);
        if (subject.status==ProbeProcessStatus::Passed) continue;
        if (subject.status==ProbeProcessStatus::Mismatch || subject.status==ProbeProcessStatus::Crashed)
            return {DriverProbeVerdict::RepairConfirmed,prefix+": "+subject.detail};
        return {DriverProbeVerdict::Inconclusive,prefix+" subject: "+subject.detail};
    }
    return {DriverProbeVerdict::Healthy,"compute, fragment and vertex mixed-image/buffer/texel/array readbacks matched descriptor-set controls"};
}
DriverProbeAssessment isolated_tiler_probe(const VkPhysicalDeviceProperties &p,uint64_t revision) {
    auto identity=driver_probe_identity(p,revision);
    auto subject=run_controlled_driver_probe("tiler-subject", "tiler-control", "bounded LOAD", identity, 15000);
    if (subject.status==ProbeProcessStatus::Passed)
        return {DriverProbeVerdict::Healthy,"direct, single indirect and available multi-indirect pressure matched bounded LOAD controls (4 patterns each)"};
    if (subject.status==ProbeProcessStatus::Mismatch || subject.status==ProbeProcessStatus::Crashed)
        return {DriverProbeVerdict::RepairConfirmed,subject.detail};
    return {DriverProbeVerdict::Inconclusive,"tiler subject: "+subject.detail};
}
} // namespace vkfix
