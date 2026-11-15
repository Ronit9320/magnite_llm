#include "reasoning_tests.h"

#include "../reasoning.h"

#include <cstdio>

int runReasoningCheck() {
    int failures = 0;
    auto check = [&failures](bool ok, const char *message) {
        fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };

    {
        const auto p = Reasoning::parse("plain answer\nsecond line");
        check(!p.hasThinking && !p.open && p.answer == "plain answer\nsecond line", "text without tags is all answer");
    }
    {
        const auto p = Reasoning::parse("<think>\nOkay, a greeting.\n</think>\n\nHello! How can I help?");
        check(p.hasThinking && !p.open && p.thinking == "Okay, a greeting." && p.answer == "Hello! How can I help?",
              "a closed block splits into thinking and answer");
    }
    {
        const auto p = Reasoning::parse("<think>\nStill working it out");
        check(p.hasThinking && p.open && p.thinking == "Still working it out" && p.answer.isEmpty(),
              "an open block is all thinking and has no answer yet");
    }
    {
        const auto p = Reasoning::parse("<think>\nStill working</thi");
        check(p.open && p.thinking == "Still working", "a closing tag cut mid-stream is not shown as thinking");
    }
    {
        const auto p = Reasoning::parse("<thi");
        check(p.open && !p.hasThinking && p.answer.isEmpty(), "a half-written opening tag shows nothing");
        const auto q = Reasoning::parse("<b>bold</b> html");
        check(!q.open && q.answer == "<b>bold</b> html", "other text starting with < is just an answer");
    }
    {
        const auto p = Reasoning::parse("Let me think about this.\n</think>\nThe answer is 4.");
        check(p.hasThinking && !p.open && p.thinking == "Let me think about this." && p.answer == "The answer is 4.",
              "a reply with only the closing tag (template opened the block) splits the same way");
    }
    {
        const auto p = Reasoning::parse("<think>\n\n</think>\n\nJust the answer.");
        check(!p.hasThinking && !p.open && p.answer == "Just the answer.", "an empty block (thinking switched off) leaves only the answer");
    }
    {
        const auto p = Reasoning::parse("<think>\nthought</think>\n    indented code\nnext");
        check(p.answer == "    indented code\nnext", "the answer keeps its own indentation");
    }
    {
        const auto p = Reasoning::parse("  \n<think>x</think>done");
        check(p.hasThinking && p.thinking == "x" && p.answer == "done", "leading whitespace before the block is ignored");
    }
    {
        const auto p = Reasoning::parse("");
        check(!p.hasThinking && !p.open && p.answer.isEmpty(), "empty text parses to nothing");
    }

    check(Reasoning::answerOnly("hi there") == "hi there", "history: text without tags is sent unchanged");
    check(Reasoning::answerOnly("<think>a</think>\n\nanswer") == "answer", "history: thinking is dropped");
    check(Reasoning::answerOnly("a\n</think>answer") == "answer", "history: closing-tag-only thinking is dropped");
    check(Reasoning::answerOnly("<think>cut off while thinking") == "<think>cut off while thinking",
          "history: a reply with no answer is kept rather than sent empty");

    fprintf(stderr, "[reasoning-check] %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
