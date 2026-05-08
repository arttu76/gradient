/*
 * gradient.c -- interactive copper-list editor for the Workbench palette.
 *
 * Build:  vc +aos68k -O1 -c99 -o gradient gradient.c -lamiga
 * Target: ECS, KS/WB 3.1, 68000 (Amiga 600).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <libraries/gadtools.h>
#include <libraries/commodities.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <graphics/gfxbase.h>
#include <graphics/gfxmacros.h>
#include <graphics/copper.h>
#include <graphics/view.h>
#include <graphics/text.h>
#include <hardware/custom.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/commodities.h>
#include <proto/icon.h>

#include <string.h>
#include <stdio.h>

/* ----------------------------------------------- daemon / persistence */

#define CONFIG_MAGIC      0x47524443UL    /* 'GRDC' */
#define CONFIG_VERSION    1
#define CONFIG_PATH_LIVE  "ENV:Gradient.prefs"
#define CONFIG_PATH_SAVE  "ENVARC:Gradient.prefs"
#define BROKER_NAME       "Gradient"
#define BROKER_TITLE      "Gradient 1.1"
#define BROKER_DESCR      "Workbench palette gradient"

/* AmigaDOS Version cookie -- `Version Gradient` searches for "$VER:". */
static const char version_cookie[] = "$VER: Gradient 1.1";
/* Force the linker to keep the cookie even though nothing else
 * references it at runtime. */
const char * const _gradient_keep_vc = version_cookie;

#define NCOLORS         4
#define MAX_STOPS       8
#define MIN_STOPS       2
#define STOPS_PER_ROW   2
#define STOP_LABEL_LEN  20

#define GID_REG         1
#define GID_ENABLE      2
#define GID_DITHER      3
#define GID_DIRECTION   4
#define GID_Y           5
#define GID_R           6
#define GID_G           7
#define GID_B           8
#define GID_ADD_BEFORE  9
#define GID_DEL_THIS    10
#define GID_ADD_AFTER   11
#define GID_SAVE        12
#define GID_USE         13
#define GID_CANCEL      14
#define GID_STOP_BASE   100

/* Editor exit dispositions, set by Save/Use/Cancel buttons (and the
 * close gadget / Ctrl-C, which both mean Cancel). */
#define EXIT_CANCEL     0
#define EXIT_USE        1   /* write ENV: only */
#define EXIT_SAVE       2   /* write ENV: AND ENVARC: */

/* Menu item user-data tags. */
#define MEN_ABOUT       1
#define MEN_SAVE        2
#define MEN_USE         3
#define MEN_CANCEL      4

#define WIN_W           320

extern struct Custom custom;

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;
struct Library       *CxBase        = NULL;
struct Library       *IconBase      = NULL;

/* vbcc startup exposes the Workbench launch message here; NULL when the
 * program was started from a CLI/Shell. We read it to find tooltypes. */
extern struct WBStartup *_WBenchMsg;

/* ------------------------------------------------------------ data model */

struct Stop {
    UWORD y;        /* 0..100 percent of screen height */
    UWORD r, g, b;  /* each 0..15 (ECS) */
};

#define DIR_NORMAL      0
#define DIR_REVERSED    1               /* flip gradient: y=100 at top */

struct ColorState {
    BOOL  enabled;
    UWORD dither;       /* 0 = off, 1 = vertical dither on */
    UWORD direction;
    UWORD count;
    struct Stop stops[MAX_STOPS];
};

static struct ColorState cstate[NCOLORS];
static UWORD curr_reg  = 0;
static UWORD curr_stop = 0;

static void init_state(void)
{
    int i;
    for (i = 0; i < NCOLORS; i++) {
        cstate[i].enabled     = FALSE;
        cstate[i].dither      = 1;
        cstate[i].direction   = DIR_NORMAL;
        cstate[i].count       = 2;
        cstate[i].stops[0].y = 0;
        cstate[i].stops[0].r = 0;
        cstate[i].stops[0].g = 0;
        cstate[i].stops[0].b = 0;
        cstate[i].stops[1].y = 100;
        cstate[i].stops[1].r = 0;
        cstate[i].stops[1].g = 15;
        cstate[i].stops[1].b = 0;
    }
    /* Default: register 3 on, dithered, normal direction.
     * Stops: y=0 (7,7,9), y=35% (7,7,7), y=100% (5,7,0). */
    cstate[3].enabled     = TRUE;
    cstate[3].dither      = 1;
    cstate[3].direction   = DIR_NORMAL;
    cstate[3].count       = 3;
    cstate[3].stops[0].y = 0;   cstate[3].stops[0].r = 7; cstate[3].stops[0].g = 7; cstate[3].stops[0].b = 9;
    cstate[3].stops[1].y = 35;  cstate[3].stops[1].r = 7; cstate[3].stops[1].g = 7; cstate[3].stops[1].b = 7;
    cstate[3].stops[2].y = 100; cstate[3].stops[2].r = 5; cstate[3].stops[2].g = 7; cstate[3].stops[2].b = 0;
}

static UWORD sort_stops(struct ColorState *cs, UWORD track)
{
    UWORD tracked_y = cs->stops[track].y;
    UWORD i, j;
    struct Stop tmp;

    for (i = 1; i < cs->count; i++) {
        tmp = cs->stops[i];
        for (j = i; j > 0 && cs->stops[j - 1].y > tmp.y; j--) {
            cs->stops[j] = cs->stops[j - 1];
        }
        cs->stops[j] = tmp;
    }
    for (i = 0; i < cs->count; i++) {
        if (cs->stops[i].y == tracked_y) return i;
    }
    return 0;
}

/* 1D dither pattern for vertical-only mode. */
static const UWORD dither16[16] = {
    0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15
};

/* Interpolate the four channels at scanline `line`, returning each in
 * 4.4 fixed point (so 0..240 = 0.0..15.0). Stops use percent (0..100). */
