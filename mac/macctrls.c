/* $Id: macctrls.c,v 1.9 2003/03/23 14:11:39 ben Exp $ */
/*
 * Copyright (c) 2003 Ben Harris
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <MacTypes.h>
#include <Appearance.h>
#include <Controls.h>
#include <ControlDefinitions.h>
#include <Menus.h>
#include <Resources.h>
#include <Sound.h>
#include <TextUtils.h>
#include <Windows.h>

#include <assert.h>
#include <string.h>

#include "putty.h"
#include "mac.h"
#include "macresid.h"
#include "dialog.h"
#include "tree234.h"

/* Range of menu IDs for popup menus */
#define MENU_MIN	1024
#define MENU_MAX	2048


union macctrl {
    struct macctrl_generic {
	enum {
	    MACCTRL_TEXT,
	    MACCTRL_RADIO,
	    MACCTRL_CHECKBOX,
	    MACCTRL_BUTTON,
	    MACCTRL_POPUP
	} type;
	/* Template from which this was generated */
	union control *ctrl;
	/* Next control in this panel */
	union macctrl *next;
    } generic;
    struct {
	struct macctrl_generic generic;
	ControlRef tbctrl;
    } text;
    struct {
	struct macctrl_generic generic;
	ControlRef *tbctrls;
    } radio;
    struct {
	struct macctrl_generic generic;
	ControlRef tbctrl;
    } checkbox;
    struct {
	struct macctrl_generic generic;
	ControlRef tbctrl;
    } button;
    struct {
	struct macctrl_generic generic;
	ControlRef tbctrl;
	MenuRef menu;
	int menuid;
	unsigned int nids;
	int *ids;
    } popup;
};

struct mac_layoutstate {
    Point pos;
    unsigned int width;
    unsigned int panelnum;
};

#define ctrlevent(mcs, mc, event) do {					\
    if ((mc)->generic.ctrl->generic.handler != NULL)			\
	(*(mc)->generic.ctrl->generic.handler)((mc)->generic.ctrl, (mcs),\
					       (mcs)->data, (event));	\
} while (0)

#define findbyctrl(mcs, ctrl)						\
    find234((mcs)->byctrl, (ctrl), macctrl_cmp_byctrl_find)

static void macctrl_layoutset(struct mac_layoutstate *, struct controlset *, 
			      WindowPtr, struct macctrls *);
static void macctrl_switchtopanel(struct macctrls *, unsigned int);
static void macctrl_text(struct macctrls *, WindowPtr,
			 struct mac_layoutstate *, union control *);
static void macctrl_radio(struct macctrls *, WindowPtr,
			  struct mac_layoutstate *, union control *);
static void macctrl_checkbox(struct macctrls *, WindowPtr,
			     struct mac_layoutstate *, union control *);
static void macctrl_button(struct macctrls *, WindowPtr,
			   struct mac_layoutstate *, union control *);
static void macctrl_popup(struct macctrls *, WindowPtr,
			  struct mac_layoutstate *, union control *);
#if !TARGET_API_MAC_CARBON
static pascal SInt32 macctrl_sys7_text_cdef(SInt16, ControlRef,
					    ControlDefProcMessage, SInt32);
static pascal SInt32 macctrl_sys7_default_cdef(SInt16, ControlRef,
					       ControlDefProcMessage, SInt32);
#endif

#if !TARGET_API_MAC_CARBON
/*
 * This trick enables us to keep all the CDEF code in the main
 * application, which makes life easier.  For details, see
 * <http://developer.apple.com/technotes/tn/tn2003.html#custom_code_base>.
 */

#pragma options align=mac68k
typedef struct {
    short		jmpabs;	/* 4EF9 */
    ControlDefUPP	theUPP;
} **PatchCDEF;
#pragma options align=reset
#endif

static void macctrl_init()
{
#if !TARGET_API_MAC_CARBON
    static int inited = 0;
    PatchCDEF cdef;

    if (inited) return;
    cdef = (PatchCDEF)GetResource(kControlDefProcResourceType, CDEF_Text);
    (*cdef)->theUPP = NewControlDefProc(macctrl_sys7_text_cdef);
    cdef = (PatchCDEF)GetResource(kControlDefProcResourceType, CDEF_Default);
    (*cdef)->theUPP = NewControlDefProc(macctrl_sys7_default_cdef);
    inited = 1;
#endif
}


