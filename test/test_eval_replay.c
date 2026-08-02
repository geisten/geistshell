/* P3 (docs/LEARNING.md): reconstruct a fake-model script from a run's journal.
 * Write a journal with known MODEL_OUTPUT events interleaved with other kinds
 * (including an oversized MODEL_INPUT that must be skipped), then reconstruct
 * and assert the script is exactly the model outputs, in order. */
#include "geistshell/eval.h"
#include "geistshell/journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char out0[] =
    "(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
    "(uses_network false) (confidence_bp 6000) (reason \"probe\") "
    "(command \"echo hi\"))";
static const char out1[] = "(recommend (kind finish) (reason \"done\"))";

static enum spg_status write_fixture(const char *path) {
    struct spg_journal_writer w;
    enum spg_status s = spg_journal_writer_open(&w, path);
    if (s != SPG_OK) {
        return s;
    }
    uint64_t seq = 0u;
    /* a big MODEL_INPUT (the context) — larger than the reconstructor's
     * scratch, so it exercises the skip-on-overflow path */
    static char big[16384];
    memset(big, 'x', sizeof big);
    s = spg_journal_writer_append(&w, 1u, 0u, SPG_JOURNAL_EVENT_MODEL_INPUT,
                                  SPG_OK, sizeof big, (const uint8_t *)big, &seq);
    if (s == SPG_OK) {
        s = spg_journal_writer_append(
            &w, 2u, seq, SPG_JOURNAL_EVENT_MODEL_OUTPUT, SPG_OK,
            sizeof out0 - 1u, (const uint8_t *)out0, &seq);
    }
    if (s == SPG_OK) {
        s = spg_journal_writer_append(&w, 3u, seq, SPG_JOURNAL_EVENT_ACTION,
                                      SPG_OK, 3u, (const uint8_t *)"act", &seq);
    }
    if (s == SPG_OK) {
        s = spg_journal_writer_append(
            &w, 4u, seq, SPG_JOURNAL_EVENT_MODEL_OUTPUT, SPG_OK,
            sizeof out1 - 1u, (const uint8_t *)out1, &seq);
    }
    const enum spg_status cs = spg_journal_writer_close(&w);
    return s != SPG_OK ? s : cs;
}

/* P7: a journal with a rejected recommendation and a policy denial tallies
 * one of each; an allow decision must NOT count as a denial. */
static enum spg_status write_recurrence_fixture(const char *path) {
    struct spg_journal_writer w;
    enum spg_status s = spg_journal_writer_open(&w, path);
    if (s != SPG_OK) {
        return s;
    }
    uint64_t seq = 0u;
    const char rej[] = "(recommendation_rejected (reason syntax))";
    const char deny[] =
        "(policy_decision (decision deny) (deny_reason "
        "SPG_POLICY_DENY_DISABLED_CAPABILITY))";
    const char allow[] =
        "(policy_decision (decision allow) (deny_reason SPG_POLICY_DENY_NONE))";
    s = spg_journal_writer_append(&w, 1u, 0u, SPG_JOURNAL_EVENT_ERROR, SPG_OK,
                                  sizeof rej - 1u, (const uint8_t *)rej, &seq);
    if (s == SPG_OK) {
        s = spg_journal_writer_append(&w, 2u, seq,
                                      SPG_JOURNAL_EVENT_POLICY_DECISION, SPG_OK,
                                      sizeof deny - 1u, (const uint8_t *)deny,
                                      &seq);
    }
    if (s == SPG_OK) {
        s = spg_journal_writer_append(&w, 3u, seq,
                                      SPG_JOURNAL_EVENT_POLICY_DECISION, SPG_OK,
                                      sizeof allow - 1u, (const uint8_t *)allow,
                                      &seq);
    }
    const enum spg_status cs = spg_journal_writer_close(&w);
    return s != SPG_OK ? s : cs;
}

static int test_recurrence(void) {
    char path[] = "/tmp/spg_recur_XXXXXX";
    int  fd     = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    close(fd);
    int rc = 1;
    if (write_recurrence_fixture(path) != SPG_OK) {
        goto done;
    }
    struct spg_recurrence r = {};
    /* accumulates: call twice -> counts double */
    if (spg_journal_recurrence(path, &r) != SPG_OK ||
        spg_journal_recurrence(path, &r) != SPG_OK) {
        goto done;
    }
    if (r.rejected != 2u || r.denied != 2u) {
        fprintf(stderr, "recurrence rejected=%zu denied=%zu\n", r.rejected,
                r.denied);
        goto done;
    }
    /* a missing journal counts nothing and is not an error */
    struct spg_recurrence z = {};
    if (spg_journal_recurrence("/tmp/spg_no_such_journal", &z) != SPG_OK ||
        z.rejected != 0u || z.denied != 0u) {
        goto done;
    }
    rc = 0;
done:
    unlink(path);
    return rc;
}

int main(void) {
    if (test_recurrence() != 0) {
        fprintf(stderr, "test_recurrence failed\n");
        return 1;
    }

    char path[] = "/tmp/spg_replay_XXXXXX";
    int  fd     = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    close(fd);

    int rc = 1;
    if (write_fixture(path) != SPG_OK) {
        fprintf(stderr, "write_fixture failed\n");
        goto done;
    }

    struct spg_fake_response responses[8];
    char                     text[4096];
    size_t                   count = 0u;
    if (spg_eval_script_from_journal(path, 8u, responses, sizeof text, text,
                                     &count) != SPG_OK) {
        fprintf(stderr, "reconstruct failed\n");
        goto done;
    }
    /* exactly the two MODEL_OUTPUTs, in order, byte-for-byte; the big
     * MODEL_INPUT and the ACTION event are absent */
    if (count != 2u) {
        fprintf(stderr, "count=%zu, expected 2\n", count);
        goto done;
    }
    if (responses[0].n != sizeof out0 - 1u ||
        memcmp(responses[0].text, out0, responses[0].n) != 0 ||
        responses[1].n != sizeof out1 - 1u ||
        memcmp(responses[1].text, out1, responses[1].n) != 0) {
        fprintf(stderr, "reconstructed script does not match the outputs\n");
        goto done;
    }

    /* capacity is enforced, not overrun */
    if (spg_eval_script_from_journal(path, 1u, responses, sizeof text, text,
                                     &count) != SPG_E_LIMIT) {
        fprintf(stderr, "max_responses cap not enforced\n");
        goto done;
    }
    if (spg_eval_script_from_journal(nullptr, 8u, responses, sizeof text, text,
                                     &count) != SPG_E_INVALID_ARG) {
        fprintf(stderr, "null arg not rejected\n");
        goto done;
    }

    rc = 0;
done:
    unlink(path);
    if (rc == 0) {
        printf("test_eval_replay: PASS\n");
    }
    return rc;
}