static void interp_at(struct ColorState *cs, LONG screen_h, UWORD line,
                      LONG *or16, LONG *og16, LONG *ob16)
{
    LONG  r16 = 0, g16 = 0, b16 = 0;
    UWORD i;
    UWORD y_pct;

    if (cs->count == 0) {
        *or16 = *og16 = *ob16 = 0;
        return;
    }
    y_pct = (UWORD)(((LONG)line * 100L) / screen_h);
    if (cs->direction == DIR_REVERSED) y_pct = 100 - y_pct;

    if (y_pct <= cs->stops[0].y) {
        r16 = (LONG)cs->stops[0].r << 4;
        g16 = (LONG)cs->stops[0].g << 4;
        b16 = (LONG)cs->stops[0].b << 4;
    } else if (y_pct >= cs->stops[cs->count - 1].y) {
        UWORD k = cs->count - 1;
        r16 = (LONG)cs->stops[k].r << 4;
        g16 = (LONG)cs->stops[k].g << 4;
        b16 = (LONG)cs->stops[k].b << 4;
    } else {
        for (i = 1; i < cs->count; i++) {
            if (y_pct <= cs->stops[i].y) {
                LONG span = (LONG)cs->stops[i].y - (LONG)cs->stops[i - 1].y;
                LONG t    = (LONG)y_pct - (LONG)cs->stops[i - 1].y;
                if (span <= 0) span = 1;
                r16 = ((LONG)cs->stops[i-1].r << 4) +
                      (((LONG)(cs->stops[i].r - cs->stops[i-1].r) << 4) * t) / span;
                g16 = ((LONG)cs->stops[i-1].g << 4) +
                      (((LONG)(cs->stops[i].g - cs->stops[i-1].g) << 4) * t) / span;
                b16 = ((LONG)cs->stops[i-1].b << 4) +
                      (((LONG)(cs->stops[i].b - cs->stops[i-1].b) << 4) * t) / span;
                break;
            }
        }
    }
    *or16 = r16; *og16 = g16; *ob16 = b16;
}

/* Apply a dither threshold (0..15) and pack to 12-bit RGB. */
static UWORD pack_dither(LONG r16, LONG g16, LONG b16, UWORD threshold, BOOL dither)
{
    UWORD r, g, b;
    if (dither) {
        r = (UWORD)((r16 >> 4) + (((r16 & 15) > (LONG)threshold) ? 1 : 0));
        g = (UWORD)((g16 >> 4) + (((g16 & 15) > (LONG)threshold) ? 1 : 0));
        b = (UWORD)((b16 >> 4) + (((b16 & 15) > (LONG)threshold) ? 1 : 0));
    } else {
        r = (UWORD)((r16 + 8) >> 4);    /* round to nearest */
        g = (UWORD)((g16 + 8) >> 4);
        b = (UWORD)((b16 + 8) >> 4);
    }
    if (r > 15) r = 15;
    if (g > 15) g = 15;
    if (b > 15) b = 15;
    return (UWORD)((r << 8) | (g << 4) | b);
}

/* ---------------------------------------------------------------- copper */

static struct UCopList *build_copperlist(LONG screen_h)
{
    struct UCopList *ucl;
    UWORD line;
    int   i, n_on = 0, per_line, max_ins;
    UWORD last[NCOLORS];
    BOOL  has_last[NCOLORS];

    for (i = 0; i < NCOLORS; i++) if (cstate[i].enabled) n_on++;
    if (n_on == 0) return NULL;

    /* Worst case per line: one CWAIT plus one CMOVE per enabled color. */
    per_line = 1 + n_on;
    max_ins  = ((int)screen_h + 4) * per_line + 8;

    ucl = (struct UCopList *)AllocMem(sizeof(struct UCopList),
                                      MEMF_PUBLIC | MEMF_CLEAR);
    if (!ucl) return NULL;

    CINIT(ucl, max_ins);
    /* CINIT (UCopperListInit) returns void; if its internal chip-RAM
     * allocation failed we can detect it by FirstCopList still being
     * NULL. Bail out cleanly instead of letting CMOVE write through
     * a NULL pointer and crash the machine. */
    if (!ucl->FirstCopList) {
        FreeMem(ucl, sizeof(struct UCopList));
        return NULL;
    }

    for (i = 0; i < NCOLORS; i++) { last[i] = 0; has_last[i] = FALSE; }

    for (line = 0; line < (UWORD)screen_h; line++) {
        LONG  r16, g16, b16;
        UWORD pending[NCOLORS];
        BOOL  pending_set[NCOLORS];
        int   n_pending = 0;
        UWORD t = dither16[line & 15];

        for (i = 0; i < NCOLORS; i++) {
            pending_set[i] = FALSE;
            if (!cstate[i].enabled) continue;
            {
                BOOL d  = (cstate[i].dither != 0);
                UWORD c;
                interp_at(&cstate[i], screen_h, line, &r16, &g16, &b16);
                c = pack_dither(r16, g16, b16, t, d);
                /* Without dither, only emit when value actually changes
                 * (so we don't burn copper on identical-color bands). */
                if (d || !has_last[i] || c != last[i]) {
                    pending[i]     = c;
                    pending_set[i] = TRUE;
                    last[i]        = c;
                    has_last[i]    = TRUE;
                    n_pending++;
                }
            }
        }

        if (n_pending > 0) {
            CWAIT(ucl, line, 0);
            for (i = 0; i < NCOLORS; i++) {
                if (pending_set[i]) {
                    CMOVE(ucl, custom.color[i], pending[i]);
                }
            }
        }
    }

    CEND(ucl);
    return ucl;
}

static void free_ucoplist(struct UCopList *ucl)
{
    struct CopList *cl, *next;
    if (!ucl) return;
    for (cl = ucl->FirstCopList; cl; cl = next) {
        next = cl->Next;
        if (cl->CopIns) {
            FreeMem(cl->CopIns,
                    (LONG)cl->MaxCount * (LONG)sizeof(struct CopIns));
        }
        FreeMem(cl, sizeof(struct CopList));
    }
    FreeMem(ucl, sizeof(struct UCopList));
}

static void detach(struct ViewPort *vp)
{
    Forbid();
    vp->UCopIns = NULL;
    Permit();
    RethinkDisplay();
    WaitTOF();
}

static void attach(struct ViewPort *vp, struct UCopList *ucl)
{
    Forbid();
    vp->UCopIns = ucl;
    Permit();
    RethinkDisplay();
}

/* Swap the viewport's user-copperlist atomically: graphics.library is
 * given the new list, RethinkDisplay re-merges, and we wait one TOF so
 * the hardware has switched off `old` before we free it. Going via
 * detach() (UCopIns = NULL between lists) would briefly drop the
 * gradient and flash the default Workbench palette on every slider
 * drag; this path keeps the previous frame visible until the new one
 * is live. */
static struct UCopList *refresh_copper(struct ViewPort *vp,
                                       struct UCopList *old,
                                       LONG screen_h)
{
    struct UCopList *fresh = build_copperlist(screen_h);
    Forbid();
    vp->UCopIns = fresh;
    Permit();
    RethinkDisplay();
    WaitTOF();
    if (old) free_ucoplist(old);
    return fresh;
}

/* ----------------------------------------------------------- UI globals */

static struct Window *gw_win;
static struct Gadget *gw_reg, *gw_enable, *gw_dither, *gw_direction;
static struct Gadget *gw_stops[MAX_STOPS];
static struct Gadget *gw_y, *gw_r, *gw_g, *gw_b;
static struct Gadget *gw_add_before, *gw_del_this, *gw_add_after;
static struct Gadget *gw_save, *gw_use, *gw_cancel;

/* Disposition the editor exited with -- determines which prefs files
 * (if any) we write before tearing down. */
static UWORD          exit_disposition = EXIT_CANCEL;

static char stop_labels[MAX_STOPS][STOP_LABEL_LEN];

