/*
 * gtkdlg.c - GTK implementation of the PuTTY configuration box.
 */

/*
 * TODO:
 * 
 *  - colour selection woe: what to do about GTK's colour selector
 *    not allowing us full resolution in our own colour selections?
 *    Perhaps making the colour resolution per-platform, at least
 *    at the config level, is actually the least unpleasant
 *    alternative.
 * 
 *  - cosmetics:
 *     + can't we _somehow_ have less leading between radio buttons?
 *     + wrapping text widgets, the horror, the horror
 *     + labels and their associated edit boxes don't line up
 *       properly
 *     + don't suppose we can fix the vertical offset labels get
 *       from their underlines?
 *     + apparently Left/Right on the treeview should be expanding
 *       and collapsing branches.
 *     + why the hell are the Up/Down focus movement keys sorting
 *       things by _width_? (See the Logging and Features panels
 *       for good examples.)
 *     + window title.
 */

/*
 * TODO when porting to GTK 2.0:
 * 
 *  - GtkTree is apparently deprecated and we should switch to
 *    GtkTreeView instead.
 *  - GtkLabel has a built-in mnemonic scheme, so we should at
 *    least consider switching to that from the current adhockery.
 */

#include <assert.h>
#include <ctype.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "gtkcols.h"
#include "gtkpanel.h"

#ifdef TESTMODE
#define PUTTY_DO_GLOBALS	       /* actually _define_ globals */
#endif

#include "putty.h"
#include "dialog.h"
#include "tree234.h"

struct Shortcut {
    GtkWidget *widget;
    struct uctrl *uc;
    int action;
};

struct Shortcuts {
    struct Shortcut sc[128];
};

struct uctrl {
    union control *ctrl;
    GtkWidget *toplevel;
    void *privdata;
    int privdata_needs_free;
    GtkWidget **buttons; int nbuttons; /* for radio buttons */
    GtkWidget *entry;         /* for editbox, combobox, filesel, fontsel */
    GtkWidget *button;        /* for filesel, fontsel */
    GtkWidget *list;	      /* for combobox, listbox */
    GtkWidget *menu;	      /* for optionmenu (==droplist) */
    GtkWidget *optmenu;	      /* also for optionmenu */
    GtkWidget *text;	      /* for text */
    GtkAdjustment *adj;       /* for the scrollbar in a list box */
};

struct dlgparam {
    tree234 *byctrl, *bywidget;
    void *data;
    struct { unsigned char r, g, b, ok; } coloursel_result;   /* 0-255 */
    /* `flags' are set to indicate when a GTK signal handler is being called
     * due to automatic processing and should not flag a user event. */
    int flags;
    struct Shortcuts *shortcuts;
    GtkWidget *window, *cancelbutton, *currtreeitem, **treeitems;
    union control *currfocus, *lastfocus;
    int ntreeitems;
    int retval;
};
#define FLAG_UPDATING_COMBO_LIST 1

enum {				       /* values for Shortcut.action */
    SHORTCUT_EMPTY,		       /* no shortcut on this key */
    SHORTCUT_FOCUS,		       /* focus the supplied widget */
    SHORTCUT_UCTRL,		       /* do something sane with uctrl */
    SHORTCUT_UCTRL_UP,		       /* uctrl is a draglist, move Up */
    SHORTCUT_UCTRL_DOWN,	       /* uctrl is a draglist, move Down */
};

/*
 * Forward references.
 */
static gboolean widget_focus(GtkWidget *widget, GdkEventFocus *event,
                             gpointer data);
static void shortcut_add(struct Shortcuts *scs, GtkWidget *labelw,
			 int chr, int action, void *ptr);
static int listitem_single_key(GtkWidget *item, GdkEventKey *event,
                               gpointer data);
static int listitem_multi_key(GtkWidget *item, GdkEventKey *event,
                                 gpointer data);
static int listitem_button(GtkWidget *item, GdkEventButton *event,
			    gpointer data);
static void menuitem_activate(GtkMenuItem *item, gpointer data);
static void coloursel_ok(GtkButton *button, gpointer data);
static void coloursel_cancel(GtkButton *button, gpointer data);
static void window_destroy(GtkWidget *widget, gpointer data);

static int uctrl_cmp_byctrl(void *av, void *bv)
{
    struct uctrl *a = (struct uctrl *)av;
    struct uctrl *b = (struct uctrl *)bv;
    if (a->ctrl < b->ctrl)
	return -1;
    else if (a->ctrl > b->ctrl)
	return +1;
    return 0;
}

static int uctrl_cmp_byctrl_find(void *av, void *bv)
{
    union control *a = (union control *)av;
    struct uctrl *b = (struct uctrl *)bv;
    if (a < b->ctrl)
	return -1;
    else if (a > b->ctrl)
	return +1;
    return 0;
}

static int uctrl_cmp_bywidget(void *av, void *bv)
{
    struct uctrl *a = (struct uctrl *)av;
    struct uctrl *b = (struct uctrl *)bv;
    if (a->toplevel < b->toplevel)
	return -1;
    else if (a->toplevel > b->toplevel)
	return +1;
    return 0;
}

static int uctrl_cmp_bywidget_find(void *av, void *bv)
{
    GtkWidget *a = (GtkWidget *)av;
    struct uctrl *b = (struct uctrl *)bv;
    if (a < b->toplevel)
	return -1;
    else if (a > b->toplevel)
	return +1;
    return 0;
}

static void dlg_init(struct dlgparam *dp)
{
    dp->byctrl = newtree234(uctrl_cmp_byctrl);
    dp->bywidget = newtree234(uctrl_cmp_bywidget);
    dp->coloursel_result.ok = FALSE;
}

static void dlg_cleanup(struct dlgparam *dp)
{
    struct uctrl *uc;

    freetree234(dp->byctrl);	       /* doesn't free the uctrls inside */
    while ( (uc = index234(dp->bywidget, 0)) != NULL) {
	del234(dp->bywidget, uc);
	if (uc->privdata_needs_free)
	    sfree(uc->privdata);
	sfree(uc->buttons);
	sfree(uc);
    }
    freetree234(dp->bywidget);
    sfree(dp->treeitems);
}

static void dlg_add_uctrl(struct dlgparam *dp, struct uctrl *uc)
{
    add234(dp->byctrl, uc);
    add234(dp->bywidget, uc);
}

static struct uctrl *dlg_find_byctrl(struct dlgparam *dp, union control *ctrl)
{
    return find234(dp->byctrl, ctrl, uctrl_cmp_byctrl_find);
}

static struct uctrl *dlg_find_bywidget(struct dlgparam *dp, GtkWidget *w)
{
    struct uctrl *ret = NULL;
    do {
	ret = find234(dp->bywidget, w, uctrl_cmp_bywidget_find);
	if (ret)
	    return ret;
	w = w->parent;
    } while (w);
    return ret;
}

void *dlg_get_privdata(union control *ctrl, void *dlg)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    return uc->privdata;
}

void dlg_set_privdata(union control *ctrl, void *dlg, void *ptr)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    uc->privdata = ptr;
    uc->privdata_needs_free = FALSE;
}

void *dlg_alloc_privdata(union control *ctrl, void *dlg, size_t size)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    uc->privdata = smalloc(size);
    uc->privdata_needs_free = FALSE;
    return uc->privdata;
}

union control *dlg_last_focused(union control *ctrl, void *dlg)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    if (dp->currfocus != ctrl)
        return dp->currfocus;
    else
        return dp->lastfocus;
}

void dlg_radiobutton_set(union control *ctrl, void *dlg, int which)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->generic.type == CTRL_RADIO);
    assert(uc->buttons != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(uc->buttons[which]), TRUE);
}

int dlg_radiobutton_get(union control *ctrl, void *dlg)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    int i;

    assert(uc->ctrl->generic.type == CTRL_RADIO);
    assert(uc->buttons != NULL);
    for (i = 0; i < uc->nbuttons; i++)
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(uc->buttons[i])))
	    return i;
    return 0;			       /* got to return something */
}

void dlg_checkbox_set(union control *ctrl, void *dlg, int checked)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->generic.type == CTRL_CHECKBOX);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(uc->toplevel), checked);
}

int dlg_checkbox_get(union control *ctrl, void *dlg)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->generic.type == CTRL_CHECKBOX);
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(uc->toplevel));
}

void dlg_editbox_set(union control *ctrl, void *dlg, char const *text)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->generic.type == CTRL_EDITBOX);
    assert(uc->entry != NULL);
    gtk_entry_set_text(GTK_ENTRY(uc->entry), text);
}

void dlg_editbox_get(union control *ctrl, void *dlg, char *buffer, int length)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->generic.type == CTRL_EDITBOX);
    assert(uc->entry != NULL);
    strncpy(buffer, gtk_entry_get_text(GTK_ENTRY(uc->entry)),
	    length);
    buffer[length-1] = '\0';
}

static void container_remove_and_destroy(GtkWidget *w, gpointer data)
{
    GtkContainer *cont = GTK_CONTAINER(data);
    /* gtk_container_remove will unref the widget for us; we need not. */
    gtk_container_remove(cont, w);
}

