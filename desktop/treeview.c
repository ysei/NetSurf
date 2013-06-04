/*
 * Copyright 2012 - 2013 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/** \file
 * Treeview handling (implementation).
 */

#include "css/utils.h"
#include "desktop/gui.h"
#include "desktop/knockout.h"
#include "desktop/plotters.h"
#include "desktop/treeview.h"
#include "render/font.h"
#include "utils/log.h"

#define FIELD_FOLDER 0
#define FIELD_FIRST_ENTRY 1

/* TODO: get rid of REDRAW_MAX -- need to be able to know window size */
#define REDRAW_MAX 8000

struct treeview_globals {
	int line_height;
	int furniture_width;
	int step_width;
	int window_padding;
	int icon_step;
} tree_g;

enum treeview_node_type {
	TREE_NODE_ROOT,
	TREE_NODE_FOLDER,
	TREE_NODE_ENTRY
};

struct treeview_text {
	const char *data;
	uint32_t len;
	int width;
};

struct treeview_field {
	enum treeview_field_flags flags;

	lwc_string *field;
	struct treeview_text value;
};

enum treeview_node_flags {
	TREE_NODE_NONE		= 0,		/**< No node flags set */
	TREE_NODE_EXPANDED	= (1 << 0),	/**< Whether node is expanded */
	TREE_NODE_SELECTED	= (1 << 1)	/**< Whether node is selected */

};

struct treeview_node {
	enum treeview_node_flags flags;
	enum treeview_node_type type;

	int height;
	int inset;

	struct treeview_node *parent;
	struct treeview_node *sibling_prev;
	struct treeview_node *sibling_next;
	struct treeview_node *children;

	void *client_data;

	struct treeview_field text;
};

struct treeview_node_entry {
	struct treeview_node base;
	struct treeview_field fields[];
};

struct treeview {
	uint32_t view_height;
	uint32_t view_width;

	struct treeview_node *root;

	struct treeview_field *fields;
	int n_fields; /* fields[n_fields] is folder, lower are entry fields */
	int field_width;

	const struct treeview_callback_table *callbacks;
	const struct core_window_callback_table *cw_t; /**< Core window callback table */
	struct core_window *cw_h; /**< Core window handle */
};


struct treeview_node_style {
	plot_style_t bg;		/**< Background */
	plot_font_style_t text;		/**< Text */
	plot_font_style_t itext;	/**< Entry field text */

	plot_style_t sbg;		/**< Selected background */
	plot_font_style_t stext;	/**< Selected text */
	plot_font_style_t sitext;	/**< Selected entry field text */
};

struct treeview_node_style plot_style_odd;
struct treeview_node_style plot_style_even;

struct treeview_resource {
	const char *url;
	struct hlcache_handle *c;
	int height;
	bool ready;
};
enum treeview_resource_id {
	TREE_RES_CONTENT = 0,
	TREE_RES_FOLDER,
	TREE_RES_SEARCH,
	TREE_RES_LAST
};
static struct treeview_resource treeview_res[TREE_RES_LAST] = {
	{ "resource:icons/content.png", NULL, 0, false },
	{ "resource:icons/directory.png", NULL, 0, false },
	{ "resource:icons/search.png", NULL, 0, false }
};



enum treeview_furniture_id {
	TREE_FURN_EXPAND = 0,
	TREE_FURN_CONTRACT,
	TREE_FURN_LAST
};
static struct treeview_text treeview_furn[TREE_FURN_LAST] = {
	{ "\xe2\x96\xb8", 3, 0 },
	{ "\xe2\x96\xbe", 3, 0 }
};


static nserror treeview_create_node_root(struct treeview_node **root)
{
	struct treeview_node *n;

	n = malloc(sizeof(struct treeview_node));
	if (n == NULL) {
		return NSERROR_NOMEM;
	}

	n->flags = TREE_NODE_EXPANDED;
	n->type = TREE_NODE_ROOT;

	n->height = 0;
	n->inset = tree_g.window_padding - tree_g.step_width;

	n->text.flags = TREE_FLAG_NONE;
	n->text.field = NULL;
	n->text.value.data = NULL;
	n->text.value.len = 0;
	n->text.value.width = 0;

	n->parent = NULL;
	n->sibling_next = NULL;
	n->sibling_prev = NULL;
	n->children = NULL;

	n->client_data = NULL;

	*root = n;

	return NSERROR_OK;
}

/**
 * Insert a treeview node into a treeview
 *
 * \param a    parentless node to insert
 * \param b    tree node to insert a as a relation of
 * \param rel  a's relationship to b
 */
static inline void treeview_insert_node(struct treeview_node *a,
		struct treeview_node *b,
		enum treeview_relationship rel)
{
	assert(a != NULL);
	assert(a->parent == NULL);
	assert(b != NULL);

