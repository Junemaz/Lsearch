#include "test_util.h"

#include <cstdio>

int main() {
  for (auto& t : ::ltest::registry()) {
    printf("[ RUN ] %s\n", t.name);
    t.fn();
  }
  printf("\n%d checks, %d failures\n", ::ltest::checks, ::ltest::failures);
  return ::ltest::failures ? 1 : 0;
}