/* The `listbox' functions can also apply to combo boxes. */
void dlg_listbox_clear(union control *ctrl, void *dlg)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    GtkContainer *cont;

    assert(uc->ctrl->generic.type == CTRL_EDITBOX ||
	   uc->ctrl->generic.type == CTRL_LISTBOX);
    assert(uc->menu != NULL || uc->list != NULL);

    cont = (uc->menu ? GTK_CONTAINER(uc->menu) : GTK_CONTAINER(uc->list));

    gtk_container_foreach(cont, container_remove_and_destroy, cont);
}

void dlg_listbox_del(union control *ctrl, void *dlg, int index)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->generic.type == CTRL_EDITBOX ||
	   uc->ctrl->generic.type == CTRL_LISTBOX);
    assert(uc->menu != NULL || uc->list != NULL);

    if (uc->menu) {
	gtk_container_remove
	    (GTK_CONTAINER(uc->menu),
	     g_list_nth_data(GTK_MENU_SHELL(uc->menu)->children, index));
    } else {
	gtk_list_clear_items(GTK_LIST(uc->list), index, index+1);
    }
}

void dlg_listbox_add(union control *ctrl, void *dlg, char const *text)
{
    dlg_listbox_addwithindex(ctrl, dlg, text, 0);
}

/*
 * Each listbox entry may have a numeric id associated with it.
 * Note that some front ends only permit a string to be stored at
 * each position, which means that _if_ you put two identical
 * strings in any listbox then you MUST not assign them different
 * IDs and expect to get meaningful results back.
 */
void dlg_listbox_addwithindex(union control *ctrl, void *dlg,
			      char const *text, int id)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->generic.type == CTRL_EDITBOX ||
	   uc->ctrl->generic.type == CTRL_LISTBOX);
    assert(uc->menu != NULL || uc->list != NULL);

    dp->flags |= FLAG_UPDATING_COMBO_LIST;

    if (uc->menu) {
	/*
	 * List item in a drop-down (but non-combo) list. Tabs are
	 * ignored; we just provide a standard menu item with the
	 * text.
	 */
	GtkWidget *menuitem = gtk_menu_item_new_with_label(text);

	gtk_container_add(GTK_CONTAINER(uc->menu), menuitem);
	gtk_widget_show(menuitem);

	gtk_object_set_data(GTK_OBJECT(menuitem), "user-data", (gpointer)id);
	gtk_signal_connect(GTK_OBJECT(menuitem), "activate",
			   GTK_SIGNAL_FUNC(menuitem_activate), dp);
    } else if (!uc->entry) {
	/*
	 * List item in a non-combo-box list box. We make all of
	 * these Columns containing GtkLabels. This allows us to do
	 * the nasty force_left hack irrespective of whether there
	 * are tabs in the thing.
	 */
	GtkWidget *listitem = gtk_list_item_new();
	GtkWidget *cols = columns_new(10);
	gint *percents;
	int i, ncols;

	/* Count the tabs in the text, and hence determine # of columns. */
	ncols = 1;
	for (i = 0; text[i]; i++)
	    if (text[i] == '\t')
		ncols++;

	assert(ncols <=
	       (uc->ctrl->listbox.ncols ? uc->ctrl->listbox.ncols : 1));
	percents = smalloc(ncols * sizeof(gint));
	percents[ncols-1] = 100;
	for (i = 0; i < ncols-1; i++) {
	    percents[i] = uc->ctrl->listbox.percentages[i];
	    percents[ncols-1] -= percents[i];
	}
	columns_set_cols(COLUMNS(cols), ncols, percents);
	sfree(percents);

	for (i = 0; i < ncols; i++) {
	    int len = strcspn(text, "\t");
	    char *dup = dupprintf("%.*s", len, text);
	    GtkWidget *label;

	    text += len;
	    if (*text) text++;
	    label = gtk_label_new(dup);
	    sfree(dup);

	    columns_add(COLUMNS(cols), label, i, 1);
	    columns_force_left_align(COLUMNS(cols), label);
	    gtk_widget_show(label);
	}
	gtk_container_add(GTK_CONTAINER(listitem), cols);
	gtk_widget_show(cols);
	gtk_container_add(GTK_CONTAINER(uc->list), listitem);
	gtk_widget_show(listitem);

        if (ctrl->listbox.multisel) {
            gtk_signal_connect(GTK_OBJECT(listitem), "key_press_event",
                               GTK_SIGNAL_FUNC(listitem_multi_key), uc->adj);
        } else {
            gtk_signal_connect(GTK_OBJECT(listitem), "key_press_event",
                               GTK_SIGNAL_FUNC(listitem_single_key), uc->adj);
        }
        gtk_signal_connect(GTK_OBJECT(listitem), "focus_in_event",
                           GTK_SIGNAL_FUNC(widget_focus), dp);
	gtk_signal_connect(GTK_OBJECT(listitem), "button_press_event",
			   GTK_SIGNAL_FUNC(listitem_button), dp);
	gtk_object_set_data(GTK_OBJECT(listitem), "user-data", (gpointer)id);
    } else {
	/*
	 * List item in a combo-box list, which means the sensible
	 * thing to do is make it a perfectly normal label. Hence
	 * tabs are disregarded.
	 */
	GtkWidget *listitem = gtk_list_item_new_with_label(text);

	gtk_container_add(GTK_CONTAINER(uc->list), listitem);
	gtk_widget_show(listitem);

	gtk_object_set_data(GTK_OBJECT(listitem), "user-data", (gpointer)id);
    }

    dp->flags &= ~FLAG_UPDATING_COMBO_LIST;
}

int dlg_listbox_getid(union control *ctrl, void *dlg, int index)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    GList *children;
    GtkObject *item;

    assert(uc->ctrl->generic.type == CTRL_EDITBOX ||
	   uc->ctrl->generic.type == CTRL_LISTBOX);
    assert(uc->menu != NULL || uc->list != NULL);

    children = gtk_container_children(GTK_CONTAINER(uc->menu ? uc->menu :
						    uc->list));
    item = GTK_OBJECT(g_list_nth_data(children, index));
    g_list_free(children);

    return (int)gtk_object_get_data(GTK_OBJECT(item), "user-data");
}

/* dlg_listbox_index returns <0 if no single element is selected. */
int dlg_listbox_index(union control *ctrl, void *dlg)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    GList *children;
    GtkWidget *item, *activeitem;
    int i;
    int selected = -1;

    assert(uc->ctrl->generic.type == CTRL_EDITBOX ||
	   uc->ctrl->generic.type == CTRL_LISTBOX);
    assert(uc->menu != NULL || uc->list != NULL);

    if (uc->menu)
	activeitem = gtk_menu_get_active(GTK_MENU(uc->menu));

    children = gtk_container_children(GTK_CONTAINER(uc->menu ? uc->menu :
						    uc->list));
    for (i = 0; children!=NULL && (item = GTK_WIDGET(children->data))!=NULL;
	 i++, children = children->next) {
	if (uc->menu ? activeitem == item :
	    GTK_WIDGET_STATE(item) == GTK_STATE_SELECTED) {
	    if (selected == -1)
		selected = i;
	    else
		selected = -2;
	}
    }
    g_list_free(children);
    return selected < 0 ? -1 : selected;
}

int dlg_listbox_issel(union control *ctrl, void *dlg, int index)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    GList *children;
    GtkWidget *item, *activeitem;

    assert(uc->ctrl->generic.type == CTRL_EDITBOX ||
	   uc->ctrl->generic.type == CTRL_LISTBOX);
    assert(uc->menu != NULL || uc->list != NULL);

    children = gtk_container_children(GTK_CONTAINER(uc->menu ? uc->menu :
						    uc->list));
    item = GTK_WIDGET(g_list_nth_data(children, index));
    g_list_free(children);

    if (uc->menu) {
	activeitem = gtk_menu_get_active(GTK_MENU(uc->menu));
	return item == activeitem;
    } else {
	return GTK_WIDGET_STATE(item) == GTK_STATE_SELECTED;
    }
}

void dlg_listbox_select(union control *ctrl, void *dlg, int index)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->generic.type == CTRL_EDITBOX ||
	   uc->ctrl->generic.type == CTRL_LISTBOX);
    assert(uc->optmenu != NULL || uc->list != NULL);

    if (uc->optmenu) {
	gtk_option_menu_set_history(GTK_OPTION_MENU(uc->optmenu), index);
    } else {
	gtk_list_select_item(GTK_LIST(uc->list), index);
    }
}

void dlg_text_set(union control *ctrl, void *dlg, char const *text)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    assert(uc->ctrl->generic.type == CTRL_TEXT);
    assert(uc->text != NULL);

    gtk_label_set_text(GTK_LABEL(uc->text), text);
}

void dlg_filesel_set(union control *ctrl, void *dlg, Filename fn)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->generic.type == CTRL_FILESELECT);
    assert(uc->entry != NULL);
    gtk_entry_set_text(GTK_ENTRY(uc->entry), fn.path);
}

void dlg_filesel_get(union control *ctrl, void *dlg, Filename *fn)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->generic.type == CTRL_FILESELECT);
    assert(uc->entry != NULL);
    strncpy(fn->path, gtk_entry_get_text(GTK_ENTRY(uc->entry)),
	    lenof(fn->path));
    fn->path[lenof(fn->path)-1] = '\0';
}

