
int callee(int n) {
  if (n > 50) {
    return 1;
  }
  return 0;
}

int main() {

  callee(1);
  callee(2);
  callee(51);
  callee(52);

  return 0;
}