	switch (rel) {
	case TREE_REL_FIRST_CHILD:
		assert(b->type != TREE_NODE_ENTRY);
		a->parent = b;
		a->sibling_next = b->children;
		if (a->sibling_next)
			a->sibling_next->sibling_prev = a;
		b->children = a;
		break;

	case TREE_REL_NEXT_SIBLING:
		assert(b->type != TREE_NODE_ROOT);
		a->sibling_prev = b;
		a->sibling_next = b->sibling_next;
		a->parent = b->parent;
		b->sibling_next = a;
		if (a->sibling_next)
			a->sibling_next->sibling_prev = a;
		break;

	default:
		assert(0);
		break;
	}

	assert(a->parent != NULL);

	a->inset = a->parent->inset + tree_g.step_width;

	if (a->parent->flags & TREE_NODE_EXPANDED) {
		/* Parent is expanded, so inserted node will be visible and
		 * affect layout */
		b = a;
		do {
			b->parent->height += b->height;
			b = b->parent;
		} while (b->parent != NULL);

		if (a->text.value.width == 0) {
			nsfont.font_width(&plot_style_odd.text,
					a->text.value.data,
					a->text.value.len,
					&(a->text.value.width));
		}
	}
}


nserror treeview_create_node_folder(struct treeview *tree,
		struct treeview_node **folder,
		struct treeview_node *relation,
		enum treeview_relationship rel,
		const struct treeview_field_data *field,
		void *data)
{
	struct treeview_node *n;

	assert(data != NULL);
	assert(tree != NULL);
	assert(tree->root != NULL);

	if (relation == NULL) {
		relation = tree->root;
		rel = TREE_REL_FIRST_CHILD;
	}

	n = malloc(sizeof(struct treeview_node));
	if (n == NULL) {
		return NSERROR_NOMEM;
	}

	n->flags = TREE_NODE_NONE;
	n->type = TREE_NODE_FOLDER;

	n->height = tree_g.line_height;

	n->text.value.data = field->value;
	n->text.value.len = field->value_len;
	n->text.value.width = 0;

	n->parent = NULL;
	n->sibling_next = NULL;
	n->sibling_prev = NULL;
	n->children = NULL;

	n->client_data = data;

	treeview_insert_node(n, relation, rel);

	*folder = n;

	return NSERROR_OK;
}



nserror treeview_update_node_entry(struct treeview *tree,
		struct treeview_node *entry,
		const struct treeview_field_data fields[],
		void *data)
{
	bool match;
	struct treeview_node_entry *e = (struct treeview_node_entry *)entry;
	int i;

	assert(data != NULL);
	assert(tree != NULL);
	assert(entry != NULL);
	assert(data == entry->client_data);
	assert(entry->parent != NULL);

	assert(fields != NULL);
	assert(fields[0].field != NULL);
	assert(lwc_string_isequal(tree->fields[0].field,
			fields[0].field, &match) == lwc_error_ok &&
			match == true);
	entry->text.value.data = fields[0].value;
	entry->text.value.len = fields[0].value_len;
	entry->text.value.width = 0;

	if (entry->parent->flags & TREE_NODE_EXPANDED) {
		/* Text will be seen, get its width */
		nsfont.font_width(&plot_style_odd.text,
				entry->text.value.data,
				entry->text.value.len,
				&(entry->text.value.width));
	} else {
		/* Just invalidate the width, since it's not needed now */
		entry->text.value.width = 0;
	}

	for (i = 1; i < tree->n_fields; i++) {
		assert(fields[i].field != NULL);
		assert(lwc_string_isequal(tree->fields[i].field,
				fields[i].field, &match) == lwc_error_ok &&
				match == true);

		e->fields[i - 1].value.data = fields[i].value;
		e->fields[i - 1].value.len = fields[i].value_len;

		if (entry->flags & TREE_NODE_EXPANDED) {
			/* Text will be seen, get its width */
			nsfont.font_width(&plot_style_odd.text,
					e->fields[i - 1].value.data,
					e->fields[i - 1].value.len,
					&(e->fields[i - 1].value.width));
		} else {
			/* Invalidate the width, since it's not needed yet */
			e->fields[i - 1].value.width = 0;
		}
	}

	return NSERROR_OK;
}