void dlg_fontsel_set(union control *ctrl, void *dlg, FontSpec fs)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->generic.type == CTRL_FONTSELECT);
    assert(uc->entry != NULL);
    gtk_entry_set_text(GTK_ENTRY(uc->entry), fs.name);
}

void dlg_fontsel_get(union control *ctrl, void *dlg, FontSpec *fs)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    assert(uc->ctrl->generic.type == CTRL_FONTSELECT);
    assert(uc->entry != NULL);
    strncpy(fs->name, gtk_entry_get_text(GTK_ENTRY(uc->entry)),
	    lenof(fs->name));
    fs->name[lenof(fs->name)-1] = '\0';
}

/*
 * Bracketing a large set of updates in these two functions will
 * cause the front end (if possible) to delay updating the screen
 * until it's all complete, thus avoiding flicker.
 */
void dlg_update_start(union control *ctrl, void *dlg)
{
    /*
     * Apparently we can't do this at all in GTK. GtkCList supports
     * freeze and thaw, but not GtkList. Bah.
     */
}

void dlg_update_done(union control *ctrl, void *dlg)
{
    /*
     * Apparently we can't do this at all in GTK. GtkCList supports
     * freeze and thaw, but not GtkList. Bah.
     */
}

void dlg_set_focus(union control *ctrl, void *dlg)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);

    switch (ctrl->generic.type) {
      case CTRL_CHECKBOX:
      case CTRL_BUTTON:
        /* Check boxes and buttons get the focus _and_ get toggled. */
        gtk_widget_grab_focus(uc->toplevel);
        break;
      case CTRL_FILESELECT:
      case CTRL_FONTSELECT:
      case CTRL_EDITBOX:
        /* Anything containing an edit box gets that focused. */
        gtk_widget_grab_focus(uc->entry);
        break;
      case CTRL_RADIO:
        /*
         * Radio buttons: we find the currently selected button and
         * focus it.
         */
        {
            int i;
            for (i = 0; i < ctrl->radio.nbuttons; i++)
                if (gtk_toggle_button_get_active
                    (GTK_TOGGLE_BUTTON(uc->buttons[i]))) {
                    gtk_widget_grab_focus(uc->buttons[i]);
                }
        }
        break;
      case CTRL_LISTBOX:
        /*
         * If the list is really an option menu, we focus it.
         * Otherwise we tell it to focus one of its children, which
         * appears to do the Right Thing.
         */
        if (uc->optmenu) {
            gtk_widget_grab_focus(uc->optmenu);
        } else {
            assert(uc->list != NULL);
            gtk_container_focus(GTK_CONTAINER(uc->list), GTK_DIR_TAB_FORWARD);
        }
        break;
    }
}

/*
 * During event processing, you might well want to give an error
 * indication to the user. dlg_beep() is a quick and easy generic
 * error; dlg_error() puts up a message-box or equivalent.
 */
void dlg_beep(void *dlg)
{
    gdk_beep();
}

static void errmsg_button_clicked(GtkButton *button, gpointer data)
{
    gtk_widget_destroy(GTK_WIDGET(data));
}

void dlg_error_msg(void *dlg, char *msg)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    GtkWidget *window, *hbox, *text, *ok;

    window = gtk_dialog_new();
    text = gtk_label_new(msg);
    gtk_misc_set_alignment(GTK_MISC(text), 0.0, 0.0);
    hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), text, FALSE, FALSE, 20);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(window)->vbox),
                       hbox, FALSE, FALSE, 20);
    gtk_widget_show(text);
    gtk_widget_show(hbox);
    gtk_window_set_title(GTK_WINDOW(window), "Error");
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
    ok = gtk_button_new_with_label("OK");
    gtk_box_pack_end(GTK_BOX(GTK_DIALOG(window)->action_area),
                     ok, FALSE, FALSE, 0);
    gtk_widget_show(ok);
    GTK_WIDGET_SET_FLAGS(ok, GTK_CAN_DEFAULT);
    gtk_window_set_default(GTK_WINDOW(window), ok);
    gtk_signal_connect(GTK_OBJECT(ok), "clicked",
                       GTK_SIGNAL_FUNC(errmsg_button_clicked), window);
    gtk_signal_connect(GTK_OBJECT(window), "destroy",
                       GTK_SIGNAL_FUNC(window_destroy), NULL);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(dp->window));
    {
	gint x, y, w, h, dx, dy;
	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_NONE);
	gdk_window_get_origin(GTK_WIDGET(dp->window)->window, &x, &y);
	gdk_window_get_size(GTK_WIDGET(dp->window)->window, &w, &h);
	dx = x + w/4;
	dy = y + h/4;
	gtk_widget_set_uposition(GTK_WIDGET(window), dx, dy);
    }
    gtk_widget_show(window);
    gtk_main();
}

/*
 * This function signals to the front end that the dialog's
 * processing is completed, and passes an integer value (typically
 * a success status).
 */
void dlg_end(void *dlg, int value)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    dp->retval = value;
    gtk_main_quit();
}

void dlg_refresh(union control *ctrl, void *dlg)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc;

    if (ctrl) {
	if (ctrl->generic.handler != NULL)
	    ctrl->generic.handler(ctrl, dp, dp->data, EVENT_REFRESH);
    } else {
	int i;

	for (i = 0; (uc = index234(dp->byctrl, i)) != NULL; i++) {
	    assert(uc->ctrl != NULL);
	    if (uc->ctrl->generic.handler != NULL)
		uc->ctrl->generic.handler(uc->ctrl, dp,
					  dp->data, EVENT_REFRESH);
	}
    }
}

void dlg_coloursel_start(union control *ctrl, void *dlg, int r, int g, int b)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    struct uctrl *uc = dlg_find_byctrl(dp, ctrl);
    gdouble cvals[4];

    GtkWidget *coloursel =
	gtk_color_selection_dialog_new("Select a colour");
    GtkColorSelectionDialog *ccs = GTK_COLOR_SELECTION_DIALOG(coloursel);

    dp->coloursel_result.ok = FALSE;

    gtk_window_set_modal(GTK_WINDOW(coloursel), TRUE);
    gtk_color_selection_set_opacity(GTK_COLOR_SELECTION(ccs->colorsel), FALSE);
    cvals[0] = r / 255.0;
    cvals[1] = g / 255.0;
    cvals[2] = b / 255.0;
    cvals[3] = 1.0;		       /* fully opaque! */
    gtk_color_selection_set_color(GTK_COLOR_SELECTION(ccs->colorsel), cvals);

    gtk_object_set_data(GTK_OBJECT(ccs->ok_button), "user-data",
			(gpointer)coloursel);
    gtk_object_set_data(GTK_OBJECT(ccs->cancel_button), "user-data",
			(gpointer)coloursel);
    gtk_object_set_data(GTK_OBJECT(coloursel), "user-data", (gpointer)uc);
    gtk_signal_connect(GTK_OBJECT(ccs->ok_button), "clicked",
		       GTK_SIGNAL_FUNC(coloursel_ok), (gpointer)dp);
    gtk_signal_connect(GTK_OBJECT(ccs->cancel_button), "clicked",
		       GTK_SIGNAL_FUNC(coloursel_cancel), (gpointer)dp);
    gtk_signal_connect_object(GTK_OBJECT(ccs->ok_button), "clicked",
			      GTK_SIGNAL_FUNC(gtk_widget_destroy),
			      (gpointer)coloursel);
    gtk_signal_connect_object(GTK_OBJECT(ccs->cancel_button), "clicked",
			      GTK_SIGNAL_FUNC(gtk_widget_destroy),
			      (gpointer)coloursel);
    gtk_widget_show(coloursel);
}

int dlg_coloursel_results(union control *ctrl, void *dlg,
			  int *r, int *g, int *b)
{
    struct dlgparam *dp = (struct dlgparam *)dlg;
    if (dp->coloursel_result.ok) {
	*r = dp->coloursel_result.r;
	*g = dp->coloursel_result.g;
	*b = dp->coloursel_result.b;
	return 1;
    } else
	return 0;
}

/* ----------------------------------------------------------------------
 * Signal handlers while the dialog box is active.
 */

static gboolean widget_focus(GtkWidget *widget, GdkEventFocus *event,
                             gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, widget);
    union control *focus;

    if (uc && uc->ctrl)
        focus = uc->ctrl;
    else
        focus = NULL;

    if (focus != dp->currfocus) {
        dp->lastfocus = dp->currfocus;
        dp->currfocus = focus;
    }

    return FALSE;
}

static void button_clicked(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(button));
    uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_ACTION);
}

static void button_toggled(GtkToggleButton *tb, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(tb));
    uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_VALCHANGE);
}

static int editbox_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    /*
     * GtkEntry has a nasty habit of eating the Return key, which
     * is unhelpful since it doesn't actually _do_ anything with it
     * (it calls gtk_widget_activate, but our edit boxes never need
     * activating). So I catch Return before GtkEntry sees it, and
     * pass it straight on to the parent widget. Effect: hitting
     * Return in an edit box will now activate the default button
     * in the dialog just like it will everywhere else.
     */
    if (event->keyval == GDK_Return && widget->parent != NULL) {
	gint return_val;
	gtk_signal_emit_stop_by_name(GTK_OBJECT(widget), "key_press_event");
	gtk_signal_emit_by_name(GTK_OBJECT(widget->parent), "key_press_event",
				event, &return_val);
	return return_val;
    }
    return FALSE;
}

