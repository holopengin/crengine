/*******************************************************

   justice integration: line breaking and spacing for Latin prose

   Fork-only.  This file is #include'd at the bottom of lvtextfm.cpp,
   as lvtextfm_vert.cpp already is, so it compiles as part of that one
   translation unit and can reach LVFormatter's members and the
   formatter's free functions without adding anything to the API that
   upstream crengine would see.

   justice is a Rust paragraph justifier (vendored under
   base/thirdparty/justice).  It answers a single question — given a
   paragraph, a measure callback, and the width each line has to end
   at, where should the lines break and how far apart should their
   words sit?  crengine then does the drawing, the word splitting, the
   optical margins and everything else exactly as it always did.

   justice is optional: USE_JUSTICE comes from crsetup.h, and when it
   is off nothing here does anything at all, so crengine justifies
   exactly as it did before this file existed.  That matters because we
   build crengine for targets no rustup installation can supply std
   for.

   See include/lvtextfm_fork.h for the types and the calling contract.

*******************************************************/

#include "lvtextfm_justice_diag.h"

#if USE_JUSTICE

#include "justice.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// A UTF-8 view over m_text, with both directions of the char <-> byte map.
//
// crengine measures in UTF-32 char indices; justice indexes a Rust &str, so
// every offset crossing the boundary is a byte offset into UTF-8.  The map is
// built once per paragraph: byte i of a char's encoding belongs to that char,
// so `char_to_byte` (m_length+1 entries) is all we need, and both directions
// are a binary search over it.
// -----------------------------------------------------------------------------
struct Utf8View {
    std::string bytes;
    std::vector<int> char_to_byte;

