// user/scheduletest.c
#include "include/types.h"
#include "include/stat.h"
#include "user/user.h"


static void burn(int iters) {
  volatile unsigned int x = 0;
  for (int i = 0; i < iters; i++) x ^= (i * 1103515245u + 12345u);
  if (x == 0xdeadbeef) write(1, "", 0);
}

// 可靠读 n 字节
static int readn(int fd, void *buf, int n) {
  int m, tot = 0;
  char *p = (char*)buf;
  while (tot < n && (m = read(fd, p + tot, n - tot)) > 0) tot += m;
  return (tot == n) ? n : -1;
}

struct result {
  char tag;        // 'A'/'B'/'C'
  int  pri;        // 最终优先级
  int  start_tick;
  int  half_tick;  // 达到一半进度时的 tick
  int  done_tick;  // 完成时的 tick
  int  marks;      // 实际打印次数
};

static void worker(char tag, int prio, int loops, int spin, int marks, int report_fd) {
  setpri(prio);

  int step = loops / marks; if (step <= 0) step = 1;
  int printed = 0;
  int start = uptime();
  int half  = -1;

  for (int i = 1; i <= loops; i++) {
    burn(spin);
    if ((i % step) == 0 && printed < marks) {
      write(1, &tag, 1); // 只输出一个字母
      printed++;
      if (printed == (marks/2) && half < 0) half = uptime();
    }
  }

  struct result r;
  r.tag = tag;
  r.pri = getpri();
  r.start_tick = start;
  r.half_tick  = (half < 0) ? uptime() : half;
  r.done_tick  = uptime();
  r.marks      = printed;

  write(report_fd, &r, sizeof(r)); // 单次写
  close(report_fd);
  exit(0);
}

static int strictly_earlier(int a_done, int b_done, int eps) {
  // a 必须至少早 eps 个 tick
  return (a_done + eps) <= b_done;
}

int
main(int argc, char *argv[]) {
  // 用法：scheduletest [hi mid low] [loops] [spin] [marks] [eps]
  int prio_hi  = 12, prio_mid = 8, prio_low = 4;
  int loops    = 1500;     // 步数：可按机器调大
  int spin     = 150000;   // 每步计算量：适当偏大以放大差异
  int marks    = 20;       // 每个子进程打印的字母数
  int eps      = 1;        // 至少早 eps 个 tick 才算更快

  if (argc >= 4) { prio_hi = atoi(argv[1]); prio_mid = atoi(argv[2]); prio_low = atoi(argv[3]); }
  if (argc >= 5) loops = atoi(argv[4]);
  if (argc >= 6) spin  = atoi(argv[5]);
  if (argc >= 7) marks = atoi(argv[6]);
  if (argc >= 8) eps   = atoi(argv[7]);
  if (marks <= 0) marks = 1;
  if (eps   <  1) eps   = 1;

  int pipes[3][2];
  for (int i = 0; i < 3; i++) pipe(pipes[i]);

  // A: 高
  if (fork() == 0) {
    close(pipes[0][0]);
    worker('A', prio_low,  loops, spin, marks, pipes[0][1]);
  }
  // B: 中
  if (fork() == 0) {
    close(pipes[1][0]);
    worker('B', prio_mid, loops, spin, marks, pipes[1][1]);
  }
  // C: 低
  if (fork() == 0) {
    close(pipes[2][0]);
    worker('C', prio_hi, loops, spin, marks, pipes[2][1]);
  }

  // 父进程读取统计
  for (int i = 0; i < 3; i++) close(pipes[i][1]);

  struct result ra, rb, rc;
  readn(pipes[0][0], &ra, sizeof(ra));
  readn(pipes[1][0], &rb, sizeof(rb));
  readn(pipes[2][0], &rc, sizeof(rc));
  close(pipes[0][0]); close(pipes[1][0]); close(pipes[2][0]);

  while (wait(0) > 0) { }
  write(1, "\n", 1); // 换行，避免 shell 黏连

  // 只由父进程打印简短统计，避免交错
  printf("stats: %c pri=%d half=%d done=%d marks=%d\n", ra.tag, ra.pri, ra.half_tick, ra.done_tick, ra.marks);
  printf("stats: %c pri=%d half=%d done=%d marks=%d\n", rb.tag, rb.pri, rb.half_tick, rb.done_tick, rb.marks);
  printf("stats: %c pri=%d half=%d done=%d marks=%d\n", rc.tag, rc.pri, rc.half_tick, rc.done_tick, rc.marks);

  // 严格判定
  // 情况1：hi > mid > low：要求 doneH + eps <= doneM 且 doneM + eps <= doneL
  int pass = 1;
  if (prio_hi > prio_mid && prio_mid > prio_low) {
    int doneH = ra.done_tick; // A 对应 hi
    int doneM = rb.done_tick; // B 对应 mid
    int doneL = rc.done_tick; // C 对应 low
    pass = strictly_earlier(doneH, doneM, eps) && strictly_earlier(doneM, doneL, eps);
  } else {
    // 情况2：给的参数不是严格递降，就对“有优先级差”的两两组合施加严格比较
    // 取出 A/B/C 三者
    const struct result *A = &ra, *B = &rb, *C = &rc;
    if (A->pri > B->pri) pass = pass && strictly_earlier(A->done_tick, B->done_tick, eps);
    if (B->pri > C->pri) pass = pass && strictly_earlier(B->done_tick, C->done_tick, eps);
    if (A->pri > C->pri) pass = pass && strictly_earlier(A->done_tick, C->done_tick, eps);
  }

  printf("verify(eps=%d): %s\n", eps, pass ? "FAIL" : "PASS");
  exit(0);
}