static void editbox_changed(GtkEditable *ed, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    if (!(dp->flags & FLAG_UPDATING_COMBO_LIST)) {
	struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(ed));
	uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_VALCHANGE);
    }
}

static void editbox_lostfocus(GtkWidget *ed, GdkEventFocus *event,
			      gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(ed));
    uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_REFRESH);
}

static int listitem_key(GtkWidget *item, GdkEventKey *event, gpointer data,
                        int multiple)
{
    GtkAdjustment *adj = GTK_ADJUSTMENT(data);

    if (event->keyval == GDK_Up || event->keyval == GDK_KP_Up ||
        event->keyval == GDK_Down || event->keyval == GDK_KP_Down ||
        event->keyval == GDK_Page_Up || event->keyval == GDK_KP_Page_Up ||
        event->keyval == GDK_Page_Down || event->keyval == GDK_KP_Page_Down) {
        /*
         * Up, Down, PgUp or PgDn have been pressed on a ListItem
         * in a list box. So, if the list box is single-selection:
         * 
         *  - if the list item in question isn't already selected,
         *    we simply select it.
         *  - otherwise, we find the next one (or next
         *    however-far-away) in whichever direction we're going,
         *    and select that.
         *     + in this case, we must also fiddle with the
         *       scrollbar to ensure the newly selected item is
         *       actually visible.
         * 
         * If it's multiple-selection, we do all of the above
         * except actually selecting anything, so we move the focus
         * and fiddle the scrollbar to follow it.
         */
        GtkWidget *list = item->parent;

        gtk_signal_emit_stop_by_name(GTK_OBJECT(item), "key_press_event");

        if (!multiple &&
            GTK_WIDGET_STATE(item) != GTK_STATE_SELECTED) {
                gtk_list_select_child(GTK_LIST(list), item);
        } else {
            int direction =
                (event->keyval==GDK_Up || event->keyval==GDK_KP_Up ||
                 event->keyval==GDK_Page_Up || event->keyval==GDK_KP_Page_Up)
                ? -1 : +1;
            int step =
                (event->keyval==GDK_Page_Down || 
                 event->keyval==GDK_KP_Page_Down ||
                 event->keyval==GDK_Page_Up || event->keyval==GDK_KP_Page_Up)
                ? 2 : 1;
            int i, n;
            GtkWidget *thisitem;
            GList *children, *chead;

            chead = children = gtk_container_children(GTK_CONTAINER(list));

            n = g_list_length(children);

            if (step == 2) {
                /*
                 * Figure out how many list items to a screenful,
                 * and adjust the step appropriately.
                 */
                step = 0.5 + adj->page_size * n / (adj->upper - adj->lower);
                step--;                /* go by one less than that */
            }

            i = 0;
            while (children != NULL) {
                if (item == children->data)
                    break;
                children = children->next;
                i++;
            }

            while (step > 0) {
                if (direction < 0 && i > 0)
                    children = children->prev, i--;
                else if (direction > 0 && i < n-1)
                    children = children->next, i++;
                step--;
            }

            if (children && children->data) {
                if (!multiple)
                    gtk_list_select_child(GTK_LIST(list),
                                          GTK_WIDGET(children->data));
                gtk_widget_grab_focus(GTK_WIDGET(children->data));
                gtk_adjustment_clamp_page
                    (adj,
                     adj->lower + (adj->upper-adj->lower) * i / n,
                     adj->lower + (adj->upper-adj->lower) * (i+1) / n);
            }

            g_list_free(chead);
        }
        return TRUE;
    }

    return FALSE;
}

static int listitem_single_key(GtkWidget *item, GdkEventKey *event,
                               gpointer data)
{
    listitem_key(item, event, data, FALSE);
}

static int listitem_multi_key(GtkWidget *item, GdkEventKey *event,
                                 gpointer data)
{
    listitem_key(item, event, data, TRUE);
}

static int listitem_button(GtkWidget *item, GdkEventButton *event,
			    gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    if (event->type == GDK_2BUTTON_PRESS ||
	event->type == GDK_3BUTTON_PRESS) {
	struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(item));
	uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_ACTION);
        return TRUE;
    }
    return FALSE;
}

static void list_selchange(GtkList *list, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(list));
    uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_SELCHANGE);
}

static void menuitem_activate(GtkMenuItem *item, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    GtkWidget *menushell = GTK_WIDGET(item)->parent;
    gpointer optmenu = gtk_object_get_data(GTK_OBJECT(menushell), "user-data");
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(optmenu));
    uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_SELCHANGE);
}

static void draglist_move(struct dlgparam *dp, struct uctrl *uc, int direction)
{
    int index = dlg_listbox_index(uc->ctrl, dp);
    GList *children = gtk_container_children(GTK_CONTAINER(uc->list));
    GtkWidget *child;

    if ((index < 0) ||
	(index == 0 && direction < 0) ||
	(index == g_list_length(children)-1 && direction > 0)) {
	gdk_beep();
	return;
    }

    child = g_list_nth_data(children, index);
    gtk_widget_ref(child);
    gtk_list_clear_items(GTK_LIST(uc->list), index, index+1);
    g_list_free(children);

    children = NULL;
    children = g_list_append(children, child);
    gtk_list_insert_items(GTK_LIST(uc->list), children, index + direction);
    gtk_list_select_item(GTK_LIST(uc->list), index + direction);
    uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_VALCHANGE);
}

static void draglist_up(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(button));
    draglist_move(dp, uc, -1);
}

static void draglist_down(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(button));
    draglist_move(dp, uc, +1);
}

static void filesel_ok(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    gpointer filesel = gtk_object_get_data(GTK_OBJECT(button), "user-data");
    struct uctrl *uc = gtk_object_get_data(GTK_OBJECT(filesel), "user-data");
    char *name = gtk_file_selection_get_filename(GTK_FILE_SELECTION(filesel));
    gtk_entry_set_text(GTK_ENTRY(uc->entry), name);
}

static void fontsel_ok(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    gpointer fontsel = gtk_object_get_data(GTK_OBJECT(button), "user-data");
    struct uctrl *uc = gtk_object_get_data(GTK_OBJECT(fontsel), "user-data");
    char *name = gtk_font_selection_dialog_get_font_name
	(GTK_FONT_SELECTION_DIALOG(fontsel));
    gtk_entry_set_text(GTK_ENTRY(uc->entry), name);
}

static void coloursel_ok(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    gpointer coloursel = gtk_object_get_data(GTK_OBJECT(button), "user-data");
    struct uctrl *uc = gtk_object_get_data(GTK_OBJECT(coloursel), "user-data");
    gdouble cvals[4];
    gtk_color_selection_get_color
	(GTK_COLOR_SELECTION(GTK_COLOR_SELECTION_DIALOG(coloursel)->colorsel),
	 cvals);
    dp->coloursel_result.r = (int) (255 * cvals[0]);
    dp->coloursel_result.g = (int) (255 * cvals[1]);
    dp->coloursel_result.b = (int) (255 * cvals[2]);
    dp->coloursel_result.ok = TRUE;
    uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_CALLBACK);
}

static void coloursel_cancel(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    gpointer coloursel = gtk_object_get_data(GTK_OBJECT(button), "user-data");
    struct uctrl *uc = gtk_object_get_data(GTK_OBJECT(coloursel), "user-data");
    dp->coloursel_result.ok = FALSE;
    uc->ctrl->generic.handler(uc->ctrl, dp, dp->data, EVENT_CALLBACK);
}