nserror treeview_create_node_entry(struct treeview *tree,
		struct treeview_node **entry,
		struct treeview_node *relation,
		enum treeview_relationship rel,
		const struct treeview_field_data fields[],
		void *data)
{
	bool match;
	struct treeview_node_entry *e;
	struct treeview_node *n;
	int i;

	assert(data != NULL);
	assert(tree != NULL);
	assert(tree->root != NULL);

	if (relation == NULL) {
		relation = tree->root;
		rel = TREE_REL_FIRST_CHILD;
	}

	e = malloc(sizeof(struct treeview_node_entry) +
			(tree->n_fields - 1) * sizeof(struct treeview_field));
	if (e == NULL) {
		return NSERROR_NOMEM;
	}


	n = (struct treeview_node *) e;

	n->flags = TREE_NODE_NONE;
	n->type = TREE_NODE_ENTRY;

	n->height = tree_g.line_height;

	assert(fields != NULL);
	assert(fields[0].field != NULL);
	assert(lwc_string_isequal(tree->fields[0].field,
			fields[0].field, &match) == lwc_error_ok &&
			match == true);
	n->text.value.data = fields[0].value;
	n->text.value.len = fields[0].value_len;
	n->text.value.width = 0;

	n->parent = NULL;
	n->sibling_next = NULL;
	n->sibling_prev = NULL;
	n->children = NULL;

	n->client_data = data;

	for (i = 1; i < tree->n_fields; i++) {
		assert(fields[i].field != NULL);
		assert(lwc_string_isequal(tree->fields[i].field,
				fields[i].field, &match) == lwc_error_ok &&
				match == true);

		e->fields[i - 1].value.data = fields[i].value;
		e->fields[i - 1].value.len = fields[i].value_len;
		e->fields[i - 1].value.width = 0;
	}

	treeview_insert_node(n, relation, rel);

	*entry = n;

	return NSERROR_OK;
}


nserror treeview_delete_node(struct treeview *tree, struct treeview_node *n)
{
	struct treeview_node_msg msg;
	msg.msg = TREE_MSG_NODE_DELETE;

	/* Destroy children first */
	while (n->children != NULL) {
		treeview_delete_node(tree, n->children);
	}

	/* Unlink node from tree */
	if (n->parent != NULL && n->parent->children == n) {
		/* Node is a first child */
		n->parent->children = n->sibling_next;

	} else if (n->sibling_prev != NULL) {
		/* Node is not first child */
		n->sibling_prev->sibling_next = n->sibling_next;
	}

	if (n->sibling_next != NULL) {
		/* Always need to do this */
		n->sibling_next->sibling_prev = n->sibling_prev;
	}

	/* Handle any special treatment */
	switch (n->type) {
	case TREE_NODE_ENTRY:
		tree->callbacks->entry(msg, n->client_data);
		break;
	case TREE_NODE_FOLDER:
		tree->callbacks->folder(msg, n->client_data);
		break;
	case TREE_NODE_ROOT:
		break;
	default:
		return NSERROR_BAD_PARAMETER;
	}

	/* Free the node */
	free(n);

	return NSERROR_OK;
}


nserror treeview_create(struct treeview **tree,
		const struct treeview_callback_table *callbacks,
		int n_fields, struct treeview_field_desc fields[],
		const struct core_window_callback_table *cw_t,
		struct core_window *cw)
{
	nserror error;
	int i;

	assert(cw_t != NULL);
	assert(cw != NULL);
	assert(callbacks != NULL);

	assert(fields != NULL);
	assert(fields[0].flags & TREE_FLAG_DEFAULT);
	assert(fields[n_fields - 1].flags & TREE_FLAG_DEFAULT);
	assert(n_fields >= 2);

	*tree = malloc(sizeof(struct treeview));
	if (*tree == NULL) {
		return NSERROR_NOMEM;
	}

	(*tree)->fields = malloc(sizeof(struct treeview_field) * n_fields);
	if ((*tree)->fields == NULL) {
		free(tree);
		return NSERROR_NOMEM;
	}

	error = treeview_create_node_root(&((*tree)->root));
	if (error != NSERROR_OK) {
		free((*tree)->fields);
		free(*tree);
		return error;
	}

	(*tree)->field_width = 0;
	for (i = 0; i < n_fields; i++) {
		struct treeview_field *f = &((*tree)->fields[i]);

		f->flags = fields[i].flags;
		f->field = lwc_string_ref(fields[i].field);
		f->value.data = lwc_string_data(fields[i].field);
		f->value.len = lwc_string_length(fields[i].field);

		nsfont.font_width(&plot_style_odd.text, f->value.data,
				f->value.len, &(f->value.width));

		if (f->flags & TREE_FLAG_SHOW_NAME)
			if ((*tree)->field_width < f->value.width)
				(*tree)->field_width = f->value.width;
	}

	(*tree)->field_width += tree_g.step_width;

	(*tree)->callbacks = callbacks;
	(*tree)->n_fields = n_fields - 1;

	(*tree)->cw_t = cw_t;
	(*tree)->cw_h = cw;

	return NSERROR_OK;
}

nserror treeview_destroy(struct treeview *tree)
{
	int f;

	assert(tree != NULL);

	/* Destroy nodes */
	treeview_delete_node(tree, tree->root);

	/* Destroy feilds */
	for (f = 0; f <= tree->n_fields; f++) {
		lwc_string_unref(tree->fields[f].field);
	}
	free(tree->fields);

	/* Free treeview */
	free(tree);

	return NSERROR_OK;
}


