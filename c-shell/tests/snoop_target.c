/* A process with a known syscall profile, for tests/test_snoop.sh.
 *
 *   getppid  x3   first occurrence before getpid
 *   getpid   x3   ties with getppid, so must be listed after it
 *   getuid   x5   highest count, so must be listed before both
 *   1000     x1   not a real syscall: must print as syscall_1000
 *
 * Raw syscall() is used so glibc cannot cache or skip any call.
 */
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
  for (int i = 0; i < 3; i++)
    syscall(SYS_getppid);
  for (int i = 0; i < 3; i++)
    syscall(SYS_getpid);
  for (int i = 0; i < 5; i++)
    syscall(SYS_getuid);
  syscall(1000);
  return 0;
}
