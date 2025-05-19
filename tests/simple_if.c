
int main() {
  int a[2] = {1, 15};
  int res[2];

  for (int i = 0; i < 2; i++) {
    if (a[i] > 10) {
      res[i] = 1;
    }
    else {
      res[i] = 0;
    }
  }

  return 0;
}