/* Track which optional gadgets are currently in the window's gadget list,
 * so we can show/hide them with AddGadget/RemoveGadget. */
static BOOL stop_in_win[MAX_STOPS];

static const char *reg_labels[] = {
    "Color 0 (background)",
    "Color 1 (text)",
    "Color 2 (border)",
    "Color 3 (highlight)",
    NULL
};

static struct NewMenu app_menus[] = {
    { NM_TITLE, "Project",   NULL,    0, 0L, NULL                   },
    {  NM_ITEM, "About...",  "?",     0, 0L, (APTR)MEN_ABOUT        },
    {  NM_ITEM, NM_BARLABEL, NULL,    0, 0L, NULL                   },
    {  NM_ITEM, "Save",      "S",     0, 0L, (APTR)MEN_SAVE         },
    {  NM_ITEM, "Use",       "U",     0, 0L, (APTR)MEN_USE          },
    {  NM_ITEM, "Cancel",    "C",     0, 0L, (APTR)MEN_CANCEL       },
    {   NM_END, NULL,        NULL,    0, 0L, NULL                   }
};

static struct Menu *menu_strip = NULL;

static void show_about(struct Window *win)
{
    struct EasyStruct es;
    static const char body[] =
        "Gradient 1.1\n\n"
        "Copper-driven gradient editor for Workbench 3.x.\n\n"
        "https://solvalou.com/gradient";
    es.es_StructSize    = sizeof(es);
    es.es_Flags         = 0;
    es.es_Title         = (UBYTE *)"About Gradient";
    es.es_TextFormat    = (UBYTE *)body;
    es.es_GadgetFormat  = (UBYTE *)"OK";
    EasyRequestArgs(win, &es, NULL, NULL);
}

/* Modal error requester. `win` may be NULL — the requester then opens
 * on the default public screen, which matters when we have to report a
 * failure that prevented the editor window from opening at all. */
static void show_error(struct Window *win, const char *body)
{
    struct EasyStruct es;
    es.es_StructSize    = sizeof(es);
    es.es_Flags         = 0;
    es.es_Title         = (UBYTE *)"Gradient";
    es.es_TextFormat    = (UBYTE *)body;
    es.es_GadgetFormat  = (UBYTE *)"OK";
    EasyRequestArgs(win, &es, NULL, NULL);
}

static void format_stop_labels(void)
{
    struct ColorState *cs = &cstate[curr_reg];
    UWORD i;
    for (i = 0; i < MAX_STOPS; i++) {
        if (i < cs->count) {
            struct Stop *sp = &cs->stops[i];
            /* "* R/G/B Y%" for the currently-edited stop, two spaces
             * for the others (so all buttons line up identically). */
            sprintf(stop_labels[i], "%s%d/%d/%d %d%%",
                    (i == curr_stop) ? "* " : "  ",
                    (int)sp->r, (int)sp->g, (int)sp->b,
                    (int)sp->y);
        } else {
            stop_labels[i][0] = '\0';
        }
    }
}

/* GadTools centers the IntuiText at gadget creation time using the label
 * length it saw then. When we change the label string at runtime, the
 * stored offsets become stale -- recompute LeftEdge so the new text
 * stays horizontally centered inside the button. */
static void center_button_text(struct Gadget *g)
{
    struct IntuiText *it;
    WORD tw;

    if (!g || !g->GadgetText) return;
    it = g->GadgetText;
    tw = IntuiTextLength(it);
    it->LeftEdge = (g->Width - tw) / 2;
}

static void erase_gadget_area(struct Gadget *g)
{
    struct RastPort *rp;
    if (!gw_win || !g) return;
    rp = gw_win->RPort;
    SetAPen(rp, 0);
    RectFill(rp,
             g->LeftEdge,
             g->TopEdge,
             g->LeftEdge + g->Width  - 1,
             g->TopEdge  + g->Height - 1);
}

/* Bring a gadget into / out of the window's gadget list. AddGadget
 * appends to the chain (position -1); visual placement is unchanged
 * because each gadget owns its LeftEdge/TopEdge. */
static void show_gadget(struct Gadget *g, BOOL *flag)
{
    if (!*flag) {
        AddGadget(gw_win, g, -1);
        *flag = TRUE;
        RefreshGList(g, gw_win, NULL, 1);
    }
}

static void hide_gadget(struct Gadget *g, BOOL *flag)
{
    if (*flag) {
        RemoveGadget(gw_win, g);
        erase_gadget_area(g);
        *flag = FALSE;
    }
}

/* Show only the stop buttons that correspond to existing stops. */
static void sync_stop_visibility(void)
{
    struct ColorState *cs = &cstate[curr_reg];
    UWORD i;
    for (i = MAX_STOPS; i > 0; i--) {
        UWORD idx = i - 1;
        if (idx >= cs->count) hide_gadget(gw_stops[idx], &stop_in_win[idx]);
    }
    for (i = 0; i < cs->count; i++) {
        show_gadget(gw_stops[i], &stop_in_win[i]);
        center_button_text(gw_stops[i]);
        RefreshGList(gw_stops[i], gw_win, NULL, 1);
    }
}

/* Add-before is enabled when current stop has room above (y > 0) and
 * we're below the cap; same for Add-after at y < 100. The buttons stay
 * visible in their layout slot in either case -- just disabled when the
 * action isn't applicable, so the management row never reflows. */
static void sync_management_visibility(void)
{
    struct ColorState *cs = &cstate[curr_reg];
    struct Stop *sp;
    BOOL can_before, can_after;

    if (cs->count == 0) {
        GT_SetGadgetAttrs(gw_add_before, gw_win, NULL, GA_Disabled, TRUE, TAG_END);
        GT_SetGadgetAttrs(gw_add_after,  gw_win, NULL, GA_Disabled, TRUE, TAG_END);
        return;
    }
    sp = &cs->stops[curr_stop];
    can_before = (sp->y > 0)   && (cs->count < MAX_STOPS);
    can_after  = (sp->y < 100) && (cs->count < MAX_STOPS);

    GT_SetGadgetAttrs(gw_add_before, gw_win, NULL, GA_Disabled, !can_before, TAG_END);
    GT_SetGadgetAttrs(gw_add_after,  gw_win, NULL, GA_Disabled, !can_after,  TAG_END);
}

/* Push the four slider levels from the model into the gadgets. Only call
 * this when the active stop has changed (cycle / stop button / add /
 * del) -- never in response to a slider drag, or you'll re-snap the
 * slider mid-drag and produce a visible "bounce" against the user's
 * mouse position. */