static int macctrl_cmp_byctrl(void *av, void *bv)
{
    union macctrl *a = (union macctrl *)av;
    union macctrl *b = (union macctrl *)bv;

    if (a->generic.ctrl < b->generic.ctrl)
	return -1;
    else if (a->generic.ctrl > b->generic.ctrl)
	return +1;
    else
	return 0;
}

static int macctrl_cmp_byctrl_find(void *av, void *bv)
{
    union control *a = (union control *)av;
    union macctrl *b = (union macctrl *)bv;

    if (a < b->generic.ctrl)
	return -1;
    else if (a > b->generic.ctrl)
	return +1;
    else
	return 0;
}

void macctrl_layoutbox(struct controlbox *cb, WindowPtr window,
		       struct macctrls *mcs)
{
    int i;
    struct mac_layoutstate curstate;
    ControlRef root;
    Rect rect;

    macctrl_init();
#if TARGET_API_MAC_CARBON
    GetPortBounds(GetWindowPort(window), &rect);
#else
    rect = window->portRect;
#endif
    curstate.pos.h = rect.left + 13;
    curstate.pos.v = rect.bottom - 59;
    curstate.width = rect.right - rect.left - (13 * 2);
    if (mac_gestalts.apprvers >= 0x100)
	CreateRootControl(window, &root);
    mcs->byctrl = newtree234(macctrl_cmp_byctrl);
    /* Count the number of panels */
    mcs->npanels = 1;
    for (i = 1; i < cb->nctrlsets; i++)
	if (strcmp(cb->ctrlsets[i]->pathname, cb->ctrlsets[i-1]->pathname))
	    mcs->npanels++;
    mcs->panels = smalloc(sizeof(*mcs->panels) * mcs->npanels);
    memset(mcs->panels, 0, sizeof(*mcs->panels) * mcs->npanels);
    curstate.panelnum = 0;
    for (i = 0; i < cb->nctrlsets; i++) {
	if (i > 0 && strcmp(cb->ctrlsets[i]->pathname,
			    cb->ctrlsets[i-1]->pathname)) {
	    curstate.pos.v = rect.top + 13;
	    curstate.panelnum++;
	    assert(curstate.panelnum < mcs->npanels);
	}
	macctrl_layoutset(&curstate, cb->ctrlsets[i], window, mcs);
    }
    macctrl_switchtopanel(mcs, 20);
}

static void macctrl_layoutset(struct mac_layoutstate *curstate,
			      struct controlset *s,
			      WindowPtr window, struct macctrls *mcs)
{
    unsigned int i;

    fprintf(stderr, "--- begin set ---\n");
    fprintf(stderr, "pathname = %s\n", s->pathname);
    if (s->boxname && *s->boxname)
	fprintf(stderr, "boxname = %s\n", s->boxname);
    if (s->boxtitle)
	fprintf(stderr, "boxtitle = %s\n", s->boxtitle);


    for (i = 0; i < s->ncontrols; i++) {
	union control *ctrl = s->ctrls[i];
	char const *s;

	switch (ctrl->generic.type) {
	  case CTRL_TEXT: s = "text"; break;
	  case CTRL_EDITBOX: s = "editbox"; break;
	  case CTRL_RADIO: s = "radio"; break;
	  case CTRL_CHECKBOX: s = "checkbox"; break;
	  case CTRL_BUTTON: s = "button"; break;
	  case CTRL_LISTBOX: s = "listbox"; break;
	  case CTRL_COLUMNS: s = "columns"; break;
	  case CTRL_FILESELECT: s = "fileselect"; break;
	  case CTRL_FONTSELECT: s = "fontselect"; break;
	  case CTRL_TABDELAY: s = "tabdelay"; break;
	  default: s = "unknown"; break;
	}
	fprintf(stderr, "  control: %s\n", s);
	switch (ctrl->generic.type) {
	  case CTRL_TEXT:
	    macctrl_text(mcs, window, curstate, ctrl);
	    break;
	  case CTRL_RADIO:
	    macctrl_radio(mcs, window, curstate, ctrl);
	    break;
	  case CTRL_CHECKBOX:
	    macctrl_checkbox(mcs, window, curstate, ctrl);
	    break;
	  case CTRL_BUTTON:
	    macctrl_button(mcs, window, curstate, ctrl);
	    break;
	  case CTRL_LISTBOX:
	    if (ctrl->listbox.height == 0)
		macctrl_popup(mcs, window, curstate, ctrl);
	    break;
	}
    }
}

