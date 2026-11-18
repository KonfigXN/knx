#include "knx.h"
extern "C" int knx_cpp_command(const char *command) {
    if (command[0] == 'c' && command[1] == 'p' && command[2] == 'p' && command[3] == 0) {
        const uint64_t product = 6 * 7;
        knx_puts("C++ module: 6 * 7 = ");
        knx_print_u64(product);
        knx_puts("\n");
        return 1;
    }
    return 0;
}