static void refresh_sliders(void)
{
    struct ColorState *cs = &cstate[curr_reg];
    struct Stop *sp;

    if (cs->count == 0) return;
    if (curr_stop >= cs->count) curr_stop = cs->count - 1;
    sp = &cs->stops[curr_stop];

    GT_SetGadgetAttrs(gw_y, gw_win, NULL, GTSL_Level, sp->y, TAG_END);
    GT_SetGadgetAttrs(gw_r, gw_win, NULL, GTSL_Level, sp->r, TAG_END);
    GT_SetGadgetAttrs(gw_g, gw_win, NULL, GTSL_Level, sp->g, TAG_END);
    GT_SetGadgetAttrs(gw_b, gw_win, NULL, GTSL_Level, sp->b, TAG_END);
}

static void refresh_ui(void)
{
    struct ColorState *cs = &cstate[curr_reg];

    if (cs->count == 0) return;
    if (curr_stop >= cs->count) curr_stop = cs->count - 1;

    GT_SetGadgetAttrs(gw_enable,    gw_win, NULL, GTCB_Checked, cs->enabled,   TAG_END);
    GT_SetGadgetAttrs(gw_dither,    gw_win, NULL, GTCB_Checked, cs->dither,    TAG_END);
    GT_SetGadgetAttrs(gw_direction, gw_win, NULL, GTCB_Checked, cs->direction, TAG_END);

    GT_SetGadgetAttrs(gw_del_this, gw_win, NULL,
        GA_Disabled, (cs->count <= MIN_STOPS), TAG_END);

    format_stop_labels();
    sync_stop_visibility();
    sync_management_visibility();
}

/* ---- Event handling --------------------------------------------------- */

static void handle_gadget_event(struct Gadget *g, UWORD code,
                                BOOL *rebuild, BOOL *ui_only,
                                BOOL *reset_sliders, BOOL *quit)
{
    struct ColorState *cs = &cstate[curr_reg];
    struct Stop *sp = &cs->stops[curr_stop];
    UWORD id = g->GadgetID;
    UWORD i;

    if (id >= GID_STOP_BASE && id < GID_STOP_BASE + MAX_STOPS) {
        UWORD idx = id - GID_STOP_BASE;
        if (idx < cs->count) {
            curr_stop      = idx;
            *ui_only       = TRUE;
            *reset_sliders = TRUE;
        }
        return;
    }

    switch (id) {
        case GID_REG:
            curr_reg       = code;
            curr_stop      = 0;
            *ui_only       = TRUE;
            *reset_sliders = TRUE;
            break;

        case GID_ENABLE:
            cs->enabled = (code != 0);
            *rebuild = TRUE;
            break;

        case GID_DITHER:
            cs->dither = (code != 0);
            *rebuild = TRUE;
            break;

        case GID_DIRECTION:
            cs->direction = (code != 0) ? DIR_REVERSED : DIR_NORMAL;
            *rebuild = TRUE;
            break;

        case GID_ADD_BEFORE:
            if (cs->count < MAX_STOPS && sp->y > 0) {
                UWORD insert_at = curr_stop;
                UWORD new_y;
                if (curr_stop == 0)
                    new_y = sp->y / 2;
                else
                    new_y = (UWORD)(((LONG)cs->stops[curr_stop - 1].y +
                                     (LONG)sp->y) / 2);
                for (i = cs->count; i > insert_at; i--)
                    cs->stops[i] = cs->stops[i - 1];
                /* Shift loop already left a copy of the original stop at
                 * insert_at; we just relocate it to new_y. */
                cs->stops[insert_at].y = new_y;
                cs->count++;
                /* The stop the user was editing has shifted up by one;
                 * keep editing the same data element. */
                curr_stop = insert_at + 1;
                *rebuild = TRUE;
            }
            break;

        case GID_ADD_AFTER:
            if (cs->count < MAX_STOPS && sp->y < 100) {
                UWORD insert_at = curr_stop + 1;
                UWORD new_y;
                if (insert_at >= cs->count)
                    new_y = (UWORD)(((LONG)sp->y + 100L) / 2);
                else
                    new_y = (UWORD)(((LONG)sp->y +
                                     (LONG)cs->stops[insert_at].y) / 2);
                for (i = cs->count; i > insert_at; i--)
                    cs->stops[i] = cs->stops[i - 1];
                cs->stops[insert_at]   = cs->stops[curr_stop];
                cs->stops[insert_at].y = new_y;
                cs->count++;
                curr_stop = insert_at;
                *rebuild = TRUE;
            }
            break;

        case GID_DEL_THIS:
            if (cs->count > MIN_STOPS) {
                for (i = curr_stop; i + 1 < cs->count; i++)
                    cs->stops[i] = cs->stops[i + 1];
                cs->count--;
                if (curr_stop >= cs->count) curr_stop = cs->count - 1;
                *rebuild = TRUE;
            }
            break;

        case GID_Y:
            sp->y = code;
            curr_stop = sort_stops(cs, curr_stop);
            *rebuild = TRUE;
            break;
        case GID_R: sp->r = code; *rebuild = TRUE; break;
        case GID_G: sp->g = code; *rebuild = TRUE; break;
        case GID_B: sp->b = code; *rebuild = TRUE; break;

        case GID_SAVE:
            exit_disposition = EXIT_SAVE;
            *quit = TRUE;
            break;
        case GID_USE:
            exit_disposition = EXIT_USE;
            *quit = TRUE;
            break;
        case GID_CANCEL:
            exit_disposition = EXIT_CANCEL;
            *quit = TRUE;
            break;

        default:
            break;
    }
}

/* ----------------------------------------------- config persistence */

struct ConfigFile {
    ULONG magic;
    UWORD version;
    UWORD reserved;
    struct ColorState colors[NCOLORS];
};

/* Sanitize a freshly-loaded ColorState. The on-disk format is just a
 * binary blob anyone could hand-edit, so clamp every field into its
 * documented range before we feed it to interp_at / build_copperlist —
 * an out-of-range count or y would walk past the stops[] array. */
static void sanitize_color_state(struct ColorState *cs)
{
    UWORD i;
    if (cs->count > MAX_STOPS) cs->count = MAX_STOPS;
    if (cs->count < MIN_STOPS) cs->count = MIN_STOPS;
    cs->dither    = (cs->dither    != 0) ? 1 : 0;
    cs->direction = (cs->direction != 0) ? DIR_REVERSED : DIR_NORMAL;
    for (i = 0; i < cs->count; i++) {
        if (cs->stops[i].y > 100) cs->stops[i].y = 100;
        if (cs->stops[i].r > 15)  cs->stops[i].r = 15;
        if (cs->stops[i].g > 15)  cs->stops[i].g = 15;
        if (cs->stops[i].b > 15)  cs->stops[i].b = 15;
    }
}

static BOOL load_config(STRPTR path)
{
    BPTR fh;
    struct ConfigFile cf;
    LONG n;
    int  i;

    fh = Open(path, MODE_OLDFILE);
    if (!fh) return FALSE;
    n = Read(fh, &cf, (LONG)sizeof(cf));
    Close(fh);
    if (n != (LONG)sizeof(cf))    return FALSE;
    if (cf.magic   != CONFIG_MAGIC)   return FALSE;
    if (cf.version != CONFIG_VERSION) return FALSE;
    for (i = 0; i < NCOLORS; i++) sanitize_color_state(&cf.colors[i]);
    memcpy(cstate, cf.colors, sizeof(cstate));
    return TRUE;
}