static void macctrl_switchtopanel(struct macctrls *mcs, unsigned int which)
{
    unsigned int i, j;
    union macctrl *mc;

    /* Panel 0 is special and always visible. */
    for (i = 1; i < mcs->npanels; i++)
	for (mc = mcs->panels[i]; mc != NULL; mc = mc->generic.next)
	    switch (mc->generic.type) {
	      case MACCTRL_TEXT:
		if (i == which)
		    ShowControl(mc->text.tbctrl);
		else
		    HideControl(mc->text.tbctrl);
		break;
	      case MACCTRL_RADIO:
		for (j = 0; j < mc->generic.ctrl->radio.nbuttons; j++)
		    if (i == which)
			ShowControl(mc->radio.tbctrls[j]);
		    else
			HideControl(mc->radio.tbctrls[j]);
		break;
	      case MACCTRL_CHECKBOX:
		if (i == which)
		    ShowControl(mc->checkbox.tbctrl);
		else
		    HideControl(mc->checkbox.tbctrl);
		break;
	      case MACCTRL_BUTTON:
		if (i == which)
		    ShowControl(mc->button.tbctrl);
		else
		    HideControl(mc->button.tbctrl);
		break;

	    }
}

static void macctrl_text(struct macctrls *mcs, WindowPtr window,
			 struct mac_layoutstate *curstate,
			 union control *ctrl)
{
    union macctrl *mc = smalloc(sizeof *mc);
    Rect bounds;

    fprintf(stderr, "    label = %s\n", ctrl->text.label);
    mc->generic.type = MACCTRL_TEXT;
    mc->generic.ctrl = ctrl;
    bounds.left = curstate->pos.h;
    bounds.right = bounds.left + curstate->width;
    bounds.top = curstate->pos.v;
    bounds.bottom = bounds.top + 16;
    if (mac_gestalts.apprvers >= 0x100) {
	SInt16 height;
	Size olen;

	mc->text.tbctrl = NewControl(window, &bounds, NULL, TRUE, 0, 0, 0,
				     kControlStaticTextProc, (long)mc);
	SetControlData(mc->text.tbctrl, kControlEntireControl,
		       kControlStaticTextTextTag,
		       strlen(ctrl->text.label), ctrl->text.label);
	GetControlData(mc->text.tbctrl, kControlEntireControl,
		       kControlStaticTextTextHeightTag,
		       sizeof(height), &height, &olen);
	fprintf(stderr, "    height = %d\n", height);
	SizeControl(mc->text.tbctrl, curstate->width, height);
	curstate->pos.v += height + 6;
    } else {
	Str255 title;

	c2pstrcpy(title, ctrl->text.label);
	mc->text.tbctrl = NewControl(window, &bounds, title, TRUE, 0, 0, 0,
				     SYS7_TEXT_PROC, (long)mc);
    }
    add234(mcs->byctrl, mc);
    mc->generic.next = mcs->panels[curstate->panelnum];
    mcs->panels[curstate->panelnum] = mc;
}

#if !TARGET_API_MAC_CARBON
static pascal SInt32 macctrl_sys7_text_cdef(SInt16 variant, ControlRef control,
				     ControlDefProcMessage msg, SInt32 param)
{
    RgnHandle rgn;

    switch (msg) {
      case drawCntl:
	if ((*control)->contrlVis)
	    TETextBox((*control)->contrlTitle + 1, (*control)->contrlTitle[0],
		      &(*control)->contrlRect, teFlushDefault);
	return 0;
      case calcCRgns:
	if (param & (1 << 31)) {
	    param &= ~(1 << 31);
	    goto calcthumbrgn;
	}
	/* FALLTHROUGH */
      case calcCntlRgn:
	rgn = (RgnHandle)param;
	RectRgn(rgn, &(*control)->contrlRect);
	return 0;
      case calcThumbRgn:
      calcthumbrgn:
	rgn = (RgnHandle)param;
	SetEmptyRgn(rgn);
	return 0;
    }

    return 0;
}
#endif

static void macctrl_radio(struct macctrls *mcs, WindowPtr window,
			  struct mac_layoutstate *curstate,
			  union control *ctrl)
{
    union macctrl *mc = smalloc(sizeof *mc);
    Rect bounds;
    Str255 title;
    unsigned int i, colwidth;

