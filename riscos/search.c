/*
 * This file is part of NetSurf, http://netsurf.sourceforge.net/
 * Licensed under the GNU General Public License,
 *                http://www.opensource.org/licenses/gpl-license
 * Copyright 2004 John M Bell <jmb202@ecs.soton.ac.uk>
 * Copyright 2005 Adrian Lees <adrianl@users.sourceforge.net> 
 */

/** \file
 * Free text search (implementation)
 */

#include <ctype.h>
#include <string.h>

#include "oslib/hourglass.h"
#include "oslib/wimp.h"

#include "netsurf/utils/config.h"
#include "netsurf/content/content.h"
#include "netsurf/desktop/browser.h"
#include "netsurf/desktop/gui.h"
#include "netsurf/desktop/selection.h"
#include "netsurf/render/box.h"
#include "netsurf/render/html.h"
#include "netsurf/riscos/gui.h"
#include "netsurf/riscos/menus.h"
#include "netsurf/riscos/wimp.h"
#include "netsurf/utils/log.h"
#include "netsurf/utils/messages.h"
#include "netsurf/utils/utils.h"


#ifdef WITH_SEARCH

#ifndef NOF_ELEMENTS
#define NOF_ELEMENTS(array) (sizeof(array)/sizeof(*(array)))
#endif

struct list_entry {
	/* start position of match */
	struct box *start_box;
	unsigned start_idx;
	/* end of match */
	struct box *end_box;
	unsigned end_idx;

	struct list_entry *prev;
	struct list_entry *next;
};

struct gui_window *search_current_window = 0;
static struct selection *search_selection = 0;

static char *search_string = 0;
static struct list_entry search_head = { 0, 0, 0, 0, 0, 0 };
static struct list_entry *search_found = &search_head;
static struct list_entry *search_current = 0;
static struct content *search_content = 0;
static bool search_prev_case_sens = false;

static void start_search(bool forwards);
static void do_search(char *string, int string_len, bool case_sens, bool forwards);
static const char *find_pattern(const char *string, int s_len,
		const char *pattern, int p_len, bool case_sens, int *m_len);
static bool find_occurrences(const char *pattern, int p_len, struct box *cur,
		bool case_sens);
static void show_status(bool found);


/**
 * Open the search dialog
 *
 * \param g the gui window to search
 */
void ro_gui_search_prepare(struct gui_window *g)
{
	struct content *c;

	assert(g != NULL);

	/* if the search dialogue is reopened over a new window, we still
	   need to cancel the previous search */
	ro_gui_search_end();

	search_current_window = g;

	ro_gui_set_icon_string(dialog_search, ICON_SEARCH_TEXT, "");
	ro_gui_set_icon_selected_state(dialog_search,
				ICON_SEARCH_CASE_SENSITIVE, false);

	c = search_current_window->bw->current_content;

	if (!c)
		return;

	/* only handle html contents */
	if (c->type != CONTENT_HTML)
		return;

	show_status(true);
	ro_gui_set_icon_shaded_state(dialog_search, ICON_SEARCH_FIND_PREV, true);
	ro_gui_set_icon_shaded_state(dialog_search, ICON_SEARCH_FIND_NEXT, true);

/* \todo build me properly please! */
	search_selection = selection_create(g->bw);
	if (!search_selection)
		warn_user("NoMemory", 0);

	selection_init(search_selection, c->data.html.layout);
}

/**
 * Handle clicks in the search dialog
 *
 * \param pointer wimp_pointer block
 * \param parent The parent window of this persistent dialog
 */
void ro_gui_search_click(wimp_pointer *pointer)
{
	if (pointer->buttons == wimp_CLICK_MENU)
		return;

	switch(pointer->i) {
		case ICON_SEARCH_FIND_PREV:
			start_search(false);
			break;
		case ICON_SEARCH_FIND_NEXT:
			start_search(true);
			break;
		case ICON_SEARCH_CANCEL:
			/* cancel the search operation */
			ro_gui_search_end();
			/* and close the window */
			ro_gui_menu_closed();
			ro_gui_dialog_close(dialog_search);
			break;
	}
}

/**
 * Handle keypresses in the search dialog
 *
 * \param key wimp_key block
 * \return true if keypress handled, false otherwise
 */
bool ro_gui_search_keypress(wimp_key *key)
{
	bool state;

	switch (key->c) {
		case 9: /* ctrl i */
			state = ro_gui_get_icon_selected_state(dialog_search, ICON_SEARCH_CASE_SENSITIVE);
			ro_gui_set_icon_selected_state(dialog_search, ICON_SEARCH_CASE_SENSITIVE, !state);
			return true;
		case wimp_KEY_RETURN:
			start_search(true);
			return true;
		case wimp_KEY_ESCAPE:
			/* cancel the search operation */
			ro_gui_search_end();
			/* and close the window */
			ro_gui_menu_closed();
			ro_gui_dialog_close(dialog_search);
			return true;
		case wimp_KEY_UP:
			start_search(false);
			return true;
		case wimp_KEY_DOWN:
			start_search(true);
			return true;

		default:
			if (key->c == 8  || /* backspace */
			    key->c == 21 || /* ctrl u */
			    (key->c >= 0x20 && key->c <= 0x7f)) {
				start_search(true);
				return true;
			}
			break;
	}

	return false;
}