static void filefont_clicked(GtkButton *button, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    struct uctrl *uc = dlg_find_bywidget(dp, GTK_WIDGET(button));

    if (uc->ctrl->generic.type == CTRL_FILESELECT) {
	GtkWidget *filesel =
	    gtk_file_selection_new(uc->ctrl->fileselect.title);
	gtk_window_set_modal(GTK_WINDOW(filesel), TRUE);
	gtk_object_set_data
	    (GTK_OBJECT(GTK_FILE_SELECTION(filesel)->ok_button), "user-data",
	     (gpointer)filesel);
	gtk_object_set_data(GTK_OBJECT(filesel), "user-data", (gpointer)uc);
	gtk_signal_connect
	    (GTK_OBJECT(GTK_FILE_SELECTION(filesel)->ok_button), "clicked",
	     GTK_SIGNAL_FUNC(filesel_ok), (gpointer)dp);
	gtk_signal_connect_object
	    (GTK_OBJECT(GTK_FILE_SELECTION(filesel)->ok_button), "clicked",
	     GTK_SIGNAL_FUNC(gtk_widget_destroy), (gpointer)filesel);
	gtk_signal_connect_object
	    (GTK_OBJECT(GTK_FILE_SELECTION(filesel)->cancel_button), "clicked",
	     GTK_SIGNAL_FUNC(gtk_widget_destroy), (gpointer)filesel);
	gtk_widget_show(filesel);
    }

    if (uc->ctrl->generic.type == CTRL_FONTSELECT) {
	gchar *spacings[] = { "c", "m", NULL };
        gchar *fontname = gtk_entry_get_text(GTK_ENTRY(uc->entry));
	GtkWidget *fontsel =
	    gtk_font_selection_dialog_new("Select a font");
	gtk_window_set_modal(GTK_WINDOW(fontsel), TRUE);
	gtk_font_selection_dialog_set_filter
	    (GTK_FONT_SELECTION_DIALOG(fontsel),
	     GTK_FONT_FILTER_BASE, GTK_FONT_ALL,
	     NULL, NULL, NULL, NULL, spacings, NULL);
	if (!gtk_font_selection_dialog_set_font_name
	    (GTK_FONT_SELECTION_DIALOG(fontsel), fontname)) {
            /*
             * If the font name wasn't found as it was, try opening
             * it and extracting its FONT property. This should
             * have the effect of mapping short aliases into true
             * XLFDs.
             */
            GdkFont *font = gdk_font_load(fontname);
            if (font) {
                XFontStruct *xfs = GDK_FONT_XFONT(font);
                Display *disp = GDK_FONT_XDISPLAY(font);
                Atom fontprop = XInternAtom(disp, "FONT", False);
                unsigned long ret;
                if (XGetFontProperty(xfs, fontprop, &ret)) {
                    char *name = XGetAtomName(disp, (Atom)ret);
                    if (name)
                        gtk_font_selection_dialog_set_font_name
                        (GTK_FONT_SELECTION_DIALOG(fontsel), name);
                }
                gdk_font_unref(font);
            }
        }
	gtk_object_set_data
	    (GTK_OBJECT(GTK_FONT_SELECTION_DIALOG(fontsel)->ok_button),
	     "user-data", (gpointer)fontsel);
	gtk_object_set_data(GTK_OBJECT(fontsel), "user-data", (gpointer)uc);
	gtk_signal_connect
	    (GTK_OBJECT(GTK_FONT_SELECTION_DIALOG(fontsel)->ok_button),
	     "clicked", GTK_SIGNAL_FUNC(fontsel_ok), (gpointer)dp);
	gtk_signal_connect_object
	    (GTK_OBJECT(GTK_FONT_SELECTION_DIALOG(fontsel)->ok_button),
	     "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy),
	     (gpointer)fontsel);
	gtk_signal_connect_object
	    (GTK_OBJECT(GTK_FONT_SELECTION_DIALOG(fontsel)->cancel_button),
	     "clicked", GTK_SIGNAL_FUNC(gtk_widget_destroy),
	     (gpointer)fontsel);
	gtk_widget_show(fontsel);
    }
}

/* ----------------------------------------------------------------------
 * This function does the main layout work: it reads a controlset,
 * it creates the relevant GTK controls, and returns a GtkWidget
 * containing the result. (This widget might be a title of some
 * sort, it might be a Columns containing many controls, or it
 * might be a GtkFrame containing a Columns; whatever it is, it's
 * definitely a GtkWidget and should probably be added to a
 * GtkVbox.)
 * 
 * `listitemheight' is used to calculate a usize for list boxes: it
 * should be the height from the size request of a GtkListItem.
 * 
 * `win' is required for setting the default button. If it is
 * non-NULL, all buttons created will be default-capable (so they
 * have extra space round them for the default highlight).
 */