/* Walk a treeview subtree, calling a callback at each node (depth first)
 *
 * \param root     Root to walk tree from (doesn't get a callback call)
 * \param full     Iff true, visit children of collapsed nodes
 * \param callback Function to call on each node
 * \param ctx      Context to pass to callback
 * \return true iff callback caused premature abort
 */
static bool treeview_walk_internal(struct treeview_node *root, bool full,
		bool (*callback)(struct treeview_node *node, void *ctx),
		void *ctx)
{
	struct treeview_node *node, *next;

	node = root;

	while (node != NULL) {
		next = (full || (node->flags & TREE_NODE_EXPANDED)) ?
				node->children : NULL;

		if (next != NULL) {
			/* Down to children */
			node = next;
		} else {
			/* No children.  As long as we're not at the root,
			 * go to next sibling if present, or nearest ancestor
			 * with a next sibling. */

			while (node != root &&
					node->sibling_next == NULL) {
				node = node->parent;
			}

			if (node == root)
				break;

			node = node->sibling_next;
		}

		assert(node != NULL);
		assert(node != root);

		if (callback(node, ctx)) {
			/* callback caused early termination */
			return true;
		}

	}
	return false;
}


nserror treeview_node_expand(struct treeview *tree,
		struct treeview_node *node)
{
	struct treeview_node *child;
	struct treeview_node_entry *e;
	int additional_height = 0;
	int i;

	assert(tree != NULL);
	assert(node != NULL);

	if (node->flags & TREE_NODE_EXPANDED) {
		/* What madness is this? */
		LOG(("Tried to expand an expanded node."));
		return NSERROR_OK;
	}

	switch (node->type) {
	case TREE_NODE_FOLDER:
		child = node->children;
		if (child == NULL) {
			/* Can't expand an empty node */
			return NSERROR_OK;
		}

		do {
			assert((child->flags & TREE_NODE_EXPANDED) == false);
			if (child->text.value.width == 0) {
				nsfont.font_width(&plot_style_odd.text,
						child->text.value.data,
						child->text.value.len,
						&(child->text.value.width));
			}

			additional_height += child->height;

			child = child->sibling_next;
		} while (child != NULL);

		break;

	case TREE_NODE_ENTRY:
		assert(node->children == NULL);

		e = (struct treeview_node_entry *)node;

		for (i = 0; i < tree->n_fields - 1; i++) {

			if (e->fields[i].value.width == 0) {
				nsfont.font_width(&plot_style_odd.text,
						e->fields[i].value.data,
						e->fields[i].value.len,
						&(e->fields[i].value.width));
			}

			/* Add height for field */
			additional_height += tree_g.line_height;
		}

		break;

	case TREE_NODE_ROOT:
		assert(node->type != TREE_NODE_ROOT);
		break;
	}

	/* Update the node */
	node->flags |= TREE_NODE_EXPANDED;

	/* And parent's heights */
	do {
		node->height += additional_height;
		node = node->parent;
	} while (node->parent != NULL);

	node->height += additional_height;

	return NSERROR_OK;
}


static bool treeview_node_contract_cb(struct treeview_node *node, void *ctx)
{
	int height_reduction;

	assert(node != NULL);
	assert(node->type != TREE_NODE_ROOT);

	if ((node->flags & TREE_NODE_EXPANDED) == false) {
		/* Nothing to do. */
		return false;
	}

	node->flags ^= TREE_NODE_EXPANDED;
	height_reduction = node->height - tree_g.line_height;

	assert(height_reduction >= 0);

	do {
		node->height -= height_reduction;
		node = node->parent;
	} while (node->parent != NULL &&
			node->parent->flags & TREE_NODE_EXPANDED);

	return false; /* Don't want to abort tree walk */
}
nserror treeview_node_contract(struct treeview *tree,
		struct treeview_node *node)
{
	assert(node != NULL);

	if ((node->flags & TREE_NODE_EXPANDED) == false) {
		/* What madness is this? */
		LOG(("Tried to contract a contracted node."));
		return NSERROR_OK;
	}

	/* Contract children. */
	treeview_walk_internal(node, false, treeview_node_contract_cb, NULL);

	/* Contract node */
	treeview_node_contract_cb(node, NULL);

	return NSERROR_OK;
}

/**
 * Redraws a treeview.
 *
 * \param tree		the tree to draw
 * \param x		X coordinate to draw the tree at (wrt plot origin)
 * \param y		Y coordinate to draw the tree at (wrt plot origin)
 * \param clip_x	clipping rectangle (wrt tree origin)
 * \param ctx		current redraw context
 */