    void build(const lChar32 * text, int len) {
        char_to_byte.clear();
        char_to_byte.reserve(len + 1);
        bytes.clear();
        char_to_byte.push_back(0);
        for (int i = 0; i < len; i++) {
            lUInt32 c = (lUInt32)text[i];
            if (c >= 0x110000 || (c >= 0xD800 && c <= 0xDFFF))
                c = 0xFFFD; // not a code point: justice would reject the encoding
            if (c < 0x80) {
                bytes += (char)c;
            } else if (c < 0x800) {
                bytes += (char)(0xC0 | (c >> 6));
                bytes += (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                bytes += (char)(0xE0 | (c >> 12));
                bytes += (char)(0x80 | ((c >> 6) & 0x3F));
                bytes += (char)(0x80 | (c & 0x3F));
            } else {
                bytes += (char)(0xF0 | (c >> 18));
                bytes += (char)(0x80 | ((c >> 12) & 0x3F));
                bytes += (char)(0x80 | ((c >> 6) & 0x3F));
                bytes += (char)(0x80 | (c & 0x3F));
            }
            char_to_byte.push_back((int)bytes.size());
        }
    }

    // Char index of the char starting at `byte`. justice only ever asks about
    // word boundaries, which are char boundaries, so the first entry >= byte
    // is exactly the char we want; a byte in the middle of a char would round
    // up, which is the conservative direction (it can only make a range
    // narrower, never invent text that is not there).
    int char_of(int byte) const {
        std::vector<int>::const_iterator it =
            std::lower_bound(char_to_byte.begin(), char_to_byte.end(), byte);
        return (int)(it - char_to_byte.begin());
    }
};

// -----------------------------------------------------------------------------
// The measure callback: how wide is byte range [start, end)?
//
// This is the whole seam with crengine.  justice deliberately knows nothing
// about positions, fonts or kerning — it asks, and the answer comes from the
// advances crengine already measured while shaping the paragraph.  Because the
// range arrives as offsets rather than as text, a word in a bold span and the
// same word in a regular span get different, correct widths, which a callback
// handed the substring could not have done.
// -----------------------------------------------------------------------------
struct JusticeMeasureCtx {
    const int * advance;     // fmt->m_advance: cumulative width through char i
    const Utf8View * view;
};

double justiceMeasure(void * ctx, uint32_t start, uint32_t end, int hyphen) {
    const JusticeMeasureCtx * m = (const JusticeMeasureCtx *)ctx;
    int a = m->view->char_of((int)start);
    int b = m->view->char_of((int)end);
    if (b <= a)
        return 0.0;
    double width = m->advance[b - 1] - (a > 0 ? m->advance[a - 1] : 0);
    // `hyphen` asks for a '-' drawn after the range, and only arrives when a
    // dictionary hyphenation point is used.  crengine's hyphenation is not
    // wired through prepare_ranged (there is no ranged entry point for it
    // yet), so this cannot currently happen; measuring the range unchanged
    // keeps prepare() succeeding rather than quietly under-measuring a line.
    (void)hyphen;
    return width;
}

// -----------------------------------------------------------------------------
// Is this paragraph one justice should touch?
//
// The rule is "Latin prose that crengine would justify normally".  Every gate
// below is a feature justice does not model, and taking it out means the
// greedy walk answers exactly as it did before this code existed — so the
// default for anything unfamiliar is to decline.
// -----------------------------------------------------------------------------
bool justiceEligible(LVFormatter * fmt, src_text_fragment_t * para, bool is_css_first_line) {
    // Only horizontal, justified, plain prose.
    if (css_wm_is_vertical(fmt->m_pbuffer->writing_mode))
        return false;
    if ((para->flags & LTEXT_FLAG_NEWLINE) != LTEXT_ALIGN_WIDTH)
        return false;  // justice exists to replace the justify step
    if (para->flags & LTEXT_LEGACY_RENDERING)
        return false;  // negative indent (hanging) is not the same as an indent
    if (para->flags & LTEXT_FLAG_PREFORMATTED)
        return false;  // preformatted text must keep its spaces exactly
    if (is_css_first_line)
        return false;  // the first-line clone sequence would desync the walk

    // Layout features this does not model, all of which change either the
    // breaks, the words, or the width available to a line.
    if (fmt->m_has_bidi || fmt->m_para_dir_is_rtl)
        return false;
    if (fmt->m_has_cjk)
        return false;  // crengine justifies CJK its own way, per the fork's charter
    if (fmt->m_has_inline_boxes)
        return false;
    if (fmt->m_has_float_to_position || fmt->m_has_ongoing_float)
        return false;  // floats move both the usable width and the line's x

    // crengine's own letter-spacing pass competes for the same leftover space.
    // It is off by default (MAX_ADDED_LETTER_SPACING_PERCENT is 0), so this
    // only rejects a page that asked for it.
    if (fmt->m_pbuffer->max_added_letter_spacing_percent > 0)
        return false;

    // Per-char refusals: an object is not text, a CJK char wants different
    // rules, and a single locked-spacing char forbids tweaking its whole word.
    for (int i = 0; i < fmt->m_length; i++) {
        lUInt16 f = fmt->m_flags[i];
        if (f & LCHAR_MANDATORY_NEWLINE) {
            // measureText() sets this on index 0 of *every* buffer (and on the
            // fragment that opens a line), which is not a break anybody can
            // take: there is no character before index 0, and inside one
            // paragraph no other fragment opens a line. Past index 0 it means a
            // real hard newline - a <br>, or preformatted text - which forces a
            // break justice knows nothing about, so let crengine have it.
            if (i > 0)
                return false;
            continue;
        }
        if (f & (LCHAR_IS_OBJECT | LCHAR_IS_CJK | LCHAR_LOCKED_SPACING))
            return false;
    }
    return true;
}

} // namespace

#endif // USE_JUSTICE

// The counters are declared unconditionally: a build without justice should
// still answer ltext_get_justice(), with zeroes - which is itself the answer
// whoever is asking wants.
int ltext_justice_seen = 0;
int ltext_justice_planned = 0;
int ltext_justice_lines = 0;
int ltext_justice_spaced = 0;

void ltext_reset_justice() {
    ltext_justice_seen = 0;
    ltext_justice_planned = 0;
    ltext_justice_lines = 0;
    ltext_justice_spaced = 0;
}

void ltext_get_justice(int *seen_out, int *planned_out,
                       int *lines_out, int *spaced_out) {
    *seen_out = ltext_justice_seen;
    *planned_out = ltext_justice_planned;
    *lines_out = ltext_justice_lines;
    *spaced_out = ltext_justice_spaced;
}

