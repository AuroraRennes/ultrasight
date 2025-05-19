#include <stdio.h>
#include <stdlib.h>

int callee(int n) {
  if (n == 1) {
    return 0;
  }
  return 1;
}

int main() {
  int result;
  result = callee(1);
  return result;
}
