// Fork-only diagnostics for the justice line breaker.

#ifndef LVTEXTFM_JUSTICE_DIAG_H_INCLUDED
#define LVTEXTFM_JUSTICE_DIAG_H_INCLUDED

// The counters live in lvtextfm_justice.cpp (included by lvtextfm.cpp); other
// translation units use the accessors below.
//
//   seen     - paragraphs justice actually looked at (0 when creengine was
//              built without it, which is how a caller tells the difference
//              between "not compiled in" and "compiled in, refused everything")
//   planned  - paragraphs justice produced a whole-paragraph plan for
//   lines    - line breaks that plan accounted for
//   spaced   - lines whose spacing justice then applied during alignment
//
// On a justified Latin page, planned == seen is what the eligibility gate
// should give, and spaced == lines means the align pass took justice's spacing
// rather than falling back to crengine's own distribution. A build without
// justice still answers, with zeroes.
void ltext_reset_justice();
void ltext_get_justice(int *seen_out, int *planned_out,
                       int *lines_out, int *spaced_out);

#endif // LVTEXTFM_JUSTICE_DIAG_H_INCLUDED