GtkWidget *layout_ctrls(struct dlgparam *dp, struct Shortcuts *scs,
			struct controlset *s, int listitemheight,
			GtkWindow *win)
{
    Columns *cols;
    GtkWidget *ret;
    int i;

    if (!s->boxname && s->boxtitle) {
        /* This controlset is a panel title. */
        return gtk_label_new(s->boxtitle);
    }

    /*
     * Otherwise, we expect to be laying out actual controls, so
     * we'll start by creating a Columns for the purpose.
     */
    cols = COLUMNS(columns_new(4));
    ret = GTK_WIDGET(cols);
    gtk_widget_show(ret);

    /*
     * Create a containing frame if we have a box name.
     */
    if (*s->boxname) {
        ret = gtk_frame_new(s->boxtitle);   /* NULL is valid here */
        gtk_container_set_border_width(GTK_CONTAINER(cols), 4);
        gtk_container_add(GTK_CONTAINER(ret), GTK_WIDGET(cols));
        gtk_widget_show(ret);
    }

    /*
     * Now iterate through the controls themselves, create them,
     * and add them to the Columns.
     */
    for (i = 0; i < s->ncontrols; i++) {
	union control *ctrl = s->ctrls[i];
	struct uctrl *uc;
	int left = FALSE;
        GtkWidget *w = NULL;

        switch (ctrl->generic.type) {
          case CTRL_COLUMNS:
            {
                static const int simplecols[1] = { 100 };
                columns_set_cols(cols, ctrl->columns.ncols,
                                 (ctrl->columns.percentages ?
                                  ctrl->columns.percentages : simplecols));
            }
            continue;                  /* no actual control created */
          case CTRL_TABDELAY:
	    {
		struct uctrl *uc = dlg_find_byctrl(dp, ctrl->tabdelay.ctrl);
		if (uc)
		    columns_taborder_last(cols, uc->toplevel);
	    }
            continue;                  /* no actual control created */
	}

	uc = smalloc(sizeof(struct uctrl));
	uc->ctrl = ctrl;
	uc->privdata = NULL;
	uc->privdata_needs_free = FALSE;
	uc->buttons = NULL;
	uc->entry = uc->list = uc->menu = NULL;
	uc->button = uc->optmenu = uc->text = NULL;

        switch (ctrl->generic.type) {
          case CTRL_BUTTON:
            w = gtk_button_new_with_label(ctrl->generic.label);
	    if (win) {
		GTK_WIDGET_SET_FLAGS(w, GTK_CAN_DEFAULT);
		if (ctrl->button.isdefault)
		    gtk_window_set_default(win, w);
		if (ctrl->button.iscancel)
		    dp->cancelbutton = w;
	    }
	    gtk_signal_connect(GTK_OBJECT(w), "clicked",
			       GTK_SIGNAL_FUNC(button_clicked), dp);
            gtk_signal_connect(GTK_OBJECT(w), "focus_in_event",
                               GTK_SIGNAL_FUNC(widget_focus), dp);
	    shortcut_add(scs, GTK_BIN(w)->child, ctrl->button.shortcut,
			 SHORTCUT_UCTRL, uc);
            break;
          case CTRL_CHECKBOX:
            w = gtk_check_button_new_with_label(ctrl->generic.label);
	    gtk_signal_connect(GTK_OBJECT(w), "toggled",
			       GTK_SIGNAL_FUNC(button_toggled), dp);
            gtk_signal_connect(GTK_OBJECT(w), "focus_in_event",
                               GTK_SIGNAL_FUNC(widget_focus), dp);
	    shortcut_add(scs, GTK_BIN(w)->child, ctrl->checkbox.shortcut,
			 SHORTCUT_UCTRL, uc);
	    left = TRUE;
            break;
          case CTRL_RADIO:
            /*
             * Radio buttons get to go inside their own Columns, no
             * matter what.
             */
            {
                gint i, *percentages;
                GSList *group;

                w = columns_new(1);
                if (ctrl->generic.label) {
                    GtkWidget *label = gtk_label_new(ctrl->generic.label);
                    columns_add(COLUMNS(w), label, 0, 1);
		    columns_force_left_align(COLUMNS(w), label);
                    gtk_widget_show(label);
		    shortcut_add(scs, label, ctrl->radio.shortcut,
				 SHORTCUT_UCTRL, uc);
                }
                percentages = g_new(gint, ctrl->radio.ncolumns);
                for (i = 0; i < ctrl->radio.ncolumns; i++) {
                    percentages[i] =
                        ((100 * (i+1) / ctrl->radio.ncolumns) -
                         100 * i / ctrl->radio.ncolumns);
                }
                columns_set_cols(COLUMNS(w), ctrl->radio.ncolumns,
                                 percentages);
                g_free(percentages);
                group = NULL;

		uc->nbuttons = ctrl->radio.nbuttons;
		uc->buttons = smalloc(uc->nbuttons * sizeof(GtkWidget *));

                for (i = 0; i < ctrl->radio.nbuttons; i++) {
                    GtkWidget *b;
                    gint colstart;

                    b = (gtk_radio_button_new_with_label
                         (group, ctrl->radio.buttons[i]));
		    uc->buttons[i] = b;
                    group = gtk_radio_button_group(GTK_RADIO_BUTTON(b));
                    colstart = i % ctrl->radio.ncolumns;
                    columns_add(COLUMNS(w), b, colstart,
                                (i == ctrl->radio.nbuttons-1 ?
                                 ctrl->radio.ncolumns - colstart : 1));
		    columns_force_left_align(COLUMNS(w), b);
                    gtk_widget_show(b);
		    gtk_signal_connect(GTK_OBJECT(b), "toggled",
				       GTK_SIGNAL_FUNC(button_toggled), dp);
                    gtk_signal_connect(GTK_OBJECT(b), "focus_in_event",
                                       GTK_SIGNAL_FUNC(widget_focus), dp);
		    if (ctrl->radio.shortcuts) {
			shortcut_add(scs, GTK_BIN(b)->child,
				     ctrl->radio.shortcuts[i],
				     SHORTCUT_UCTRL, uc);
		    }
                }
            }
            break;
          case CTRL_EDITBOX:
            if (ctrl->editbox.has_list) {
                w = gtk_combo_new();
		gtk_combo_set_value_in_list(GTK_COMBO(w), FALSE, TRUE);
		uc->entry = GTK_COMBO(w)->entry;
                uc->list = GTK_COMBO(w)->list;
            } else {
                w = gtk_entry_new();
                if (ctrl->editbox.password)
                    gtk_entry_set_visibility(GTK_ENTRY(w), FALSE);
		uc->entry = w;
            }
	    gtk_signal_connect(GTK_OBJECT(uc->entry), "changed",
			       GTK_SIGNAL_FUNC(editbox_changed), dp);
	    gtk_signal_connect(GTK_OBJECT(uc->entry), "key_press_event",
			       GTK_SIGNAL_FUNC(editbox_key), dp);
            gtk_signal_connect(GTK_OBJECT(uc->entry), "focus_in_event",
                               GTK_SIGNAL_FUNC(widget_focus), dp);
            /*
             * Edit boxes, for some strange reason, have a minimum
             * width of 150 in GTK 1.2. We don't want this - we'd
             * rather the edit boxes acquired their natural width
             * from the column layout of the rest of the box.
             */
            {
                GtkRequisition req;
                gtk_widget_size_request(w, &req);
                gtk_widget_set_usize(w, 10, req.height);
            }
            if (ctrl->generic.label) {
                GtkWidget *label, *container;

                label = gtk_label_new(ctrl->generic.label);
		shortcut_add(scs, label, ctrl->editbox.shortcut,
			     SHORTCUT_FOCUS, uc->entry);

		container = columns_new(4);
                if (ctrl->editbox.percentwidth == 100) {
                    columns_add(COLUMNS(container), label, 0, 1);
		    columns_force_left_align(COLUMNS(container), label);
                    columns_add(COLUMNS(container), w, 0, 1);
                } else {
                    gint percentages[2];
                    percentages[1] = ctrl->editbox.percentwidth;
                    percentages[0] = 100 - ctrl->editbox.percentwidth;
                    columns_set_cols(COLUMNS(container), 2, percentages);
                    columns_add(COLUMNS(container), label, 0, 1);
		    columns_force_left_align(COLUMNS(container), label);
                    columns_add(COLUMNS(container), w, 1, 1);
                }
                gtk_widget_show(label);
                gtk_widget_show(w);

                w = container;
            }
	    gtk_signal_connect(GTK_OBJECT(uc->entry), "focus_out_event",
			       GTK_SIGNAL_FUNC(editbox_lostfocus), dp);
            break;
          case CTRL_FILESELECT:
          case CTRL_FONTSELECT:
            {
                GtkWidget *ww;
                GtkRequisition req;
                char *browsebtn =
                    (ctrl->generic.type == CTRL_FILESELECT ?
                     "Browse..." : "Change...");

                gint percentages[] = { 75, 25 };
                w = columns_new(4);
                columns_set_cols(COLUMNS(w), 2, percentages);

                if (ctrl->generic.label) {
                    ww = gtk_label_new(ctrl->generic.label);
                    columns_add(COLUMNS(w), ww, 0, 2);
		    columns_force_left_align(COLUMNS(w), ww);
                    gtk_widget_show(ww);
		    shortcut_add(scs, ww,
				 (ctrl->generic.type == CTRL_FILESELECT ?
				  ctrl->fileselect.shortcut :
				  ctrl->fontselect.shortcut),
				 SHORTCUT_UCTRL, uc);
                }

                uc->entry = ww = gtk_entry_new();
                gtk_widget_size_request(ww, &req);
                gtk_widget_set_usize(ww, 10, req.height);
                columns_add(COLUMNS(w), ww, 0, 1);
                gtk_widget_show(ww);

                uc->button = ww = gtk_button_new_with_label(browsebtn);
                columns_add(COLUMNS(w), ww, 1, 1);
                gtk_widget_show(ww);

		gtk_signal_connect(GTK_OBJECT(uc->entry), "key_press_event",
				   GTK_SIGNAL_FUNC(editbox_key), dp);
		gtk_signal_connect(GTK_OBJECT(uc->entry), "changed",
				   GTK_SIGNAL_FUNC(editbox_changed), dp);
                gtk_signal_connect(GTK_OBJECT(uc->entry), "focus_in_event",
                                   GTK_SIGNAL_FUNC(widget_focus), dp);
                gtk_signal_connect(GTK_OBJECT(uc->button), "focus_in_event",
                                   GTK_SIGNAL_FUNC(widget_focus), dp);
		gtk_signal_connect(GTK_OBJECT(ww), "clicked",
				   GTK_SIGNAL_FUNC(filefont_clicked), dp);
            }
            break;
          case CTRL_LISTBOX:
            if (ctrl->listbox.height == 0) {
                uc->optmenu = w = gtk_option_menu_new();
		uc->menu = gtk_menu_new();
		gtk_option_menu_set_menu(GTK_OPTION_MENU(w), uc->menu);
		gtk_object_set_data(GTK_OBJECT(uc->menu), "user-data",
				    (gpointer)uc->optmenu);
                gtk_signal_connect(GTK_OBJECT(uc->optmenu), "focus_in_event",
                                   GTK_SIGNAL_FUNC(widget_focus), dp);
            } else {
                uc->list = gtk_list_new();
                if (ctrl->listbox.multisel) {
                    gtk_list_set_selection_mode(GTK_LIST(uc->list),
                                                GTK_SELECTION_MULTIPLE);
                } else {
                    gtk_list_set_selection_mode(GTK_LIST(uc->list),
                                                GTK_SELECTION_SINGLE);
                }
                w = gtk_scrolled_window_new(NULL, NULL);
                gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(w),
                                                      uc->list);
                gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w),
                                               GTK_POLICY_NEVER,
                                               GTK_POLICY_AUTOMATIC);
                uc->adj = gtk_scrolled_window_get_vadjustment
                    (GTK_SCROLLED_WINDOW(w));

                gtk_widget_show(uc->list);
		gtk_signal_connect(GTK_OBJECT(uc->list), "selection-changed",
				   GTK_SIGNAL_FUNC(list_selchange), dp);
                gtk_signal_connect(GTK_OBJECT(uc->list), "focus_in_event",
                                   GTK_SIGNAL_FUNC(widget_focus), dp);

                /*
                 * Adjust the height of the scrolled window to the
                 * minimum given by the height parameter.
                 * 
                 * This piece of guesswork is a horrid hack based
                 * on looking inside the GTK 1.2 sources
                 * (specifically gtkviewport.c, which appears to be
                 * the widget which provides the border around the
                 * scrolling area). Anyone lets me know how I can
                 * do this in a way which isn't at risk from GTK
                 * upgrades, I'd be grateful.
                 */
		{
		    int edge = GTK_WIDGET(uc->list)->style->klass->ythickness;
                    gtk_widget_set_usize(w, 10,
                                         2*edge + (ctrl->listbox.height *
						   listitemheight));
		}

                if (ctrl->listbox.draglist) {
                    /*
                     * GTK doesn't appear to make it easy to
                     * implement a proper draggable list; so
                     * instead I'm just going to have to put an Up
                     * and a Down button to the right of the actual
                     * list box. Ah well.
                     */
                    GtkWidget *cols, *button;
                    static const gint percentages[2] = { 80, 20 };

                    cols = columns_new(4);
                    columns_set_cols(COLUMNS(cols), 2, percentages);
                    columns_add(COLUMNS(cols), w, 0, 1);
                    gtk_widget_show(w);
                    button = gtk_button_new_with_label("Up");
                    columns_add(COLUMNS(cols), button, 1, 1);
                    gtk_widget_show(button);
		    gtk_signal_connect(GTK_OBJECT(button), "clicked",
				       GTK_SIGNAL_FUNC(draglist_up), dp);
                    gtk_signal_connect(GTK_OBJECT(button), "focus_in_event",
                                       GTK_SIGNAL_FUNC(widget_focus), dp);
                    button = gtk_button_new_with_label("Down");
                    columns_add(COLUMNS(cols), button, 1, 1);
                    gtk_widget_show(button);
		    gtk_signal_connect(GTK_OBJECT(button), "clicked",
				       GTK_SIGNAL_FUNC(draglist_down), dp);
                    gtk_signal_connect(GTK_OBJECT(button), "focus_in_event",
                                       GTK_SIGNAL_FUNC(widget_focus), dp);

                    w = cols;
                }

            }
            if (ctrl->generic.label) {
                GtkWidget *label, *container;

                label = gtk_label_new(ctrl->generic.label);

		container = columns_new(4);
                if (ctrl->listbox.percentwidth == 100) {
                    columns_add(COLUMNS(container), label, 0, 1);
		    columns_force_left_align(COLUMNS(container), label);
                    columns_add(COLUMNS(container), w, 0, 1);
                } else {
                    gint percentages[2];
                    percentages[1] = ctrl->listbox.percentwidth;
                    percentages[0] = 100 - ctrl->listbox.percentwidth;
                    columns_set_cols(COLUMNS(container), 2, percentages);
                    columns_add(COLUMNS(container), label, 0, 1);
		    columns_force_left_align(COLUMNS(container), label);
                    columns_add(COLUMNS(container), w, 1, 1);
                }
                gtk_widget_show(label);
                gtk_widget_show(w);
		shortcut_add(scs, label, ctrl->listbox.shortcut,
			     SHORTCUT_UCTRL, uc);
                w = container;
            }
            break;
          case CTRL_TEXT:
            uc->text = w = gtk_label_new(ctrl->generic.label);
            gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.0);
            gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
            /* FIXME: deal with wrapping! */
            break;
        }

	assert(w != NULL);

	columns_add(cols, w,
		    COLUMN_START(ctrl->generic.column),
		    COLUMN_SPAN(ctrl->generic.column));
	if (left)
	    columns_force_left_align(cols, w);
	gtk_widget_show(w);

	uc->toplevel = w;
	dlg_add_uctrl(dp, uc);
    }

    return ret;
}