    fprintf(stderr, "    label = %s\n", ctrl->radio.label);
    mc->generic.type = MACCTRL_RADIO;
    mc->generic.ctrl = ctrl;
    mc->radio.tbctrls =
	smalloc(sizeof(*mc->radio.tbctrls) * ctrl->radio.nbuttons);
    colwidth = (curstate->width + 13) /	ctrl->radio.ncolumns;
    for (i = 0; i < ctrl->radio.nbuttons; i++) {
	fprintf(stderr, "    button = %s\n", ctrl->radio.buttons[i]);
	bounds.top = curstate->pos.v;
	bounds.bottom = bounds.top + 16;
	bounds.left = curstate->pos.h + colwidth * (i % ctrl->radio.ncolumns);
	if (i == ctrl->radio.nbuttons - 1 ||
	    i % ctrl->radio.ncolumns == ctrl->radio.ncolumns - 1) {
	    bounds.right = curstate->pos.h + curstate->width;
	    curstate->pos.v += 22;
	} else
	    bounds.right = bounds.left + colwidth - 13;
	c2pstrcpy(title, ctrl->radio.buttons[i]);
	mc->radio.tbctrls[i] = NewControl(window, &bounds, title, TRUE,
					  0, 0, 1, radioButProc, (long)mc);
    }
    add234(mcs->byctrl, mc);
    mc->generic.next = mcs->panels[curstate->panelnum];
    mcs->panels[curstate->panelnum] = mc;
    ctrlevent(mcs, mc, EVENT_REFRESH);
}

static void macctrl_checkbox(struct macctrls *mcs, WindowPtr window,
			     struct mac_layoutstate *curstate,
			     union control *ctrl)
{
    union macctrl *mc = smalloc(sizeof *mc);
    Rect bounds;
    Str255 title;

    fprintf(stderr, "    label = %s\n", ctrl->checkbox.label);
    mc->generic.type = MACCTRL_CHECKBOX;
    mc->generic.ctrl = ctrl;
    bounds.left = curstate->pos.h;
    bounds.right = bounds.left + curstate->width;
    bounds.top = curstate->pos.v;
    bounds.bottom = bounds.top + 16;
    c2pstrcpy(title, ctrl->checkbox.label);
    mc->checkbox.tbctrl = NewControl(window, &bounds, title, TRUE, 0, 0, 1,
				     checkBoxProc, (long)mc);
    add234(mcs->byctrl, mc);
    curstate->pos.v += 22;
    mc->generic.next = mcs->panels[curstate->panelnum];
    mcs->panels[curstate->panelnum] = mc;
    ctrlevent(mcs, mc, EVENT_REFRESH);
}

static void macctrl_button(struct macctrls *mcs, WindowPtr window,
			   struct mac_layoutstate *curstate,
			   union control *ctrl)
{
    union macctrl *mc = smalloc(sizeof *mc);
    Rect bounds;
    Str255 title;

    fprintf(stderr, "    label = %s\n", ctrl->button.label);
    if (ctrl->button.isdefault)
	fprintf(stderr, "    is default\n");
    mc->generic.type = MACCTRL_BUTTON;
    mc->generic.ctrl = ctrl;
    bounds.left = curstate->pos.h;
    bounds.right = bounds.left + 100; /* XXX measure string */
    bounds.top = curstate->pos.v;
    bounds.bottom = bounds.top + 20;
    c2pstrcpy(title, ctrl->button.label);
    mc->button.tbctrl = NewControl(window, &bounds, title, TRUE, 0, 0, 1,
				   pushButProc, (long)mc);
    if (mac_gestalts.apprvers >= 0x100) {
	Boolean isdefault = ctrl->button.isdefault;

	SetControlData(mc->button.tbctrl, kControlEntireControl,
		       kControlPushButtonDefaultTag,
		       sizeof(isdefault), &isdefault);
    } else if (ctrl->button.isdefault) {
	InsetRect(&bounds, -4, -4);
	NewControl(window, &bounds, title, TRUE, 0, 0, 1,
		   SYS7_DEFAULT_PROC, (long)mc);
    }
    if (mac_gestalts.apprvers >= 0x110) {
	Boolean iscancel = ctrl->button.iscancel;

	SetControlData(mc->button.tbctrl, kControlEntireControl,
		       kControlPushButtonCancelTag,
		       sizeof(iscancel), &iscancel);
    }
    add234(mcs->byctrl, mc);
    mc->generic.next = mcs->panels[curstate->panelnum];
    mcs->panels[curstate->panelnum] = mc;
    curstate->pos.v += 26;
}