static BOOL save_config(STRPTR path)
{
    BPTR fh;
    struct ConfigFile cf;
    LONG n;

    cf.magic    = CONFIG_MAGIC;
    cf.version  = CONFIG_VERSION;
    cf.reserved = 0;
    memcpy(cf.colors, cstate, sizeof(cstate));

    fh = Open(path, MODE_NEWFILE);
    if (!fh) return FALSE;
    n = Write(fh, &cf, (LONG)sizeof(cf));
    Close(fh);
    return (n == (LONG)sizeof(cf));
}

/* Persist to both the live (ENV:) and archived (ENVARC:) locations -- the
 * Amiga prefs convention. ENV: is what the next launch reads; ENVARC: is
 * what S:Startup-Sequence copies back into ENV: after a reboot. Returns
 * TRUE only if both files were written; the caller surfaces failure via
 * a requester. */
static BOOL save_prefs_pair(void)
{
    BOOL live = save_config(CONFIG_PATH_LIVE);
    BOOL arc  = save_config(CONFIG_PATH_SAVE);
    return live && arc;
}

/* ----------------------------------------------- CLI args */

struct CliArgs {
    LONG   background;
    STRPTR load_path;
};

static struct RDArgs *cli_rdargs = NULL;
static LONG           cli_argbuf[2];

static void parse_cli(struct CliArgs *out)
{
    out->background = 0;
    out->load_path  = NULL;

    cli_argbuf[0] = 0;
    cli_argbuf[1] = 0;

    /* No template parse if we were launched from Workbench (no argv).
     * vbcc's startup detects WB launch and presents argc==0, but
     * dos.library's pr_CLI being NULL is the canonical check. */
    if (((struct Process *)FindTask(NULL))->pr_CLI == 0) return;

    cli_rdargs = ReadArgs("BACKGROUND/S,LOAD/K", cli_argbuf, NULL);
    if (cli_rdargs) {
        out->background = cli_argbuf[0];
        out->load_path  = (STRPTR)cli_argbuf[1];
    }
}

static void free_cli(void)
{
    if (cli_rdargs) { FreeArgs(cli_rdargs); cli_rdargs = NULL; }
}

/* When launched from Workbench, parse_cli is a no-op (no Shell). Read
 * tooltypes from the program's icon instead via icon.library. We honour
 * the same names as the CLI: BACKGROUND, LOAD. (DONOTWAIT is consumed
 * by Workbench itself before we ever run.) */
static char wb_load_buf[256];

static void parse_wb_tooltypes(struct CliArgs *out)
{
    struct WBArg      *wba;
    struct DiskObject *dob;
    BPTR               olddir;
    STRPTR             tt_load;
    STRPTR            *ttypes;

    if (!_WBenchMsg)                  return;
    if (_WBenchMsg->sm_NumArgs < 1)   return;

    IconBase = OpenLibrary("icon.library", 36L);
    if (!IconBase) return;

    wba    = &_WBenchMsg->sm_ArgList[0];
    olddir = CurrentDir(wba->wa_Lock);
    dob    = GetDiskObject(wba->wa_Name);
    CurrentDir(olddir);

    if (!dob) return;

    ttypes = (STRPTR *)dob->do_ToolTypes;
    if (FindToolType(ttypes, (UBYTE *)"BACKGROUND")) out->background = 1;

    tt_load = (STRPTR)FindToolType(ttypes, (UBYTE *)"LOAD");
    if (tt_load) {
        strncpy(wb_load_buf, tt_load, sizeof(wb_load_buf) - 1);
        wb_load_buf[sizeof(wb_load_buf) - 1] = '\0';
        out->load_path = wb_load_buf;
    }

    FreeDiskObject(dob);
}

/* ----------------------------------------------- snapshot / revert */

static struct ColorState cstate_snap[NCOLORS];
static UWORD             curr_reg_snap;
static UWORD             curr_stop_snap;

static void snapshot_state(void)
{
    memcpy(cstate_snap, cstate, sizeof(cstate));
    curr_reg_snap  = curr_reg;
    curr_stop_snap = curr_stop;
}

static void revert_state(void)
{
    memcpy(cstate, cstate_snap, sizeof(cstate));
    curr_reg  = curr_reg_snap;
    curr_stop = curr_stop_snap;
}

/* ----------------------------------------------- editor window helpers */

static struct Screen *g_scr   = NULL;
static APTR           g_vi    = NULL;
static struct Gadget *g_glist = NULL;
static BOOL           editor_open = FALSE;

static void close_editor_window(void)
{
    UWORD i;
    if (menu_strip) {
        if (gw_win) ClearMenuStrip(gw_win);
        FreeMenus(menu_strip);
        menu_strip = NULL;
    }
    if (gw_win) {
        CloseWindow(gw_win);
        gw_win = NULL;
    }
    if (g_glist) {
        FreeGadgets(g_glist);
        g_glist = NULL;
    }
    for (i = 0; i < MAX_STOPS; i++) stop_in_win[i] = FALSE;
    editor_open = FALSE;
}