void treeview_redraw(struct treeview *tree, int x, int y, struct rect *clip,
		const struct redraw_context *ctx)
{
	struct redraw_context new_ctx = *ctx;
	struct treeview_node *node, *root, *next;
	struct treeview_node_entry *entry;
	struct treeview_node_style *style = &plot_style_odd;
	struct content_redraw_data data;
	struct rect r;
	uint32_t count = 0;
	int render_y = y;
	int inset;
	int x0, y0, y1;
	int baseline = (tree_g.line_height * 3 + 2) / 4;
	enum treeview_resource_id res = TREE_RES_CONTENT;
	plot_style_t *bg_style;
	plot_font_style_t *text_style;
	plot_font_style_t *infotext_style;
	int height;

	assert(tree != NULL);
	assert(tree->root != NULL);
	assert(tree->root->flags & TREE_NODE_EXPANDED);

	/* Start knockout rendering if it's available for this plotter */
	if (ctx->plot->option_knockout)
		knockout_plot_start(ctx, &new_ctx);

	/* Set up clip rectangle */
	r.x0 = clip->x0 + x;
	r.y0 = clip->y0 + y;
	r.x1 = clip->x1 + x;
	r.y1 = clip->y1 + y;
	new_ctx.plot->clip(&r);

	/* Draw the tree */
	node = root = tree->root;

	/* Setup common content redraw data */
	data.width = 17;
	data.height = 17;
	data.scale = 1;
	data.repeat_x = false;
	data.repeat_y = false;

	while (node != NULL) {
		int i;
		next = (node->flags & TREE_NODE_EXPANDED) ?
				node->children : NULL;

		if (next != NULL) {
			/* down to children */
			node = next;
		} else {
			/* No children.  As long as we're not at the root,
			 * go to next sibling if present, or nearest ancestor
			 * with a next sibling. */

			while (node != root &&
					node->sibling_next == NULL) {
				node = node->parent;
			}

			if (node == root)
				break;

			node = node->sibling_next;
		}

		assert(node != NULL);
		assert(node != root);
		assert(node->type == TREE_NODE_FOLDER ||
				node->type == TREE_NODE_ENTRY);

		count++;
		inset = node->inset;
		height = (node->type == TREE_NODE_ENTRY) ? node->height :
				tree_g.line_height;

		if ((render_y + height) < r.y0) {
			/* This node's line is above clip region */
			render_y += height;
			continue;
		}

		style = (count & 0x1) ? &plot_style_odd : &plot_style_even;
		if (node->flags & TREE_NODE_SELECTED) {
			bg_style = &style->sbg;
			text_style = &style->stext;
			infotext_style = &style->sitext;
		} else {
			bg_style = &style->bg;
			text_style = &style->text;
			infotext_style = &style->itext;
		}

		/* Render background */
		y0 = render_y;
		y1 = render_y + height;
		new_ctx.plot->rectangle(r.x0, y0, r.x1, y1, bg_style);

		/* Render toggle */
		if (node->flags & TREE_NODE_EXPANDED) {
			new_ctx.plot->text(inset, render_y + baseline,
					treeview_furn[TREE_FURN_CONTRACT].data,
					treeview_furn[TREE_FURN_CONTRACT].len,
					text_style);
		} else {
			new_ctx.plot->text(inset, render_y + baseline,
					treeview_furn[TREE_FURN_EXPAND].data,
					treeview_furn[TREE_FURN_EXPAND].len,
					text_style);
		}

		/* Render icon */
		if (node->type == TREE_NODE_ENTRY)
			res = TREE_RES_CONTENT;
		else if (node->type == TREE_NODE_FOLDER)
			res = TREE_RES_FOLDER;

		if (treeview_res[res].ready) {
			/* Icon resource is available */
			data.x = inset + tree_g.step_width;
			data.y = render_y + ((tree_g.line_height -
					treeview_res[res].height + 1) / 2);
			data.background_colour = bg_style->fill_colour;

			content_redraw(treeview_res[res].c,
					&data, &r, &new_ctx);
		}

		/* Render text */
		x0 = inset + tree_g.step_width + tree_g.icon_step;
		new_ctx.plot->text(x0, render_y + baseline,
				node->text.value.data, node->text.value.len,
				text_style);

		/* Rendered the node */
		render_y += tree_g.line_height;
		if (render_y > r.y1) {
			/* Passed the bottom of what's in the clip region.
			 * Done. */
			break;
		}


		if (node->type != TREE_NODE_ENTRY ||
				!(node->flags & TREE_NODE_EXPANDED))
			/* Done everything for this node */
			continue;

		/* Render expanded entry fields */
		entry = (struct treeview_node_entry *)node;
		for (i = 0; i < tree->n_fields - 1; i++) {
			struct treeview_field *ef = &(tree->fields[i + 1]);

			if (ef->flags & TREE_FLAG_SHOW_NAME) {
				int max_width = tree->field_width;

				new_ctx.plot->text(x0 + max_width -
							ef->value.width -
							tree_g.step_width,
						render_y + baseline,
						ef->value.data,
						ef->value.len,
						infotext_style);

				new_ctx.plot->text(x0 + max_width,
						render_y + baseline,
						entry->fields[i].value.data,
						entry->fields[i].value.len,
						infotext_style);
			} else {
				new_ctx.plot->text(x0, render_y + baseline,
						entry->fields[i].value.data,
						entry->fields[i].value.len,
						infotext_style);

			}

			/* Rendered the expanded entry field */
			render_y += tree_g.line_height;
		}

		/* Finshed rendering expanded entry */

		if (render_y > r.y1) {
			/* Passed the bottom of what's in the clip region.
			 * Done. */
			break;
		}
	}

	if (render_y < r.y1) {
		/* Fill the blank area at the bottom */
		y0 = render_y;
		new_ctx.plot->rectangle(r.x0, y0, r.x1, r.y1,
				&plot_style_even.bg);
		
	}

	/* Rendering complete */
	if (ctx->plot->option_knockout)
		knockout_plot_end();
}

