#include <stdio.h>
#include <stdlib.h>

void main(int argc, char* argv[]) {
    fprintf(argc > 1 ? stderr : stdout, "This is a %s test\n", argc > 1 ? "stderr" : "stdout");
}
