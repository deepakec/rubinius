// RUN: %llvmgcc -S %s -o - | llvm-as -f -o /dev/null

/* GCC wasn't handling 64 bit constants right fixed */

void main() {
  long long Var = 123455678902ll;
  printf("%lld\n", Var);
}
