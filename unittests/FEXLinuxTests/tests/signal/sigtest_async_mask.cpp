#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

// --- Test 1: sigsuspend loop mask restoration & waking on deferred Required signals ---

static volatile sig_atomic_t g_usr1_count = 0;
static volatile sig_atomic_t g_usr1_mask_ok = 1;
static volatile sig_atomic_t g_segv_count = 0;

static void suspend_usr1_handler(int) {
  sigset_t cur {};
  if (sigprocmask(0, nullptr, &cur) != 0 || !sigismember(&cur, SIGUSR1) || sigismember(&cur, SIGUSR2)) {
    g_usr1_mask_ok = 0;
  }
  g_usr1_count = g_usr1_count + 1;
}

static void suspend_segv_handler(int) {
  g_segv_count = g_segv_count + 1;
}

static int run_sigsuspend_child() {
  alarm(5);

  struct sigaction act {};
  act.sa_handler = suspend_usr1_handler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;
  if (sigaction(SIGUSR1, &act, nullptr) != 0) {
    return 1;
  }

  sigset_t oldset {};
  sigset_t blockset {};
  sigemptyset(&blockset);
  sigaddset(&blockset, SIGUSR1);
  sigaddset(&blockset, SIGUSR2);
  if (sigprocmask(SIG_BLOCK, &blockset, &oldset) != 0) {
    return 2;
  }
  sigdelset(&oldset, SIGUSR1);
  sigdelset(&oldset, SIGUSR2);

  // Part 1: Repeated block -> raise -> while (!flag) sigsuspend(&old) loop.
  // Verifies rt_sigreturn restores the pre-suspend mask on both logical and host masks.
  g_usr1_count = 0;
  g_usr1_mask_ok = 1;
  for (int round = 1; round <= 5; ++round) {
    if (raise(SIGUSR1) != 0) {
      return 3;
    }
    // SIGUSR1 must still be blocked on the host, so it cannot fire before sigsuspend.
    if (g_usr1_count != round - 1) {
      return 4;
    }
    while (g_usr1_count < round) {
      errno = 0;
      const int rc = sigsuspend(&oldset);
      if (rc != -1 || errno != EINTR) {
        return 5;
      }
    }
    sigset_t cur {};
    if (sigprocmask(0, nullptr, &cur) != 0 || !sigismember(&cur, SIGUSR1) || !sigismember(&cur, SIGUSR2)) {
      return 6;
    }
    if (!g_usr1_mask_ok) {
      return 7;
    }
  }

  // Part 2: Repeated sigsuspend loop woken asynchronously by another thread.
  const pthread_t target = pthread_self();
  std::atomic<int> requested_round {0};
  std::atomic<bool> done {false};
  std::thread sender([&] {
    int sent = 0;
    while (!done.load(std::memory_order_acquire)) {
      const int want = requested_round.load(std::memory_order_acquire);
      if (want > sent) {
        usleep(1000);
        pthread_kill(target, SIGUSR1);
        sent = want;
      } else {
        sched_yield();
      }
    }
  });

  for (int round = 1; round <= 5; ++round) {
    const int target_count = g_usr1_count + 1;
    requested_round.store(round, std::memory_order_release);
    while (g_usr1_count < target_count) {
      errno = 0;
      const int rc = sigsuspend(&oldset);
      if (rc != -1 || errno != EINTR) {
        done.store(true, std::memory_order_release);
        sender.join();
        return 8;
      }
    }
    sigset_t cur {};
    if (sigprocmask(0, nullptr, &cur) != 0 || !sigismember(&cur, SIGUSR1) || !sigismember(&cur, SIGUSR2)) {
      done.store(true, std::memory_order_release);
      sender.join();
      return 9;
    }
  }
  done.store(true, std::memory_order_release);
  sender.join();

  // Confirm SIGUSR1 is still blocked on the host after async sigsuspend iterations.
  const int before_final_raise = g_usr1_count;
  if (raise(SIGUSR1) != 0) {
    return 10;
  }
  if (g_usr1_count != before_final_raise) {
    return 11;
  }
  while (g_usr1_count == before_final_raise) {
    if (sigsuspend(&oldset) != -1 || errno != EINTR) {
      return 12;
    }
  }

  // Part 3: sigsuspend waking on an already-queued Required signal (SIGSEGV raised while masked).
  struct sigaction segv_act {};
  segv_act.sa_handler = suspend_segv_handler;
  sigemptyset(&segv_act.sa_mask);
  segv_act.sa_flags = 0;
  if (sigaction(SIGSEGV, &segv_act, nullptr) != 0) {
    return 13;
  }

  sigset_t segv_block {};
  sigset_t no_segv {};
  sigemptyset(&segv_block);
  sigaddset(&segv_block, SIGSEGV);
  if (sigprocmask(SIG_BLOCK, &segv_block, &no_segv) != 0) {
    return 14;
  }
  sigdelset(&no_segv, SIGSEGV);

  g_segv_count = 0;
  for (int round = 1; round <= 2; ++round) {
    if (raise(SIGSEGV) != 0) {
      return 15;
    }
    if (g_segv_count != round - 1) {
      return 16;
    }
    sigset_t pending {};
    if (sigpending(&pending) != 0 || !sigismember(&pending, SIGSEGV)) {
      return 17;
    }
    while (g_segv_count < round) {
      errno = 0;
      const int rc = sigsuspend(&no_segv);
      if (rc != -1 || errno != EINTR) {
        return 18;
      }
    }
    sigset_t cur {};
    if (sigprocmask(0, nullptr, &cur) != 0 || !sigismember(&cur, SIGSEGV)) {
      return 19;
    }
  }

  return 0;
}