static BOOL open_editor_window(void)
{
    struct Gadget   *prev_gad;
    struct NewGadget ng;
    LONG             win_h;
    UWORD            i;
    UWORD            y;
    struct Screen   *scr = g_scr;
    APTR             vi  = g_vi;

    if (editor_open) return TRUE;

    /* Default the color selector to the first register the user has
     * enabled -- saves a click in the common case where only one
     * register is being driven (often Color 3). Falls back to 0 if
     * nothing is enabled. */
    curr_reg = 0;
    for (i = 0; i < NCOLORS; i++) {
        if (cstate[i].enabled) { curr_reg = i; break; }
    }
    curr_stop = 0;

    /* Snapshot current state so Cancel can revert. */
    snapshot_state();

    /* Pre-fill stop label buffers with the longest possible string so
     * GadTools' creation-time centering uses the max width. Runtime
     * relabel then re-centers via center_button_text(). */
    for (i = 0; i < MAX_STOPS; i++)
        strcpy(stop_labels[i], "* 15/15/15 100%");

    g_glist  = NULL;
    prev_gad = CreateContext(&g_glist);

    {
        UWORD top0      = scr->WBorTop + scr->Font->ta_YSize + 6;
        UWORD col_lbl   = 90;
        UWORD col_w     = WIN_W - col_lbl - 14;
        UWORD x_label_w = 30;
        UWORD x;

        memset(&ng, 0, sizeof(ng));
        ng.ng_TextAttr   = scr->Font;
        ng.ng_VisualInfo = vi;

        /* CYCLE: color register */
        ng.ng_LeftEdge   = col_lbl;
        ng.ng_TopEdge    = top0;
        ng.ng_Width      = col_w;
        ng.ng_Height     = 14;
        ng.ng_GadgetText = (UBYTE *)"Color:";
        ng.ng_GadgetID   = GID_REG;
        gw_reg = CreateGadget(CYCLE_KIND, prev_gad, &ng,
            GTCY_Labels, (ULONG)reg_labels,
            GTCY_Active, curr_reg,
            TAG_END);
        prev_gad = gw_reg;

        /* CHECKBOX: enabled */
        ng.ng_TopEdge    = top0 + 18;
        ng.ng_Width      = 26;
        ng.ng_Height     = 11;
        ng.ng_GadgetText = (UBYTE *)"Enabled:";
        ng.ng_GadgetID   = GID_ENABLE;
        gw_enable = CreateGadget(CHECKBOX_KIND, prev_gad, &ng,
            GTCB_Checked, cstate[curr_reg].enabled,
            TAG_END);
        prev_gad = gw_enable;

        /* CHECKBOX: dither (vertical only) */
        ng.ng_LeftEdge   = col_lbl;
        ng.ng_TopEdge    = top0 + 36;
        ng.ng_Width      = 26;
        ng.ng_Height     = 11;
        ng.ng_GadgetText = (UBYTE *)"Dither:";
        ng.ng_GadgetID   = GID_DITHER;
        gw_dither = CreateGadget(CHECKBOX_KIND, prev_gad, &ng,
            GTCB_Checked, cstate[curr_reg].dither,
            TAG_END);
        prev_gad = gw_dither;

        /* CHECKBOX: reverse direction (off = normal top->bottom) */
        ng.ng_LeftEdge   = col_lbl;
        ng.ng_TopEdge    = top0 + 54;
        ng.ng_Width      = 26;
        ng.ng_Height     = 11;
        ng.ng_GadgetText = (UBYTE *)"Reverse:";
        ng.ng_GadgetID   = GID_DIRECTION;
        gw_direction = CreateGadget(CHECKBOX_KIND, prev_gad, &ng,
            GTCB_Checked, cstate[curr_reg].direction,
            TAG_END);
        prev_gad = gw_direction;

        /* Stop buttons. */
        y = top0 + 74;
        {
            UWORD bw   = (WIN_W - 20 - (STOPS_PER_ROW - 1) * 4) / STOPS_PER_ROW;
            UWORD bh   = 14;
            UWORD rows = (MAX_STOPS + STOPS_PER_ROW - 1) / STOPS_PER_ROW;
            UWORD row, col;

            for (i = 0; i < MAX_STOPS; i++) {
                row = i / STOPS_PER_ROW;
                col = i % STOPS_PER_ROW;
                x = 10 + col * (bw + 4);

                ng.ng_LeftEdge   = x;
                ng.ng_TopEdge    = y + row * (bh + 2);
                ng.ng_Width      = bw;
                ng.ng_Height     = bh;
                ng.ng_GadgetText = (UBYTE *)stop_labels[i];
                ng.ng_GadgetID   = GID_STOP_BASE + i;
                gw_stops[i] = CreateGadget(BUTTON_KIND, prev_gad, &ng, TAG_END);
                prev_gad = gw_stops[i];
                stop_in_win[i] = TRUE;   /* will be in window after OpenWindow */
            }
            y = y + rows * (bh + 2);
        }

        /* Sliders. */
        y += 6;
        ng.ng_LeftEdge   = x_label_w;
        ng.ng_TopEdge    = y;
        ng.ng_Width      = WIN_W - x_label_w - 50;
        ng.ng_Height     = 12;
        ng.ng_GadgetText = (UBYTE *)"Y";
        ng.ng_GadgetID   = GID_Y;
        gw_y = CreateGadget(SLIDER_KIND, prev_gad, &ng,
            GTSL_Min, 0, GTSL_Max, 100,
            GTSL_Level, cstate[curr_reg].stops[curr_stop].y,
            GTSL_LevelFormat, (ULONG)"%3ld%%",
            GTSL_LevelPlace, PLACETEXT_RIGHT,
            GTSL_MaxLevelLen, 4,
            GA_Immediate, TRUE, GA_RelVerify, TRUE,
            TAG_END);
        prev_gad = gw_y;

        ng.ng_TopEdge    = y + 14;
        ng.ng_GadgetText = (UBYTE *)"R";
        ng.ng_GadgetID   = GID_R;
        gw_r = CreateGadget(SLIDER_KIND, prev_gad, &ng,
            GTSL_Min, 0, GTSL_Max, 15,
            GTSL_Level, cstate[curr_reg].stops[curr_stop].r,
            GTSL_LevelFormat, (ULONG)"%2ld",
            GTSL_LevelPlace, PLACETEXT_RIGHT,
            GTSL_MaxLevelLen, 2,
            GA_Immediate, TRUE, GA_RelVerify, TRUE,
            TAG_END);
        prev_gad = gw_r;

        ng.ng_TopEdge    = y + 28;
        ng.ng_GadgetText = (UBYTE *)"G";
        ng.ng_GadgetID   = GID_G;
        gw_g = CreateGadget(SLIDER_KIND, prev_gad, &ng,
            GTSL_Min, 0, GTSL_Max, 15,
            GTSL_Level, cstate[curr_reg].stops[curr_stop].g,
            GTSL_LevelFormat, (ULONG)"%2ld",
            GTSL_LevelPlace, PLACETEXT_RIGHT,
            GTSL_MaxLevelLen, 2,
            GA_Immediate, TRUE, GA_RelVerify, TRUE,
            TAG_END);
        prev_gad = gw_g;

        ng.ng_TopEdge    = y + 42;
        ng.ng_GadgetText = (UBYTE *)"B";
        ng.ng_GadgetID   = GID_B;
        gw_b = CreateGadget(SLIDER_KIND, prev_gad, &ng,
            GTSL_Min, 0, GTSL_Max, 15,
            GTSL_Level, cstate[curr_reg].stops[curr_stop].b,
            GTSL_LevelFormat, (ULONG)"%2ld",
            GTSL_LevelPlace, PLACETEXT_RIGHT,
            GTSL_MaxLevelLen, 2,
            GA_Immediate, TRUE, GA_RelVerify, TRUE,
            TAG_END);
        prev_gad = gw_b;

        y += 56;

        /* Stop-management row: Add before / Delete this / Add after. */
        {
            UWORD bw = (WIN_W - 20 - 16) / 3;
            UWORD bh = 14;

            x = 10;
            ng.ng_LeftEdge   = x;
            ng.ng_TopEdge    = y;
            ng.ng_Width      = bw;
            ng.ng_Height     = bh;
            ng.ng_GadgetText = (UBYTE *)"Add before";
            ng.ng_GadgetID   = GID_ADD_BEFORE;
            gw_add_before = CreateGadget(BUTTON_KIND, prev_gad, &ng, TAG_END);
            prev_gad = gw_add_before;

            x += bw + 8;
            ng.ng_LeftEdge   = x;
            ng.ng_GadgetText = (UBYTE *)"Delete this";
            ng.ng_GadgetID   = GID_DEL_THIS;
            gw_del_this = CreateGadget(BUTTON_KIND, prev_gad, &ng, TAG_END);
            prev_gad = gw_del_this;

            x += bw + 8;
            ng.ng_LeftEdge   = x;
            ng.ng_GadgetText = (UBYTE *)"Add after";
            ng.ng_GadgetID   = GID_ADD_AFTER;
            gw_add_after = CreateGadget(BUTTON_KIND, prev_gad, &ng, TAG_END);
            prev_gad = gw_add_after;

            y += bh + 6;
        }

        /* Standard Amiga prefs row: Save / Use / Cancel. */
        {
            UWORD bw = (WIN_W - 20 - 16) / 3;
            UWORD bh = 16;

            x = 10;
            ng.ng_LeftEdge   = x;
            ng.ng_TopEdge    = y;
            ng.ng_Width      = bw;
            ng.ng_Height     = bh;
            ng.ng_GadgetText = (UBYTE *)"Save";
            ng.ng_GadgetID   = GID_SAVE;
            gw_save = CreateGadget(BUTTON_KIND, prev_gad, &ng, TAG_END);
            prev_gad = gw_save;

            x += bw + 8;
            ng.ng_LeftEdge   = x;
            ng.ng_GadgetText = (UBYTE *)"Use";
            ng.ng_GadgetID   = GID_USE;
            gw_use = CreateGadget(BUTTON_KIND, prev_gad, &ng, TAG_END);
            prev_gad = gw_use;

            x += bw + 8;
            ng.ng_LeftEdge   = x;
            ng.ng_GadgetText = (UBYTE *)"Cancel";
            ng.ng_GadgetID   = GID_CANCEL;
            gw_cancel = CreateGadget(BUTTON_KIND, prev_gad, &ng, TAG_END);
            prev_gad = gw_cancel;

            if (!gw_cancel) { close_editor_window(); return FALSE; }

            y += bh;
        }
        win_h = y + scr->WBorBottom + 4;
    }

    gw_win = OpenWindowTags(NULL,
        WA_Title,        (ULONG)"Gradient",
        WA_Width,        WIN_W,
        WA_Height,       win_h,
        WA_Left,         60,
        WA_Top,          20,
        WA_DragBar,      TRUE,
        WA_DepthGadget,  TRUE,
        WA_CloseGadget,  TRUE,
        WA_Activate,     TRUE,
        WA_SimpleRefresh,TRUE,
        WA_IDCMP,        IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW
                       | IDCMP_MENUPICK
                       | CYCLEIDCMP | BUTTONIDCMP
                       | CHECKBOXIDCMP | SLIDERIDCMP,
        WA_NewLookMenus, TRUE,
        WA_Gadgets,      (ULONG)g_glist,
        WA_PubScreen,    (ULONG)scr,
        TAG_END);
    if (!gw_win) { close_editor_window(); return FALSE; }

    menu_strip = CreateMenus(app_menus, GTMN_NewLookMenus, TRUE, TAG_END);
    if (menu_strip) {
        if (LayoutMenus(menu_strip, vi, GTMN_NewLookMenus, TRUE, TAG_END))
            SetMenuStrip(gw_win, menu_strip);
    }

    GT_RefreshWindow(gw_win, NULL);
    editor_open = TRUE;
    refresh_ui();
    refresh_sliders();

    return TRUE;
}

