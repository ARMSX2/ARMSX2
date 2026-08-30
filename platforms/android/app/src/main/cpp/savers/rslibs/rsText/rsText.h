/*
 * Stub for rsText (upstream is LGPL-2.1).
 *
 * Upstream rsText renders a bitmap font, used by the savers only for the on-screen frame-rate
 * readout behind their kStatistics flag. That overlay is meaningless as a wallpaper, and the
 * real implementation would drag in a font atlas and its own GL path, so it is stubbed rather
 * than ported. Signatures match upstream so the call sites compile unchanged.
 */
#ifndef SAVERS_RSTEXT_STUB_H
#define SAVERS_RSTEXT_STUB_H

#include <string>
#include <vector>

class rsText {
public:
    void draw(std::string &) {}
    void draw(std::vector<std::string> &) {}
};

#endif
