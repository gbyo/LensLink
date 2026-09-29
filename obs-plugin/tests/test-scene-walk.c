/*
 * Preview-membership walk over a fake scene graph (scene-walk.h). The
 * cases mirror what an operator can build in OBS: direct items, groups,
 * nested scenes, hidden items at every level, one source referenced from
 * several places, and a malformed graph that must not hang or overflow.
 */

#include "scene-walk.h"
#include "test.h"

#define MAX_ITEMS 16
#define MAX_NODES 40

struct fake_item {
	bool visible;
	int container;      /* node index, or -1 for a plain source */
	const char *uuid;   /* LensLink source UUID, or NULL */
};

struct fake_node {
	struct fake_item items[MAX_ITEMS];
	int count;
};

struct fake_graph {
	struct fake_node nodes[MAX_NODES];
	int count;
};

static void graph_reset(struct fake_graph *g)
{
	memset(g, 0, sizeof(*g));
	g->count = 0;
}

static int add_node(struct fake_graph *g)
{
	return g->count++;
}

static void add_item(struct fake_graph *g, int node, bool visible,
		     int container, const char *uuid)
{
	struct fake_node *n = &g->nodes[node];
	n->items[n->count].visible = visible;
	n->items[n->count].container = container;
	n->items[n->count].uuid = uuid;
	n->count++;
}

static void enum_items(void *ctx, const void *container,
		       bool (*cb)(void *, const struct lenslink_walk_item *),
		       void *param)
{
	struct fake_graph *g = ctx;
	/* Containers are node index + 1 so index 0 is a valid non-NULL. */
	const struct fake_node *n = &g->nodes[(size_t)container - 1];
	for (int i = 0; i < n->count; i++) {
		struct lenslink_walk_item item = {
			.visible = n->items[i].visible,
			.container = n->items[i].container >= 0
					     ? (const void *)(size_t)(
						       n->items[i].container + 1)
					     : NULL,
			.lenslink_uuid = n->items[i].uuid,
		};
		if (!cb(param, &item))
			return;
	}
}

static const struct lenslink_walk_ops ops = {.enum_items = enum_items};

static bool walk(struct fake_graph *g, int root, struct lenslink_uuid_set *out)
{
	return lenslink_scene_walk(&ops, g, (const void *)(size_t)(root + 1),
				   out);
}

static void test_direct(void)
{
	struct fake_graph g;
	struct lenslink_uuid_set set;
	graph_reset(&g);
	int scene = add_node(&g);
	add_item(&g, scene, true, -1, "cam-a");
	add_item(&g, scene, false, -1, "cam-b");  /* hidden item */
	add_item(&g, scene, true, -1, NULL);      /* some other source */

	CHECK(walk(&g, scene, &set));
	CHECK(set.count == 1);
	CHECK(lenslink_uuid_set_has(&set, "cam-a"));
	CHECK(!lenslink_uuid_set_has(&set, "cam-b"));
}

static void test_no_scene(void)
{
	struct fake_graph g;
	struct lenslink_uuid_set set;
	graph_reset(&g);
	/* Studio Mode off / no preview scene: an empty, successful walk. */
	CHECK(lenslink_scene_walk(&ops, &g, NULL, &set));
	CHECK(set.count == 0);
}

static void test_groups(void)
{
	struct fake_graph g;
	struct lenslink_uuid_set set;
	graph_reset(&g);
	int scene = add_node(&g);
	int shown = add_node(&g);
	int hidden = add_node(&g);
	add_item(&g, scene, true, shown, NULL);   /* visible group */
	add_item(&g, scene, false, hidden, NULL); /* hidden group */
	add_item(&g, shown, true, -1, "in-visible-group");
	add_item(&g, shown, false, -1, "hidden-in-visible-group");
	add_item(&g, hidden, true, -1, "in-hidden-group");

	CHECK(walk(&g, scene, &set));
	CHECK(set.count == 1);
	CHECK(lenslink_uuid_set_has(&set, "in-visible-group"));
	CHECK(!lenslink_uuid_set_has(&set, "hidden-in-visible-group"));
	CHECK(!lenslink_uuid_set_has(&set, "in-hidden-group"));
}

static void test_nested_scenes(void)
{
	struct fake_graph g;
	struct lenslink_uuid_set set;
	graph_reset(&g);
	int preview = add_node(&g);
	int inner = add_node(&g);
	int inner2 = add_node(&g);
	add_item(&g, preview, true, inner, NULL);
	add_item(&g, inner, true, -1, "nested-visible");
	add_item(&g, inner, false, -1, "nested-item-hidden");
	add_item(&g, inner, true, inner2, NULL);
	add_item(&g, inner2, true, -1, "two-deep");

	CHECK(walk(&g, preview, &set));
	CHECK(set.count == 2);
	CHECK(lenslink_uuid_set_has(&set, "nested-visible"));
	CHECK(lenslink_uuid_set_has(&set, "two-deep"));

	/* The nested scene's own item hidden in the parent: nothing under
	 * it is visible, whatever its children's flags say. */
	g.nodes[preview].items[0].visible = false;
	CHECK(walk(&g, preview, &set));
	CHECK(set.count == 0);
}