/* Drain all pending editor-window events. Returns TRUE if the user
 * pressed Save / Use / Cancel (or closed the window): the caller is
 * responsible for applying `exit_disposition` and tearing the window
 * down. */
static BOOL drain_editor_messages(struct UCopList **ucl)
{
    struct IntuiMessage *imsg;
    BOOL any_rebuild = FALSE;
    BOOL any_ui      = FALSE;
    BOOL any_reset   = FALSE;
    BOOL want_close  = FALSE;

    while ((imsg = GT_GetIMsg(gw_win->UserPort)) != NULL) {
        ULONG class    = imsg->Class;
        UWORD code     = imsg->Code;
        APTR  iaddress = imsg->IAddress;
        BOOL  rb_local    = FALSE;
        BOOL  ui_local    = FALSE;
        BOOL  reset_local = FALSE;
        BOOL  quit_local  = FALSE;

        switch (class) {
            case IDCMP_CLOSEWINDOW:
                exit_disposition = EXIT_CANCEL;
                quit_local = TRUE;
                break;

            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(gw_win);
                GT_EndRefresh(gw_win, TRUE);
                break;

            case IDCMP_MENUPICK: {
                UWORD mn = code;
                while (mn != MENUNULL && menu_strip) {
                    struct MenuItem *mi = ItemAddress(menu_strip, mn);
                    if (!mi) break;
                    {
                        ULONG act = (ULONG)GTMENUITEM_USERDATA(mi);
                        switch (act) {
                            case MEN_ABOUT:
                                show_about(gw_win);
                                break;
                            case MEN_SAVE:
                                exit_disposition = EXIT_SAVE;
                                quit_local = TRUE;
                                break;
                            case MEN_USE:
                                exit_disposition = EXIT_USE;
                                quit_local = TRUE;
                                break;
                            case MEN_CANCEL:
                                exit_disposition = EXIT_CANCEL;
                                quit_local = TRUE;
                                break;
                            default: break;
                        }
                    }
                    mn = mi->NextSelect;
                }
                break;
            }

            case IDCMP_GADGETUP:
            case IDCMP_MOUSEMOVE: {
                struct Gadget *g = (struct Gadget *)iaddress;
                if (g)
                    handle_gadget_event(g, code,
                                        &rb_local, &ui_local,
                                        &reset_local, &quit_local);
                break;
            }

            default:
                break;
        }

        GT_ReplyIMsg(imsg);

        if (rb_local)    any_rebuild = TRUE;
        if (ui_local)    any_ui      = TRUE;
        if (reset_local) any_reset   = TRUE;
        if (quit_local)  want_close  = TRUE;
    }

    if (any_rebuild) {
        *ucl = refresh_copper(&g_scr->ViewPort, *ucl, g_scr->Height);
        refresh_ui();
    } else if (any_ui) {
        refresh_ui();
    }
    if (any_reset) refresh_sliders();

    return want_close;
}

/* Persist or revert per the disposition the user picked, then reset
 * disposition to the safe default for next time. */