#if !TARGET_API_MAC_CARBON
static pascal SInt32 macctrl_sys7_default_cdef(SInt16 variant,
					       ControlRef control,
					       ControlDefProcMessage msg,
					       SInt32 param)
{
    RgnHandle rgn;
    Rect rect;
    int oval;

    switch (msg) {
      case drawCntl:
	if ((*control)->contrlVis) {
	    rect = (*control)->contrlRect;
	    PenNormal();
	    PenSize(3, 3);
	    oval = (rect.bottom - rect.top) / 2 + 2;
	    FrameRoundRect(&rect, oval, oval);
	}
	return 0;
      case calcCRgns:
	if (param & (1 << 31)) {
	    param &= ~(1 << 31);
	    goto calcthumbrgn;
	}
	/* FALLTHROUGH */
      case calcCntlRgn:
	rgn = (RgnHandle)param;
	RectRgn(rgn, &(*control)->contrlRect);
	return 0;
      case calcThumbRgn:
      calcthumbrgn:
	rgn = (RgnHandle)param;
	SetEmptyRgn(rgn);
	return 0;
    }

    return 0;
}
#endif

static void macctrl_popup(struct macctrls *mcs, WindowPtr window,
			  struct mac_layoutstate *curstate,
			  union control *ctrl)
{
    union macctrl *mc = smalloc(sizeof *mc);
    Rect bounds;
    Str255 title;
    unsigned int labelwidth;
    static int nextmenuid;
    int menuid;
    MenuRef menu;

    /* 
     * <http://developer.apple.com/qa/tb/tb42.html> explains how to
     * create a popup menu with dynamic content.
     */
    assert(ctrl->listbox.height == 0);
    assert(!ctrl->listbox.draglist);
    assert(!ctrl->listbox.multisel);

    fprintf(stderr, "    label = %s\n", ctrl->listbox.label);
    fprintf(stderr, "    percentwidth = %d\n", ctrl->listbox.percentwidth);

    mc->generic.type = MACCTRL_POPUP;
    mc->generic.ctrl = ctrl;
    c2pstrcpy(title, ctrl->button.label);

    /* Find a spare menu ID and create the menu */
    while (GetMenuHandle(nextmenuid) != NULL)
	if (++nextmenuid >= MENU_MAX) nextmenuid = MENU_MIN;
    menuid = nextmenuid++;
    menu = NewMenu(menuid, "\pdummy");
    if (menu == NULL) return;
    mc->popup.menu = menu;
    mc->popup.menuid = menuid;
    InsertMenu(menu, kInsertHierarchicalMenu);

    /* The menu starts off empty */
    mc->popup.nids = 0;
    mc->popup.ids = NULL;

    bounds.left = curstate->pos.h;
    bounds.right = bounds.left + curstate->width;
    bounds.top = curstate->pos.v;
    bounds.bottom = bounds.top + 20;
    /* XXX handle percentwidth == 100 */
    labelwidth = curstate->width * (100 - ctrl->listbox.percentwidth) / 100;
    mc->popup.tbctrl = NewControl(window, &bounds, title, TRUE,
				  popupTitleLeftJust, menuid, labelwidth,
				  popupMenuProc + popupFixedWidth, (long)mc);
    add234(mcs->byctrl, mc);
    curstate->pos.v += 26;
    mc->generic.next = mcs->panels[curstate->panelnum];
    mcs->panels[curstate->panelnum] = mc;
    ctrlevent(mcs, mc, EVENT_REFRESH);
}


void macctrl_activate(WindowPtr window, EventRecord *event)
{
    Boolean active = (event->modifiers & activeFlag) != 0;
    GrafPtr saveport;
    ControlRef root;

    GetPort(&saveport);
    SetPort((GrafPtr)GetWindowPort(window));
    if (mac_gestalts.apprvers >= 0x100) {
	SetThemeWindowBackground(window, active ?
				 kThemeBrushModelessDialogBackgroundActive :
				 kThemeBrushModelessDialogBackgroundInactive,
				 TRUE);
	GetRootControl(window, &root);
	if (active)
	    ActivateControl(root);
	else
	    DeactivateControl(root);
    } else {
	/* (De)activate controls one at a time */
    }
    SetPort(saveport);
}

