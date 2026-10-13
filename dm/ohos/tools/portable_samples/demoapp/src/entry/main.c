// 入口：模拟"另一个项目"。只要它能编过，就说明 L1 对这个项目够用。
// Entry point: stands in for "another project". If it compiles, L1 is sufficient for this project.
#include "demoapp.h"

#include <stdio.h>

int main(void) {
    char buf[256];
    int n = demo_report(buf, sizeof buf);
    if (n < 0) return 1;
    printf("%s\n", buf);
    return 0;
}