struct selparam {
    struct dlgparam *dp;
    Panels *panels;
    GtkWidget *panel, *treeitem;
    struct Shortcuts shortcuts;
};

static void treeitem_sel(GtkItem *item, gpointer data)
{
    struct selparam *sp = (struct selparam *)data;

    panels_switch_to(sp->panels, sp->panel);

    sp->dp->shortcuts = &sp->shortcuts;
    sp->dp->currtreeitem = sp->treeitem;
}

static void window_destroy(GtkWidget *widget, gpointer data)
{
    gtk_main_quit();
}

int win_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;

    if (event->keyval == GDK_Escape && dp->cancelbutton) {
	gtk_signal_emit_by_name(GTK_OBJECT(dp->cancelbutton), "clicked");
	return TRUE;
    }

    if ((event->state & GDK_MOD1_MASK) &&
	(unsigned char)event->string[0] > 0 &&
	(unsigned char)event->string[0] <= 127) {
	int schr = (unsigned char)event->string[0];
	struct Shortcut *sc = &dp->shortcuts->sc[schr];

	switch (sc->action) {
	  case SHORTCUT_FOCUS:
	    gtk_widget_grab_focus(sc->widget);
	    break;
	  case SHORTCUT_UCTRL:
	    /*
	     * We must do something sensible with a uctrl.
	     * Precisely what this is depends on the type of
	     * control.
	     */
	    switch (sc->uc->ctrl->generic.type) {
	      case CTRL_CHECKBOX:
	      case CTRL_BUTTON:
		/* Check boxes and buttons get the focus _and_ get toggled. */
		gtk_widget_grab_focus(sc->uc->toplevel);
		gtk_signal_emit_by_name(GTK_OBJECT(sc->uc->toplevel),
					"clicked");
		break;
	      case CTRL_FILESELECT:
	      case CTRL_FONTSELECT:
		/* File/font selectors have their buttons pressed (ooer),
		 * and focus transferred to the edit box. */
		gtk_signal_emit_by_name(GTK_OBJECT(sc->uc->button),
					"clicked");
		gtk_widget_grab_focus(sc->uc->entry);
		break;
	      case CTRL_RADIO:
		/*
		 * Radio buttons are fun, because they have
		 * multiple shortcuts. We must find whether the
		 * activated shortcut is the shortcut for the whole
		 * group, or for a particular button. In the former
		 * case, we find the currently selected button and
		 * focus it; in the latter, we focus-and-click the
		 * button whose shortcut was pressed.
		 */
		if (schr == sc->uc->ctrl->radio.shortcut) {
		    int i;
		    for (i = 0; i < sc->uc->ctrl->radio.nbuttons; i++)
			if (gtk_toggle_button_get_active
			    (GTK_TOGGLE_BUTTON(sc->uc->buttons[i]))) {
			    gtk_widget_grab_focus(sc->uc->buttons[i]);
			}
		} else if (sc->uc->ctrl->radio.shortcuts) {
		    int i;
		    for (i = 0; i < sc->uc->ctrl->radio.nbuttons; i++)
			if (schr == sc->uc->ctrl->radio.shortcuts[i]) {
			    gtk_widget_grab_focus(sc->uc->buttons[i]);
			    gtk_signal_emit_by_name
				(GTK_OBJECT(sc->uc->buttons[i]), "clicked");
			}
		}
		break;
	      case CTRL_LISTBOX:
		/*
		 * If the list is really an option menu, we focus
		 * and click it. Otherwise we tell it to focus one
		 * of its children, which appears to do the Right
		 * Thing.
		 */
		if (sc->uc->optmenu) {
		    GdkEventButton bev;
		    gint returnval;

		    gtk_widget_grab_focus(sc->uc->optmenu);
		    /* Option menus don't work using the "clicked" signal.
		     * We need to manufacture a button press event :-/ */
		    bev.type = GDK_BUTTON_PRESS;
		    bev.button = 1;
		    gtk_signal_emit_by_name(GTK_OBJECT(sc->uc->optmenu),
					    "button_press_event",
					    &bev, &returnval);
		} else {
                    assert(sc->uc->list != NULL);

                    gtk_container_focus(GTK_CONTAINER(sc->uc->list),
                                        GTK_DIR_TAB_FORWARD);
		}
		break;
	    }
	    break;
	}
    }

    return FALSE;
}

int tree_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;

    if (event->keyval == GDK_Up || event->keyval == GDK_KP_Up ||
        event->keyval == GDK_Down || event->keyval == GDK_KP_Down) {
        int i, j = -1;
        for (i = 0; i < dp->ntreeitems; i++)
            if (widget == dp->treeitems[i]) {
                if (event->keyval == GDK_Up || event->keyval == GDK_KP_Up) {
                    if (i > 0)
                        j = i-1;
                } else {
                    if (i < dp->ntreeitems-1)
                        j = i+1;
                }
                break;
            }
        gtk_signal_emit_stop_by_name(GTK_OBJECT(widget),
                                     "key_press_event");
        if (j >= 0) {
            gint return_val;
            gtk_signal_emit_by_name(GTK_OBJECT(dp->treeitems[j]), "toggle");
            gtk_widget_grab_focus(dp->treeitems[j]);
        }
        return TRUE;
    }

    return FALSE;
}

gint tree_focus(GtkContainer *container, GtkDirectionType direction,
                gpointer data)
{
    struct dlgparam *dp = (struct dlgparam *)data;
    int i, f, dir;

    gtk_signal_emit_stop_by_name(GTK_OBJECT(container), "focus");
    dir = (direction == GTK_DIR_UP || direction == GTK_DIR_LEFT ||
           direction == GTK_DIR_TAB_BACKWARD) ? -1 : +1;

    /*
     * See if any of the treeitems has the focus.
     */
    f = -1;
    for (i = 0; i < dp->ntreeitems; i++)
        if (GTK_WIDGET_HAS_FOCUS(dp->treeitems[i])) {
            f = i;
            break;
        }

    /*
     * If there's a focused treeitem, we return FALSE to cause the
     * focus to move on to some totally other control. If not, we
     * focus the selected one.
     */
    if (f >= 0)
        return FALSE;
    else {
        gtk_widget_grab_focus(dp->currtreeitem);
        return TRUE;
    }
}

void shortcut_add(struct Shortcuts *scs, GtkWidget *labelw,
		  int chr, int action, void *ptr)
{
    GtkLabel *label = GTK_LABEL(labelw);
    gchar *currstr, *pattern;
    int i;

    if (chr == NO_SHORTCUT)
	return;

    chr = tolower((unsigned char)chr);

    assert(scs->sc[chr].action == SHORTCUT_EMPTY);

    scs->sc[chr].action = action;

    if (action == SHORTCUT_FOCUS) {
	scs->sc[chr].uc = NULL;
	scs->sc[chr].widget = (GtkWidget *)ptr;
    } else {
	scs->sc[chr].widget = NULL;
	scs->sc[chr].uc = (struct uctrl *)ptr;
    }

    gtk_label_get(label, &currstr);
    for (i = 0; currstr[i]; i++)
	if (tolower((unsigned char)currstr[i]) == chr) {
	    GtkRequisition req;

	    pattern = dupprintf("%*s_", i, "");

	    gtk_widget_size_request(GTK_WIDGET(label), &req);
	    gtk_label_set_pattern(label, pattern);
	    gtk_widget_set_usize(GTK_WIDGET(label), -1, req.height);

	    sfree(pattern);
	    break;
	}
}