void macctrl_click(WindowPtr window, EventRecord *event)
{
    Point mouse;
    ControlHandle control;
    int part;
    GrafPtr saveport;
    union macctrl *mc;
    struct macctrls *mcs = mac_winctrls(window);
    int i;

    GetPort(&saveport);
    SetPort((GrafPtr)GetWindowPort(window));
    mouse = event->where;
    GlobalToLocal(&mouse);
    part = FindControl(mouse, window, &control);
    if (control != NULL) {
	mc = (union macctrl *)GetControlReference(control);
	switch (mc->generic.type) {
	  case MACCTRL_POPUP:
	    TrackControl(control, mouse, (ControlActionUPP)-1);
	    ctrlevent(mcs, mc, EVENT_SELCHANGE);
	  case MACCTRL_RADIO:
	    if (TrackControl(control, mouse, NULL) != 0) {
		for (i = 0; i < mc->generic.ctrl->radio.nbuttons; i++)
		    if (mc->radio.tbctrls[i] == control)
			SetControlValue(mc->radio.tbctrls[i],
					kControlRadioButtonCheckedValue);
		    else
			SetControlValue(mc->radio.tbctrls[i],
					kControlRadioButtonUncheckedValue);
		ctrlevent(mcs, mc, EVENT_VALCHANGE);
	    }
	    break;
	  case MACCTRL_CHECKBOX:
	    if (TrackControl(control, mouse, NULL) != 0) {
		SetControlValue(control, !GetControlValue(control));
		ctrlevent(mcs, mc, EVENT_VALCHANGE);
	    }
	    break;
	  case MACCTRL_BUTTON:
	    if (TrackControl(control, mouse, NULL) != 0)
		ctrlevent(mcs, mc, EVENT_ACTION);
	    break;
	}
    }
    SetPort(saveport);
}

void macctrl_update(WindowPtr window)
{
#if TARGET_API_MAC_CARBON
    RgnHandle visrgn;
#endif
    Rect rect;
    GrafPtr saveport;

    BeginUpdate(window);
    GetPort(&saveport);
    SetPort((GrafPtr)GetWindowPort(window));
    if (mac_gestalts.apprvers >= 0x101) {
#if TARGET_API_MAC_CARBON
	GetPortBounds(GetWindowPort(window), &rect);
#else
	rect = window->portRect;
#endif
	InsetRect(&rect, -1, -1);
	DrawThemeModelessDialogFrame(&rect, mac_frontwindow() == window ?
				     kThemeStateActive : kThemeStateInactive);
    }
#if TARGET_API_MAC_CARBON
    visrgn = NewRgn();
    GetPortVisibleRegion(GetWindowPort(window), visrgn);
    UpdateControls(window, visrgn);
    DisposeRgn(visrgn);
#else
    UpdateControls(window, window->visRgn);
#endif
    SetPort(saveport);
    EndUpdate(window);
}

#if TARGET_API_MAC_CARBON
#define EnableItem EnableMenuItem
#define DisableItem DisableMenuItem
#endif
void macctrl_adjustmenus(WindowPtr window)
{
    MenuHandle menu;

    menu = GetMenuHandle(mFile);
    DisableItem(menu, iSave); /* XXX enable if modified */
    EnableItem(menu, iSaveAs);
    EnableItem(menu, iDuplicate);

    menu = GetMenuHandle(mEdit);
    DisableItem(menu, 0);
}

void macctrl_close(WindowPtr window)
{
    struct macctrls *mcs = mac_winctrls(window);
    union macctrl *mc;

    /*
     * Mostly, we don't bother disposing of the Toolbox controls,
     * since that will happen automatically when the window is
     * disposed of.  Popup menus are an exception, because we have to
     * dispose of the menu ourselves, and doing that while the control
     * still holds a reference to it seems rude.
     */
    while ((mc = index234(mcs->byctrl, 0)) != NULL) {
	switch (mc->generic.type) {
	  case MACCTRL_POPUP:
	    DisposeControl(mc->popup.tbctrl);
	    DeleteMenu(mc->popup.menuid);
	    DisposeMenu(mc->popup.menu);
	    break;
	}
	del234(mcs->byctrl, mc);
	sfree(mc);
    }

    freetree234(mcs->byctrl);
    mcs->byctrl = NULL;
    sfree(mcs->panels);
    mcs->panels = NULL;
}

