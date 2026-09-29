/*
 * The operator's camera order (rig-order.h): stable, keyed by UUID,
 * reconciled against whatever cameras currently exist.
 */

#include "rig-order.h"
#include "test.h"

/* Renders the display order of `present` as "BAC". */
static void order_of(struct lenslink_rig_order *o, const char *const *present,
		     size_t n, char *out)
{
	size_t idx[LENSLINK_RIG_MAX];
	size_t k = lenslink_rig_order_arrange(o, present, n, idx);
	for (size_t i = 0; i < k; i++)
		out[i] = present[idx[i]][0];
	out[k] = '\0';
}

static void test_saved_order_wins(void)
{
	struct lenslink_rig_order o;
	const char *saved[] = {"B", "A", "C"};
	const char *present[] = {"A", "B", "C"};
	char s[16];

	lenslink_rig_order_clear(&o);
	lenslink_rig_order_import(&o, saved, 3);
	order_of(&o, present, 3, s);
	CHECK_STR(s, "BAC");
}

static void test_new_appends_removed_drops(void)
{
	struct lenslink_rig_order o;
	const char *saved[] = {"B", "A", "C"};
	char s[16];

	lenslink_rig_order_clear(&o);
	lenslink_rig_order_import(&o, saved, 3);

	const char *with_d[] = {"A", "B", "C", "D"};
	order_of(&o, with_d, 4, s);
	CHECK_STR(s, "BACD");

	const char *without_c[] = {"A", "B", "D"};
	order_of(&o, without_c, 3, s);
	CHECK_STR(s, "BAD");

	/* C returns (same UUID, same collection — an undo): its old slot. */
	order_of(&o, with_d, 4, s);
	CHECK_STR(s, "BACD");
}

static void test_export_is_present_only(void)
{
	struct lenslink_rig_order o;
	const char *saved[] = {"B", "A", "C"};
	const char *without_c[] = {"A", "B", "D"};
	char out[8][LENSLINK_UUID_SIZE];

	lenslink_rig_order_clear(&o);
	lenslink_rig_order_import(&o, saved, 3);
	size_t n = lenslink_rig_order_export(&o, without_c, 3, out, 8);
	CHECK(n == 3);
	CHECK_STR(out[0], "B");
	CHECK_STR(out[1], "A");
	CHECK_STR(out[2], "D"); /* no trace of C */
}

static void test_first_run_uses_first_seen_order(void)
{
	struct lenslink_rig_order o;
	const char *first[] = {"X", "Y"};
	char s[16];
	lenslink_rig_order_clear(&o);
	order_of(&o, first, 2, s);
	CHECK_STR(s, "XY");

	/* Later, with the list handed back in another order (creation order
	 * differs between launches): the established order holds. */
	const char *again[] = {"Y", "X"};
	order_of(&o, again, 2, s);
	CHECK_STR(s, "XY");
}

static void test_order_ignores_everything_but_uuid(void)
{
	/* Nothing but the UUID list feeds the arrangement: there is no
	 * input through which tally, health or a rename could reorder. */
	struct lenslink_rig_order o;
	const char *present[] = {"C", "A", "B"};
	char s[16];
	lenslink_rig_order_clear(&o);
	order_of(&o, present, 3, s);
	for (int i = 0; i < 5; i++) {
		char again[16];
		order_of(&o, present, 3, again);
		CHECK_STR(again, s);
	}
}

static void test_import_sanitizes(void)
{
	struct lenslink_rig_order o;
	const char *saved[] = {"A", "", "A", NULL, "B"};
	const char *present[] = {"B", "A"};
	char s[16];
	lenslink_rig_order_clear(&o);
	lenslink_rig_order_import(&o, saved, 5);
	CHECK(o.count == 2);
	order_of(&o, present, 2, s);
	CHECK_STR(s, "AB");
}

static void test_duplicate_present_skipped(void)
{
	struct lenslink_rig_order o;
	const char *present[] = {"A", "A", "", "B"};
	char s[16];
	lenslink_rig_order_clear(&o);
	order_of(&o, present, 4, s);
	CHECK_STR(s, "AB");
}

static void test_move(void)
{
	struct lenslink_rig_order o;
	const char *present[] = {"A", "B", "C"};
	char s[16];
	lenslink_rig_order_clear(&o);
	order_of(&o, present, 3, s);
	CHECK_STR(s, "ABC");

	CHECK(lenslink_rig_order_move(&o, present, 3, "C", -1));
	order_of(&o, present, 3, s);
	CHECK_STR(s, "ACB");
	CHECK(lenslink_rig_order_move(&o, present, 3, "A", +1));
	order_of(&o, present, 3, s);
	CHECK_STR(s, "CAB");

	CHECK(!lenslink_rig_order_move(&o, present, 3, "C", -1)); /* edge */
	CHECK(!lenslink_rig_order_move(&o, present, 3, "B", +1)); /* edge */
	CHECK(!lenslink_rig_order_move(&o, present, 3, "Z", +1)); /* absent */
	CHECK(!lenslink_rig_order_move(&o, present, 3, "A", 2));  /* bad delta */
}

static void test_move_keeps_absent_slot(void)
{
	struct lenslink_rig_order o;
	const char *all[] = {"A", "B", "C"};
	const char *no_b[] = {"A", "C"};
	char s[16];
	lenslink_rig_order_clear(&o);
	order_of(&o, all, 3, s);

	/* B is away; the operator swaps A and C. When B returns it must be
	 * between where they were, not thrown to the bottom. */
	CHECK(lenslink_rig_order_move(&o, no_b, 2, "C", -1));
	order_of(&o, no_b, 2, s);
	CHECK_STR(s, "CA");
	order_of(&o, all, 3, s);
	CHECK_STR(s, "CBA");
}

static void test_capacity(void)
{
	/* A long session churning through many transient cameras never
	 * overflows the list and never loses a present camera. */
	struct lenslink_rig_order o;
	char names[LENSLINK_RIG_MAX * 3][8];
	const char *present[2];
	lenslink_rig_order_clear(&o);
	for (int i = 0; i < LENSLINK_RIG_MAX * 3; i++) {
		snprintf(names[i], sizeof(names[i]), "c%d", i);
		present[0] = "keeper";
		present[1] = names[i];
		size_t idx[2];
		size_t k = lenslink_rig_order_arrange(&o, present, 2, idx);
		CHECK(k == 2);
		CHECK(o.count <= LENSLINK_RIG_MAX);
	}
	size_t idx[2];
	present[0] = "keeper";
	present[1] = "c0";
	lenslink_rig_order_arrange(&o, present, 2, idx);
	CHECK(strcmp(present[idx[0]], "keeper") == 0);
}

int main(void)
{
	test_saved_order_wins();
	test_new_appends_removed_drops();
	test_export_is_present_only();
	test_first_run_uses_first_seen_order();
	test_order_ignores_everything_but_uuid();
	test_import_sanitizes();
	test_duplicate_present_skipped();
	test_move();
	test_move_keeps_absent_slot();
	test_capacity();
	TEST_MAIN_END();
}