/**
 * Begins/continues the search process
 * Note that this may be called many times for a single search.
 *
 * \param  forwards  search forwards from start/current position
 */

void start_search(bool forwards)
{
	int string_len;
	char *string;

	string = ro_gui_get_icon_string(dialog_search, ICON_SEARCH_TEXT);
	assert(string);

	string_len = strlen(string);
	if (string_len <= 0) {
		show_status(true);
		ro_gui_set_icon_shaded_state(dialog_search, ICON_SEARCH_FIND_PREV, true);
		ro_gui_set_icon_shaded_state(dialog_search, ICON_SEARCH_FIND_NEXT, true);
		gui_window_set_scroll(search_current_window, 0, 0);
		return;
	}

	do_search(string, string_len,
		ro_gui_get_icon_selected_state(dialog_search,
						ICON_SEARCH_CASE_SENSITIVE),
		forwards);
}

/**
 * Ends the search process, invalidating all global state and
 * freeing the list of found boxes
 */
void ro_gui_search_end(void)
{
	struct list_entry *a, *b;

	if (search_selection) {
		selection_clear(search_selection, true);
		selection_destroy(search_selection);
	}
	search_selection = 0;

	search_current_window = 0;

	if (search_string)
		free(search_string);
	search_string = 0;

	for (a = search_found->next; a; a = b) {
		b = a->next;
		free(a);
	}
	search_found->prev = 0;
	search_found->next = 0;

	search_current = 0;

	search_content = 0;

	search_prev_case_sens = false;
}

/**
 * Search for a string in the box tree
 *
 * \param string the string to search for
 * \param string_len length of search string
 * \param case_sens whether to perform a case sensitive search
 * \param forwards direction to search in
 */
void do_search(char *string, int string_len, bool case_sens, bool forwards)
{
	struct content *c;
	struct box *box;
	struct list_entry *a, *b;
	int x,y;
	bool new = false;

	if (!search_current_window)
		return;

	c = search_current_window->bw->current_content;

	if (!c)
		return;

	/* only handle html contents */
	if (c->type != CONTENT_HTML)
		return;

	box = c->data.html.layout;

	if (!box)
		return;

//	LOG(("'%s' - '%s' (%p, %p) %p (%d, %d) %d", search_string, string, search_content, c, search_found->next, search_prev_case_sens, case_sens, forwards));

	selection_clear(search_selection, true);

	/* check if we need to start a new search or continue an old one */
	if (!search_string || c != search_content || !search_found->next ||
	    search_prev_case_sens != case_sens ||
	    (case_sens && strcmp(string, search_string) != 0) ||
	    (!case_sens && strcasecmp(string, search_string) != 0)) {

		if (search_string)
			free(search_string);
		search_current = 0;
		for (a = search_found->next; a; a = b) {
			b = a->next;
			free(a);
		}
		search_found->prev = 0;
		search_found->next = 0;

		search_string = strdup(string);

		xhourglass_on();
		if (!find_occurrences(string, string_len, box, case_sens)) {
			for (a = search_found->next; a; a = b) {
				b = a->next;
				free(a);
			}
			search_found->prev = 0;
			search_found->next = 0;

			xhourglass_off();
			return;
		}
		xhourglass_off();

		new = true;
		search_content = c;
		search_prev_case_sens = case_sens;
	}

//	LOG(("%d %p %p (%p, %p)", new, search_found->next, search_current, search_current->prev, search_current->next));

	if (new) {
		/* new search, beginning at the top of the page */
		search_current = search_found->next;
	}
	else if (search_current) {
		/* continued search in the direction specified */
		if (forwards) {
			if (search_current->next)
				search_current = search_current->next;
		}
		else {
			if (search_current->prev)
				search_current = search_current->prev;
		}
	}

	show_status(search_current != NULL);

	ro_gui_set_icon_shaded_state(dialog_search, ICON_SEARCH_FIND_PREV,
		!search_current || !search_current->prev);
	ro_gui_set_icon_shaded_state(dialog_search, ICON_SEARCH_FIND_NEXT,
		!search_current || !search_current->next);

	if (!search_current)
		return;

	selection_set_start(search_selection, search_current->start_box,
		search_current->start_idx);
	selection_set_end(search_selection, search_current->end_box,
		search_current->end_idx);

	/* get box position and jump to it */
	box_coords(search_current->start_box, &x, &y);
//	LOG(("%p (%d, %d)", search_current, x, y));
	gui_window_set_scroll(search_current_window, x, y);

}


/**
 * Find the first occurrence of 'match' in 'string' and return its index
 *
 * /param  string     the string to be searched (unterminated)
 * /param  s_len      length of the string to be searched
 * /param  pattern    the pattern for which we are searching (unterminated)
 * /param  p_len      length of pattern
 * /param  case_sens  true iff case sensitive match required
 * /param  m_len      accepts length of match in bytes
 * /return pointer to first match, NULL if none
 */