void dlg_update_start(union control *ctrl, void *dlg)
{

    /* No-op for now */
}

void dlg_update_done(union control *ctrl, void *dlg)
{

    /* No-op for now */
}

void dlg_set_focus(union control *ctrl, void *dlg)
{

    if (mac_gestalts.apprvers >= 0x100) {
	/* Use SetKeyboardFocus() */
    } else {
	/* Do our own mucking around */
    }
}

union control *dlg_last_focused(union control *ctrl, void *dlg)
{

    return NULL;
}

void dlg_beep(void *dlg)
{

    SysBeep(30);
}

void dlg_error_msg(void *dlg, char *msg)
{
    Str255 pmsg;

    c2pstrcpy(pmsg, msg);
    ParamText(pmsg, NULL, NULL, NULL);
    StopAlert(128, NULL);
}

void dlg_end(void *dlg, int value)
{

};

void dlg_refresh(union control *ctrl, void *dlg)
{

};

void *dlg_get_privdata(union control *ctrl, void *dlg)
{

    return NULL;
}

void dlg_set_privdata(union control *ctrl, void *dlg, void *ptr)
{

    fatalbox("dlg_set_privdata");
}

void *dlg_alloc_privdata(union control *ctrl, void *dlg, size_t size)
{

    fatalbox("dlg_alloc_privdata");
}


/*
 * Radio Button control
 */

void dlg_radiobutton_set(union control *ctrl, void *dlg, int whichbutton)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);
    int i;

    assert(mc != NULL);
    for (i = 0; i < ctrl->radio.nbuttons; i++) {
	if (i == whichbutton)
	    SetControlValue(mc->radio.tbctrls[i],
			    kControlRadioButtonCheckedValue);
	else
	    SetControlValue(mc->radio.tbctrls[i],
			    kControlRadioButtonUncheckedValue);
    }

};

int dlg_radiobutton_get(union control *ctrl, void *dlg)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);
    int i;

    assert(mc != NULL);
    for (i = 0; i < ctrl->radio.nbuttons; i++) {
	if (GetControlValue(mc->radio.tbctrls[i])  ==
	    kControlRadioButtonCheckedValue)
	    return i;
    }
    return -1;
};


/*
 * Check Box control
 */

void dlg_checkbox_set(union control *ctrl, void *dlg, int checked)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);

    assert(mc != NULL);
    SetControlValue(mc->checkbox.tbctrl,
		    checked ? kControlCheckBoxCheckedValue :
		              kControlCheckBoxUncheckedValue);
}

int dlg_checkbox_get(union control *ctrl, void *dlg)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);

    assert(mc != NULL);
    return GetControlValue(mc->checkbox.tbctrl);
}


/*
 * Edit Box control
 */

void dlg_editbox_set(union control *ctrl, void *dlg, char const *text)
{

};

void dlg_editbox_get(union control *ctrl, void *dlg, char *buffer, int length)
{

};


/*
 * List Box control
 */

static void dlg_macpopup_clear(union control *ctrl, void *dlg)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);
    MenuRef menu = mc->popup.menu;
    unsigned int i, n;

    fprintf(stderr, "      popup_clear\n");
    n = CountMItems(menu);
    for (i = 0; i < n; i++)
	DeleteMenuItem(menu, n - i);
    mc->popup.nids = 0;
    sfree(mc->popup.ids);
    mc->popup.ids = NULL;
    SetControlMaximum(mc->popup.tbctrl, CountMItems(menu));
}

void dlg_listbox_clear(union control *ctrl, void *dlg)
{

    if (ctrl->listbox.height == 0)
	dlg_macpopup_clear(ctrl, dlg);
}

static void dlg_macpopup_del(union control *ctrl, void *dlg, int index)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);
    MenuRef menu = mc->popup.menu;

    fprintf(stderr, "      popup_del %d\n", index);
    DeleteMenuItem(menu, index + 1);
    if (mc->popup.ids != NULL)
	memcpy(mc->popup.ids + index, mc->popup.ids + index + 1,
	       (mc->popup.nids - index - 1) * sizeof(*mc->popup.ids));
    SetControlMaximum(mc->popup.tbctrl, CountMItems(menu));
}

