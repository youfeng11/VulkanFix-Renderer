#include "core/vk_common.h"

FILE* logger_file_from_peer() { return get_writable_log_file(); }
void logger_write_from_peer(unsigned writer, unsigned line) {
    LOGI("logger line %u:%u", writer, line);
}