// --- Test 2: re-raise with SIG_DFL inside handler terminates on rt_sigreturn ---

static int g_reraise_pipe_write_fd = -1;

static void reraise_dfl_handler(int sig) {
  signal(sig, SIG_DFL);
  raise(sig);
  // sig is blocked during handler execution, so execution must reach here before rt_sigreturn.
  const char marker[2] = {'O', 'K'};
  [[maybe_unused]] ssize_t ignored = write(g_reraise_pipe_write_fd, marker, sizeof(marker));
}

// --- Test 3: two queued deferred signals with asymmetric sa_mask & non-starvation ---

static volatile sig_atomic_t g_asym_segv_ran = 0;
static volatile sig_atomic_t g_asym_ill_ran = 0;
static volatile sig_atomic_t g_asym_usr1_ran = 0;
static volatile sig_atomic_t g_asym_usr1_ran_in_target = 0;
static volatile sig_atomic_t g_asym_blocked_violated = 0;
static volatile sig_atomic_t g_asym_in_segv = 0;
static volatile sig_atomic_t g_asym_in_ill = 0;
static volatile sig_atomic_t g_asym_target_is_segv = 1;

static void asym_usr1_handler(int) {
  g_asym_usr1_ran = g_asym_usr1_ran + 1;
}

static void asym_segv_handler(int) {
  g_asym_in_segv = 1;
  if (g_asym_target_is_segv) {
    // SIGSEGV blocks SIGILL, while queued SIGILL's sa_mask blocks SIGUSR1.
    // SIGUSR1 must NOT be blocked while SIGSEGV's handler runs.
    sigset_t cur {};
    if (sigprocmask(0, nullptr, &cur) == 0 && !sigismember(&cur, SIGUSR1) && sigismember(&cur, SIGILL)) {
      const int before = g_asym_usr1_ran;
      raise(SIGUSR1);
      if (g_asym_usr1_ran == before + 1) {
        g_asym_usr1_ran_in_target = 1;
      }
    }
  } else if (g_asym_in_ill) {
    // SIGILL's sa_mask blocked SIGSEGV.
    g_asym_blocked_violated = 1;
  }
  g_asym_segv_ran = g_asym_segv_ran + 1;
  g_asym_in_segv = 0;
}

static void asym_ill_handler(int) {
  g_asym_in_ill = 1;
  if (!g_asym_target_is_segv) {
    // SIGILL blocks SIGSEGV, while queued SIGSEGV's sa_mask blocks SIGUSR1.
    // SIGUSR1 must NOT be blocked while SIGILL's handler runs.
    sigset_t cur {};
    if (sigprocmask(0, nullptr, &cur) == 0 && !sigismember(&cur, SIGUSR1) && sigismember(&cur, SIGSEGV)) {
      const int before = g_asym_usr1_ran;
      raise(SIGUSR1);
      if (g_asym_usr1_ran == before + 1) {
        g_asym_usr1_ran_in_target = 1;
      }
    }
  } else if (g_asym_in_segv) {
    // SIGSEGV's sa_mask blocked SIGILL.
    g_asym_blocked_violated = 1;
  }
  g_asym_ill_ran = g_asym_ill_ran + 1;
  g_asym_in_ill = 0;
}