static void apply_disposition(struct UCopList **ucl)
{
    BOOL ok = TRUE;
    switch (exit_disposition) {
        case EXIT_SAVE:
            ok = save_prefs_pair();
            if (!ok) show_error(gw_win,
                "Could not write Gradient preferences.\n"
                "Check that ENV: and ENVARC: are mounted\n"
                "and writable.");
            break;
        case EXIT_USE:
            ok = save_config(CONFIG_PATH_LIVE);
            if (!ok) show_error(gw_win,
                "Could not write ENV:Gradient.prefs.\n"
                "Check that ENV: is mounted and writable.");
            break;
        case EXIT_CANCEL:
        default:
            revert_state();
            *ucl = refresh_copper(&g_scr->ViewPort, *ucl, g_scr->Height);
            break;
    }
    exit_disposition = EXIT_CANCEL;
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    struct CliArgs    cli;
    struct UCopList  *ucl       = NULL;
    struct MsgPort   *cx_port   = NULL;
    CxObj            *cx_broker = NULL;
    struct NewBroker  nb;
    LONG              cx_err    = 0;
    BOOL              done      = FALSE;
    int               rc        = 0;

    init_state();
    parse_cli(&cli);
    parse_wb_tooltypes(&cli);   /* no-op if launched from CLI */

    /* Load config: explicit path > ENV: > built-in defaults. */
    if (cli.load_path) load_config(cli.load_path);
    else               load_config(CONFIG_PATH_LIVE);

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37L);
    if (!IntuitionBase) { rc = 10; goto cleanup; }
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37L);
    if (!GfxBase)       { rc = 10; goto cleanup; }
    GadToolsBase = OpenLibrary("gadtools.library", 37L);
    if (!GadToolsBase)  { rc = 10; goto cleanup; }
    CxBase = OpenLibrary("commodities.library", 37L);
    if (!CxBase)        { rc = 10; goto cleanup; }

    /* Drop priority -- the copper does the actual work, our task just
     * waits on signals. -1 keeps us out of foreground apps' way. */
    SetTaskPri(FindTask(NULL), -1);

    g_scr = LockPubScreen(NULL);
    if (!g_scr) { rc = 10; goto cleanup; }

    g_vi = GetVisualInfo(g_scr, TAG_END);
    if (!g_vi) { rc = 10; goto cleanup; }

    /* Register with Commodities Exchange before touching the shared
     * ViewPort. NBU_UNIQUE rejects a second concurrent instance and
     * notifies the existing broker via CXCMD_UNIQUE -- which the
     * running instance uses to pop its editor window. Doing this
     * before build_copperlist/attach matters: if we lost the
     * uniqueness race after attaching, our cleanup path's detach()
     * would null out vp->UCopIns and leave the running instance
     * visibly "disabled" until something rebuilt the copperlist. */
    cx_port = CreateMsgPort();
    if (!cx_port) { rc = 10; goto cleanup; }

    memset(&nb, 0, sizeof(nb));
    nb.nb_Version  = NB_VERSION;
    nb.nb_Name     = (UBYTE *)BROKER_NAME;
    nb.nb_Title    = (UBYTE *)BROKER_TITLE;
    nb.nb_Descr    = (UBYTE *)BROKER_DESCR;
    nb.nb_Unique   = NBU_UNIQUE | NBU_NOTIFY;
    nb.nb_Flags    = COF_SHOW_HIDE;
    nb.nb_Pri      = 0;
    nb.nb_Port     = cx_port;
    cx_broker = CxBroker(&nb, &cx_err);
    if (!cx_broker) {
        /* Duplicate -- the existing instance got CXCMD_UNIQUE and
         * popped its window. We just exit, having touched nothing
         * shared. */
        rc = 0;
        goto cleanup;
    }
    ActivateCxObj(cx_broker, 1);

    ucl = build_copperlist(g_scr->Height);
    if (ucl) attach(&g_scr->ViewPort, ucl);

    if (!cli.background) {
        if (!open_editor_window()) show_error(NULL,
            "Could not open the Gradient editor window.\n"
            "The broker is still active in Commodities Exchange;\n"
            "try Show again from there to retry.");
    }

    /* Main loop: react to commodities and (when open) editor events. */
    while (!done) {
        ULONG cx_sigmask  = 1L << cx_port->mp_SigBit;
        ULONG win_sigmask = editor_open
                          ? (1L << gw_win->UserPort->mp_SigBit)
                          : 0L;
        ULONG sigs = Wait(cx_sigmask | win_sigmask | SIGBREAKF_CTRL_C);

        if (sigs & SIGBREAKF_CTRL_C) { done = TRUE; break; }

        if (editor_open && (sigs & win_sigmask)) {
            if (drain_editor_messages(&ucl)) {
                apply_disposition(&ucl);
                close_editor_window();
            }
        }

        if (sigs & cx_sigmask) {
            CxMsg *cmsg;
            while ((cmsg = (CxMsg *)GetMsg(cx_port)) != NULL) {
                LONG mid   = CxMsgID(cmsg);
                LONG mtype = CxMsgType(cmsg);
                ReplyMsg((struct Message *)cmsg);

                if (mtype != CXM_COMMAND) continue;
                switch (mid) {
                    case CXCMD_KILL:
                        done = TRUE;
                        break;
                    case CXCMD_DISABLE:
                        ActivateCxObj(cx_broker, 0);
                        if (ucl) {
                            detach(&g_scr->ViewPort);
                            free_ucoplist(ucl);
                            ucl = NULL;
                        }
                        break;
                    case CXCMD_ENABLE:
                        ActivateCxObj(cx_broker, 1);
                        if (!ucl) {
                            ucl = build_copperlist(g_scr->Height);
                            if (ucl) attach(&g_scr->ViewPort, ucl);
                        }
                        break;
                    case CXCMD_APPEAR:
                    case CXCMD_UNIQUE:
                        if (!editor_open) {
                            if (!open_editor_window()) show_error(NULL,
                                "Could not open the Gradient editor window.");
                        } else if (gw_win) {
                            WindowToFront(gw_win);
                        }
                        break;
                    case CXCMD_DISAPPEAR:
                        if (editor_open) {
                            exit_disposition = EXIT_CANCEL;
                            apply_disposition(&ucl);
                            close_editor_window();
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }

cleanup:
    if (editor_open) close_editor_window();
    if (cx_broker)   DeleteCxObjAll(cx_broker);
    if (cx_port) {
        struct Message *m;
        while ((m = GetMsg(cx_port)) != NULL) ReplyMsg(m);
        DeleteMsgPort(cx_port);
    }
    if (ucl) {
        if (g_scr) detach(&g_scr->ViewPort);
        free_ucoplist(ucl);
    }
    if (g_vi)           FreeVisualInfo(g_vi);
    if (g_scr)          UnlockPubScreen(NULL, g_scr);
    if (IconBase)       CloseLibrary(IconBase);
    if (CxBase)         CloseLibrary(CxBase);
    if (GadToolsBase)   CloseLibrary(GadToolsBase);
    if (GfxBase)        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)  CloseLibrary((struct Library *)IntuitionBase);
    free_cli();
    return rc;
}
