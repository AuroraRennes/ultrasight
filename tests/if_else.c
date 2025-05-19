
int main() {
  int a[4] = {1, 2, 51, 52};
  int res[4];

  for (int i = 0; i < 4; i++) {
    if (a[i] > 50) {
      res[i] = 1;
    }
    else {
      res[i] = 0;
    }
  }

  return 0;
}