void dlg_listbox_del(union control *ctrl, void *dlg, int index)
{

    if (ctrl->listbox.height == 0)
	dlg_macpopup_del(ctrl, dlg, index);
}

static void dlg_macpopup_add(union control *ctrl, void *dlg, char const *text)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);
    MenuRef menu = mc->popup.menu;
    Str255 itemstring;

    fprintf(stderr, "      popup_add %s\n", text);
    assert(text[0] != '\0');
    c2pstrcpy(itemstring, text);
    AppendMenu(menu, "\pdummy");
    SetMenuItemText(menu, CountMItems(menu), itemstring);
    SetControlMaximum(mc->popup.tbctrl, CountMItems(menu));
}

void dlg_listbox_add(union control *ctrl, void *dlg, char const *text)
{

    if (ctrl->listbox.height == 0)
	dlg_macpopup_add(ctrl, dlg, text);
}

static void dlg_macpopup_addwithindex(union control *ctrl, void *dlg,
				      char const *text, int id)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);
    MenuRef menu = mc->popup.menu;
    unsigned int index;

    fprintf(stderr, "      popup_addwthindex %s, %d\n", text, id);
    dlg_macpopup_add(ctrl, dlg, text);
    index = CountMItems(menu) - 1;
    if (mc->popup.nids <= index) {
	mc->popup.nids = index + 1;
	mc->popup.ids = srealloc(mc->popup.ids,
				 mc->popup.nids * sizeof(*mc->popup.ids));
    }
    mc->popup.ids[index] = id;
}

void dlg_listbox_addwithindex(union control *ctrl, void *dlg,
			      char const *text, int id)
{

    if (ctrl->listbox.height == 0)
	dlg_macpopup_addwithindex(ctrl, dlg, text, id);
}

int dlg_listbox_getid(union control *ctrl, void *dlg, int index)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);

    if (ctrl->listbox.height == 0) {
	assert(mc->popup.ids != NULL && mc->popup.nids > index);
	return mc->popup.ids[index];
    }
    return 0;
}

int dlg_listbox_index(union control *ctrl, void *dlg)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);

    if (ctrl->listbox.height == 0)
	return GetControlValue(mc->popup.tbctrl) - 1;
    return 0;
};

int dlg_listbox_issel(union control *ctrl, void *dlg, int index)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);

    if (ctrl->listbox.height == 0)
	return GetControlValue(mc->popup.tbctrl) - 1 == index;
    return 0;
};

void dlg_listbox_select(union control *ctrl, void *dlg, int index)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);

    if (ctrl->listbox.height == 0)
	SetControlValue(mc->popup.tbctrl, index + 1);
};


/*
 * Text control
 */

void dlg_text_set(union control *ctrl, void *dlg, char const *text)
{
    struct macctrls *mcs = dlg;
    union macctrl *mc = findbyctrl(mcs, ctrl);
    Str255 title;

    assert(mc != NULL);
    if (mac_gestalts.apprvers >= 0x100)
	SetControlData(mc->text.tbctrl, kControlEntireControl,
		       kControlStaticTextTextTag,
		       strlen(ctrl->text.label), ctrl->text.label);
    else {
	c2pstrcpy(title, text);
	SetControlTitle(mc->text.tbctrl, title);
    }
}


/*
 * File Selector control
 */

void dlg_filesel_set(union control *ctrl, void *dlg, Filename fn)
{

}

void dlg_filesel_get(union control *ctrl, void *dlg, Filename *fn)
{

}


/*
 * Font Selector control
 */

void dlg_fontsel_set(union control *ctrl, void *dlg, FontSpec fn)
{

}

void dlg_fontsel_get(union control *ctrl, void *dlg, FontSpec *fn)
{

}


/*
 * Printer enumeration
 */

printer_enum *printer_start_enum(int *nprinters)
{

    *nprinters = 0;
    return NULL;
}

char *printer_get_name(printer_enum *pe, int thing)
{

    return "<none>";
}

void printer_finish_enum(printer_enum *pe)
{

}


/*
 * Colour selection stuff
 */

void dlg_coloursel_start(union control *ctrl, void *dlg,
			 int r, int g, int b)
{

}

int dlg_coloursel_results(union control *ctrl, void *dlg,
			  int *r, int *g, int *b)
{

    return 0;
}

/*
 * Local Variables:
 * c-file-style: "simon"
 * End:
 */