// Rounding that does not pull every later word along with it: each word's
// target x is rounded from an unrounded running total, so the error stays
// under half a pixel no matter how long the line is.
static inline int justiceRound(double v) {
    return (int)(v >= 0 ? v + 0.5 : v - 0.5);
}

// -----------------------------------------------------------------------------
// Solve one paragraph's breaks and spacing.
//
// On success `plan` holds one entry per line, laid out so that crengine's own
// walk can be driven straight from it: line i's char_end is exactly where line
// i+1 starts, so `wrapPos = char_end - 1` lands on the whitespace the break
// belongs to (the char carrying LCHAR_ALLOW_WRAP_AFTER) and `endp = char_end`
// hands addLineHorizontal a range that ends on that run's last char — which is
// what crengine's greedy walk produces for itself.
//
// Any problem at all leaves `plan.valid` false and the paragraph on the greedy
// path; there is no partial state to unwind.
// -----------------------------------------------------------------------------
void justicePlanParagraph( LVFormatter* fmt, src_text_fragment_t * para,
                           int indent_first, int indent_rest,
                           bool is_css_first_line, justice_plan_t & plan ) {
    plan.valid = false;
    plan.lines.clear();

#if !USE_JUSTICE
    // Built without justice: decline everything, which is exactly the
    // behaviour crengine had before this existed. `seen` stays 0 too, so
    // ltext_get_justice() can tell "not compiled in" from "compiled in and
    // refused everything" - the specs rely on that to skip cleanly.
    (void)fmt; (void)para; (void)indent_first; (void)indent_rest; (void)is_css_first_line;
#else
    ltext_justice_seen++;
    if (!justiceEligible(fmt, para, is_css_first_line))
        return;
    if (fmt->m_length <= 0)
        return;

    // The usable width, worked out exactly the way alignLineHorizontal will:
    //     usable = available_width - (frmline->x - x_offset) - rightIndent
    // rightIndent is 0 for LTR (BiDi was refused above), and frmline->x is the
    // line indent plus whatever the floats shift it by — floats were refused
    // too, so this number is the same one the align pass will compute later.
    int x_offset = 0;
    int width = fmt->getAvailableWidthAtY(fmt->m_line_advance,
                                          fmt->m_pbuffer->strut_height, x_offset);
    int shift = fmt->getCurrentLineX();
    double usable_first = width - (indent_first + shift - x_offset);
    double usable_rest = width - (indent_rest + shift - x_offset);
    if (usable_first <= 0 || usable_rest <= 0)
        return;

    // The space width crengine is going to expand from: the mean advance of
    // the paragraph's real (non-collapsed) spaces.  justice prices stretch and
    // shrink as fractions of it, so it has to be the same number crengine
    // would have used, or the two would disagree about what "full width"
    // costs.  A paragraph with no space at all has no gap to widen, so any
    // positive stand-in keeps the solver's bounds meaningful.
    double space = 0.0;
    int spaces = 0;
    for (int i = 0; i < fmt->m_length; i++) {
        if ((fmt->m_flags[i] & LCHAR_IS_SPACE) && !(fmt->m_flags[i] & LCHAR_IS_COLLAPSED_SPACE)) {
            space += fmt->m_advance[i] - (i > 0 ? fmt->m_advance[i - 1] : 0);
            spaces++;
        }
    }
    space = spaces > 0 ? space / spaces : 1.0;
    if (!(space > 0.0))
        space = 1.0;

    Utf8View view;
    view.build(fmt->m_text, fmt->m_length);

    JusticeMeasureCtx ctx;
    ctx.advance = fmt->m_advance;
    ctx.view = &view;

    JusticeJob * job = justice_prepare(view.bytes.data(), view.bytes.size(),
                                       space, justiceMeasure, &ctx);
    if (!job)
        return; // a malformed input crengine handed us: decline, don't log

    JusticeOptions options;
    justice_options_default(&options);
    // crengine runs its own optical-margins pass before alignLineHorizontal,
    // so justice has to aim at the usable width exactly.  Leaving these at
    // their defaults would let it overshoot into the margin and hope the two
    // agreed on how much.
    options.hanging = 0.0;
    options.opening = 0.0;
    options.protrusion = 0.0;

    // One width per line, the final entry repeating: the first line is the
    // only one that can carry text-indent.
    double widths[2];
    widths[0] = usable_first;
    widths[1] = usable_rest;
    if (justice_solve(job, widths, 2, &options) != 0) {
        justice_free(job);
        return;
    }

    uint32_t line_count = justice_line_count(job);
    std::vector<uint32_t> starts(line_count);
    std::vector<double> spacings(line_count);
    std::vector<double> trackings(line_count);
    bool ok = line_count > 0;
    for (uint32_t i = 0; ok && i < line_count; i++) {
        JusticeLine solved;
        if (justice_line(job, i, &solved) != 0) {
            ok = false;
            break;
        }
        starts[i] = solved.byte_start;
        spacings[i] = solved.word_spacing;
        trackings[i] = solved.tracking;
    }
    justice_free(job);
    if (!ok)
        return;

    // A line's end is where the *next* line starts, not where its own last
    // word stops: the whitespace in between belongs to neither, and crengine's
    // wrapPos has to point at it.  The last line runs to the paragraph end so
    // trailing whitespace is absorbed and stripped exactly as the greedy walk
    // would absorb it.
    for (uint32_t i = 0; i < line_count; i++) {
        justice_line_t line;
        line.char_start = view.char_of((int)starts[i]);
        line.char_end = (i + 1 < line_count)
                            ? view.char_of((int)starts[i + 1])
                            : fmt->m_length;
        line.word_spacing = spacings[i];
        line.tracking = trackings[i];
        plan.lines.push_back(line);
    }

    // Finally, check the invariants the walk below relies on.  It would cope
    // with a broken plan (the guard at the call site falls back), but refusing
    // here means the greedy path is chosen before any line is emitted rather
    // than halfway through the paragraph.
    if (plan.lines[0].char_start != 0)
        plan.lines.clear();
    for (size_t i = 0; i < plan.lines.size(); i++) {
        if (plan.lines[i].char_end <= plan.lines[i].char_start) {
            plan.lines.clear();
            break;
        }
        if (i > 0 && plan.lines[i].char_start != plan.lines[i - 1].char_end) {
            plan.lines.clear();
            break;
        }
        // Every break must sit on a char crengine allows a break after, so
        // the override cannot ask for something the flag tables forbid.
        if (i + 1 < plan.lines.size()
                && !(fmt->m_flags[plan.lines[i].char_end - 1] & LCHAR_ALLOW_WRAP_AFTER)) {
            plan.lines.clear();
            break;
        }
    }
    if (plan.lines.empty())
        return;

    plan.valid = true;
    ltext_justice_planned++;
    ltext_justice_lines += (int)plan.lines.size();
#endif // USE_JUSTICE
}