static volatile sig_atomic_t g_unblocked_segv_ran = 0;
static volatile sig_atomic_t g_unblocked_ill_ran = 0;
static volatile sig_atomic_t g_unblocked_nested_observed = 0;

static void unblocked_segv_handler(int) {
  sigset_t cur {};
  if (sigprocmask(0, nullptr, &cur) == 0 && sigismember(&cur, SIGILL)) {
    g_unblocked_nested_observed = 1;
  }
  for (int i = 0; i < 1000 && !g_unblocked_ill_ran && !g_unblocked_nested_observed; ++i) {
    sched_yield();
  }
  g_unblocked_segv_ran = g_unblocked_segv_ran + 1;
}

static void unblocked_ill_handler(int) {
  sigset_t cur {};
  if (sigprocmask(0, nullptr, &cur) == 0 && sigismember(&cur, SIGSEGV)) {
    g_unblocked_nested_observed = 1;
  }
  for (int i = 0; i < 1000 && !g_unblocked_segv_ran && !g_unblocked_nested_observed; ++i) {
    sched_yield();
  }
  g_unblocked_ill_ran = g_unblocked_ill_ran + 1;
}

static int run_queued_asymmetric_child() {
  alarm(5);

  struct sigaction usr1_act {};
  usr1_act.sa_handler = asym_usr1_handler;
  sigemptyset(&usr1_act.sa_mask);
  usr1_act.sa_flags = 0;
  if (sigaction(SIGUSR1, &usr1_act, nullptr) != 0) {
    return 1;
  }

  sigset_t both {};
  sigemptyset(&both);
  sigaddset(&both, SIGSEGV);
  sigaddset(&both, SIGILL);

  // Part 1a: SIGSEGV blocks {SIGILL}; SIGILL blocks {SIGSEGV, SIGUSR1}.
  // Queuing both SIGILL and SIGSEGV and unblocking must not leak queued SIGILL's extra sa_mask
  // (SIGUSR1) while SIGSEGV's handler runs.
  {
    g_asym_segv_ran = 0;
    g_asym_ill_ran = 0;
    g_asym_usr1_ran = 0;
    g_asym_usr1_ran_in_target = 0;
    g_asym_blocked_violated = 0;
    g_asym_in_segv = 0;
    g_asym_in_ill = 0;
    g_asym_target_is_segv = 1;

    struct sigaction segv_act {};
    segv_act.sa_handler = asym_segv_handler;
    sigemptyset(&segv_act.sa_mask);
    sigaddset(&segv_act.sa_mask, SIGILL);

    struct sigaction ill_act {};
    ill_act.sa_handler = asym_ill_handler;
    sigemptyset(&ill_act.sa_mask);
    sigaddset(&ill_act.sa_mask, SIGSEGV);
    sigaddset(&ill_act.sa_mask, SIGUSR1);

    if (sigaction(SIGSEGV, &segv_act, nullptr) != 0 || sigaction(SIGILL, &ill_act, nullptr) != 0) {
      return 2;
    }

    sigset_t saved {};
    if (sigprocmask(SIG_BLOCK, &both, &saved) != 0) {
      return 3;
    }
    if (raise(SIGILL) != 0 || raise(SIGSEGV) != 0) {
      return 4;
    }
    if (sigprocmask(SIG_SETMASK, &saved, nullptr) != 0) {
      return 5;
    }

    if (g_asym_segv_ran != 1 || g_asym_ill_ran != 1 || g_asym_blocked_violated != 0 || g_asym_usr1_ran_in_target != 1) {
      return 6;
    }
  }

  // Part 1b: Symmetric case: SIGILL blocks {SIGSEGV}; SIGSEGV blocks {SIGILL, SIGUSR1}.
  {
    g_asym_segv_ran = 0;
    g_asym_ill_ran = 0;
    g_asym_usr1_ran = 0;
    g_asym_usr1_ran_in_target = 0;
    g_asym_blocked_violated = 0;
    g_asym_in_segv = 0;
    g_asym_in_ill = 0;
    g_asym_target_is_segv = 0;

    struct sigaction segv_act {};
    segv_act.sa_handler = asym_segv_handler;
    sigemptyset(&segv_act.sa_mask);
    sigaddset(&segv_act.sa_mask, SIGILL);
    sigaddset(&segv_act.sa_mask, SIGUSR1);

    struct sigaction ill_act {};
    ill_act.sa_handler = asym_ill_handler;
    sigemptyset(&ill_act.sa_mask);
    sigaddset(&ill_act.sa_mask, SIGSEGV);

    if (sigaction(SIGSEGV, &segv_act, nullptr) != 0 || sigaction(SIGILL, &ill_act, nullptr) != 0) {
      return 7;
    }

    sigset_t saved {};
    if (sigprocmask(SIG_BLOCK, &both, &saved) != 0) {
      return 8;
    }
    if (raise(SIGSEGV) != 0 || raise(SIGILL) != 0) {
      return 9;
    }
    if (sigprocmask(SIG_SETMASK, &saved, nullptr) != 0) {
      return 10;
    }

    if (g_asym_segv_ran != 1 || g_asym_ill_ran != 1 || g_asym_blocked_violated != 0 || g_asym_usr1_ran_in_target != 1) {
      return 11;
    }
  }

  // Part 2: Two queued signals with empty sa_mask (neither blocks the other).
  // The second deliverable deferred signal must run nested while the first handler is active
  // rather than being starved behind an RW InterruptFaultPage until rt_sigreturn.
  {
    g_unblocked_segv_ran = 0;
    g_unblocked_ill_ran = 0;
    g_unblocked_nested_observed = 0;

    struct sigaction segv_act {};
    segv_act.sa_handler = unblocked_segv_handler;
    sigemptyset(&segv_act.sa_mask);

    struct sigaction ill_act {};
    ill_act.sa_handler = unblocked_ill_handler;
    sigemptyset(&ill_act.sa_mask);

    if (sigaction(SIGSEGV, &segv_act, nullptr) != 0 || sigaction(SIGILL, &ill_act, nullptr) != 0) {
      return 12;
    }

    sigset_t saved {};
    if (sigprocmask(SIG_BLOCK, &both, &saved) != 0) {
      return 13;
    }
    if (raise(SIGILL) != 0 || raise(SIGSEGV) != 0) {
      return 14;
    }
    if (sigprocmask(SIG_SETMASK, &saved, nullptr) != 0) {
      return 15;
    }

    if (g_unblocked_segv_ran != 1 || g_unblocked_ill_ran != 1 || g_unblocked_nested_observed != 1) {
      return 16;
    }
  }

  return 0;
}

} // namespace