struct treeview_selection_walk_data {
	enum {
		TREEVIEW_WALK_HAS_SELECTION,
		TREEVIEW_WALK_CLEAR_SELECTION,
		TREEVIEW_WALK_SELECT_ALL
	} purpose;
	union {
		bool has_selection;
		struct {
			bool required;
			struct rect *rect;
		} redraw;
	} data;
	int current_y;
};
static bool treeview_node_selection_walk_cb(struct treeview_node *node,
		void *ctx)
{
	struct treeview_selection_walk_data *sw = ctx;
	int height;
	bool changed = false;

	height = (node->type == TREE_NODE_ENTRY) ? node->height :
			tree_g.line_height;
	sw->current_y += height;

	switch (sw->purpose) {
	case TREEVIEW_WALK_HAS_SELECTION:
		if (node->flags & TREE_NODE_SELECTED) {
			sw->data.has_selection = true;
			return true; /* Can abort tree walk */
		}
		break;

	case TREEVIEW_WALK_CLEAR_SELECTION:
		if (node->flags & TREE_NODE_SELECTED) {
			node->flags ^= TREE_NODE_SELECTED;
			changed = true;
		}
		break;

	case TREEVIEW_WALK_SELECT_ALL:
		if (!(node->flags & TREE_NODE_SELECTED)) {
			node->flags ^= TREE_NODE_SELECTED;
			changed = true;
		}
		break;
	}

	if (changed) {
		if (sw->data.redraw.required == false) {
			sw->data.redraw.required = true;
			sw->data.redraw.rect->y0 = sw->current_y - height;
		}

		if (sw->current_y > sw->data.redraw.rect->y1) {
			sw->data.redraw.rect->y1 = sw->current_y;
		}
	}

	return false; /* Don't stop walk */
}

bool treeview_has_selection(struct treeview *tree)
{
	struct treeview_selection_walk_data sw;

	sw.purpose = TREEVIEW_WALK_HAS_SELECTION;
	sw.data.has_selection = false;

	treeview_walk_internal(tree->root, false,
			treeview_node_selection_walk_cb, &sw);

	return sw.data.has_selection;
}

bool treeview_clear_selection(struct treeview *tree, struct rect *rect)
{
	struct treeview_selection_walk_data sw;

	rect->x0 = 0;
	rect->y0 = 0;
	rect->x1 = REDRAW_MAX;
	rect->y1 = 0;

	sw.purpose = TREEVIEW_WALK_CLEAR_SELECTION;
	sw.data.redraw.required = false;
	sw.data.redraw.rect = rect;
	sw.current_y = 0;

	treeview_walk_internal(tree->root, false,
			treeview_node_selection_walk_cb, &sw);

	return sw.data.redraw.required;
}

bool treeview_select_all(struct treeview *tree, struct rect *rect)
{
	struct treeview_selection_walk_data sw;

	rect->x0 = 0;
	rect->y0 = 0;
	rect->x1 = REDRAW_MAX;
	rect->y1 = 0;

	sw.purpose = TREEVIEW_WALK_SELECT_ALL;
	sw.data.redraw.required = false;
	sw.data.redraw.rect = rect;
	sw.current_y = 0;

	treeview_walk_internal(tree->root, false,
			treeview_node_selection_walk_cb, &sw);

	return sw.data.redraw.required;
}