static void test_not_in_preview_scene(void)
{
	/* A source shown by a projector, another scene, or the program
	 * output is simply not in the Preview scene's graph: no PVW. */
	struct fake_graph g;
	struct lenslink_uuid_set set;
	graph_reset(&g);
	int preview = add_node(&g);
	int program = add_node(&g);
	add_item(&g, preview, true, -1, "in-preview");
	add_item(&g, program, true, -1, "projector-or-program-only");

	CHECK(walk(&g, preview, &set));
	CHECK(lenslink_uuid_set_has(&set, "in-preview"));
	CHECK(!lenslink_uuid_set_has(&set, "projector-or-program-only"));
}

static void test_duplicates(void)
{
	struct fake_graph g;
	struct lenslink_uuid_set set;
	graph_reset(&g);
	int preview = add_node(&g);
	int shared = add_node(&g);
	add_item(&g, shared, true, -1, "cam-a");

	/* The same source twice in one scene: one entry. */
	add_item(&g, preview, true, -1, "cam-a");
	add_item(&g, preview, true, -1, "cam-a");
	CHECK(walk(&g, preview, &set));
	CHECK(set.count == 1);

	/* One nested scene referenced twice, first through a hidden item
	 * and then a visible one: the hidden reference must not mark it
	 * visited and hide the visible one. */
	graph_reset(&g);
	preview = add_node(&g);
	shared = add_node(&g);
	add_item(&g, shared, true, -1, "cam-a");
	add_item(&g, preview, false, shared, NULL);
	add_item(&g, preview, true, shared, NULL);
	CHECK(walk(&g, preview, &set));
	CHECK(lenslink_uuid_set_has(&set, "cam-a"));

	/* And referenced twice visibly: walked once, still found. */
	graph_reset(&g);
	preview = add_node(&g);
	shared = add_node(&g);
	add_item(&g, shared, true, -1, "cam-a");
	add_item(&g, preview, true, shared, NULL);
	add_item(&g, preview, true, shared, NULL);
	CHECK(walk(&g, preview, &set));
	CHECK(set.count == 1);

	/* A source hidden in one place and visible in another is visible. */
	graph_reset(&g);
	preview = add_node(&g);
	add_item(&g, preview, false, -1, "cam-a");
	add_item(&g, preview, true, -1, "cam-a");
	CHECK(walk(&g, preview, &set));
	CHECK(lenslink_uuid_set_has(&set, "cam-a"));
}

static void test_cycle(void)
{
	/* OBS refuses these, but a malformed graph must not hang. */
	struct fake_graph g;
	struct lenslink_uuid_set set;
	graph_reset(&g);
	int a = add_node(&g);
	int b = add_node(&g);
	add_item(&g, a, true, b, NULL);
	add_item(&g, b, true, a, NULL);
	add_item(&g, b, true, -1, "cam-a");
	add_item(&g, a, true, a, NULL); /* self-reference */

	CHECK(walk(&g, a, &set));
	CHECK(set.count == 1);
	CHECK(lenslink_uuid_set_has(&set, "cam-a"));
}

static void test_bounds(void)
{
	struct fake_graph g;
	struct lenslink_uuid_set set;

	/* Nesting deeper than the bound: unknown, so nothing is claimed. */
	graph_reset(&g);
	int prev = add_node(&g);
	int root = prev;
	add_item(&g, prev, true, -1, "cam-a");
	for (int i = 0; i < LENSLINK_WALK_MAX_DEPTH + 4; i++) {
		int next = add_node(&g);
		add_item(&g, prev, true, next, NULL);
		add_item(&g, next, true, -1, "cam-deep");
		prev = next;
	}
	CHECK(!walk(&g, root, &set));
	CHECK(set.count == 0);

	/* More sources than the set holds: also unknown, never truncated
	 * into a partly-wrong answer. 9 nested scenes x 8 sources = 72. */
	static char names[9][8][16];
	graph_reset(&g);
	int top = add_node(&g);
	for (int i = 0; i < 9; i++) {
		int inner = add_node(&g);
		add_item(&g, top, true, inner, NULL);
		for (int j = 0; j < 8; j++) {
			snprintf(names[i][j], sizeof(names[i][j]), "c%d-%d", i,
				 j);
			add_item(&g, inner, true, -1, names[i][j]);
		}
	}
	CHECK(!walk(&g, top, &set));
	CHECK(set.count == 0);

	/* The set itself refuses the 65th distinct entry but accepts a
	 * repeat of one it holds. */
	struct lenslink_uuid_set full;
	full.count = 0;
	for (int i = 0; i < LENSLINK_WALK_MAX_SOURCES; i++) {
		char n[16];
		snprintf(n, sizeof(n), "u%d", i);
		CHECK(lenslink_uuid_set_add(&full, n));
	}
	CHECK(!lenslink_uuid_set_add(&full, "one-too-many"));
	CHECK(lenslink_uuid_set_add(&full, "u3"));
}

int main(void)
{
	test_direct();
	test_no_scene();
	test_groups();
	test_nested_scenes();
	test_not_in_preview_scene();
	test_duplicates();
	test_cycle();
	test_bounds();
	TEST_MAIN_END();
}