TEST_CASE("Signals: sigsuspend loop restores pre-suspend mask and wakes on deferred signals") {
  const pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    _exit(run_sigsuspend_child());
  }

  int status = 0;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("Signals: re-raise with SIG_DFL inside handler terminates on sigreturn") {
  for (int tested_signal : {SIGUSR1, SIGSEGV, SIGILL}) {
    INFO("signal = " << tested_signal);
    int pipefd[2] {-1, -1};
    REQUIRE(pipe(pipefd) == 0);

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      close(pipefd[0]);
      alarm(5);

      // Avoid generating core dumps for SIGSEGV/SIGILL default termination.
      struct rlimit rl {0, 0};
      setrlimit(RLIMIT_CORE, &rl);

      g_reraise_pipe_write_fd = pipefd[1];

      struct sigaction act {};
      act.sa_handler = reraise_dfl_handler;
      sigemptyset(&act.sa_mask);
      act.sa_flags = 0;
      if (sigaction(tested_signal, &act, nullptr) != 0) {
        _exit(40);
      }

      if (raise(tested_signal) != 0) {
        _exit(41);
      }

      // Reaching here means the re-raised signal was swallowed on rt_sigreturn.
      _exit(42);
    }

    close(pipefd[1]);
    char buf[2] {};
    const ssize_t n = read(pipefd[0], buf, sizeof(buf));
    close(pipefd[0]);

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(n == 2);
    CHECK(buf[0] == 'O');
    CHECK(buf[1] == 'K');
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == tested_signal);
  }
}

TEST_CASE("Signals: queued signals honor asymmetric sa_mask without starving") {
  const pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    _exit(run_queued_asymmetric_child());
  }

  int status = 0;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}
