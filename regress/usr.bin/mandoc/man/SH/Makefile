# $OpenBSD: Makefile,v 1.9 2025/07/27 15:31:02 schwarze Exp $

REGRESS_TARGETS	 = broken broken_eline empty_before font
REGRESS_TARGETS	+= longarg noarg paragraph vert
LINT_TARGETS	= broken broken_eline empty_before noarg
HTML_TARGETS	= paragraph
TAG_TARGETS	= paragraph

# groff-1.22.3 defects:
# - .SH without args just before EOF causes two additional blank lines.
# - Empty .SH heads cause additional vertical spacing.
# - If .TP or .IP breaks .SH, the tag becomes bold.
# - If .HP breaks .SH, a line is broken after the next input line.
# - If .RS/RE/nf/fi breaks .SH, the next input line becomes bold.
# - .UR does not break .SH.
# - blank line in .SH next line scope causes blank lines and bogus indent

SKIP_GROFF	= broken broken_eline noarg

.include <bsd.regress.mk>