// -----------------------------------------------------------------------------
// Put one line's words where the solver asked for them.
//
// The words are already stacked naturally by addLineHorizontal, so this is a
// delta: each word's target x is everything before it (its own natural width,
// the gap it opens, and its glyphs' tracking) accumulated in order.  Only
// boundaries crengine may open up are given word-spacing — a word split by a
// style change sits inside one justice word and must not grow a gap.
//
// The line's reported width becomes the true extent of the last word, so
// alignLineHorizontal's usual distribution sees the half-pixel left over and
// finishes the line rather than expanding it all over again.
// -----------------------------------------------------------------------------
void justiceApplySpacing( formatted_line_t * frmline, const justice_line_t & line ) {
    int words = frmline ? (int)frmline->word_count : 0;
    if (words <= 0)
        return;
    ltext_justice_spaced++;

    double pos = 0.0;
    for (int i = 0; i < words; i++) {
        formatted_word_t * word = &frmline->words[i];
        if (i > 0)
            word->x = justiceRound(pos);
        pos += word->width;
        if (i + 1 < words) {
            if (word->flags & LTEXT_WORD_CAN_ADD_SPACE_AFTER)
                pos += line.word_spacing;
            pos += line.tracking * word->distinct_glyphs;
        }
    }
    formatted_word_t * last = &frmline->words[words - 1];
    frmline->width = last->x + last->width;
}