const char *find_pattern(const char *string, int s_len, const char *pattern,
		int p_len, bool case_sens, int *m_len)
{
	struct { const char *ss, *s, *p; bool first; } context[16];
	const char *ep = pattern + p_len;
	const char *es = string  + s_len;
	const char *p = pattern - 1;  /* a virtual '*' before the pattern */
	const char *ss = string;
	const char *s = string;
	bool first = true;
	int top = 0;

	while (p < ep) {
		bool matches;
		if (p < pattern || *p == '*') {
			char ch;

			/* skip any further asterisks; one is the same as many */
			do p++; while (p < ep && *p == '*');

			/* if we're at the end of the pattern, yes, it matches */
			if (p >= ep) break;

			/* anything matches a # so continue matching from
			   here, and stack a context that will try to match
			   the wildcard against the next character */
			
			ch = *p;
			if (ch != '#') {
				/* scan forwards until we find a match for this char */
				if (!case_sens) ch = toupper(ch);
				while (s < es) {
					if (case_sens) {
						if (*s == ch) break;
					} else if (toupper(*s) == ch)
						break;
					s++;
				}
			}

			if (s < es) {
				/* remember where we are in case the match fails;
					we can then resume */
				if (top < (int)NOF_ELEMENTS(context)) {
					context[top].ss = ss;
					context[top].s  = s + 1;
					context[top].p  = p - 1;    /* ptr to last asterisk */
					context[top].first = first;
					top++;
				}

				if (first) {
					ss = s;  /* remember first non-'*' char */
					first = false;
				}

				matches = true;
			}
			else
				matches = false;
		}
		else if (s < es) {
			char ch = *p;
			if (ch == '#')
				matches = true;
			else {
				if (case_sens)
					matches = (*s == ch);
				else
					matches = (toupper(*s) == toupper(ch));
			}
			if (matches && first) {
				ss = s;  /* remember first non-'*' char */
				first = false;
			}
		}
		else
			matches = false;

		if (matches) {
			p++; s++;
		}
		else {
			/* doesn't match, resume with stacked context if we have one */
			if (--top < 0) return NULL;  /* no match, give up */
			
			ss = context[top].ss;
			s  = context[top].s;
			p  = context[top].p;
			first = context[top].first;
		}
	}

	/* end of pattern reached */
	*m_len = s - ss;
	return ss;
}


/**
 * Finds all occurrences of a given string in the box tree
 *
 * \param pattern   the string pattern to search for
 * \param p_len     pattern length
 * \param cur       pointer to the current box
 * \param case_sens whether to perform a case sensitive search
 * \return true on success, false on memory allocation failure
 */
bool find_occurrences(const char *pattern, int p_len, struct box *cur,
		bool case_sens)
{
	struct box *a;

	/* ignore this box, if there's no visible text */
	if (!cur->object && cur->text) {
		const char *text = cur->text;
		unsigned length = cur->length;

		while (length > 0) {
			unsigned match_length;
			unsigned match_offset;
			const char *new_text;
			struct list_entry *entry;
			const char *pos = find_pattern(text, length,
					pattern, p_len, case_sens, &match_length);
			if (!pos) break;

			/* found string in box => add to list */
			entry = calloc(1, sizeof(*entry));
			if (!entry) {
				warn_user("NoMemory", 0);
				return false;
			}
	
			match_offset = pos - cur->text;
	
			entry->start_box = cur;
			entry->start_idx = match_offset;
			entry->end_box = cur;
			entry->end_idx = match_offset + match_length;
			entry->next = 0;
			entry->prev = search_found->prev;
			if (!search_found->prev)
				search_found->next = entry;
			else
				search_found->prev->next = entry;
			search_found->prev = entry;

			new_text = pos + match_length;
			length -= (new_text - text);
			text = new_text;
		}
	}

	/* and recurse */
	for (a = cur->children; a; a = a->next) {
		if (!find_occurrences(pattern, p_len, a, case_sens))
			return false;
	}

	return true;
}



/**
 * Determines whether any portion of the given text box should be
 * selected because it matches the current search string.
 *
 * \param  g          gui window
 * \param  box        box being tested
 * \param  start_idx  byte offset within text box of highlight start
 * \param  end_idx    byte offset of highlight end
 * \return true iff part of the box should be highlighted
 */

bool gui_search_term_highlighted(struct gui_window *g, struct box *box,
		unsigned *start_idx, unsigned *end_idx) 
{
	if (g == search_current_window && search_selection) {
		if (selection_defined(search_selection))
			return selection_highlighted(search_selection, box,
					start_idx, end_idx);
	}
	return false;
}


/**
 * Change the displayed search status.
 *
 * \param found  search pattern matched in text
 */

void show_status(bool found)
{
	ro_gui_set_icon_string(dialog_search, ICON_SEARCH_STATUS,
			found ? "" : messages_get("Notfound"));
}

#endif