int do_config_box(void)
{
    GtkWidget *window, *hbox, *vbox, *cols, *label,
	*tree, *treescroll, *panels, *panelvbox;
    int index, level, listitemheight;
    struct controlbox *ctrlbox;
    char *path;
    GtkTreeItem *treeitemlevels[8];
    GtkTree *treelevels[8];
    Config cfg;
    struct dlgparam dp;
    struct sesslist sl;
    struct Shortcuts scs;

    struct selparam *selparams = NULL;
    int nselparams = 0, selparamsize = 0;

    do_defaults(NULL, &cfg);

    dlg_init(&dp);

    {
        GtkWidget *listitem = gtk_list_item_new_with_label("foo");
        GtkRequisition req;
        gtk_widget_size_request(listitem, &req);
        listitemheight = req.height;
        gtk_widget_unref(listitem);
    }

    sl.nsessions = 0;

    for (index = 0; index < lenof(scs.sc); index++) {
	scs.sc[index].action = SHORTCUT_EMPTY;
    }

    ctrlbox = ctrl_new_box();
    setup_config_box(ctrlbox, &sl, FALSE, 0);
    unix_setup_config_box(ctrlbox, FALSE);

    window = gtk_dialog_new();
    hbox = gtk_hbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(window)->vbox), hbox, TRUE, TRUE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
    gtk_widget_show(hbox);
    vbox = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);
    gtk_widget_show(vbox);
    cols = columns_new(4);
    gtk_box_pack_start(GTK_BOX(vbox), cols, FALSE, FALSE, 0);
    gtk_widget_show(cols);
    label = gtk_label_new("Category:");
    columns_add(COLUMNS(cols), label, 0, 1);
    columns_force_left_align(COLUMNS(cols), label);
    gtk_widget_show(label);
    treescroll = gtk_scrolled_window_new(NULL, NULL);
    tree = gtk_tree_new();
    gtk_signal_connect(GTK_OBJECT(tree), "focus_in_event",
                       GTK_SIGNAL_FUNC(widget_focus), &dp);
    shortcut_add(&scs, label, 'g', SHORTCUT_FOCUS, tree);
    gtk_tree_set_view_mode(GTK_TREE(tree), GTK_TREE_VIEW_ITEM);
    gtk_tree_set_selection_mode(GTK_TREE(tree), GTK_SELECTION_BROWSE);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(treescroll),
					  tree);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(treescroll),
				   GTK_POLICY_NEVER,
				   GTK_POLICY_AUTOMATIC);
    gtk_signal_connect(GTK_OBJECT(tree), "focus",
		       GTK_SIGNAL_FUNC(tree_focus), &dp);
    gtk_widget_show(tree);
    gtk_widget_show(treescroll);
    gtk_box_pack_start(GTK_BOX(vbox), treescroll, TRUE, TRUE, 0);
    panels = panels_new();
    gtk_box_pack_start(GTK_BOX(hbox), panels, TRUE, TRUE, 0);
    gtk_widget_show(panels);

    panelvbox = NULL;
    path = NULL;
    level = 0;
    for (index = 0; index < ctrlbox->nctrlsets; index++) {
	struct controlset *s = ctrlbox->ctrlsets[index];
	GtkWidget *w;

	if (!*s->pathname) {
	    w = layout_ctrls(&dp, &scs, s, listitemheight, GTK_WINDOW(window));
	    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(window)->action_area),
			       w, TRUE, TRUE, 0);
	} else {
	    int j = path ? ctrl_path_compare(s->pathname, path) : 0;
	    if (j != INT_MAX) {        /* add to treeview, start new panel */
		char *c;
		GtkWidget *treeitem;
		int first;

		/*
		 * We expect never to find an implicit path
		 * component. For example, we expect never to see
		 * A/B/C followed by A/D/E, because that would
		 * _implicitly_ create A/D. All our path prefixes
		 * are expected to contain actual controls and be
		 * selectable in the treeview; so we would expect
		 * to see A/D _explicitly_ before encountering
		 * A/D/E.
		 */
		assert(j == ctrl_path_elements(s->pathname) - 1);

		c = strrchr(s->pathname, '/');
		if (!c)
		    c = s->pathname;
		else
		    c++;

		treeitem = gtk_tree_item_new_with_label(c);
		assert(j-1 < level);
		if (j > 0) {
		    if (!treelevels[j-1]) {
			treelevels[j-1] = GTK_TREE(gtk_tree_new());
			gtk_tree_item_set_subtree
			    (treeitemlevels[j-1],
			     GTK_WIDGET(treelevels[j-1]));
			gtk_tree_item_expand(treeitemlevels[j-1]);
		    }
		    gtk_tree_append(treelevels[j-1], treeitem);
		} else {
		    gtk_tree_append(GTK_TREE(tree), treeitem);
		}
		treeitemlevels[j] = GTK_TREE_ITEM(treeitem);
		treelevels[j] = NULL;
		level = j+1;

                gtk_signal_connect(GTK_OBJECT(treeitem), "key_press_event",
                                   GTK_SIGNAL_FUNC(tree_key_press), &dp);
                gtk_signal_connect(GTK_OBJECT(treeitem), "focus_in_event",
                                   GTK_SIGNAL_FUNC(widget_focus), &dp);

		gtk_widget_show(treeitem);

		path = s->pathname;

		first = (panelvbox == NULL);

		panelvbox = gtk_vbox_new(FALSE, 4);
		gtk_container_add(GTK_CONTAINER(panels), panelvbox);
		if (first) {
		    panels_switch_to(PANELS(panels), panelvbox);
		    gtk_tree_select_child(GTK_TREE(tree), treeitem);
		}

		if (nselparams >= selparamsize) {
		    selparamsize += 16;
		    selparams = srealloc(selparams,
					 selparamsize * sizeof(*selparams));
		}
		selparams[nselparams].dp = &dp;
		selparams[nselparams].panels = PANELS(panels);
		selparams[nselparams].panel = panelvbox;
		selparams[nselparams].shortcuts = scs;   /* structure copy */
		selparams[nselparams].treeitem = treeitem;
		nselparams++;

	    }

	    w = layout_ctrls(&dp,
			     &selparams[nselparams-1].shortcuts,
			     s, listitemheight, NULL);
	    gtk_box_pack_start(GTK_BOX(panelvbox), w, FALSE, FALSE, 0);
            gtk_widget_show(w);
	}
    }

    dp.ntreeitems = nselparams;
    dp.treeitems = smalloc(dp.ntreeitems * sizeof(GtkWidget *));

    for (index = 0; index < nselparams; index++) {
	gtk_signal_connect(GTK_OBJECT(selparams[index].treeitem), "select",
			   GTK_SIGNAL_FUNC(treeitem_sel),
			   &selparams[index]);
        dp.treeitems[index] = selparams[index].treeitem;
    }

    dp.data = &cfg;
    dlg_refresh(NULL, &dp);

    dp.shortcuts = &selparams[0].shortcuts;
    dp.currtreeitem = dp.treeitems[0];
    dp.lastfocus = NULL;
    dp.retval = 0;
    dp.window = window;

    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_widget_show(window);

    /*
     * Set focus into the first available control.
     */
    for (index = 0; index < ctrlbox->nctrlsets; index++) {
	struct controlset *s = ctrlbox->ctrlsets[index];
        int done = 0;
        int j;

	if (*s->pathname) {
            for (j = 0; j < s->ncontrols; j++)
                if (s->ctrls[j]->generic.type != CTRL_TABDELAY &&
                    s->ctrls[j]->generic.type != CTRL_COLUMNS &&
                    s->ctrls[j]->generic.type != CTRL_TEXT) {
                    dlg_set_focus(s->ctrls[j], &dp);
                    dp.lastfocus = s->ctrls[j];
                    done = 1;
                    break;
                }
        }
        if (done)
            break;
    }

    gtk_signal_connect(GTK_OBJECT(window), "destroy",
		       GTK_SIGNAL_FUNC(window_destroy), NULL);
    gtk_signal_connect(GTK_OBJECT(window), "key_press_event",
		       GTK_SIGNAL_FUNC(win_key_press), &dp);

    gtk_main();

    dlg_cleanup(&dp);
    sfree(selparams);

    return dp.retval;
}

/* ======================================================================
 * Below here is a stub main program which allows the dialog box
 * code to be compiled and tested with a minimal amount of the rest
 * of PuTTY.
 */

#ifdef TESTMODE

/* Compile command for testing:

gcc -g -o gtkdlg gtk{dlg,cols,panel}.c ../{config,dialog,settings}.c \
                 ../{misc,tree234,be_none}.c ux{store,misc,print,cfg}.c \
                 -I. -I.. -I../charset -DTESTMODE `gtk-config --cflags --libs`
 */

void modalfatalbox(char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

char *cp_name(int codepage)
{
    return (codepage == 123 ? "testing123" :
            codepage == 234 ? "testing234" :
            codepage == 345 ? "testing345" :
            "unknown");
}

char *cp_enumerate(int index)
{
    return (index == 0 ? "testing123" :
            index == 1 ? "testing234" :
            NULL);
}

int decode_codepage(char *cp_name)
{
    return (!strcmp(cp_name, "testing123") ? 123 :
            !strcmp(cp_name, "testing234") ? 234 :
            !strcmp(cp_name, "testing345") ? 345 :
            -2);
}

struct printer_enum_tag { int dummy; } printer_test;

printer_enum *printer_start_enum(int *nprinters_ptr) {
    *nprinters_ptr = 2;
    return &printer_test;
}
char *printer_get_name(printer_enum *pe, int i) {
    return (i==0 ? "lpr" : i==1 ? "lpr -Pfoobar" : NULL);
}
void printer_finish_enum(printer_enum *pe) { }

char *platform_default_s(const char *name)
{
    return NULL;
}

int platform_default_i(const char *name, int def)
{
    return def;
}

FontSpec platform_default_fontspec(const char *name)
{
    FontSpec ret;
    if (!strcmp(name, "Font"))
	strcpy(ret.name, "fixed");
    else
	*ret.name = '\0';
    return ret;
}

Filename platform_default_filename(const char *name)
{
    Filename ret;
    if (!strcmp(name, "LogFileName"))
	strcpy(ret.path, "putty.log");
    else
	*ret.path = '\0';
    return ret;
}

char *x_get_default(const char *key)
{
    return NULL;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    printf("returned %d\n", do_config_box());
    return 0;
}

#endif