struct treeview_mouse_action {
	struct treeview *tree;
	browser_mouse_state mouse;
	int x;
	int y;
	int current_y;
};
static bool treeview_node_mouse_action_cb(struct treeview_node *node, void *ctx)
{
	struct treeview_mouse_action *ma = ctx;
	struct rect r;
	bool redraw = false;
	bool click;
	int height;
	enum {
		TV_NODE_ACTION_NONE		= 0,
		TV_NODE_ACTION_SELECTION	= (1 << 0)
	} action = TV_NODE_ACTION_NONE;
	enum {
		TV_NODE_SECTION_TOGGLE,
		TV_NODE_SECTION_NODE
	} section = TV_NODE_SECTION_NODE;
	nserror err;

	r.x0 = 0;
	r.x1 = REDRAW_MAX;

	height = (node->type == TREE_NODE_ENTRY) ? node->height :
			tree_g.line_height;

	/* Skip line if we've not reached mouse y */
	if (ma->y > ma->current_y + height) {
		ma->current_y += height;
		return false; /* Don't want to abort tree walk */
	}

	/* Find where the mouse is */
	if (ma->x >= node->inset - 1 &&
			ma->x < node->inset + tree_g.step_width) {
		section = TV_NODE_SECTION_TOGGLE;
	}

	click = ma->mouse & (BROWSER_MOUSE_CLICK_1 | BROWSER_MOUSE_CLICK_2);

	if (((node->type == TREE_NODE_FOLDER) &&
			(ma->mouse & BROWSER_MOUSE_DOUBLE_CLICK) && click) ||
			(section == TV_NODE_SECTION_TOGGLE && click)) {
		/* Clear any existing selection */
		redraw |= treeview_clear_selection(ma->tree, &r);

		/* Toggle node expansion */
		if (node->flags & TREE_NODE_EXPANDED) {
			err = treeview_node_contract(ma->tree, node);
		} else {
			err = treeview_node_expand(ma->tree, node);
		}

		/* Set up redraw */
		redraw = true;
		if (r.y0 > ma->current_y)
			r.y0 = ma->current_y;
		r.y1 = REDRAW_MAX;

	} else if ((node->type == TREE_NODE_ENTRY) &&
			(ma->mouse & BROWSER_MOUSE_DOUBLE_CLICK) && click) {
		struct treeview_node_msg msg;
		msg.msg = TREE_MSG_NODE_LAUNCH;
		msg.data.node_launch.mouse = ma->mouse;

		/* Clear any existing selection */
		redraw |= treeview_clear_selection(ma->tree, &r);

		/* Tell client an entry was launched */
		tree->callbacks->entry(msg, n->client_data);

	} else if (ma->mouse & BROWSER_MOUSE_PRESS_1 &&
			!(node->flags & TREE_NODE_SELECTED) &&
			section != TV_NODE_SECTION_TOGGLE) {
		/* Clear any existing selection */
		redraw |= treeview_clear_selection(ma->tree, &r);

		/* Select node */
		action |= TV_NODE_ACTION_SELECTION;

	} else if (ma->mouse & BROWSER_MOUSE_PRESS_2 ||
			(ma->mouse & BROWSER_MOUSE_PRESS_1 &&
			 ma->mouse & BROWSER_MOUSE_MOD_2)) {
		/* Toggle selection of node */
		action |= TV_NODE_ACTION_SELECTION;
	}

	if (action & TV_NODE_ACTION_SELECTION) {
		/* Handle change in selection */
		node->flags ^= TREE_NODE_SELECTED;

		/* Redraw */
		if (!redraw) {
			r.y0 = ma->current_y;
			r.y1 = ma->current_y + height;
			redraw = true;
		} else {
			if (r.y0 > ma->current_y)
				r.y0 = ma->current_y;
			if (r.y1 < ma->current_y + height)
				r.y1 = ma->current_y + height;
		}
	}

	if (redraw) {
		ma->tree->cw_t->redraw_request(ma->tree->cw_h, r);
	}

	return true; /* Reached line with click; stop walking tree */
}
void treeview_mouse_action(struct treeview *tree,
		browser_mouse_state mouse, int x, int y)
{
	struct treeview_mouse_action ma;

	ma.tree = tree;
	ma.mouse = mouse;
	ma.x = x;
	ma.y = y;
	ma.current_y = 0;

	treeview_walk_internal(tree->root, false,
			treeview_node_mouse_action_cb, &ma);
}



/* Mix two colours according to the proportion given by p.
 * Where 0 <= p <= 255
 * p=0   gives result=c0
 * p=255 gives result=c1
 */
#define mix_colour(c0, c1, p)						\
	((((((c1 & 0xff00ff) * (255 - p)) +				\
	    ((c0 & 0xff00ff) * (      p))   ) >> 8) & 0xff00ff) |	\
	 (((((c1 & 0x00ff00) * (255 - p)) +				\
	    ((c0 & 0x00ff00) * (      p))   ) >> 8) & 0x00ff00))


