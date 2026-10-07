// Myanmar-only bridge to modern HarfBuzz. Private Qt implementation API.
#ifndef QMYANMARHARFBUZZ_P_H
#define QMYANMARHARFBUZZ_P_H
#include "qtextengine_p.h"
QT_BEGIN_NAMESPACE
bool qt_myanmar_harfbuzz_enabled();
bool qt_myanmar_character(uint character);
bool qt_myanmar_continuation(uint character, uint previous);
struct QMyanmarGlyph {
    quint32 glyph;
    QFixed advance;
    QFixedPoint offset;
    bool clusterStart;
};
struct QMyanmarShape {
    QVector<QMyanmarGlyph> glyphs;
    QVector<unsigned short> clusters;
    QFixed ascent, descent, leading;
};
bool qt_shape_myanmar(const QString &text, QFontEngine *font, bool kerning, QMyanmarShape *result);
QT_END_NAMESPACE
#endif
