#include "restart_tests.h"

#include "../notice_tracker.h"
#include "../restart_policy.h"

#include <cstdio>

int runRestartPolicyCheck() {
    int failures = 0;
    auto check = [&failures](bool ok, const char *message) {
        fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };

    RestartPolicy policy;
    const int expected[] = {1000, 2000, 4000, 8000, 16000};
    bool sequence = true;
    for (int delay : expected) {
        const auto next = policy.nextDelayMs();
        sequence = sequence && next && *next == delay;
    }
    check(sequence, "delays are 1, 2, 4, 8 and 16 seconds");
    check(!policy.nextDelayMs().has_value(), "no sixth attempt: the budget is spent");
    check(policy.attempts() == 5, "attempts() reports the five used");

    policy.reset();
    check(policy.attempts() == 0, "reset() zeroes attempts");

    policy.nextDelayMs();
    policy.nextDelayMs();
    policy.noteHealthy();
    const auto afterHealthy = policy.nextDelayMs();
    check(afterHealthy && *afterHealthy == 1000, "noteHealthy() makes the next delay the first again");

    // Repair notices: remembered by text, because a relaunched backend starts a new list (review finding).
    NoticeTracker notices;
    check(notices.unseen({"models.db set aside"}) == QStringList({"models.db set aside"}), "first notice is new");
    check(notices.unseen({"models.db set aside"}).isEmpty(), "the same notice is not shown twice");
    check(notices.unseen({"rag.db set aside"}) == QStringList({"rag.db set aside"}),
          "after a backend restart the new process's first notice is still shown");
    check(notices.unseen({"rag.db set aside"}).isEmpty(), "and is shown only once");
    check(notices.unseen({"models.db set aside", "rag.db set aside", "adapters.db set aside"}) == QStringList({"adapters.db set aside"}),
          "a longer list shows only what is new");
    check(NoticeTracker().unseen({}).isEmpty(), "no notices, nothing to show");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