static void treeview_init_plot_styles(int font_pt_size)
{
	/* Background colour */
	plot_style_even.bg.stroke_type = PLOT_OP_TYPE_NONE;
	plot_style_even.bg.stroke_width = 0;
	plot_style_even.bg.stroke_colour = 0;
	plot_style_even.bg.fill_type = PLOT_OP_TYPE_SOLID;
	plot_style_even.bg.fill_colour = gui_system_colour_char("Window");

	/* Text colour */
	plot_style_even.text.family = PLOT_FONT_FAMILY_SANS_SERIF;
	plot_style_even.text.size = font_pt_size * FONT_SIZE_SCALE;
	plot_style_even.text.weight = 400;
	plot_style_even.text.flags = FONTF_NONE;
	plot_style_even.text.foreground = gui_system_colour_char("WindowText");
	plot_style_even.text.background = gui_system_colour_char("Window");

	/* Entry field text colour */
	plot_style_even.itext = plot_style_even.text;
	plot_style_even.itext.foreground = mix_colour(
			plot_style_even.text.foreground,
			plot_style_even.text.background, 255 * 10 / 16);

	/* Selected background colour */
	plot_style_even.sbg = plot_style_even.bg;
	plot_style_even.sbg.fill_colour = gui_system_colour_char("Highlight");

	/* Selected text colour */
	plot_style_even.stext = plot_style_even.text;
	plot_style_even.stext.foreground =
			gui_system_colour_char("HighlightText");
	plot_style_even.stext.background = gui_system_colour_char("Highlight");

	/* Selected entry field text colour */
	plot_style_even.sitext = plot_style_even.stext;
	plot_style_even.sitext.foreground = mix_colour(
			plot_style_even.stext.foreground,
			plot_style_even.stext.background, 255 * 25 / 32);


	/* Odd numbered node styles */
	plot_style_odd.bg = plot_style_even.bg;
	plot_style_odd.bg.fill_colour = mix_colour(
			plot_style_even.bg.fill_colour,
			plot_style_even.text.foreground, 255 * 15 / 16);
	plot_style_odd.text = plot_style_even.text;
	plot_style_odd.text.background = plot_style_odd.bg.fill_colour;
	plot_style_odd.itext = plot_style_odd.text;
	plot_style_odd.itext.foreground = mix_colour(
			plot_style_odd.text.foreground,
			plot_style_odd.text.background, 255 * 10 / 16);

	plot_style_odd.sbg = plot_style_even.sbg;
	plot_style_odd.stext = plot_style_even.stext;
	plot_style_odd.sitext = plot_style_even.sitext;
}


/**
 * Callback for hlcache.
 */
static nserror treeview_res_cb(hlcache_handle *handle,
		const hlcache_event *event, void *pw)
{
	struct treeview_resource *r = pw;

	switch (event->type) {
	case CONTENT_MSG_READY:
	case CONTENT_MSG_DONE:
		r->ready = true;
		r->height = content_get_height(handle);
		break;

	default:
		break;
	}

	return NSERROR_OK;
}


static void treeview_init_resources(void)
{
	int i;

	for (i = 0; i < TREE_RES_LAST; i++) {
		nsurl *url;
		if (nsurl_create(treeview_res[i].url, &url) == NSERROR_OK) {
			hlcache_handle_retrieve(url, 0, NULL, NULL,
					treeview_res_cb,
					&(treeview_res[i]), NULL,
					CONTENT_IMAGE, &(treeview_res[i].c));
			nsurl_unref(url);
		}
	}
}


static void treeview_init_furniture(void)
{
	int i;
	tree_g.furniture_width = 0;

	for (i = 0; i < TREE_FURN_LAST; i++) {
		nsfont.font_width(&plot_style_odd.text,
				treeview_furn[i].data,
				treeview_furn[i].len,
				&(treeview_furn[i].width));

		if (treeview_furn[i].width > tree_g.furniture_width)
			tree_g.furniture_width = treeview_furn[i].width;
	}

	tree_g.furniture_width += 5;
}


nserror treeview_init(void)
{
	int font_px_size;
	int font_pt_size = 11;

	treeview_init_plot_styles(font_pt_size);
	treeview_init_resources();
	treeview_init_furniture();

	font_px_size = (font_pt_size * FIXTOINT(nscss_screen_dpi) + 36) / 72;

	tree_g.line_height = (font_px_size * 8 + 3) / 6;
	tree_g.step_width = tree_g.furniture_width;
	tree_g.window_padding = 6;
	tree_g.icon_step = 23;

	return NSERROR_OK;
}


nserror treeview_fini(void)
{
	int i;

	for (i = 0; i < TREE_RES_LAST; i++) {
		hlcache_handle_release(treeview_res[i].c);
	}

	return NSERROR_OK;
}


struct treeview_node * treeview_get_root(struct treeview *tree)
{
	assert(tree != NULL);
	assert(tree->root != NULL);

	return tree->root;
}